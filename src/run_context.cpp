#include "runtime_internal.h"

namespace huxerui::agentkit {

ArtifactChanges RunContext::Artifacts() const { return ArtifactChanges(runtime_, run_); }

namespace {

using detail::Fail;
using detail::RuntimeState;

AgentResult<void> Active(const RuntimeState& state, const RunId& id) {
  if (state.stage != RuntimeState::Stage::Started) return Fail<void>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  const auto found = state.runs.find(id);
  if (found == state.runs.end() || detail::Terminal(found->second.view.state) || found->second.stop.stop_requested()) {
    return Fail<void>(ErrorCode::AlreadyClosed, "Run closed");
  }
  return AgentResult<void>::Success();
}

} // namespace

RunContext::RunContext(std::shared_ptr<RuntimeState> runtime, RunId run)
    : runtime_(std::move(runtime)), run_(std::move(run)) {}

RunId RunContext::Id() const { return run_; }

CommandService RunContext::Commands() { return CommandService(runtime_, run_); }

std::stop_token RunContext::StopToken() const { return runtime_->runs.at(run_).stop.get_token(); }

ModelProviderRef RunContext::Model() const { return runtime_->runs.at(run_).view.model; }

Async<ModelReply> RunContext::CompleteAsync(ModelInput input) {
  return [](RunContext context, ModelInput request) -> Async<ModelReply> {
    auto active = Active(*context.runtime_, context.run_);
    if (!active.Succeeded()) co_return AgentResult<ModelReply>::Failure(active.Error());
    co_return co_await context.runtime_->RunHostModel(context.run_, std::move(request));
  }(*this, std::move(input));
}

Async<Receipt<InputRequested>> RunContext::RequestInputAsync(CommandId command, FormRequest request) {
  return [](RunContext context, CommandId id, FormRequest form) -> Async<Receipt<InputRequested>> {
    auto active = Active(*context.runtime_, context.run_);
    if (!active.Succeeded()) co_return AgentResult<Receipt<InputRequested>>::Failure(active.Error());
    co_return co_await context.runtime_->RequestForm(context.run_, std::move(id), std::move(form));
  }(*this, std::move(command), std::move(request));
}

Async<InteractionOutcome> RunContext::WaitForInputAsync(InteractionId id) {
  return [](RunContext context, InteractionId interaction_id) -> Async<InteractionOutcome> {
    auto& state = *context.runtime_;
    auto active = Active(state, context.run_);
    if (!active.Succeeded()) co_return AgentResult<InteractionOutcome>::Failure(active.Error());
    auto& run = state.runs.at(context.run_);
    auto& session = state.sessions.at(run.view.session);
    auto allowed = co_await state.WithSession(session.summary.id, run.principal, Capability::SubmitInteraction);
    if (!allowed.Succeeded()) co_return AgentResult<InteractionOutcome>::Failure(allowed.Error());
    active = Active(state, context.run_);
    if (!active.Succeeded()) co_return AgentResult<InteractionOutcome>::Failure(active.Error());
    auto form = std::ranges::find(session.interactions, interaction_id, &InteractionSnapshot::id);
    if (form == session.interactions.end() || form->run != context.run_ || !std::holds_alternative<FormRequest>(form->body)) {
      co_return Fail<InteractionOutcome>(ErrorCode::NotFound, "Form is unavailable to this run");
    }
    co_return co_await state.WaitForInteraction(context.run_, std::move(interaction_id));
  }(*this, std::move(id));
}

Async<InteractionOutcome> RunContext::AskAsync(FormRequest form) {
  return [](RunContext context, FormRequest request) -> Async<InteractionOutcome> {
    auto result = co_await context.RequestInputAsync(NewCommandId(), std::move(request));
    if (!result.Succeeded()) co_return AgentResult<InteractionOutcome>::Failure(result.Error());
    co_return co_await context.WaitForInputAsync(std::get<InputRequested>(result.Value().outcome).id);
  }(*this, std::move(form));
}

Async<Receipt<ToolCallAccepted>> RunContext::StartToolAsync(CommandId command, ToolRequest request) {
  return [](RunContext context, CommandId id, ToolRequest value) -> Async<Receipt<ToolCallAccepted>> {
    auto active = Active(*context.runtime_, context.run_);
    if (!active.Succeeded()) co_return AgentResult<Receipt<ToolCallAccepted>>::Failure(active.Error());
    co_return co_await context.runtime_->RequestTool(context.run_, std::move(id), std::move(value));
  }(*this, std::move(command), std::move(request));
}

Async<ToolResult> RunContext::WaitForToolAsync(ToolCallId call) {
  return [](RunContext context, ToolCallId id) -> Async<ToolResult> {
    auto active = Active(*context.runtime_, context.run_);
    if (!active.Succeeded()) co_return AgentResult<ToolResult>::Failure(active.Error());
    auto result = co_await context.runtime_->WaitForHostedTool(context.run_, std::move(id));
    if (!result.Succeeded()) co_return result;
    co_return co_await context.runtime_->ProjectToolResult(context.run_, std::move(result).Value());
  }(*this, std::move(call));
}

Async<ToolResult> RunContext::CallToolAsync(ToolRequest request) {
  return [](RunContext context, ToolRequest value) -> Async<ToolResult> {
    auto call = co_await context.StartToolAsync(NewCommandId(), std::move(value));
    if (!call.Succeeded()) co_return AgentResult<ToolResult>::Failure(call.Error());
    co_return co_await context.WaitForToolAsync(std::get<ToolCallAccepted>(call.Value().outcome).call);
  }(*this, std::move(request));
}

Async<std::vector<RunSteerInput>> RunContext::ReadPendingSteersAsync() {
  return [](RunContext context) -> Async<std::vector<RunSteerInput>> {
    auto& state = *context.runtime_;
    auto active = Active(state, context.run_);
    if (!active.Succeeded()) co_return AgentResult<std::vector<RunSteerInput>>::Failure(active.Error());
    auto& run = state.runs.at(context.run_);
    const auto& session = state.sessions.at(run.view.session);
    std::vector<RunSteerInput> result;
    run.observed_steers.clear();
    for (const auto& [id, input] : run.pending_steers) {
      const auto message = std::ranges::find_if(session.messages, [&](const auto& entry) { return entry.ref.message == id; });
      result.push_back({id, input, message == session.messages.end() ? std::vector<SideChatConclusion>{}
                                                                    : detail::MessageConclusions(*message)});
      run.observed_steers.push_back(id);
    }
    for (auto& input : result) {
      if (input.conclusions.empty()) continue;
      auto actor = state.Request(run.principal, Capability::ReadContext, &session, &run).actor;
      auto allowed = co_await state.CheckConclusions(session.summary.id, std::move(actor), input.conclusions, {});
      if (!allowed.Succeeded()) input.conclusions.clear();
    }
    co_return AgentResult<std::vector<RunSteerInput>>::Success(std::move(result));
  }(*this);
}

Async<void> RunContext::SaveCheckpointAsync(RunCheckpoint checkpoint) {
  return [](RunContext context, RunCheckpoint value) -> Async<void> {
    auto& state = *context.runtime_;
    auto active = Active(state, context.run_);
    if (!active.Succeeded()) co_return active;
    if (value.stage.empty() || !value.schema_version || !detail::ValidUtf8(value.stage)) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Checkpoint requires a valid stage and schema version");
    }
    auto valid = detail::ValidateJson(value.state, JsonSchema{JsonValue{true}});
    if (!valid.Succeeded()) co_return valid;
    auto& run = state.runs.at(context.run_);
    auto& session = state.sessions.at(run.view.session);
    if (value.steer_through) {
      auto last = std::ranges::find(run.observed_steers, *value.steer_through);
      if (last == run.observed_steers.end()) {
        co_return Fail<void>(ErrorCode::Conflict, "Read current steering inputs before applying them");
      }
      auto count = static_cast<std::size_t>(last - run.observed_steers.begin()) + 1;
      if (count > run.pending_steers.size() ||
          !std::equal(run.observed_steers.begin(), last + 1, run.pending_steers.begin(),
              [](const auto& observed, const auto& pending) { return observed == pending.first; })) {
        co_return Fail<void>(ErrorCode::Conflict, "The steering boundary changed");
      }
      state.ApplySteers(session, run, count, true);
      run.observed_steers.clear();
    }
    run.checkpoint = std::move(value);
    ++session.sequence;
    co_return co_await RuntimeState::WhenSaved(state.Persist(session));
  }(*this, std::move(checkpoint));
}

} // namespace huxerui::agentkit
