#pragma once

#include <huxerui/agentkit/runtime.h>
#include <algorithm>

namespace agentkit_preview {

inline huxerui::agentkit::Async<huxerui::agentkit::SessionSummary> FindRunSession(
    huxerui::agentkit::RunContext run, huxerui::agentkit::AgentId agent) {
  namespace kit = huxerui::agentkit;
  using Result = kit::AgentResult<kit::SessionSummary>;
  std::optional<kit::PageCursor> cursor;
  do {
    auto next = co_await run.Sessions().ListAsync({.agent = agent}, cursor);
    if (!next.Succeeded()) co_return Result::Failure(next.Error());
    auto own = std::ranges::find_if(next.Value().sessions, [&](const auto& item) { return item.active_run == run.Id(); });
    if (own != next.Value().sessions.end()) co_return Result::Success(*own);
    cursor = next.Value().next;
  } while (cursor);
  co_return Result::Failure({.code = kit::ErrorCode::NotFound, .message = "The running conversation is unavailable"});
}

inline huxerui::agentkit::Async<huxerui::agentkit::ProjectTaskSnapshot> FindProjectTask(
    huxerui::agentkit::RunContext run, huxerui::agentkit::ProjectId project, huxerui::agentkit::ProjectTaskId id) {
  namespace kit = huxerui::agentkit;
  using Result = kit::AgentResult<kit::ProjectTaskSnapshot>;
  auto snapshot = co_await run.Projects().ReadAsync(std::move(project));
  if (!snapshot.Succeeded()) co_return Result::Failure(snapshot.Error());
  auto found = std::ranges::find(snapshot.Value().tasks, id, &kit::ProjectTaskSnapshot::id);
  if (found == snapshot.Value().tasks.end()) co_return Result::Failure({
      .code = kit::ErrorCode::NotFound, .message = "Project task is unavailable"});
  co_return Result::Success(*found);
}

inline huxerui::agentkit::Async<void> FinishProjectTask(
    huxerui::agentkit::RunContext run, huxerui::agentkit::TaskExecutionRef attempt,
    std::vector<huxerui::agentkit::ArtifactRef> artifacts,
    std::vector<huxerui::agentkit::SourceSlice> evidence = {}, std::optional<huxerui::agentkit::AgentError> failure = {}) {
  namespace kit = huxerui::agentkit;
  auto task = co_await FindProjectTask(run, attempt.project, attempt.task);
  if (!task.Succeeded()) co_return kit::AgentResult<void>::Failure(task.Error());
  if (task.Value().attempt == attempt.attempt && task.Value().outcome &&
      task.Value().outcome->state == (failure ? kit::ProjectTaskState::Failed : kit::ProjectTaskState::Succeeded)) {
    co_return kit::AgentResult<void>::Success();
  }
  auto change = failure ? kit::TaskChange{kit::FailTask{attempt.attempt, *failure, std::move(artifacts)}} :
      kit::TaskChange{kit::CompleteTask{attempt.attempt, {kit::JsonValue{true}, std::move(evidence)}, std::move(artifacts)}};
  auto result = co_await run.Projects().UpdateTaskAsync(attempt.project, {"finish-task:" + run.Id().value},
      kit::ChangeTask{attempt.task, task.Value().revision, std::move(change)});
  if (!result.Succeeded()) co_return kit::AgentResult<void>::Failure(result.Error());
  co_return kit::AgentResult<void>::Success();
}

} // namespace agentkit_preview
