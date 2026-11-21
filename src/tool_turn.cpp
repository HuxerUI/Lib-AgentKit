#include "tool_turn.h"

namespace huxerui::agentkit::detail {
namespace {

bool Conflict(const ToolTurnSchedule::Slot& first, const ToolTurnSchedule::Slot& second) {
  if (!first.claims || !second.claims) return true;
  return std::ranges::any_of(*first.claims, [&](const auto& a) {
    return std::ranges::any_of(*second.claims, [&](const auto& b) {
      return a.key == b.key && (a.mode == ResourceAccessMode::Write || b.mode == ResourceAccessMode::Write);
    });
  });
}

} // namespace

auto ToolTurnSchedule::Find(const ToolCallId& call) -> Slot& {
  return *std::ranges::find(slots, call, &Slot::call);
}

void ToolTurnSchedule::AdmitReady() {
  if (failure) return;
  auto active = static_cast<std::size_t>(std::ranges::count(slots, Phase::Admitted, &Slot::phase));
  for (auto& slot : slots) {
    if (slot.phase == Phase::Done || slot.phase == Phase::Admitted) continue;
    if (slot.phase != Phase::Ready || active == limit ||
        std::ranges::any_of(slots, [&](const auto& held) {
          return held.phase == Phase::Admitted && Conflict(slot, held);
        })) break;
    slot.phase = Phase::Admitted;
    ++active;
    auto ready = std::exchange(slot.ready, {});
    ready->Complete(true);
  }
}

Task<bool> ToolTurnSchedule::Acquire(ToolCallId call, std::optional<std::vector<ToolResourceClaim>> claims) {
  if (failure) co_return false;
  auto& slot = Find(call);
  slot.claims = std::move(claims);
  co_return co_await AwaitCallback<bool>([self = shared_from_this(), call](CallbackCompletion<bool> completion) {
    auto& entry = self->Find(call);
    if (self->failure) completion.Complete(false);
    else {
      entry.phase = Phase::Ready;
      entry.ready = std::move(completion);
      self->AdmitReady();
    }
    return [weak = self->weak_from_this(), call] {
      if (auto schedule = weak.lock()) schedule->Find(call).ready.reset();
    };
  });
}

void ToolTurnSchedule::Release(const ToolCallId& call) {
  Find(call).phase = Phase::Preparing;
}

bool ToolTurnSchedule::Done() const {
  return std::ranges::all_of(slots, [](const auto& slot) { return slot.phase == Phase::Done; });
}

void ToolTurnSchedule::Finish(const ToolCallId& call) {
  Find(call).phase = Phase::Done;
  AdmitReady();
  if (drained && Done()) {
    auto completion = std::exchange(drained, {});
    completion->Complete(true);
  }
}

void ToolTurnSchedule::Abort(const AgentError& reason) {
  if (failure) {
    if (failure->code != ErrorCode::ResultUnknown && reason.code == ErrorCode::ResultUnknown) failure = reason;
    return;
  }
  failure = reason;
  for (auto& slot : slots) {
    if (auto pending = std::exchange(slot.ready, {})) pending->Complete(false);
  }
}

Task<void> ToolTurnSchedule::Drain() {
  if (Done()) co_return;
  co_await AwaitCallback<bool>([self = shared_from_this()](CallbackCompletion<bool> completion) {
    if (self->Done()) completion.Complete(true);
    else self->drained = std::move(completion);
    return [weak = self->weak_from_this()] {
      if (auto schedule = weak.lock()) schedule->drained.reset();
    };
  });
}

void RuntimeState::AbortToolTurn(RunExecution& run, const AgentError& error) {
  if (!run.tool_turn) return;
  const bool first = !run.tool_turn->failure;
  run.tool_turn->Abort(error);
  if (!first) return;
  auto& session = sessions.at(run.view.session);
  for (auto& [call, attempt] : run.tool_attempts) {
    if (attempt.issued) continue;
    attempt.InterruptReview();
    if (!attempt.plan.approval) continue;
    auto approval = std::ranges::find(session.interactions, *attempt.plan.approval, &InteractionSnapshot::id);
    if (approval != session.interactions.end()) CloseInteraction(session, *approval, InteractionState::Invalidated);
  }
}

void RuntimeState::CancelToolTurnTasks(RunExecution& run) {
  if (auto schedule = std::exchange(run.tool_turn, {})) {
    for (auto& slot : schedule->slots) slot.task.Cancel();
  }
}

void RuntimeState::RecordModelTool(SessionData& session, const RunId& run, PendingModelCall& pending) {
  if (pending.local_call) return;
  pending.local_call = ToolCallId{MakeId()};
  session.tools.push_back({.id = *pending.local_call, .run = run, .tool = *pending.tool, .state = ToolCallState::Preparing,
      .registered_sequence = ++session.sequence});
}

Task<void> RuntimeState::DriveModelTool(RunId id, PendingModelCall& pending) {
  auto& run = runs.at(id);
  const auto call = *pending.local_call;
  try {
    auto result = co_await ExecuteTool(id, *pending.tool, ToolRequest{pending.tool->id, pending.call.arguments}, call);
    if (Terminal(run.view.state) || run.suspended) co_return;
    if (!result.Succeeded()) {
      pending.terminal_error = result.Error();
    } else {
      auto output = std::move(result).Value();
      if (output.outcome == ToolOutcome::ResultUnknown || output.outcome == ToolOutcome::PartiallyApplied) {
        pending.terminal_error = output.reason.value_or(Error(ErrorCode::ResultUnknown, "Resolve tool effects before continuing"));
        pending.terminal_error->code = ErrorCode::ResultUnknown;
      } else {
        if (run.host_model_step && !output.published_artifacts.empty())
          run.host_model_step->published.emplace(call, std::move(output.published_artifacts));
        pending.result = ModelToolResult{pending.call.provider_call_id, output.call,
                                        std::move(output.model_value), std::move(output.reason)};
      }
    }
  } catch (...) {
    if (Terminal(run.view.state) || run.suspended) co_return;
    ReportException(std::current_exception());
    SettleToolException(run, call);
    const auto& tools = sessions.at(run.view.session).tools;
    const auto snapshot = std::ranges::find(tools, call, &ToolCallSnapshot::id);
    pending.terminal_error = snapshot->reason.value_or(Error(ErrorCode::InvalidState, "Tool handler failed"));
  }
  run.tool_attempts.erase(call);
}

Async<void> RuntimeState::DriveParallelTools(RunId id) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  auto schedule = std::make_shared<ToolTurnSchedule>(options.max_parallel_tools_per_turn);
  const auto budget = RunBudget(run).tool_calls.value_or(128);
  for (auto& pending : run.pending_calls) {
    if (pending.error || !pending.tool || pending.terminal_error) break;
    const auto definition = ResolveTool(*pending.tool);
    if (!definition || definition->concurrency == ToolConcurrency::Serial) break;
    if (!pending.budget_reserved) {
      if (run.view.usage.tool_calls >= budget) break;
      ++run.view.usage.tool_calls;
      pending.budget_reserved = true;
    }
    RecordModelTool(session, id, pending);
    schedule->slots.push_back({.call = *pending.local_call,
        .phase = pending.result ? ToolTurnSchedule::Phase::Done : ToolTurnSchedule::Phase::Preparing});
  }
  if (schedule->slots.empty()) co_return Fail<void>(ErrorCode::BudgetExceeded, "Run reached its tool budget");

  run.tool_turn = schedule;
  run.model_boundary = false;
  ++session.sequence;
  auto saved = co_await WhenSaved(Persist(session));
  if (!saved.Succeeded() || run.stop.stop_requested()) {
    run.tool_turn.reset();
    co_return saved;
  }
  for (std::size_t index = 0; index < schedule->slots.size(); ++index) {
    auto& slot = schedule->slots[index];
    if (slot.phase == ToolTurnSchedule::Phase::Done) continue;
    slot.task = options.tasks.Launch(
        [self = shared_from_this(), schedule, id, index, call = slot.call]() -> Task<void> {
          auto& run = self->runs.at(id);
          if (Terminal(run.view.state) || run.suspended) co_return;
          auto& pending = run.pending_calls[index];
          co_await self->DriveModelTool(id, pending);
          if (Terminal(run.view.state) || run.suspended) co_return;
          if (pending.terminal_error) self->AbortToolTurn(run, *pending.terminal_error);
          auto& session = self->sessions.at(run.view.session);
          ++session.sequence;
          auto committed = co_await WhenSaved(self->Persist(session));
          if (!committed.Succeeded()) self->AbortToolTurn(run, committed.Error());
          schedule->Finish(call);
        });
  }
  co_await schedule->Drain();
  run.tool_turn.reset();
  if (schedule->failure && !run.stop.stop_requested()) co_return AgentResult<void>::Failure(*schedule->failure);
  co_return AgentResult<void>::Success();
}

} // namespace huxerui::agentkit::detail
