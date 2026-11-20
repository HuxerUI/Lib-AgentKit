#include "tool_turn.h"
#include "json_wire.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace huxerui::agentkit::detail {
namespace {

struct PendingCall {
  std::string id;
  std::string name;
  std::string arguments;
  bool complete = false;
};

struct Round {
  std::string text;
  std::optional<JsonValue> structured;
  Usage usage{};
  std::vector<PendingCall> calls;
  std::optional<ModelFinished> finished;
  std::size_t argument_bytes = 0;
};

void PublishText(SessionData& session, RunExecution& run, const std::string& text) {
  if (text.empty()) return;
  if (!run.streaming) {
    const MessageId message_id{MakeId()};
    run.streaming = message_id;
    session.messages.push_back(Message{
        .ref = {session.summary.id, message_id, 1}, .sequence = ++session.sequence, .run = run.view.id,
        .author = MessageAuthor::Agent, .state = MessageState::Streaming,
        .blocks = {{BlockId{MakeId()}, TextBlock{text, true}}}});
  } else {
    auto message = std::ranges::find_if(session.messages, [&](const auto& item) {
      return item.ref.message == *run.streaming;
    });
    std::get<TextBlock>(message->blocks.front().body).text += text;
    ++session.sequence;
  }
}

void SealMessage(SessionData& session, RunExecution& run, MessageState state) {
  if (!run.streaming) return;
  auto message = std::ranges::find_if(session.messages, [&](const auto& item) {
    return item.ref.message == *run.streaming;
  });
  message->state = state;
  ++message->ref.version;
  ++session.sequence;
  run.streaming.reset();
}

AgentResult<void> ApplyModelEvent(SessionData& session, RunExecution& run,
                                  Round& round, ModelEvent event) {
  auto protocol = [](std::string message) { return Fail<void>(ErrorCode::ProtocolError, std::move(message)); };
  if (round.finished) return protocol("Extra model event after completion");
  if (auto* text = std::get_if<ModelTextDelta>(&event)) {
    if (!ValidUtf8(text->utf8) || text->utf8.size() > (4 << 20) - round.text.size()) {
      return protocol("Text exceeds limits or contains invalid UTF-8");
    }
    round.text += text->utf8;
    if (text->show_in_transcript) PublishText(session, run, text->utf8);
  } else if (auto* start = std::get_if<ModelToolStarted>(&event)) {
    if (start->provider_call_id.empty() || start->name.empty() || round.calls.size() >= 128 ||
        std::ranges::find(round.calls, start->provider_call_id, &PendingCall::id) != round.calls.end()) {
      return protocol("Invalid or duplicate model call identity");
    }
    round.calls.push_back({std::move(start->provider_call_id), std::move(start->name)});
  } else if (auto* delta = std::get_if<ModelToolArguments>(&event)) {
    auto call = std::ranges::find(round.calls, delta->provider_call_id, &PendingCall::id);
    if (call == round.calls.end() || call->complete || delta->bytes.size() > (1 << 20) - round.argument_bytes) {
      return protocol("Unexpected tool argument delta");
    }
    round.argument_bytes += delta->bytes.size();
    call->arguments += delta->bytes;
  } else if (auto* end = std::get_if<ModelToolFinished>(&event)) {
    auto call = std::ranges::find(round.calls, end->provider_call_id, &PendingCall::id);
    if (call == round.calls.end() || std::exchange(call->complete, true)) {
      return protocol("Unexpected tool call completion");
    }
  } else if (auto* structured = std::get_if<ModelStructuredOutput>(&event)) {
    if (round.structured) return protocol("Duplicate structured result");
    round.structured = std::move(structured->value);
  } else if (auto* usage = std::get_if<ModelUsage>(&event)) {
    if (usage->usage.input_tokens < round.usage.input_tokens || usage->usage.output_tokens < round.usage.output_tokens) {
      return protocol("Usage must be a cumulative turn total");
    }
    round.usage = usage->usage;
  } else {
    round.finished = std::get<ModelFinished>(std::move(event));
  }
  return AgentResult<void>::Success();
}

Async<void> PrepareModelRequest(RuntimeState& state, const SessionData& session, const RunExecution& run,
                                ModelTurnRequest& request) {
  if (!state.agents.at(run.view.agent.id).skill_sources.empty()) {
    auto skills = co_await state.PrepareSkills(run, request);
    if (!skills.Succeeded()) co_return skills;
  }
  if (!run.delegate) {
    auto selection = co_await state.PrepareSelectedContext(session, run, request);
    if (!selection.Succeeded()) co_return selection;
  }
  if (!run.delegate && session.origin.kind == SessionOriginKind::Forked) {
    request.continuation.reset();
    request.allow_server_state = false;
    if (run.separate_inherited_history) {
      auto history = co_await state.AssembleInheritedHistory(run);
      if (!history.Succeeded()) co_return AgentResult<void>::Failure(history.Error());
      std::vector<ModelItem> items;
      items.reserve(history.Value().size() + request.items.size());
      for (auto& item : history.Value()) items.emplace_back(std::move(item));
      std::ranges::move(request.items, std::back_inserter(items));
      request.items = std::move(items);
    } else {
      auto permitted = co_await state.CheckInheritedHistory(run);
      if (!permitted.Succeeded()) co_return permitted;
    }
  }
  std::uint64_t bytes = request.instructions.size();
  for (const auto& summary : request.available_skills)
    bytes += summary.ref.source.value.size() + summary.ref.id.value.size() + summary.name.size() + summary.description.size();
  for (const auto& skill : request.skill_instructions) bytes += skill.markdown.size();
  for (const auto& resource : run.model_skill_resources) bytes += resource.bytes;
  for (const auto& item : request.items) {
    if (const auto* content = std::get_if<ModelContent>(&item)) {
      if (const auto* text = std::get_if<std::string>(&content->body)) bytes += text->size();
      if (const auto* value = std::get_if<JsonValue>(&content->body)) bytes += ToWireJson(*value).dump().size();
    } else if (const auto* result = std::get_if<ModelToolResult>(&item);
               result && result->value && (run.delegate || !run.delegations.empty())) {
      bytes += ToWireJson(*result->value).dump().size();
    }
  }
  const auto limit = state.RunBudget(run).context_bytes.value_or(4 << 20);
  if (bytes > limit)
    co_return Fail<void>(ErrorCode::BudgetExceeded, "Run reached its context budget");
  co_return co_await state.PrepareModelAttachments(session, run, request, limit - bytes);
}

Async<Round> ReadRound(RuntimeState& state, SessionData& session, RunExecution& run,
                       ModelRegistration& model, ModelTurnRequest request, Revision authorized_at) {
  if (authorized_at != state.access_revision) co_return Fail<Round>(ErrorCode::Conflict, "Model input access changed before submission");
  if (run.stop.stop_requested()) co_return Fail<Round>(ErrorCode::AlreadyClosed, "Run closed");
  auto opened = co_await model.provider->StartTurnAsync(std::move(request));
  if (!opened.Succeeded()) co_return AgentResult<Round>::Failure(opened.Error());
  auto reader = std::shared_ptr<ModelTurnStream>(std::move(opened).Value());
  if (!reader) co_return Fail<Round>(ErrorCode::ProtocolError, "Model returned an empty stream");
  run.stream = reader;
  Round round;
  std::optional<AgentError> error;
  while (!run.stop.stop_requested()) {
    auto event = co_await reader->ReadAsync();
    if (run.stop.stop_requested()) break;
    if (!event.Succeeded()) { error = event.Error(); break; }
    if (!event.Value()) break;
    const auto* delta = std::get_if<ModelTextDelta>(&*event.Value());
    const bool publishes_text = delta && delta->show_in_transcript && !delta->utf8.empty();
    auto applied = ApplyModelEvent(session, run, round, std::move(*event.Value()));
    if (!applied.Succeeded()) { error = applied.Error(); break; }
    if (publishes_text) {
      auto committed = co_await RuntimeState::WhenSaved(state.Persist(session));
      if (!committed.Succeeded()) { error = committed.Error(); break; }
    }
  }
  run.stream.reset();
  run.view.usage.input_tokens += std::min(round.usage.input_tokens, UINT64_MAX - run.view.usage.input_tokens);
  run.view.usage.output_tokens += std::min(round.usage.output_tokens, UINT64_MAX - run.view.usage.output_tokens);
  if (run.stop.stop_requested()) co_return Fail<Round>(ErrorCode::AlreadyClosed, "Run stopped reading model output");
  if (error) {
    auto stopped = co_await reader->StopAsync();
    static_cast<void>(stopped);
    co_return AgentResult<Round>::Failure(std::move(*error));
  }
  if (!round.finished) co_return Fail<Round>(ErrorCode::ProtocolError, "Model stream ended without its finish event");
  const auto reason = round.finished->reason;
  if (reason == ModelFinishReason::Length) co_return Fail<Round>(ErrorCode::BudgetExceeded, "Model reached its output limit");
  if (reason == ModelFinishReason::Filtered || reason == ModelFinishReason::Refused) {
    co_return Fail<Round>(ErrorCode::ProviderFailure, "Model filtered or refused the output");
  }
  if ((reason == ModelFinishReason::ToolCalls) != !round.calls.empty() ||
      std::ranges::any_of(round.calls, [](const auto& call) { return !call.complete; })) {
    co_return Fail<Round>(ErrorCode::ProtocolError, "Model tool stream is incomplete");
  }
  co_return AgentResult<Round>::Success(std::move(round));
}

bool AtTokenBudget(const RunExecution& run, const Budget& budget) {
  if (!budget.tokens) return false;
  const auto input = run.view.usage.input_tokens;
  return input >= *budget.tokens || run.view.usage.output_tokens >= *budget.tokens - input;
}

bool OverTokenBudget(const RunExecution& run, const Budget& budget) {
  if (!budget.tokens) return false;
  const auto input = run.view.usage.input_tokens;
  return input > *budget.tokens || run.view.usage.output_tokens > *budget.tokens - input;
}

} // namespace

void RuntimeState::ApplySteers(SessionData& session, RunExecution& run, std::size_t count, bool apply) {
  if (!count) return;
  auto boundary = run.pending_steers.begin() + static_cast<std::ptrdiff_t>(count);
  for (auto next = run.pending_steers.begin(); next != boundary; ++next) {
    auto& [message, input] = *next;
    auto state = std::ranges::find(run.view.steering, message, &RunSteerSnapshot::message);
    state->state = apply ? SteerState::Applied : SteerState::NotApplied;
    if (!apply) {
      state->reason = Error(ErrorCode::AlreadyClosed, "Run ended before this input was applied");
    }
    SettleCollaborationSteer(session, *state, apply);
    if (state->state == SteerState::Applied) {
      run.items.push_back(ModelContent{ModelContent::Role::User, std::move(input.text)});
      for (const auto& ref : input.attachments) run.items.push_back(ModelContent{ModelContent::Role::User, ref});
      run.input.context.insert(run.input.context.end(), input.context.begin(), input.context.end());
      for (const auto& skill : input.skills) {
        if (std::ranges::find(run.input.skills, skill) == run.input.skills.end()) run.input.skills.push_back(skill);
      }
    }
    for (auto& [scope, command] : commands) {
      auto* accepted = std::get_if<RunSteered>(&command.accepted);
      if (accepted && accepted->message == message) {
        Resolve(command, session, apply ? CommandState::Applied : CommandState::Failed, state->reason);
        break;
      }
    }
  }
  run.pending_steers.erase(run.pending_steers.begin(), boundary);
  ++session.sequence;
}

void RuntimeState::Launch(RunExecution& run) {
  const auto id = run.view.id;
  run.task = options.tasks.Launch([self = shared_from_this(), id] { return self->Drive(id); });
  if (run.deadline && !run.delegate) {
    auto duration = *run.deadline - Now();
    run.timer = options.tasks.Launch([self = shared_from_this(), id, duration] { return self->RunDeadline(id, duration); });
  }
}

Task<void> RuntimeState::RunDeadline(RunId id, Duration duration) {
  co_await Delay(std::max(Duration::zero(), duration));
  auto& run = runs.at(id);
  if (!Terminal(run.view.state) && run.view.state != RunState::Cancelling) Stop(run, RunEndReason::Deadline);
}

void RuntimeState::Finish(RunExecution& run, RunState state, RunEndReason reason, std::optional<AgentError> error,
                          std::optional<RunCompletion> result) {
  if (Terminal(run.view.state)) return;
  auto& session = sessions.at(run.view.session);
  if (HasActiveDelegates(run)) {
    if (run.finishing) return;
    run.finishing = true;
    run.stop.request_stop();
    UpdateRunState(session, run, RunState::Cancelling);
    StopDelegates(run);
    options.tasks.Launch([self = shared_from_this(), id = run.view.id, state, reason,
        error = std::move(error), result = std::move(result)]() mutable -> Task<void> {
      auto& execution = self->runs.at(id);
      while (self->HasActiveDelegates(execution)) co_await Delay(Duration(20));
      self->Finish(execution, state, reason, std::move(error), std::move(result));
    });
    return;
  }
  UpdateRunState(session, run, state);
  run.view.end_reason = reason;
  run.view.failure = std::move(error);
  run.view.result = std::move(result);
  FinishWork(session, run.view);
  auto* owner = FinishGoalRun(run.view);
  run.view.waits.clear();
  run.timer.Cancel();
  SealMessage(session, run, state == RunState::Succeeded ? MessageState::Complete : MessageState::Interrupted);
  ApplySteers(session, run, run.pending_steers.size(), false);
  run.items.clear();
  run.continuation.reset();
  run.pending_calls.clear();
  run.host_model_step.reset();
  run.model_boundary = false;
  run.tool_attempts.clear();
  for (auto& tool : session.tools) {
    if (tool.run != run.view.id || (tool.state != ToolCallState::Preparing && tool.state != ToolCallState::AwaitingApproval &&
                                   tool.state != ToolCallState::Ready && tool.state != ToolCallState::Reviewing)) continue;
    if (tool.review && (tool.review->outcome == ToolReviewOutcome::Pending || tool.review->outcome == ToolReviewOutcome::Approved)) {
      tool.review->outcome = ToolReviewOutcome::Invalidated;
      tool.review->reason = "Run ended before the reviewed plan executed";
    }
    tool.state = ToolCallState::Cancelled;
  }
  for (auto& [command, tool] : run.tool_requests) {
    if (!tool.result && !tool.failure) {
      const auto call = std::ranges::find(session.tools, tool.call, &ToolCallSnapshot::id);
      const auto code = call != session.tools.end() && call->state == ToolCallState::ResultUnknown
          ? ErrorCode::ResultUnknown : ErrorCode::AlreadyClosed;
      tool.failure = Error(code, "Run ended before this tool completed");
    }
    if (ArtifactChangeTool(tool.ref.id)) SettleArtifactChange(commands.at({run.principal, command}), session, tool);
  }
  for (auto& interaction : session.interactions) {
    if (interaction.run == run.view.id) CloseInteraction(session, interaction, InteractionState::Cancelled);
  }
  if (!agents.at(run.view.agent.id).skill_sources.empty()) {
    for (auto& [key, command] : commands) {
      if (command.input.operation == Operation::ActivateSkill && command.input.run == run.view.id &&
          command.progress.state == CommandState::Scheduled) {
        Resolve(command, session, CommandState::Withdrawn, Error(ErrorCode::AlreadyClosed, "Run ended before Skill activation"));
        command.progress.resolution->commit = {};
      }
    }
  }
  if (session.summary.active_run == run.view.id) session.summary.active_run.reset();
  ++session.sequence;
  auto* project = FinishProjectRun(run.view);
  if (auto* comparison = FinishForkCandidate(run.view)) project = comparison;
  auto saved = Persist(session, false, {}, owner, project);
  if (run.view.parent) {
    auto& parent = runs.at(*run.view.parent);
    if (auto waiter = std::exchange(parent.delegate_waiter, {})) waiter->Complete(true);
    ScheduleDelegates(session);
  } else if (stage == Stage::Started && (session.summary.queued_inputs || session.current_goal)) {
    options.tasks.Launch([self = shared_from_this(), id = session.summary.id, saved]() -> Task<void> {
      auto committed = co_await WhenSaved(saved);
      if (committed.Succeeded()) co_await self->ConsumeQueue(id);
    });
  }
}

void RuntimeState::Stop(RunExecution& run, RunEndReason reason) {
  auto& session = sessions.at(run.view.session);
  UpdateRunState(session, run, RunState::Cancelling);
  run.view.waits.clear();
  run.suspended = false;
  run.stop.request_stop();
  if (auto waiter = std::exchange(run.delegate_waiter, {})) waiter->Complete(false);
  StopDelegates(run);
  run.InterruptToolReviews();
  if (run.tool_turn) run.tool_turn->Abort(Error(ErrorCode::AlreadyClosed, "Run stopped"));
  const bool revised_linked_work = reason == RunEndReason::GoalRevised && RetireWorkWithGoal(run);
  for (auto& interaction : session.interactions) {
    if (interaction.run == run.view.id) {
      const auto state = reason == RunEndReason::Deadline ? InteractionState::Expired :
          revised_linked_work ? InteractionState::Invalidated : InteractionState::Cancelled;
      CloseInteraction(session, interaction, state);
    }
  }
  ++session.sequence;
  if (session.retention == SessionRetention::Ephemeral) Persist(session);
  options.tasks.Launch([self = shared_from_this(), id = run.view.id, stream = run.stream, reason] {
    return StopStream(self, id, stream, reason);
  });
}

Task<void> RuntimeState::StopStream(std::shared_ptr<RuntimeState> self, RunId id,
                                    std::shared_ptr<ModelTurnStream> stream, RunEndReason reason) {
  auto& run = self->runs.at(id);
  TaskHandle stopping;
  auto stop_received = std::make_shared<bool>(!stream);
  if (stream) {
    stopping = self->options.tasks.Launch([self, id, stream, stop_received]() -> Task<void> {
      try {
        auto result = co_await stream->StopAsync();
        self->runs.at(id).model_stop = result.Succeeded() ? result.Value() : ModelStopOutcome::ProviderUnknown;
      } catch (...) { self->ReportException(std::current_exception()); }
      *stop_received = true;
    });
  }
  for (int elapsed = 0; elapsed < 2000 && (run.ExecutingTool() || run.stream || !*stop_received); elapsed += 20) {
    co_await Delay(Duration(20));
  }
  stopping.Cancel();
  if (stream && !run.model_stop) run.model_stop = ModelStopOutcome::ProviderUnknown;
  run.task.Cancel();
  run.host_tool_task.Cancel();
  self->CancelToolTurnTasks(run);
  run.stream.reset();
  auto& session = self->sessions.at(run.view.session);
  for (const auto& [call, attempt] : run.tool_attempts) {
    if (!attempt.executing) continue;
    auto pending = std::ranges::find(session.tools, call, &ToolCallSnapshot::id);
    if (attempt.issued && (attempt.executing == ToolEffect::WriteResource || attempt.executing == ToolEffect::ExternalSubmit)) {
      pending->state = ToolCallState::ResultUnknown;
      pending->reason = Error(ErrorCode::ResultUnknown, "Reconcile tool effects before retrying");
    } else {
      pending->state = ToolCallState::Cancelled;
    }
  }
  while (self->HasActiveDelegates(run)) co_await Delay(Duration(20));
  if (reason == RunEndReason::UserCancelled || reason == RunEndReason::ParentCancelled || reason == RunEndReason::GoalRevised) {
    self->Finish(run, RunState::Cancelled, reason);
  } else {
    self->Finish(run, RunState::Failed, reason, Error(
        reason == RunEndReason::Deadline ? ErrorCode::DeadlineExceeded :
        reason == RunEndReason::Budget ? ErrorCode::BudgetExceeded : ErrorCode::RuntimeClosed, "Run interrupted"));
  }
}

Async<void> RuntimeState::DriveToolTurn(RunId id) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  const AgentError* recorded_failure = nullptr;
  for (const auto& pending : run.pending_calls) {
    if (pending.terminal_error && (!recorded_failure || pending.terminal_error->code == ErrorCode::ResultUnknown))
      recorded_failure = &*pending.terminal_error;
  }
  if (recorded_failure) co_return AgentResult<void>::Failure(*recorded_failure);
  while (!run.pending_calls.empty() && !run.stop.stop_requested()) {
    if (!run.view.active_skills.empty()) {
      auto skills = co_await VerifyActiveSkills(run, true);
      if (!skills.Succeeded()) co_return skills;
    }
    auto& pending = run.pending_calls.front();
    const bool skill_tool = !pending.error && pending.tool && SkillTool(pending.tool->id.value);
    const bool delegation_tool = !pending.error && pending.tool && pending.tool->id == DelegationToolId;
    if (!pending.result && !pending.error && pending.tool && !skill_tool && !delegation_tool && options.max_parallel_tools_per_turn > 1) {
      const auto definition = ResolveTool(*pending.tool);
      if (definition && definition->concurrency != ToolConcurrency::Serial) {
        auto dispatched = co_await DriveParallelTools(id);
        if (!dispatched.Succeeded()) co_return dispatched;
        continue;
      }
    }
    if (!pending.budget_reserved) {
      if (run.view.usage.tool_calls >= RunBudget(run).tool_calls.value_or(128)) {
        co_return Fail<void>(ErrorCode::BudgetExceeded, "Run reached its tool budget");
      }
      ++run.view.usage.tool_calls;
      pending.budget_reserved = true;
      if (skill_tool || delegation_tool) {
        if (delegation_tool) pending.delegation = CommandId{MakeId()};
        ++session.sequence;
        auto saved = co_await WhenSaved(Persist(session));
        if (run.stop.stop_requested()) co_return AgentResult<void>::Success();
        if (!saved.Succeeded()) co_return saved;
      }
    }
    run.model_boundary = skill_tool || delegation_tool;
    if (pending.result) {
      run.items.push_back(std::move(*pending.result));
    } else if (pending.error || !pending.tool) {
      run.items.push_back(ModelToolResult{pending.call.provider_call_id, {}, {}, pending.error.value_or(
          Error(ErrorCode::PermissionDenied, "Tool was not offered for this turn"))});
    } else if (skill_tool || delegation_tool) {
      AgentResult<JsonValue> result = Fail<JsonValue>(ErrorCode::InvalidState, "Missing delegation command");
      if (skill_tool) result = co_await ExecuteSkillTool(id, pending.tool->id.value, pending.call.arguments);
      else if (pending.delegation) result = co_await ExecuteDelegationTool(id, *pending.delegation, pending.call.arguments);
      if (run.stop.stop_requested()) co_return AgentResult<void>::Success();
      if (!result.Succeeded() && (result.Error().code == ErrorCode::PersistenceFailure ||
                                 result.Error().code == ErrorCode::CommitUncertain)) {
        co_return AgentResult<void>::Failure(result.Error());
      }
      if (result.Succeeded()) run.items.push_back(ModelToolResult{pending.call.provider_call_id, {}, std::move(result).Value(), {}});
      else run.items.push_back(ModelToolResult{pending.call.provider_call_id, {}, {}, result.Error()});
    } else {
      RecordModelTool(session, id, pending);
      co_await DriveModelTool(id, pending);
      if (run.stop.stop_requested()) co_return AgentResult<void>::Success();
      if (pending.terminal_error) co_return AgentResult<void>::Failure(*pending.terminal_error);
      run.items.push_back(std::move(*pending.result));
    }
    run.pending_calls.pop_front();
    run.model_boundary = true;
    ++session.sequence;
    auto committed = co_await WhenSaved(Persist(session));
    if (!committed.Succeeded()) co_return committed;
  }
  co_return AgentResult<void>::Success();
}

namespace {

bool SupportsCalls(const ModelCapabilities& capabilities, std::size_t count) {
  return count == 0 || (capabilities.tool_calls && (capabilities.multi_tool_calls || count == 1));
}

void QueueModelCalls(RunExecution& run, const Round& round, const std::vector<ModelToolSpec>& available,
                     const std::vector<SkillSummary>& catalog) {
  for (const auto& call : round.calls) {
    auto decoded = ParseJson(call.arguments);
    const auto spec = std::ranges::find(available, call.name, &ModelToolSpec::name);
    auto error = decoded.Succeeded() ? std::nullopt : std::optional<AgentError>(decoded.Error());
    if (!error && spec != available.end() && spec->tool.id == SkillActivationTool)
      error = CheckSkillOffer(decoded.Value(), catalog);
    run.pending_calls.push_back({
        ModelToolCall{call.id, call.name, decoded.Succeeded() ? std::move(decoded).Value() : JsonValue{}},
        spec == available.end() ? std::nullopt : std::optional<ToolRef>(spec->tool),
        std::move(error)});
    run.items.push_back(run.pending_calls.back().call);
  }
}

Task<std::vector<ModelToolSpec>> DiscoverModelTools(RuntimeState& runtime, const SessionData& session,
    const RunExecution& run, std::span<const ToolId> filter = {}) {
  std::vector<ModelToolSpec> available;
  for (const auto& registered : runtime.CaptureAgentTools(runtime.agents.at(run.view.agent.id))) {
    const auto& tool = registered.ref;
    if ((!filter.empty() && std::ranges::find(filter, tool.id) == filter.end()) ||
        !runtime.AllowsRunTool(run, tool)) continue;
    auto request = runtime.Request(run.principal, Capability::DiscoverTool, &session, &run);
    request.tool = tool;
    request.recipient_model = run.view.model.id;
    auto discovered = co_await runtime.Require(std::move(request));
    if (run.stop.stop_requested()) break;
    if (discovered.Succeeded())
      available.push_back({tool, tool.id.value, registered.definition->description, registered.definition->input_schema});
  }
  co_return available;
}

Async<std::vector<ArtifactRef>> ReadModelPublications(RuntimeState& runtime, const SessionData& session,
    const RunExecution& run, const HostedModelStep& step) {
  using Result = AgentResult<std::vector<ArtifactRef>>;
  if (step.published.empty()) co_return Result::Success({});
  const auto epoch = runtime.access_revision;
  const auto publication_owner = runtime.Request(run.principal, Capability::ReadArtifact, &session, &run).actor;
  std::vector<ArtifactRef> artifacts;
  // Results enter the transcript in model call order even when tools execute in parallel.
  for (std::size_t i = *step.input_index + 1; i < run.items.size(); ++i) {
    const auto* result = std::get_if<ModelToolResult>(&run.items[i]);
    if (!result || !result->call) continue;
    const auto call = *result->call;
    const auto refs = step.published.find(call);
    if (refs == step.published.end()) continue;
    auto actor = publication_owner;
    actor.call = call;
    for (const auto& ref : refs->second) {
      auto artifact = co_await runtime.ReadArtifact(ref, actor);
      if (run.stop.stop_requested()) co_return Fail<std::vector<ArtifactRef>>(ErrorCode::AlreadyClosed, "Run closed");
      if (epoch != runtime.access_revision)
        co_return Fail<std::vector<ArtifactRef>>(ErrorCode::Conflict, "Artifact access changed while completing the model step");
      if (!artifact.Succeeded()) continue;
      const auto& record = artifact.Value().record;
      if (record.metadata.ref == ref && record.session == session.summary.id && record.run == run.view.id &&
          record.call == call) artifacts.push_back(ref);
    }
  }
  auto user = runtime.User();
  if (!user.Succeeded()) co_return Result::Failure(user.Error());
  if (user.Value() != run.principal)
    co_return Fail<std::vector<ArtifactRef>>(ErrorCode::PermissionDenied, "The acting principal changed");
  co_return Result::Success(std::move(artifacts));
}

} // namespace

Async<ModelReply> RuntimeState::RunHostModel(RunId id, ModelInput input) {
  using Result = AgentResult<ModelReply>;
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  const auto& agent = agents.at(run.view.agent.id);
  auto& model = models.at(run.view.model.id);
  const auto caps = model.provider->Capabilities();
  std::ranges::sort(input.allowed_tools);
  input.allowed_tools.erase(std::ranges::unique(input.allowed_tools).begin(), input.allowed_tools.end());
  const bool allow_tools = !input.allowed_tools.empty();
  const bool ordinary_tools = std::ranges::any_of(input.allowed_tools, [](const ToolId& id) { return !SkillTool(id.value); });
  if (run.host_model_busy) co_return Fail<ModelReply>(ErrorCode::Busy, "A model turn is already open");
  if (run.deadline && Now() >= *run.deadline) co_return Fail<ModelReply>(ErrorCode::DeadlineExceeded, "Run expired");
  if (allow_tools && (!caps.tool_calls || input.output_schema)) {
    co_return Fail<ModelReply>(ErrorCode::Unsupported, "Use an unstructured completion with a model that supports tools");
  }
  if (!ValidUtf8(input.instruction)) co_return Fail<ModelReply>(ErrorCode::InvalidArgument, "Invalid UTF-8 instructions");
  if (input.output_schema && !caps.structured_output)
    co_return Fail<ModelReply>(ErrorCode::Unsupported, "Model does not support structured output");
  if (ordinary_tools && (run.dispatched_host_tool || std::ranges::any_of(run.tool_requests, [](const auto& entry) {
        return !entry.second.result && !entry.second.failure;
      }))) co_return Fail<ModelReply>(ErrorCode::Busy, "An independent hosted tool call is pending");
  if (run.host_model_step && run.host_model_step->input_index) {
    const auto& step = *run.host_model_step;
    const auto index = *step.input_index;
    const auto* item = index < run.items.size() ? std::get_if<ModelContent>(&run.items[index]) : nullptr;
    const auto* original = item ? std::get_if<std::string>(&item->body) : nullptr;
    const bool same_scope = step.allowed_tools.empty() ? !ordinary_tools : step.allowed_tools == input.allowed_tools;
    if (!allow_tools || !same_scope || !original || item->role != ModelContent::Role::User || *original != input.instruction ||
        std::ranges::any_of(run.pending_calls, [&](const auto& call) {
          return call.tool && std::ranges::find(input.allowed_tools, call.tool->id) == input.allowed_tools.end();
        })) {
      co_return Fail<ModelReply>(ErrorCode::Conflict, "Resume the original hosted model step");
    }
  } else if (allow_tools) {
    run.host_model_step = HostedModelStep{};
  } else {
    run.host_model_step.reset();
  }
  if (run.host_model_step) run.host_model_step->allowed_tools = input.allowed_tools;
  run.host_model_busy = true;
  const auto reset_busy = std::unique_ptr<bool, void(*)(bool*)>(&run.host_model_busy, [](bool* value) { *value = false; });
  if (!run.input.skills.empty()) {
    auto skills = co_await ActivateInputSkills(run);
    if (!skills.Succeeded()) co_return Result::Failure(skills.Error());
  }

  const auto before = run.view.usage;
  for (;;) {
    if (!run.pending_calls.empty()) {
      auto dispatched = co_await DriveToolTurn(id);
      if (!dispatched.Succeeded()) co_return Result::Failure(dispatched.Error());
    }
    if (run.stop.stop_requested()) co_return Fail<ModelReply>(ErrorCode::AlreadyClosed, "Run closed");
    auto budget = RunBudget(run);
    if (AtTokenBudget(run, budget) || run.view.usage.model_turns >= budget.model_turns.value_or(32))
      co_return Fail<ModelReply>(ErrorCode::BudgetExceeded, "Run reached its model budget");

    const auto authorized_at = access_revision;
    auto outbound = Request(run.principal, Capability::SendToModel, &session, &run);
    outbound.recipient_model = model.ref.id;
    auto allowed = co_await Require(std::move(outbound));
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    if (run.stop.stop_requested()) co_return Fail<ModelReply>(ErrorCode::AlreadyClosed, "Run closed");
    if (allow_tools && !run.host_model_step->input_index) {
      run.host_model_step->input_index = run.items.size();
      run.items.push_back(ModelContent{ModelContent::Role::User, input.instruction});
    }
    ModelTurnRequest request{.turn = {MakeId()}, .run = id, .provider = run.view.model,
        .continuation = allow_tools ? run.continuation : std::nullopt,
        .allow_server_state = false, .instructions = agent.instructions, .items = run.items,
        .output_schema = input.output_schema, .deadline = run.deadline, .stop = run.stop.get_token()};
    auto context = co_await AssembleProjectInput(run, input.context);
    if (!context.Succeeded()) co_return Result::Failure(context.Error());
    for (auto& item : context.Value()) request.items.push_back(std::move(item));
    auto goal_context = co_await GoalContext(run);
    if (!goal_context.Succeeded()) co_return Result::Failure(goal_context.Error());
    if (goal_context.Value()) request.items.push_back(std::move(*goal_context.Value()));
    if (!allow_tools) request.items.push_back(ModelContent{ModelContent::Role::User, input.instruction});
    auto prepared = co_await PrepareModelRequest(*this, session, run, request);
    if (!prepared.Succeeded()) co_return Result::Failure(prepared.Error());
    const bool use_continuation = allow_tools && request.items.size() == run.items.size();
    if (!use_continuation) request.continuation.reset();
    if (allow_tools) {
      request.tools = co_await DiscoverModelTools(*this, session, run, input.allowed_tools);
      if (run.stop.stop_requested()) co_return Fail<ModelReply>(ErrorCode::AlreadyClosed, "Run closed");
      for (auto& tool : SkillTools(request)) {
        if (std::ranges::find(input.allowed_tools, tool.tool.id) != input.allowed_tools.end())
          request.tools.push_back(std::move(tool));
      }
    }
    const auto offered = request.tools;
    const auto catalog = request.available_skills;
    budget = RunBudget(run);
    if (AtTokenBudget(run, budget) || run.view.usage.model_turns >= budget.model_turns.value_or(32))
      co_return Fail<ModelReply>(ErrorCode::BudgetExceeded, "Run reached its model budget");
    ++run.view.usage.model_turns;
    ++session.sequence;
    auto committed = co_await WhenSaved(Persist(session));
    if (!committed.Succeeded()) co_return Result::Failure(committed.Error());
    if (run.stop.stop_requested()) co_return Fail<ModelReply>(ErrorCode::AlreadyClosed, "Run closed");
    auto response = co_await ReadRound(*this, session, run, model, std::move(request), authorized_at);
    if (!response.Succeeded()) {
      SealMessage(session, run, MessageState::Interrupted);
      Persist(session);
      co_return Result::Failure(response.Error());
    }
    auto round = std::move(response).Value();
    if ((run.delegate || !run.delegations.empty()) && OverTokenBudget(run, RunBudget(run)))
      co_return Fail<ModelReply>(ErrorCode::BudgetExceeded, "Run exceeded its token budget");
    if (!SupportsCalls(caps, round.calls.size()) || (!round.calls.empty() && !allow_tools))
      co_return Fail<ModelReply>(ErrorCode::ProtocolError, "Provider emitted an unrequested tool call");
    SealMessage(session, run, MessageState::Complete);
    if (!round.calls.empty()) {
      if (AtTokenBudget(run, RunBudget(run))) co_return Fail<ModelReply>(ErrorCode::BudgetExceeded, "Run reached its token budget");
      if (!round.text.empty()) run.items.push_back(ModelContent{ModelContent::Role::Assistant, round.text});
      QueueModelCalls(run, round, offered, catalog);
      run.continuation.reset();
      if (use_continuation && round.finished->resume) {
        if (round.finished->resume->provider != run.view.model || !round.finished->resume->format_version)
          co_return Fail<ModelReply>(ErrorCode::ProtocolError, "Continuation does not match the configured model");
        run.continuation = ModelContinuation{std::move(*round.finished->resume), run.items.size()};
      }
      run.model_boundary = true;
      committed = co_await WhenSaved(Persist(session));
      if (!committed.Succeeded()) co_return Result::Failure(committed.Error());
      continue;
    }
    if (input.output_schema) {
      if (!round.structured) co_return Fail<ModelReply>(ErrorCode::ProtocolError, "Provider did not return structured output");
      auto valid = ValidateJson(*round.structured, *input.output_schema);
      if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
    }
    auto completed_step = std::exchange(run.host_model_step, {});
    if (completed_step) {
      run.continuation.reset();
      run.model_boundary = false;
    }
    ++session.sequence;
    committed = co_await WhenSaved(Persist(session));
    if (!committed.Succeeded()) co_return Result::Failure(committed.Error());
    if (run.stop.stop_requested()) co_return Fail<ModelReply>(ErrorCode::AlreadyClosed, "Run closed");
    const auto after = run.view.usage;
    std::vector<ArtifactRef> artifacts;
    if (completed_step) {
      auto visible = co_await ReadModelPublications(*this, session, run, *completed_step);
      if (!visible.Succeeded()) co_return Result::Failure(visible.Error());
      artifacts = std::move(visible).Value();
    }
    co_return Result::Success({std::move(round.text), std::move(round.structured),
        {after.input_tokens - before.input_tokens, after.output_tokens - before.output_tokens,
         after.model_turns - before.model_turns, after.tool_calls - before.tool_calls}, std::move(artifacts)});
  }
}

Task<void> RuntimeState::Drive(RunId id) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  const auto& agent = agents.at(run.view.agent.id);
  auto& model = models.at(run.view.model.id);
  auto fail = [&](AgentError error) {
    const auto reason = error.code == ErrorCode::BudgetExceeded ? RunEndReason::Budget :
        error.code == ErrorCode::ProtocolError || error.code == ErrorCode::ProviderFailure ? RunEndReason::Interrupted :
        error.code == ErrorCode::ResultUnknown ? RunEndReason::ToolOutcomeUnknown : RunEndReason::Failed;
    Finish(run, RunState::Failed, reason, std::move(error));
  };
  try {
    if ((run.deadline && Now() >= *run.deadline)) { Stop(run, RunEndReason::Deadline); co_return; }
    if (run.delegate) {
      const auto& parent = runs.at(*run.view.parent);
      auto allowed = co_await CheckDelegation(parent, {run.view.agent.id, run.input, run.delegate->budget, run.delegate->expected_result});
      if (allowed.Succeeded()) allowed = co_await Require(Request(run.principal, Capability::StartSessionRun, &session, &run));
      if (run.stop.stop_requested()) co_return;
      if (!allowed.Succeeded()) { fail(allowed.Error()); co_return; }
    }
    const auto validate_result = [&](const RunCompletion& result) {
      if (!run.delegate || !run.delegate->expected_result) return AgentResult<void>::Success();
      if (!result.structured) return Fail<void>(ErrorCode::ProtocolError, "The delegated result requires structured output");
      return ValidateJson(*result.structured, *run.delegate->expected_result);
    };
    if (run.view.trigger == RunTrigger::ArtifactChange) {
      co_await DriveArtifactChange(id);
      co_return;
    }
    if (run.restoring && !run.view.active_skills.empty()) {
      auto skills = co_await VerifyActiveSkills(run);
      if (run.stop.stop_requested()) co_return;
      if (!skills.Succeeded()) {
        Finish(run, RunState::Failed, RunEndReason::Interrupted, skills.Error());
        co_return;
      }
    }
    if (const auto workflow = workflows.find(run.view.agent.id); workflow != workflows.end()) {
      if (!run.delegate) {
        auto access = co_await Require(Request(run.principal, Capability::StartSessionRun, &session, &run));
        if (run.stop.stop_requested()) co_return;
        if (!access.Succeeded()) { fail(access.Error()); co_return; }
      }
      if (!run.input.skills.empty()) {
        auto skills = co_await ActivateInputSkills(run);
        if (run.stop.stop_requested()) co_return;
        if (!skills.Succeeded()) { fail(skills.Error()); co_return; }
      }
      if (!run.input.skills.empty() && run.model_boundary) {
        run.model_boundary = false;
        ++session.sequence;
        auto saved = co_await WhenSaved(Persist(session));
        if (run.stop.stop_requested()) co_return;
        if (!saved.Succeeded()) { fail(saved.Error()); co_return; }
      }
      RunContext context(shared_from_this(), id);
      RunInput input{id, run.view.trigger, run.input, run.view.work_request, run.view.task,
                     run.view.goal, {}, run.artifact_action};
      const auto first = std::ranges::find(session.messages, std::optional<RunId>{id}, &Message::run);
      if (first != session.messages.end()) input.conclusions = MessageConclusions(*first);
      if (!input.conclusions.empty()) {
        auto actor = Request(run.principal, Capability::ReadContext, &session, &run).actor;
        auto permitted = co_await CheckConclusions(session.summary.id, std::move(actor), input.conclusions, {});
        if (run.stop.stop_requested()) co_return;
        if (!permitted.Succeeded()) input.conclusions.clear();
      }
      AgentResult<RunCompletion> outcome = Fail<RunCompletion>(ErrorCode::InvalidState, "No run handler");
      if (run.restoring && run.checkpoint) {
        RunResume resume{run.checkpoint, run.view.waits};
        outcome = co_await workflow->second.restore(std::move(context), std::move(input), std::move(resume));
      } else {
        outcome = co_await workflow->second.run(std::move(context), std::move(input));
      }
      if (run.stop.stop_requested()) co_return;
      if (run.dispatched_host_tool) {
        auto tool = co_await WaitForHostedTool(id, *run.dispatched_host_tool);
        if (run.stop.stop_requested()) co_return;
        if (outcome.Succeeded() && (!tool.Succeeded() || tool.Value().outcome != ToolOutcome::Succeeded)) {
          fail(tool.Succeeded() ? tool.Value().reason.value_or(Error(ErrorCode::InvalidState, "Workflow tool failed"))
                                : tool.Error());
          co_return;
        }
      }
      if (!outcome.Succeeded()) { fail(outcome.Error()); co_return; }
      auto valid = validate_result(outcome.Value());
      if (!valid.Succeeded()) { fail(valid.Error()); co_return; }
      if (!outcome.Value().summary.empty()) PublishText(session, run, outcome.Value().summary);
      Finish(run, RunState::Succeeded, RunEndReason::Completed, {}, std::move(outcome).Value());
      co_return;
    }
    while (!run.stop.stop_requested()) {
      if ((run.deadline && Now() >= *run.deadline)) { Stop(run, RunEndReason::Deadline); co_return; }
      if (!run.pending_calls.empty()) {
        auto dispatched = co_await DriveToolTurn(id);
        if (run.stop.stop_requested()) co_return;
        if (!dispatched.Succeeded()) { fail(dispatched.Error()); co_return; }
      }
      auto budget = RunBudget(run);
      if (AtTokenBudget(run, budget) || run.view.usage.model_turns >= budget.model_turns.value_or(32)) {
        fail(Error(ErrorCode::BudgetExceeded, "Run reached its model budget"));
        co_return;
      }
      const auto authorized_at = access_revision;
      auto outbound = Request(run.principal, Capability::SendToModel, &session, &run);
      outbound.recipient_model = model.ref.id;
      auto access = co_await Require(std::move(outbound));
      if (run.stop.stop_requested()) co_return;
      if (!access.Succeeded()) { fail(access.Error()); co_return; }
      auto available = co_await DiscoverModelTools(*this, session, run);
      if (run.stop.stop_requested()) co_return;
      ApplySteers(session, run, run.pending_steers.size(), true);
      if (!run.input.skills.empty()) {
        auto skills = co_await ActivateInputSkills(run);
        if (run.stop.stop_requested()) co_return;
        if (!skills.Succeeded()) { fail(skills.Error()); co_return; }
        if (authorized_at != access_revision) continue;
      }
      auto context = co_await AssembleProjectInput(run);
      if (run.stop.stop_requested()) co_return;
      if (!context.Succeeded()) { fail(context.Error()); co_return; }
      auto goal_context = co_await GoalContext(run);
      if (run.stop.stop_requested()) co_return;
      if (!goal_context.Succeeded()) { fail(goal_context.Error()); co_return; }
      const bool uses_context = !context.Value().empty() || goal_context.Value().has_value();
      ModelTurnRequest request{
          .turn = {MakeId()}, .run = id, .provider = run.view.model, .continuation = uses_context ? std::nullopt : run.continuation,
          .allow_server_state = !uses_context && agent.skill_sources.empty() && model.allow_server_state, .instructions = agent.instructions,
          .items = run.items, .tools = available,
          .output_schema = run.delegate ? run.delegate->expected_result : std::nullopt,
          .deadline = run.deadline, .stop = run.stop.get_token()};
      if (request.output_schema && !model.provider->Capabilities().structured_output) {
        fail(Error(ErrorCode::Unsupported, "Delegate model does not support structured output"));
        co_return;
      }
      for (auto& item : context.Value()) request.items.push_back(std::move(item));
      if (goal_context.Value()) request.items.push_back(std::move(*goal_context.Value()));
      auto prepared = co_await PrepareModelRequest(*this, session, run, request);
      if (run.stop.stop_requested()) co_return;
      if (!prepared.Succeeded()) { fail(prepared.Error()); co_return; }
      if (model.provider->Capabilities().tool_calls) {
        auto builtin = SkillTools(request);
        std::ranges::move(builtin, std::back_inserter(available));
        if (auto delegation = DelegationTool(run)) available.push_back(std::move(*delegation));
        request.tools = available;
      }
      const auto skill_catalog = request.available_skills;
      budget = RunBudget(run);
      if (AtTokenBudget(run, budget) || run.view.usage.model_turns >= budget.model_turns.value_or(32)) {
        fail(Error(ErrorCode::BudgetExceeded, "Run reached its context budget"));
        co_return;
      }
      ++run.view.usage.model_turns;
      run.model_boundary = false;
      ++session.sequence;
      auto committed = co_await WhenSaved(Persist(session));
      if (run.stop.stop_requested()) co_return;
      if (!committed.Succeeded()) { fail(committed.Error()); co_return; }
      auto response = co_await ReadRound(*this, session, run, model, std::move(request), authorized_at);
      if (run.stop.stop_requested()) co_return;
      if (!response.Succeeded()) { fail(response.Error()); co_return; }
      auto round = std::move(response).Value();
      if ((run.delegate || !run.delegations.empty()) && OverTokenBudget(run, RunBudget(run))) {
        fail(Error(ErrorCode::BudgetExceeded, "Run exceeded its token budget"));
        co_return;
      }
      SealMessage(session, run, MessageState::Complete);
      if (!round.text.empty()) run.items.push_back(ModelContent{ModelContent::Role::Assistant, round.text});

      const auto call_prefix = run.items.size();
      if (round.finished->resume && !uses_context) {
        if (round.finished->resume->provider != run.view.model || !round.finished->resume->format_version) {
          fail(Error(ErrorCode::ProtocolError, "Continuation does not match the configured model"));
          co_return;
        }
        run.continuation = ModelContinuation{std::move(*round.finished->resume), call_prefix};
      } else {
        run.continuation.reset();
      }
      if (AtTokenBudget(run, RunBudget(run)) && (!round.calls.empty() || !run.pending_steers.empty())) {
        fail(Error(ErrorCode::BudgetExceeded, "Run reached its token budget"));
        co_return;
      }
      if (round.calls.empty()) {
        if (!run.pending_steers.empty()) {
          run.model_boundary = true;
          auto checkpoint = co_await WhenSaved(Persist(session));
          if (!checkpoint.Succeeded()) { fail(checkpoint.Error()); co_return; }
          continue;
        }
        RunCompletion completion{std::move(round.text), std::move(round.structured), {}};
        if (run.delegate) {
          for (const auto& tool : session.tools) {
            if (tool.run == id) completion.artifacts.insert(completion.artifacts.end(), tool.artifacts.begin(), tool.artifacts.end());
          }
        }
        auto valid = validate_result(completion);
        if (!valid.Succeeded()) { fail(valid.Error()); co_return; }
        Finish(run, RunState::Succeeded, RunEndReason::Completed, {}, std::move(completion));
        co_return;
      }
      if (!SupportsCalls(model.provider->Capabilities(), round.calls.size())) {
        fail(Error(ErrorCode::ProtocolError, "Model exceeded its declared tool capabilities"));
        co_return;
      }
      QueueModelCalls(run, round, available, skill_catalog);
      if (run.continuation) run.continuation->prefix_items = run.items.size();
      run.model_boundary = true;
      auto checkpoint = co_await WhenSaved(Persist(session));
      if (!checkpoint.Succeeded()) { fail(checkpoint.Error()); co_return; }
    }
  } catch (...) {
    ReportException(std::current_exception());
    if (!run.stop.stop_requested()) {
      fail(Error(ErrorCode::InvalidState, "A runtime extension threw an exception"));
    }
  }
}

} // namespace huxerui::agentkit::detail
