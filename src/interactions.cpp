#include "runtime_internal.h"

#include <algorithm>

namespace huxerui::agentkit {
namespace detail {
namespace {

AgentError ClosedRun() { return Error(ErrorCode::AlreadyClosed, "Target run has ended"); }

template<class T>
std::optional<AgentResult<Receipt<T>>> Replay(RuntimeState& state, const PrincipalId& user,
                                              const CommandId& command, const CommandInput& key) {
  auto prior = state.Find(user, command, key);
  if (!prior.Succeeded()) return AgentResult<Receipt<T>>::Failure(prior.Error());
  if (prior.Value()) return state.ReceiptFor<T>(user, command);
  return {};
}

} // namespace

void RuntimeState::CloseInteraction(SessionData& session, InteractionSnapshot& interaction,
                                    InteractionState state, std::optional<JsonValue> value) {
  if (interaction.state != InteractionState::Pending) return;
  interaction.state = state;
  ++interaction.version;
  ++session.sequence;
  --session.summary.pending_interactions;
  DecideOperation(session, interaction, state);
  session.replies.insert_or_assign(interaction.id, value);
  if (auto timer = session.interaction_timers.extract(interaction.id); !timer.empty()) timer.mapped().Cancel();
  if (auto waiter = session.waiters.extract(interaction.id); !waiter.empty()) {
    waiter.mapped().Complete(InteractionOutcome{state, std::move(value)});
  }
}

Async<InteractionOutcome> RuntimeState::WaitForInteraction(RunId id, InteractionId interaction_id, bool alongside_approvals) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  auto interaction = std::ranges::find(session.interactions, interaction_id, &InteractionSnapshot::id);
  if (interaction == session.interactions.end() || interaction->run != id) {
    co_return Fail<InteractionOutcome>(ErrorCode::NotFound, "Interaction unavailable");
  }
  if (run.waiting || run.stop.stop_requested() || run.waiting_for_tool_approvals.contains(interaction_id) ||
      (!alongside_approvals && !run.waiting_for_tool_approvals.empty())) {
    co_return Fail<InteractionOutcome>(ErrorCode::InvalidState, "Run cannot register the wait");
  }
  auto matches = [&](const RunWait& wait) {
    const auto* subject = std::get_if<InteractionId>(&wait.subject);
    return subject && *subject == interaction_id;
  };
  bool changed = false;
  if (interaction->state == InteractionState::Pending && interaction->expires_at && *interaction->expires_at <= Now()) {
    CloseInteraction(session, *interaction, InteractionState::Expired);
    changed = true;
  }
  InteractionOutcome outcome;
  if (interaction->state != InteractionState::Pending) {
    outcome = {interaction->state, session.replies.at(interaction_id)};
  } else {
    if (interaction->expires_at) ScheduleInteractionExpiry(id, interaction_id, *interaction->expires_at);
    if (alongside_approvals) run.waiting_for_tool_approvals.insert(interaction_id);
    else run.waiting = interaction_id;
    const auto end_wait = [&] {
      if (alongside_approvals) run.waiting_for_tool_approvals.erase(interaction_id);
      else run.waiting.reset();
    };
    if (std::ranges::none_of(run.view.waits, matches)) {
      run.view.waits.push_back({WaitKind::Input, interaction_id, interaction->expires_at});
    }
    UpdateRunState(session, run, RunState::WaitingForInput);
    ++session.sequence;
    auto published = co_await WhenSaved(Persist(session));
    if (!published.Succeeded()) {
      end_wait();
      co_return AgentResult<InteractionOutcome>::Failure(published.Error());
    }
    outcome = co_await AwaitCallback<InteractionOutcome>(
        [this, id, interaction_id](CallbackCompletion<InteractionOutcome> complete) {
          auto& state = sessions.at(runs.at(id).view.session);
          const auto found = std::ranges::find(state.interactions, interaction_id, &InteractionSnapshot::id);
          if (found->state != InteractionState::Pending) {
            complete.Complete({found->state, state.replies.at(interaction_id)});
          } else {
            state.waiters.emplace(interaction_id, std::move(complete));
          }
          return [weak = weak_from_this(), id, interaction_id] {
            if (auto self = weak.lock()) self->sessions.at(self->runs.at(id).view.session).waiters.erase(interaction_id);
          };
        });
    end_wait();
    changed = true;
  }
  changed |= std::erase_if(run.view.waits, matches) != 0;
  if (!run.stop.stop_requested() && run.waiting_for_tool_approvals.empty()) {
    changed |= run.view.state != RunState::Running;
    UpdateRunState(session, run, RunState::Running);
  }
  std::shared_ptr<SaveTicket> saved;
  if (changed) {
    ++session.sequence;
    saved = Persist(session);
  } else if (session.retention == SessionRetention::Stored && !saves.empty()) {
    saved = saves.back().ticket;
  }
  auto committed = co_await WhenSaved(saved);
  if (!committed.Succeeded()) co_return AgentResult<InteractionOutcome>::Failure(committed.Error());
  co_return AgentResult<InteractionOutcome>::Success(std::move(outcome));
}

Async<InteractionOutcome> RuntimeState::Ask(RunId id, FormRequest request, bool alongside_approvals) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  if (run.stop.stop_requested() || run.waiting || (!alongside_approvals && !run.waiting_for_tool_approvals.empty())) {
    co_return Fail<InteractionOutcome>(ErrorCode::InvalidState, "Run cannot request input");
  }
  if (request.title.empty() || !ValidUtf8(request.title)) {
    co_return Fail<InteractionOutcome>(ErrorCode::InvalidArgument, "A valid form title is required");
  }
  auto expires_at = request.expires_at;
  if (expires_at && *expires_at <= Now()) {
    co_return Fail<InteractionOutcome>(ErrorCode::DeadlineExceeded, "The form deadline has passed");
  }
  const InteractionId interaction_id{MakeId()};
  session.interactions.push_back(InteractionSnapshot{
      .id = interaction_id, .version = 1, .session = session.summary.id, .run = id, .recipient = run.principal,
      .state = InteractionState::Pending, .body = std::move(request), .expires_at = expires_at});
  ++session.summary.pending_interactions;
  co_return co_await WaitForInteraction(id, interaction_id, alongside_approvals);
}

void RuntimeState::ScheduleInteractionExpiry(RunId id, InteractionId interaction, TimePoint deadline) {
  auto& timers = sessions.at(runs.at(id).view.session).interaction_timers;
  if (timers.contains(interaction)) return;
  timers.emplace(interaction, options.tasks.Launch([self = shared_from_this(), id, interaction, deadline] {
    return self->ExpireInteraction(id, interaction, deadline - self->Now());
  }));
}

Async<Receipt<InputRequested>> RuntimeState::RequestForm(RunId id, CommandId command, FormRequest request) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  if (command.value.empty() || request.title.empty() || !ValidUtf8(request.title)) {
    co_return Fail<Receipt<InputRequested>>(ErrorCode::InvalidArgument, "Form requires a command ID and valid title");
  }
  auto active = [&] { return stage == Stage::Started && !Terminal(run.view.state) && !run.stop.stop_requested(); };
  if (!active()) co_return Fail<Receipt<InputRequested>>(ErrorCode::AlreadyClosed, "Run closed");
  auto allowed = co_await Require(Request(run.principal, Capability::SubmitInteraction, &session));
  if (!allowed.Succeeded()) co_return AgentResult<Receipt<InputRequested>>::Failure(allowed.Error());
  if (!active()) co_return Fail<Receipt<InputRequested>>(ErrorCode::AlreadyClosed, "Run closed");
  if (auto existing = run.form_requests.find(command); existing != run.form_requests.end()) {
    const auto& prior = existing->second.request;
    if (prior.title != request.title || prior.schema.document != request.schema.document ||
        prior.presentation != request.presentation || prior.expires_at != request.expires_at) {
      co_return Fail<Receipt<InputRequested>>(ErrorCode::Conflict, "Form command is already bound to another request");
    }
    auto recorded = co_await ReplayReceipt(AgentResult<Receipt<InputRequested>>::Success(
        {existing->second.stamp, existing->second.result}));
    co_return recorded;
  }
  auto expires = request.expires_at;
  if (expires && *expires <= Now()) {
    co_return Fail<Receipt<InputRequested>>(ErrorCode::DeadlineExceeded, "The form deadline has passed");
  }
  auto valid = ValidateJson(request.presentation, JsonSchema{JsonValue{true}});
  if (!valid.Succeeded()) co_return AgentResult<Receipt<InputRequested>>::Failure(valid.Error());
  const InteractionId interaction{MakeId()};
  ++session.sequence;
  InputRequested result{interaction, 1};
  CommitStamp stamp{.command = command, .commit = {MakeId()}, .session = session.summary.id, .session_sequence = session.sequence};
  run.form_requests.emplace(command, FormRecord{request, stamp, result});
  session.interactions.push_back(InteractionSnapshot{
      .id = interaction, .version = 1, .session = session.summary.id, .run = id, .recipient = run.principal,
      .state = InteractionState::Pending, .body = std::move(request), .expires_at = expires});
  run.view.waits.push_back({WaitKind::Input, interaction, expires});
  ++session.summary.pending_interactions;
  auto committed = co_await WhenSaved(Persist(session, false, stamp.commit));
  if (!committed.Succeeded()) co_return AgentResult<Receipt<InputRequested>>::Failure(committed.Error());
  const auto found = std::ranges::find(session.interactions, interaction, &InteractionSnapshot::id);
  if (expires && found->state == InteractionState::Pending) ScheduleInteractionExpiry(id, interaction, *expires);
  co_return AgentResult<Receipt<InputRequested>>::Success({std::move(stamp), std::move(result)});
}

Task<void> RuntimeState::ExpireInteraction(RunId id, InteractionId interaction_id, Duration delay) {
  co_await Delay(std::max(Duration::zero(), delay));
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  session.interaction_timers.erase(interaction_id);
  auto interaction = std::ranges::find(session.interactions, interaction_id, &InteractionSnapshot::id);
  if (interaction == session.interactions.end() || interaction->state != InteractionState::Pending) co_return;
  CloseInteraction(session, *interaction, InteractionState::Expired);
  Persist(session);
}

Async<Receipt<InteractionConsumed>> RuntimeState::Submit(std::shared_ptr<RuntimeState> self, SessionId id,
    PrincipalId user, CommandId command, InteractionReply reply, std::shared_ptr<const RenderedExecution> rendered) {
  auto access = co_await self->WithSession(id, user, rendered ? Capability::ReadSession : Capability::SubmitInteraction);
  if (!access.Succeeded()) co_return AgentResult<Receipt<InteractionConsumed>>::Failure(access.Error());
  const CommandInput key = rendered ? rendered->key : CommandInput{.operation = Operation::Submit, .session = id, .reply = reply};
  const bool approved = rendered && rendered->approved;
  if (!approved) {
    if (auto prior = Replay<InteractionConsumed>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  }
  if (reply.id.value.empty() || !reply.expected_version) {
    co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::InvalidArgument, "Interaction ID and revision are required");
  }
  auto& session = self->sessions.at(id);
  auto interaction = std::ranges::find(session.interactions, reply.id, &InteractionSnapshot::id);
  if (interaction == session.interactions.end() || interaction->recipient != user) {
    co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::NotFound, "Interaction unavailable");
  }
  if (interaction->state != InteractionState::Pending) {
    co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::AlreadyClosed, "Interaction closed");
  }
  if (interaction->expires_at && *interaction->expires_at <= self->Now()) {
    self->CloseInteraction(session, *interaction, InteractionState::Expired);
    self->Persist(session);
    co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::DeadlineExceeded, "Interaction expired");
  }
  if (interaction->version != reply.expected_version) {
    co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::Conflict, "Interaction revision changed");
  }
  auto result = InteractionState::Submitted;
  std::optional<JsonValue> value;
  if (const auto* form = std::get_if<FormRequest>(&interaction->body)) {
    if (const auto* answer = std::get_if<JsonValue>(&reply.response)) {
      auto valid = ValidateJson(*answer, form->schema);
      if (!valid.Succeeded()) co_return AgentResult<Receipt<InteractionConsumed>>::Failure(valid.Error());
      value = *answer;
    } else if (std::holds_alternative<DismissForm>(reply.response)) {
      result = InteractionState::Cancelled;
    } else {
      co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::InvalidArgument, "Submit form data or dismiss the form");
    }
  } else {
    const auto* decision = std::get_if<ApprovalDecision>(&reply.response);
    if (!decision) co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::InvalidArgument, "Submit approve or deny");
    result = *decision == ApprovalDecision::Approve ? InteractionState::Approved : InteractionState::Denied;
  }
  if (rendered) {
    auto permit = Request(user, Capability::SubmitInteraction, &session);
    permit.target.interaction = reply.id;
    permit.target.run = interaction->run;
    auto gate = co_await self->CheckRenderedCommit(*rendered, user, command, std::move(permit));
    if (!gate.Succeeded()) co_return AgentResult<Receipt<InteractionConsumed>>::Failure(gate.Error());
    if (!gate.Value()) co_return co_await self->ReplayReceipt(self->ReceiptFor<InteractionConsumed>(user, command));
    interaction = std::ranges::find(session.interactions, reply.id, &InteractionSnapshot::id);
    if (interaction == session.interactions.end() || interaction->recipient != user ||
        interaction->state != InteractionState::Pending || interaction->version != reply.expected_version) {
      co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::AlreadyClosed, "Bound interaction changed before submission");
    }
    if (interaction->expires_at && *interaction->expires_at <= self->Now()) {
      self->CloseInteraction(session, *interaction, InteractionState::Expired);
      self->Persist(session);
      co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::DeadlineExceeded, "Interaction expired");
    }
  }
  std::optional<CommandId> operation;
  if (result == InteractionState::Approved) {
    if (const auto* binding = std::get_if<ToolApproval>(&interaction->body)) {
      const auto call = std::ranges::find(session.tools, binding->call, &ToolCallSnapshot::id);
      bool stale_sources = false;
      if (interaction->run) {
        const auto& run = self->runs.at(*interaction->run);
        const auto attempt = run.tool_attempts.find(binding->call);
        stale_sources = attempt != run.tool_attempts.end() && self->SourcePlanStale(session, attempt->second.plan);
      }
      const bool stale_tool = call != session.tools.end() && call->tool.catalog && !self->ResolveTool(call->tool);
      if (stale_sources || stale_tool) {
        self->CloseInteraction(session, *interaction, InteractionState::Invalidated);
        auto saved = co_await WhenSaved(self->Persist(session));
        if (!saved.Succeeded()) co_return AgentResult<Receipt<InteractionConsumed>>::Failure(saved.Error());
        co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::Conflict,
            stale_sources ? "A source used by the prepared plan changed" : "Tool revision is unavailable");
      }
    }
    if (session.current_goal) {
      const auto& goal = session.goals.at(*session.current_goal);
      if (goal.view.completion_review && goal.view.completion_review->approval.interaction == reply.id) {
        if (rendered) co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::PermissionDenied,
            "Goal completion must be approved through the goal interaction");
        co_return co_await self->ApproveGoal(id, goal.view.ref.id, user, std::move(command), std::move(reply));
      }
    }
    if (const auto* binding = std::get_if<OperationApproval>(&interaction->body)) operation = binding->pending_command;
  }
  self->CloseInteraction(session, *interaction, result, std::move(value));
  auto receipt = co_await self->FinishOperation(session, user, command, key,
      InteractionConsumed{reply.id, interaction->version}, false, approved);
  if (receipt.Succeeded() && operation) self->ScheduleOperation(user, *operation);
  co_return receipt;
}

Async<Receipt<RunSteered>> RuntimeState::Steer(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
                                              CommandId command, RunId expected, SendInput input,
                                              std::shared_ptr<const RenderedExecution> rendered) {
  auto access = co_await self->WithSession(id, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(access.Error());
  const CommandInput key = rendered ? rendered->key : CommandInput{.operation = Operation::Steer, .session = id, .run = expected, .input = input};
  const bool approved = rendered && rendered->approved;
  if (!approved) {
    if (auto prior = Replay<RunSteered>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  }
  auto valid = self->CheckInput(input, true);
  if (!valid.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(valid.Error());
  auto& session = self->sessions.at(id);
  auto found = self->runs.find(expected);
  if (expected.value.empty()) co_return Fail<Receipt<RunSteered>>(ErrorCode::InvalidArgument, "A target run ID is required");
  if (found == self->runs.end() || found->second.view.session != id) {
    co_return Fail<Receipt<RunSteered>>(ErrorCode::NotFound, "Target run unavailable");
  }
  auto permit = Request(user, Capability::SteerRun, &session);
  permit.target.run = expected;
  if (!rendered) {
    access = co_await self->Require(permit);
    if (!access.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(access.Error());
  }
  valid = co_await self->ValidateUserSources(session, user, input.context, found->second.view.model.id);
  if (!valid.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(valid.Error());
  const auto attachment_epoch = self->access_revision;
  auto attachments = co_await self->PrepareInputAttachments(session, user, input, found->second.view.model);
  if (!attachments.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(attachments.Error());
  auto conclusions = co_await self->ResolveConclusions(id, user, input, found->second.view.model.id);
  if (!conclusions.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(conclusions.Error());
  input.side_chat_conclusions.clear();
  input.recovered_conclusions.clear();
  input.recovered_steer_conclusions.clear();
  if (rendered) {
    auto gate = co_await self->CheckRenderedCommit(*rendered, user, command, std::move(permit));
    if (!gate.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(gate.Error());
    if (!gate.Value()) co_return co_await self->ReplayReceipt(self->ReceiptFor<RunSteered>(user, command));
  }
  if (!approved) {
    if (auto prior = Replay<RunSteered>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  }
  auto& run = found->second;
  if (Terminal(run.view.state)) co_return AgentResult<Receipt<RunSteered>>::Failure(ClosedRun());
  if (session.summary.active_run != expected) co_return Fail<Receipt<RunSteered>>(ErrorCode::Conflict, "Active run changed");
  if (run.view.state == RunState::Cancelling) {
    co_return Fail<Receipt<RunSteered>>(ErrorCode::InvalidState, "Target run is cancelling");
  }
  if (!attachments.Value().empty() && attachment_epoch != self->access_revision)
    co_return Fail<Receipt<RunSteered>>(ErrorCode::Conflict, "Attachment authorization changed");
  valid = self->CommitInputAttachments(session, input, std::move(attachments).Value());
  if (!valid.Succeeded()) co_return AgentResult<Receipt<RunSteered>>::Failure(valid.Error());
  auto prepared_input = rendered ? std::optional{input} : std::nullopt;
  const auto msg = self->RecordSteer(session, run, std::move(input), MessageAuthor::User, std::move(conclusions).Value());
  co_return co_await self->FinishOperation(session, user, command, key, RunSteered{expected, msg}, true, approved,
      nullptr, {}, CommandState::Scheduled, std::move(prepared_input));
}

MessageId RuntimeState::RecordSteer(SessionData& session, RunExecution& run, SendInput input, MessageAuthor author,
                                     std::vector<SideChatConclusion> conclusions) {
  const MessageId msg{MakeId()};
  AppendInput(session, run.view.id, input, msg, author, std::move(conclusions));
  run.view.steering.push_back({msg, SteerState::Pending, {}});
  run.pending_steers.emplace_back(msg, std::move(input));
  for (auto& [id, attempt] : run.tool_attempts) {
    if (attempt.issued) continue;
    attempt.InterruptReview();
    if (attempt.plan.approval) {
      auto approval = std::ranges::find(session.interactions, *attempt.plan.approval, &InteractionSnapshot::id);
      if (approval != session.interactions.end()) CloseInteraction(session, *approval, InteractionState::Invalidated);
    }
  }
  return msg;
}

Async<CommandReceipt> RuntimeState::Cancel(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
                                           CommandId command, RunId target) {
  auto access = co_await self->WithSession(id, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(access.Error());
  CommandInput key{.operation = Operation::Cancel, .session = id, .run = target};
  if (auto prior = Replay<std::monostate>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto& session = self->sessions.at(id);
  auto found = self->runs.find(target);
  if (found == self->runs.end() || found->second.view.session != id) {
    co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Target run unavailable");
  }
  auto permit = Request(user, Capability::CancelRun, &session);
  permit.target.run = target;
  access = co_await self->Require(std::move(permit));
  if (!access.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(access.Error());
  if (auto prior = Replay<std::monostate>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto& run = found->second;
  if (Terminal(run.view.state) || run.view.state == RunState::Cancelling) {
    co_return AgentResult<CommandReceipt>::Failure(ClosedRun());
  }
  self->Stop(run, RunEndReason::UserCancelled);
  self->Accept(user, command, std::move(key), session, std::monostate{});
  co_return co_await self->SaveReceipt<std::monostate>(session, user, command);
}

AgentResult<void> RuntimeState::ReportProgress(const RunId& id, const ToolCallId& call, std::string text) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  const auto attempt = run.tool_attempts.find(call);
  if (attempt == run.tool_attempts.end() || !attempt->second.executing || run.stop.stop_requested()) {
    return Fail<void>(ErrorCode::AlreadyClosed, "Tool closed");
  }
  auto tool = std::ranges::find(session.tools, call, &ToolCallSnapshot::id);
  if (tool == session.tools.end()) return Fail<void>(ErrorCode::NotFound, "Tool unavailable");
  if (!ValidUtf8(text) || text.size() > 4096) return Fail<void>(ErrorCode::InvalidArgument, "Invalid progress text");
  tool->progress = std::move(text);
  ++session.sequence;
  Persist(session);
  return AgentResult<void>::Success();
}

} // namespace detail

using detail::RuntimeState;

Async<Receipt<RunSteered>> Conversation::SteerAsync(CommandId command, RunId expected, SendInput input) {
  return RuntimeState::Steer(state_, id_, principal_, std::move(command), std::move(expected), std::move(input));
}

Async<Receipt<InteractionConsumed>> Conversation::SubmitAsync(CommandId command, InteractionReply reply) {
  return RuntimeState::Submit(state_, id_, principal_, std::move(command), std::move(reply));
}

Async<CommandReceipt> Conversation::CancelAsync(CommandId command, RunId run) {
  return RuntimeState::Cancel(state_, id_, principal_, std::move(command), std::move(run));
}

ToolContext::ToolContext(std::shared_ptr<RuntimeState> state, SessionId session, RunId run, ToolCallId call,
                         std::stop_token stop, std::vector<ResourceAccess> resources)
    : state_(std::move(state)), session_(std::move(session)), run_(std::move(run)), call_(std::move(call)),
      stop_(stop), resources_(std::move(resources)) {}

ToolCallId ToolContext::CallId() const { return call_; }

std::stop_token ToolContext::StopToken() const { return stop_; }

std::vector<ResourceAccess> ToolContext::Resources() const { return resources_; }

AgentResult<void> ToolContext::ReportProgress(std::string text) {
  return state_->ReportProgress(run_, call_, std::move(text));
}

Async<InteractionOutcome> ToolContext::AskAsync(FormRequest request) {
  return [](std::shared_ptr<RuntimeState> state, RunId run, ToolCallId call,
            FormRequest form) -> Async<InteractionOutcome> {
    const auto& execution = state->runs.at(run);
    const auto attempt = execution.tool_attempts.find(call);
    if (attempt == execution.tool_attempts.end() || !attempt->second.executing) {
      co_return detail::Fail<InteractionOutcome>(ErrorCode::AlreadyClosed, "Tool call closed");
    }
    if (attempt->second.parallel) {
      co_return detail::Fail<InteractionOutcome>(ErrorCode::InvalidState, "Parallel tools cannot request input during execution");
    }
    co_return co_await state->Ask(std::move(run), std::move(form), bool(execution.tool_turn));
  }(state_, run_, call_, std::move(request));
}

} // namespace huxerui::agentkit
