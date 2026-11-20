#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

auto HostToolFor(RunExecution& run, const ToolCallId& call) {
  return std::ranges::find_if(run.tool_requests, [&](const auto& entry) { return entry.second.call == call; });
}

bool CanSchedule(const RuntimeState& runtime, const RunExecution& run) {
  return runtime.stage == RuntimeState::Stage::Started && !Terminal(run.view.state) && !run.stop.stop_requested() &&
         (!run.deadline || runtime.Now() < *run.deadline);
}

AgentResult<ToolResult> ToolReceipt(const HostToolRecord& entry) {
  if (entry.result) return AgentResult<ToolResult>::Success(*entry.result);
  return AgentResult<ToolResult>::Failure(*entry.failure);
}

} // namespace

Async<Receipt<ToolCallAccepted>> RuntimeState::RequestTool(RunId id, CommandId command, ToolRequest request) {
  using Registration = AgentResult<Receipt<ToolCallAccepted>>;
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  if (command.value.empty() || request.tool.value.empty()) {
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::InvalidArgument, "Tool and command IDs are required");
  }
  if (!CanSchedule(*this, run)) co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::AlreadyClosed, "Run closed");
  auto duplicate = [&]() -> std::optional<Registration> {
    auto existing = run.tool_requests.find(command);
    if (existing == run.tool_requests.end()) return {};
    const auto& record = existing->second;
    if (record.request.tool != request.tool || record.request.arguments != request.arguments) {
      return Fail<Receipt<ToolCallAccepted>>(ErrorCode::Conflict, "Tool command already has different arguments");
    }
    return Registration::Success({record.stamp, ToolCallAccepted{record.call}});
  };
  if (auto existing = duplicate()) {
    auto receipt = co_await ReplayReceipt(std::move(*existing));
    if (receipt.Succeeded()) ScheduleHostedTool(run, std::get<ToolCallAccepted>(receipt.Value().outcome).call);
    co_return receipt;
  }
  if (run.UsingHostedModelTools())
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::Busy, "A hosted model tool loop is active");
  const auto& agent = agents.at(run.view.agent.id);
  const auto definition = FindAgentTool(agent, request.tool);
  if (!definition) {
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::PermissionDenied, "Tool is outside the agent declaration");
  }
  if (!AllowsRunTool(run, definition->ref)) {
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::PermissionDenied, "Tool is outside the work request");
  }
  auto valid = ValidateJson(request.arguments, definition->definition->input_schema);
  if (!valid.Succeeded()) co_return Registration::Failure(valid.Error());
  auto ref = definition->ref;
  auto discover = Request(run.principal, Capability::DiscoverTool, &session, &run);
  discover.tool = ref;
  auto access = co_await Require(std::move(discover));
  if (!access.Succeeded()) co_return Registration::Failure(access.Error());
  if (!CanSchedule(*this, run)) co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::AlreadyClosed, "Run closed");
  if (auto existing = duplicate()) {
    auto receipt = co_await ReplayReceipt(std::move(*existing));
    if (receipt.Succeeded()) ScheduleHostedTool(run, std::get<ToolCallAccepted>(receipt.Value().outcome).call);
    co_return receipt;
  }
  if (run.UsingHostedModelTools())
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::Busy, "A hosted model tool loop is active");
  if (std::ranges::any_of(run.tool_requests, [](const auto& entry) {
        return !entry.second.result && !entry.second.failure;
      })) {
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::Busy, "A tool is already pending in this workflow");
  }
  if (!ResolveTool(ref)) co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::Conflict, "Tool revision is unavailable");
  if (run.view.usage.tool_calls >= RunBudget(run).tool_calls.value_or(128)) {
    co_return Fail<Receipt<ToolCallAccepted>>(ErrorCode::BudgetExceeded, "Run reached its tool budget");
  }

  const ToolCallId call{MakeId()};
  ++run.view.usage.tool_calls;
  ++session.sequence;
  CommitStamp stamp{.command = command, .commit = {MakeId()}, .session = session.summary.id, .session_sequence = session.sequence};
  run.tool_requests.emplace(command, HostToolRecord{std::move(request), ref, stamp, call});
  session.tools.push_back({.id = call, .run = id, .tool = std::move(ref), .state = ToolCallState::Preparing});
  auto saved = co_await WhenSaved(Persist(session, false, stamp.commit));
  if (!saved.Succeeded()) co_return Registration::Failure(saved.Error());
  ScheduleHostedTool(run, call);
  co_return Registration::Success({std::move(stamp), ToolCallAccepted{call}});
}

void RuntimeState::ScheduleHostedTool(RunExecution& run, ToolCallId call) {
  const auto entry = HostToolFor(run, call);
  if (!CanSchedule(*this, run) || entry == run.tool_requests.end() || entry->second.result || entry->second.failure ||
      run.dispatched_host_tool) return;
  run.dispatched_host_tool = call;
  run.host_tool_task = options.tasks.Launch([self = shared_from_this(), id = run.view.id, call = std::move(call)] {
    return self->ExecuteHostedTool(id, call);
  });
}

Task<void> RuntimeState::ExecuteHostedTool(RunId id, ToolCallId call) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  auto& entry = HostToolFor(run, call)->second;
  auto outcome = Fail<ToolResult>(ErrorCode::AlreadyClosed, "Run closed");
  try {
    if (CanSchedule(*this, run)) outcome = co_await ExecuteTool(id, entry.ref, entry.request, call);
  } catch (...) {
    ReportException(std::current_exception());
    SettleToolException(run, call);
    const auto tool = std::ranges::find(session.tools, call, &ToolCallSnapshot::id);
    outcome = AgentResult<ToolResult>::Failure(
        tool->reason.value_or(Error(ErrorCode::InvalidState, "Tool handler failed")));
  }
  run.tool_attempts.erase(call);
  if (outcome.Succeeded()) entry.result = std::move(outcome).Value();
  else entry.failure = outcome.Error();
  if (ArtifactChangeTool(entry.ref.id)) SettleArtifactChange(commands.at({run.principal, HostToolFor(run, call)->first}), session, entry);
  ++session.sequence;
  auto saved = co_await WhenSaved(Persist(session));
  run.dispatched_host_tool.reset();
  if (run.host_tool_waiter) {
    auto reply = *std::exchange(run.host_tool_waiter, std::nullopt);
    reply.Complete(saved.Succeeded() ? ToolReceipt(entry) : AgentResult<ToolResult>::Failure(saved.Error()));
  }
}

Async<ToolResult> RuntimeState::WaitForHostedTool(RunId id, ToolCallId call) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  const auto entry = HostToolFor(run, call);
  if (entry == run.tool_requests.end()) co_return Fail<ToolResult>(ErrorCode::NotFound, "Tool call is unavailable to this workflow");
  if (run.host_tool_waiter) co_return Fail<ToolResult>(ErrorCode::Busy, "Workflow is already waiting for a tool");
  if (entry->second.result || entry->second.failure) {
    if (session.retention == SessionRetention::Stored && !saves.empty()) {
      auto saved = co_await WhenSaved(saves.back().ticket);
      if (!saved.Succeeded()) co_return AgentResult<ToolResult>::Failure(saved.Error());
    }
    if (session.retention == SessionRetention::Stored && storage_error) {
      co_return AgentResult<ToolResult>::Failure(*storage_error);
    }
    co_return ToolReceipt(entry->second);
  }
  ScheduleHostedTool(run, call);
  if (run.dispatched_host_tool != call) co_return Fail<ToolResult>(ErrorCode::AlreadyClosed, "Tool cannot be started");
  auto wait = [this, id, call](CallbackCompletion<AgentResult<ToolResult>> completion) {
    auto& run = runs.at(id);
    if (run.dispatched_host_tool != call) {
      const auto entry = HostToolFor(run, call);
      completion.Complete(ToolReceipt(entry->second));
      return std::function<void()>{};
    }
    run.host_tool_waiter = std::move(completion);
    return std::function<void()>{[weak = weak_from_this(), id] {
      if (auto self = weak.lock()) self->runs.at(id).host_tool_waiter.reset();
    }};
  };
  co_return co_await AwaitCallback<AgentResult<ToolResult>>(std::move(wait));
}

Async<ToolResult> RuntimeState::ProjectToolResult(RunId id, ToolResult result) {
  const auto& run = runs.at(id);
  const auto& session = sessions.at(run.view.session);
  auto actor = Request(run.principal, Capability::ReadArtifact, &session, &run).actor;
  actor.call = result.call;

  ArtifactVersions refs;
  for (const auto& ref : result.published_artifacts) refs.emplace(ref.id, ref.version);
  for (const auto& ref : result.existing_artifacts) refs.emplace(ref.id, ref.version);
  for (const auto& block : result.blocks) CollectArtifactRefs(refs, block);

  ArtifactVersions visible;
  const auto epoch = access_revision;
  for (const auto& [id, version] : refs) {
    auto artifact = co_await ReadArtifact({id, version}, actor);
    if (epoch != access_revision)
      co_return Fail<ToolResult>(ErrorCode::Conflict, "Tool result access changed");
    if (artifact.Succeeded()) visible.emplace(id, version);
  }

  if (run.stop.stop_requested()) co_return Fail<ToolResult>(ErrorCode::AlreadyClosed, "Run closed");
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<ToolResult>::Failure(user.Error());
  if (user.Value() != run.principal)
    co_return Fail<ToolResult>(ErrorCode::PermissionDenied, "The acting principal changed");

  FilterArtifactRefs(result.published_artifacts, visible);
  FilterArtifactRefs(result.existing_artifacts, visible);
  std::erase_if(result.blocks, [&](const auto& block) { return !VisibleArtifactBlock(block, visible); });
  // A host payload can repeat the artifact references carried by the typed fields.
  if (visible.size() != refs.size()) result.model_value.reset();
  co_return AgentResult<ToolResult>::Success(std::move(result));
}

} // namespace huxerui::agentkit::detail
