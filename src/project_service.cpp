#include "runtime_internal.h"

namespace huxerui::agentkit {
namespace detail {

std::optional<RunId> RuntimeState::DependencyRun(const RunWait& wait) const {
  if (const auto* message = std::get_if<CollaborationMessageId>(&wait.subject)) {
    auto record = FindMessage(*message);
    if (!record || record->second.outcome) return std::nullopt;
    const auto& item = record->second.summary;
    if (item.target_run) return item.target_run;
    if (item.delivery == DeliveryState::Queued) return sessions.at(item.to).summary.active_run;
  } else if (const auto* execution = std::get_if<TaskExecutionRef>(&wait.subject)) {
    const auto* project = PublishedProject(execution->project);
    if (!project) return std::nullopt;
    auto found = project->tasks.find(execution->task);
    if (found == project->tasks.end() || found->second.attempts.contains(execution->attempt)) return std::nullopt;
    const auto& task = found->second.snapshot;
    if (task.attempt != execution->attempt) return std::nullopt;
    if (task.run) return task.run;
    if (task.claimant && sessions.contains(*task.claimant)) return sessions.at(*task.claimant).summary.active_run;
  }
  return std::nullopt;
}

bool RuntimeState::WaitCycle(RunId waiter, const RunWait& wait) const {
  std::vector<RunId> pending;
  if (auto run = DependencyRun(wait)) pending.push_back(*run);
  std::set<RunId> visited;
  while (!pending.empty()) {
    auto id = std::move(pending.back());
    pending.pop_back();
    if (id == waiter) return true;
    if (!visited.insert(id).second) continue;
    auto run = runs.find(id);
    if (run == runs.end() || Terminal(run->second.view.state)) continue;
    for (const auto& edge : run->second.view.waits) {
      if (auto target = DependencyRun(edge)) pending.push_back(*target);
      if (const auto* command = std::get_if<CommandId>(&edge.subject)) {
        const auto found = run->second.delegations.find(*command);
        if (found != run->second.delegations.end()) pending.insert(pending.end(), found->second.begin(), found->second.end());
      }
    }
  }
  return false;
}

Async<TaskOutcome> RuntimeState::WaitProjectTask(TaskExecutionRef execution, WaitOptions options, std::optional<RunId> bound) {
  using Event = AgentResult<std::optional<TaskOutcome>>;
  using Result = AgentResult<TaskOutcome>;
  auto read = [execution, bound](RuntimeState& runtime) -> Async<std::optional<TaskOutcome>> {
    auto actor = co_await runtime.ProjectAccess(execution.project, bound, Capability::WaitForResult);
    if (!actor.Succeeded()) co_return Event::Failure(actor.Error());
    auto inspect = runtime.ProjectPolicy(actor.Value(), execution.project, Capability::WaitForResult);
    inspect.target.task = execution.task;
    auto permission = co_await runtime.Require(std::move(inspect));
    if (!permission.Succeeded()) co_return Event::Failure(permission.Error());
    auto* project = runtime.PublishedProject(execution.project);
    if (!project) co_return Fail<std::optional<TaskOutcome>>(ErrorCode::NotFound, "Project unavailable");
    auto task = project->tasks.find(execution.task);
    if (task == project->tasks.end()) co_return Fail<std::optional<TaskOutcome>>(ErrorCode::NotFound, "Task unavailable");
    auto result = task->second.attempts.find(execution.attempt);
    if (result != task->second.attempts.end()) {
      auto projected = result->second;
      std::vector<SourceSlice> sources;
      for (const auto& artifact : projected.artifacts) sources.push_back({artifact});
      if (projected.evidence) sources.insert(sources.end(), projected.evidence->sources.begin(), projected.evidence->sources.end());
      auto accessible = co_await runtime.ValidateProjectSources(execution.project, actor.Value(), sources);
      if (!accessible.Succeeded()) {
        projected.artifacts.clear();
        projected.evidence.reset();
      }
      co_return Event::Success(std::move(projected));
    }
    if (task->second.snapshot.attempt != execution.attempt) {
      co_return Fail<std::optional<TaskOutcome>>(ErrorCode::Conflict, "Task execution attempt is unavailable");
    }
    co_return Event::Success(std::nullopt);
  };
  auto result = co_await read(*this);
  if (!result.Succeeded()) co_return Result::Failure(result.Error());
  if (result.Value()) co_return Result::Success(*result.Value());

  RunWait wait{WaitKind::Dependency, execution, options.deadline};
  RunExecution* run = bound ? &runs.at(*bound) : nullptr;
  auto matches = [&](const RunWait& other) {
    const auto* task = std::get_if<TaskExecutionRef>(&other.subject);
    return other.kind == WaitKind::Dependency && task && *task == execution;
  };
  if (run) {
    auto earlier = std::ranges::find(run->view.waits, WaitKind::Dependency, &RunWait::kind);
    if (earlier != run->view.waits.end()) {
      if (!run->restoring || !matches(*earlier)) co_return Fail<TaskOutcome>(ErrorCode::Busy, "Run already awaits a dependency");
      wait.deadline = earlier->deadline;
    } else {
      if (WaitCycle(*bound, wait)) co_return Fail<TaskOutcome>(ErrorCode::WaitCycle, "Task wait would create a cycle");
      run->view.waits.push_back(wait);
      auto& session = sessions.at(run->view.session);
      UpdateRunState(session, *run, RunState::WaitingForDependency);
      ++session.sequence;
      auto saved = co_await WhenSaved(Persist(session));
      if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
    }
  }

  auto self = shared_from_this();
  TaskHandle observer;
  TaskHandle timer;
  auto subscription = std::make_shared<std::optional<Subscription>>();
  auto attach = [self, read, wait, bound, subscription, &observer, &timer](CallbackCompletion<Result> reply) {
    observer = self->options.tasks.Launch([self, read, wait, bound, reply, subscription]() -> Task<void> {
      auto installed = co_await Observe<std::optional<TaskOutcome>>(self,
          [weak = self->weak_from_this(), wait, bound, reply](auto status) {
            if (!status.Succeeded()) reply.Complete(Result::Failure(status.Error()));
            else if (*status.Value()) reply.Complete(Result::Success(**status.Value()));
            else if (auto runtime = weak.lock(); runtime && bound && runtime->WaitCycle(*bound, wait)) {
              reply.Complete(Fail<TaskOutcome>(ErrorCode::WaitCycle, "Task wait formed a cycle"));
            }
          }, read);
      if (installed.Succeeded()) *subscription = std::move(installed).Value();
      else reply.Complete(Result::Failure(installed.Error()));
    });
    if (wait.deadline) {
      const auto duration = std::max(Duration::zero(), *wait.deadline - self->Now());
      timer = self->options.tasks.Launch([self, read, reply, duration]() -> Task<void> {
        co_await Delay(duration);
        auto latest = co_await read(*self);
        if (!latest.Succeeded()) reply.Complete(Result::Failure(latest.Error()));
        else if (latest.Value()) reply.Complete(Result::Success(*latest.Value()));
        else reply.Complete(Fail<TaskOutcome>(ErrorCode::DeadlineExceeded, "Task wait expired"));
      });
    }
    return [subscription, observer, timer] { subscription->reset(); observer.Cancel(); timer.Cancel(); };
  };
  auto answer = co_await AwaitCallback<Result>(std::move(attach));
  subscription->reset();
  observer.Cancel();
  timer.Cancel();
  if (run && std::erase_if(run->view.waits, matches) && !run->stop.stop_requested()) {
    auto& session = sessions.at(run->view.session);
    UpdateRunState(session, *run, RunState::Running);
    ++session.sequence;
    auto saved = co_await WhenSaved(Persist(session));
    if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
  }
  co_return answer;
}

} // namespace detail

ProjectService::ProjectService(std::shared_ptr<detail::RuntimeState> runtime, std::optional<RunId> actor)
    : runtime_(std::move(runtime)), actor_(std::move(actor)) {}

Async<ProjectSnapshot> ProjectService::ReadAsync(ProjectId project) {
  return [](ProjectService service, ProjectId id) -> Async<ProjectSnapshot> {
    co_return co_await service.runtime_->ReadProject(std::move(id), service.actor_);
  }(*this, std::move(project));
}

Async<Subscription> ProjectService::ObserveAsync(ProjectId project, Observer<ProjectSnapshot> observer) {
  return detail::RuntimeState::Observe(runtime_, std::move(observer),
      [id = std::move(project), actor = actor_](detail::RuntimeState& runtime) { return runtime.ReadProject(id, actor); });
}

Async<Receipt<TaskUpdated>> ProjectService::UpdateTaskAsync(ProjectId project, CommandId command, TaskCommand request) {
  return [](ProjectService service, ProjectId id, CommandId command, TaskCommand request) -> Async<Receipt<TaskUpdated>> {
    co_return co_await service.runtime_->UpdateProjectTask(std::move(id), std::move(command), std::move(request), service.actor_);
  }(*this, std::move(project), std::move(command), std::move(request));
}

Async<TaskOutcome> ProjectService::WaitForTaskAsync(TaskExecutionRef execution, WaitOptions options) {
  return [](ProjectService service, TaskExecutionRef execution, WaitOptions options) -> Async<TaskOutcome> {
    co_return co_await service.runtime_->WaitProjectTask(std::move(execution), options, service.actor_);
  }(*this, std::move(execution), options);
}

Async<Receipt<SharedContextRef>> ProjectService::UpdateContextAsync(ProjectId project, CommandId command, SharedContextCommand request) {
  return [](ProjectService service, ProjectId id, CommandId command, SharedContextCommand request) -> Async<Receipt<SharedContextRef>> {
    co_return co_await service.runtime_->UpdateSharedContext(std::move(id), std::move(command), std::move(request), service.actor_);
  }(*this, std::move(project), std::move(command), std::move(request));
}

Async<SharedContextItem> ProjectService::ReadContextAsync(SharedContextRef ref) {
  return [](ProjectService service, SharedContextRef version) -> Async<SharedContextItem> {
    co_return co_await service.runtime_->ReadSharedContext(std::move(version), service.actor_);
  }(*this, std::move(ref));
}

ProjectService AgentRuntime::Projects() { return ProjectService(state_); }

ProjectService RunContext::Projects() { return ProjectService(runtime_, run_); }

} // namespace huxerui::agentkit
