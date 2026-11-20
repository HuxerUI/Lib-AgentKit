#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

using TaskResult = AgentResult<Receipt<TaskUpdated>>;

bool TerminalTask(ProjectTaskState state) {
  return state == ProjectTaskState::Succeeded || state == ProjectTaskState::Failed || state == ProjectTaskState::Cancelled;
}

const ArtifactRecord* ProjectArtifact(const RuntimeState& runtime, ProjectId id, const ArtifactRef& ref) {
  for (const auto& [sid, working] : runtime.sessions) {
    const auto* session = runtime.Published(working);
    if (!session || session->summary.project != id || session->summary.lifecycle == SessionLifecycle::Deleting) continue;
    auto found = std::ranges::find_if(session->artifacts, [&](const auto& record) {
      return record.record.metadata.ref == ref && record.record.availability == ArtifactAvailability::Available;
    });
    if (found != session->artifacts.end()) return &found->record;
  }
  return nullptr;
}

AgentResult<std::vector<SourceSlice>> TaskInputs(const RuntimeState& runtime, const ProjectData& project,
                                                  const ProjectTaskSnapshot& task) {
  auto inputs = task.spec.input.context;
  auto add = [&](SourceSlice slice) {
    if (std::ranges::find(inputs, slice) == inputs.end()) inputs.push_back(std::move(slice));
  };
  for (const auto& slice : inputs) {
    const auto* ref = std::get_if<SharedContextRef>(&slice.source);
    if (!ref) continue;
    auto item = project.context.find(ref->item);
    if (ref->project != project.id || item == project.context.end() || !ref->version || ref->version > item->second.size() ||
        item->second[ref->version - 1].state != SharedContextState::Published) {
      return Fail<std::vector<SourceSlice>>(ErrorCode::NotFound, "A required shared context version is unavailable");
    }
  }
  for (const auto& edge : task.spec.dependencies) {
    const auto& upstream = project.tasks.at(edge.upstream).snapshot;
    const bool partial = upstream.state == ProjectTaskState::Failed && edge.accept_partial &&
        runtime.options.verify && !edge.required_artifact_types.empty();
    if (!upstream.outcome || (upstream.state != ProjectTaskState::Succeeded && !partial)) {
      return Fail<std::vector<SourceSlice>>(ErrorCode::InvalidState, "Waiting for an upstream task");
    }
    for (const auto& type : edge.required_artifact_types) {
      auto chosen = std::ranges::find_if(upstream.outcome->artifacts, [&](const auto& ref) {
        const auto* artifact = ProjectArtifact(runtime, project.id, ref);
        return artifact && artifact->metadata.type == type;
      });
      if (chosen == upstream.outcome->artifacts.end()) {
        return Fail<std::vector<SourceSlice>>(ErrorCode::NotFound, "A required upstream artifact is unavailable");
      }
      add({*chosen});
    }
  }
  return AgentResult<std::vector<SourceSlice>>::Success(std::move(inputs));
}

AgentResult<void> CheckSpec(const RuntimeState& runtime, const ProjectData& project, const ProjectTaskId& id,
                            ProjectTaskSpec& spec) {
  if (spec.title.empty() || spec.title.size() > 512 || !ValidUtf8(spec.title)) {
    return Fail<void>(ErrorCode::InvalidArgument, "Project task title is required");
  }
  auto valid = runtime.CheckInput(spec.input);
  if (!valid.Succeeded()) return valid;
  valid = ValidateJson(spec.completion_conditions, JsonSchema{JsonValue{true}});
  if (!valid.Succeeded()) return valid;
  std::map<ProjectTaskId, TaskDependency> edges;
  for (auto& edge : spec.dependencies) {
    if (edge.upstream != id && !project.tasks.contains(edge.upstream)) {
      return Fail<void>(ErrorCode::NotFound, "The upstream task is outside this project");
    }
    if (std::ranges::any_of(edge.required_artifact_types, [](const auto& type) { return type.empty() || !ValidUtf8(type); })) {
      return Fail<void>(ErrorCode::InvalidArgument, "An upstream artifact type is invalid");
    }
    auto [entry, inserted] = edges.try_emplace(edge.upstream, edge);
    if (!inserted) {
      entry->second.accept_partial &= edge.accept_partial;
      auto& types = entry->second.required_artifact_types;
      types.insert(types.end(), edge.required_artifact_types.begin(), edge.required_artifact_types.end());
    }
  }
  spec.dependencies.clear();
  std::vector<ProjectTaskId> pending;
  for (auto& [upstream, edge] : edges) {
    std::ranges::sort(edge.required_artifact_types);
    auto duplicates = std::ranges::unique(edge.required_artifact_types);
    edge.required_artifact_types.erase(duplicates.begin(), duplicates.end());
    pending.push_back(upstream);
    spec.dependencies.push_back(std::move(edge));
  }
  std::set<ProjectTaskId> seen;
  while (!pending.empty()) {
    auto next = std::move(pending.back());
    pending.pop_back();
    if (next == id) return Fail<void>(ErrorCode::Conflict, "Task dependencies contain a cycle");
    if (!seen.insert(next).second) continue;
    for (const auto& edge : project.tasks.at(next).snapshot.spec.dependencies) pending.push_back(edge.upstream);
  }
  return AgentResult<void>::Success();
}

AgentResult<void> CheckAssignee(const RuntimeState& runtime, const ProjectData& project, const TaskAssignee& assignee) {
  if (const auto* session = std::get_if<SessionId>(&assignee)) {
    auto found = runtime.sessions.find(*session);
    const auto* image = found == runtime.sessions.end() ? nullptr : runtime.Published(found->second);
    if (!image || image->summary.project != project.id || image->summary.lifecycle != SessionLifecycle::Open) {
      return Fail<void>(ErrorCode::NotFound, "The assigned session is outside the project");
    }
  } else if (!runtime.agents.contains(std::get<AgentId>(assignee))) {
    return Fail<void>(ErrorCode::NotFound, "Unknown task assignee");
  }
  return AgentResult<void>::Success();
}

void SetOutcome(StoredProjectTask& task, ProjectId project, ProjectTaskState state, std::vector<ArtifactRef> artifacts = {},
                std::optional<Evidence> evidence = {}, std::optional<AgentError> reason = {}) {
  auto& view = task.snapshot;
  view.state = state;
  if (!view.attempt) return;
  TaskOutcome result{.execution = {project, view.id, *view.attempt}, .goal = view.attempt_goal, .state = state,
                     .artifacts = std::move(artifacts), .evidence = std::move(evidence), .reason = std::move(reason)};
  view.outcome = result;
  task.attempts.insert_or_assign(*view.attempt, std::move(result));
}

void ResetAttempt(ProjectTaskSnapshot& task) {
  task.attempt.reset();
  task.claimant.reset();
  task.run.reset();
  task.attempt_goal.reset();
  task.outcome.reset();
  task.resolved_inputs.clear();
  task.state = ProjectTaskState::Pending;
}

} // namespace

void RuntimeState::RefreshTaskReadiness(ProjectData& project) {
  for (auto& [id, entry] : project.tasks) {
    auto& task = entry.snapshot;
    if (task.attempt || TerminalTask(task.state)) continue;
    auto inputs = TaskInputs(*this, project, task);
    auto ready = inputs.Succeeded() ? (task.assignee ? ProjectTaskState::Assigned : ProjectTaskState::Ready)
                                   : ProjectTaskState::Blocked;
    auto reason = inputs.Succeeded() ? std::nullopt : std::optional{inputs.Error().message};
    if (task.state != ready || task.blocked_reason != reason) {
      task.state = ready;
      task.blocked_reason = std::move(reason);
      ++task.revision;
    }
  }
}

Async<Receipt<TaskUpdated>> RuntimeState::UpdateProjectTask(ProjectId id, CommandId command, TaskCommand request,
                                                            std::optional<RunId> bound) {
  auto actor = co_await ProjectAccess(id, bound, Capability::ManageTask);
  if (!actor.Succeeded()) co_return TaskResult::Failure(actor.Error());
  ProjectCommand key{request};
  auto replay = ReplayProject<TaskUpdated>(actor.Value(), id, command, key);
  if (!replay.Succeeded()) co_return TaskResult::Failure(replay.Error());
  if (replay.Value()) co_return co_await ReplayReceipt(TaskResult::Success(*replay.Value()));
  if (storage_error && options.store) co_return TaskResult::Failure(*storage_error);

  auto& project = projects.try_emplace(id, ProjectData{.id = id}).first->second;
  if (auto* create = std::get_if<CreateTask>(&request)) {
    ProjectTaskId task_id{MakeId()};
    auto valid = CheckSpec(*this, project, task_id, create->spec);
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    SessionData* goal_session = nullptr;
    if (const auto& goal_id = create->spec.goal) {
      const auto* home = GoalHome(*goal_id);
      if (!home || home->summary.project != id || home->retention != SessionRetention::Stored) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::NotFound, "The stored goal is outside this project");
      }
      goal_session = &sessions.at(home->summary.id);
      const auto& goal = goal_session->goals.at(*goal_id);
      const auto expected = goal.view.revision;
      auto policy = CollaborationAccess(actor.Value(), Capability::ManageGoal, *home);
      policy.target.goal = *goal_id;
      policy.goal = goal.view.ref;
      valid = co_await Require(std::move(policy));
      if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
      auto current = Actor(bound, Capability::ManageTask);
      if (!current.Succeeded()) co_return TaskResult::Failure(current.Error());
      if (current.Value().principal != actor.Value().principal) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::PermissionDenied, "The acting principal changed");
      }
      replay = ReplayProject<TaskUpdated>(actor.Value(), id, command, key);
      if (!replay.Succeeded()) co_return TaskResult::Failure(replay.Error());
      if (replay.Value()) co_return co_await ReplayReceipt(TaskResult::Success(*replay.Value()));
      if (storage_error) co_return TaskResult::Failure(*storage_error);
      if (goal_session->summary.lifecycle != SessionLifecycle::Open || goal_session->current_goal != *goal_id ||
          goal.view.revision != expected || goal.view.state == GoalState::Completed || goal.view.state == GoalState::Cancelled ||
          (ActorGoal(actor.Value()) && ActorGoal(actor.Value()) != goal.view.ref)) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The goal changed");
      }
    }
    if (create->expected_project != project.revision) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Project revision changed");
    auto& task = project.tasks.emplace(task_id, StoredProjectTask{ProjectTaskSnapshot{
        .id = task_id, .revision = 0, .spec = std::move(create->spec), .state = ProjectTaskState::Pending}}).first->second.snapshot;
    RefreshTaskReadiness(project);
    ++project.revision;
    if (goal_session) {
      auto& goal = goal_session->goals.at(*task.spec.goal);
      goal.view.tasks.push_back(task.id);
      std::ranges::sort(goal.view.tasks);
      ClearGoalVerification(*goal_session, goal);
      if (goal.view.state == GoalState::Verifying || goal.view.state == GoalState::AwaitingCompletionApproval) {
        goal.view.state = GoalState::Blocked;
        goal.view.reason = "Goal task membership changed";
      }
      ++goal.view.revision;
      ++goal_session->sequence;
    }
    co_return co_await CommitProject(project, actor.Value(), std::move(command), std::move(key),
                                     TaskUpdated{task.id, task.revision, task.attempt}, goal_session);
  }

  auto& change = std::get<ChangeTask>(request);
  const auto found = project.tasks.find(change.task);
  if (found == project.tasks.end()) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::NotFound, "Project task unavailable");
  auto& stored = found->second;
  if (change.expected != stored.snapshot.revision) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Task revision changed");
  auto prepared = stored;
  auto& task = prepared.snapshot;
  SessionData* start_session = nullptr;
  std::optional<RunStartBinding> binding;
  std::optional<Revision> agent_revision;
  RunExecution* cancel = nullptr;
  auto allowed = ProjectPolicy(actor.Value(), id, Capability::ManageTask);
  allowed.target.task = task.id;
  allowed.target.session = task.claimant;
  auto access = co_await Require(std::move(allowed));
  if (!access.Succeeded()) co_return TaskResult::Failure(access.Error());

  if (const auto* edit = std::get_if<EditTask>(&change.change)) {
    if (edit->expected_project != project.revision) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Project revision changed");
    if (edit->spec.goal != task.spec.goal) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Change goal membership through the goal service");
    }
    if (task.attempt || TerminalTask(task.state)) {
      const auto origin = ActorGoal(actor.Value());
      const auto* home = origin ? GoalHome(origin->id) : nullptr;
      if (!TerminalTask(task.state) || !task.outcome || !task.attempt_goal || !task.spec.goal || !origin || !home ||
          origin->id != *task.spec.goal || task.attempt_goal->id != origin->id ||
          task.attempt_goal->version >= origin->version || home->goals.at(origin->id).view.ref != origin ||
          edit->spec == task.spec) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Task is already claimed or closed");
      }
      ResetAttempt(task);
    }
    task.spec = edit->spec;
    auto valid = CheckSpec(*this, project, task.id, task.spec);
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
  } else if (const auto* assign = std::get_if<AssignTask>(&change.change)) {
    if (task.attempt || TerminalTask(task.state)) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Task is already claimed or closed");
    auto valid = CheckAssignee(*this, project, assign->assignee);
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    task.assignee = assign->assignee;
  } else if (const auto* claim = std::get_if<ClaimTask>(&change.change)) {
    if (task.attempt || (task.state != ProjectTaskState::Ready && task.state != ProjectTaskState::Assigned)) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Task cannot be claimed");
    }
    auto valid = CheckAssignee(*this, project, TaskAssignee{claim->session});
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    auto& target = sessions.at(claim->session);
    if (task.assignee && *task.assignee != TaskAssignee{claim->session} &&
        *task.assignee != TaskAssignee{target.summary.agent.id}) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::PermissionDenied, "The claimant does not match the assignment");
    }
    valid = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, target));
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    if (task.spec.goal) {
      const auto* home = GoalHome(*task.spec.goal);
      if (!home) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::NotFound, "The task goal is unavailable");
      task.attempt_goal = home->goals.at(*task.spec.goal).view.ref;
    }
    if (const auto origin = ActorGoal(actor.Value()); origin && task.spec.goal && task.attempt_goal != origin) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The task belongs to a different goal definition");
    }
    valid = co_await AuthorizeGoalDispatch(target, actor.Value(), task.attempt_goal);
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    auto inputs = TaskInputs(*this, project, task);
    if (!inputs.Succeeded()) co_return TaskResult::Failure(inputs.Error());
    valid = co_await ValidateProjectSources(id, actor.Value(), inputs.Value());
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    for (const auto& edge : task.spec.dependencies) {
      const auto upstream = project.tasks.at(edge.upstream).snapshot;
      if (upstream.state != ProjectTaskState::Failed) continue;
      if (!upstream.outcome || !options.verify) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Partial upstream result changed");
      Evidence partial{.sources = inputs.Value()};
      auto accepted = co_await options.verify({id, upstream.claimant, upstream.outcome->execution,
          upstream.revision, upstream.spec.completion_conditions, std::move(partial), {}});
      if (!accepted.Succeeded()) co_return TaskResult::Failure(accepted.Error());
      if (accepted.Value().status != VerificationStatus::Passed) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Partial upstream output was not accepted");
      }
    }
    task.attempt = AttemptId{MakeId()};
    task.claimant = claim->session;
    task.resolved_inputs = std::move(inputs).Value();
    task.state = ProjectTaskState::Assigned;
  } else if (const auto* release = std::get_if<ReleaseTask>(&change.change)) {
    if (task.attempt != release->attempt || task.state != ProjectTaskState::Assigned) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The claim is no longer releasable");
    }
    SetOutcome(prepared, id, ProjectTaskState::Cancelled, {}, {}, Error(ErrorCode::AlreadyClosed, "Claim released"));
    ResetAttempt(task);
  } else if (const auto* start = std::get_if<StartTask>(&change.change)) {
    if (task.attempt != start->attempt || task.state != ProjectTaskState::Assigned || !task.claimant) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The task claim changed");
    }
    start_session = &sessions.at(*task.claimant);
    if (start_session->summary.active_run) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Busy, "The claimant session is busy");
    auto permit = co_await Require(CollaborationAccess(actor.Value(), Capability::StartSessionRun, *start_session));
    if (!permit.Succeeded()) co_return TaskResult::Failure(permit.Error());
    permit = co_await AuthorizeGoalDispatch(*start_session, actor.Value(), task.attempt_goal);
    if (!permit.Succeeded()) co_return TaskResult::Failure(permit.Error());
    if (const auto origin = ActorGoal(actor.Value()); origin && task.spec.goal && task.attempt_goal != origin) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The task belongs to a different goal definition");
    }
    agent_revision = start_session->summary.binding_revision;
    auto fixed = co_await ResolveStart(*start_session, actor.Value().principal, {});
    if (!fixed.Succeeded()) co_return TaskResult::Failure(fixed.Error());
    binding = fixed.Value();
    auto valid = co_await ValidateProjectSources(id, actor.Value(), task.resolved_inputs);
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    task.state = ProjectTaskState::Running;
  } else if (std::holds_alternative<CompleteTask>(change.change) || std::holds_alternative<FailTask>(change.change)) {
    const auto* complete = std::get_if<CompleteTask>(&change.change);
    const auto* fail = std::get_if<FailTask>(&change.change);
    const auto attempt = complete ? complete->attempt : fail->attempt;
    const auto& artifacts = complete ? complete->artifacts : fail->artifacts;
    if (task.state != ProjectTaskState::Running || task.attempt != attempt || !task.run || (bound && bound != task.run)) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The task execution attempt changed");
    }
    std::vector<SourceSlice> output;
    for (const auto& ref : artifacts) {
      const auto* artifact = ProjectArtifact(*this, id, ref);
      if (!artifact || (artifact->run != task.run && !ReceivedArtifact(actor.Value(), *artifact))) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::PermissionDenied, "Artifact was not produced or received by this attempt");
      }
      output.push_back({ref});
    }
    auto valid = co_await ValidateProjectSources(id, actor.Value(), output);
    if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
    if (complete) {
      valid = ValidateJson(complete->evidence.values, JsonSchema{JsonValue{true}});
      if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
      valid = co_await ValidateProjectSources(id, actor.Value(), complete->evidence.sources);
      if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
      if (options.verify) {
        auto verified = co_await options.verify({id, task.claimant, TaskExecutionRef{id, task.id, attempt},
            task.revision, task.spec.completion_conditions, complete->evidence, runs.at(*task.run).stop.get_token()});
        if (!verified.Succeeded()) co_return TaskResult::Failure(verified.Error());
        if (verified.Value().status != VerificationStatus::Passed) {
          co_return Fail<Receipt<TaskUpdated>>(ErrorCode::InvalidState, verified.Value().reason);
        }
        valid = co_await ValidateProjectSources(id, actor.Value(), verified.Value().verified);
        if (!valid.Succeeded()) co_return TaskResult::Failure(valid.Error());
      } else if (!std::holds_alternative<std::nullptr_t>(task.spec.completion_conditions.value)) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Unsupported, "Task conditions require a host verifier");
      }
      if (runs.at(*task.run).view.state == RunState::Failed || runs.at(*task.run).view.state == RunState::Cancelled) {
        co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The producing run did not complete successfully");
      }
      SetOutcome(prepared, id, ProjectTaskState::Succeeded, artifacts, complete->evidence);
    } else {
      SetOutcome(prepared, id, ProjectTaskState::Failed, artifacts, {}, fail->reason);
    }
  } else if (std::holds_alternative<CancelTask>(change.change)) {
    if (TerminalTask(task.state)) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::AlreadyClosed, "Task has ended");
    if (task.run && !Terminal(runs.at(*task.run).view.state)) cancel = &runs.at(*task.run);
    SetOutcome(prepared, id, ProjectTaskState::Cancelled, {}, {}, Error(ErrorCode::AlreadyClosed, "Task cancelled"));
  } else {
    if (task.state != ProjectTaskState::Failed && task.state != ProjectTaskState::Cancelled) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::InvalidState, "Only failed or cancelled tasks can be retried");
    }
    ResetAttempt(task);
  }

  auto current = Actor(bound, Capability::ManageTask);
  if (!current.Succeeded()) co_return TaskResult::Failure(current.Error());
  if (current.Value().principal != actor.Value().principal) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::PermissionDenied, "The acting principal changed");
  replay = ReplayProject<TaskUpdated>(actor.Value(), id, command, key);
  if (!replay.Succeeded()) co_return TaskResult::Failure(replay.Error());
  if (replay.Value()) co_return co_await ReplayReceipt(TaskResult::Success(*replay.Value()));
  if (storage_error && options.store) co_return TaskResult::Failure(*storage_error);
  if (stored.snapshot.revision != change.expected) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "Task revision changed");
  if ((std::holds_alternative<ClaimTask>(change.change) || start_session)) {
    auto inputs = TaskInputs(*this, project, task);
    if (!inputs.Succeeded()) co_return TaskResult::Failure(inputs.Error());
    if (inputs.Value() != task.resolved_inputs) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "A dependency output changed");
    auto dispatch = CheckGoalDispatch(sessions.at(*task.claimant), task.attempt_goal);
    if (!dispatch.Succeeded()) co_return TaskResult::Failure(dispatch.Error());
  }

  RunExecution* running = nullptr;
  const SessionData* goal_home = nullptr;
  if (start_session) {
    if (start_session->summary.active_run) co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Busy, "The claimant session is busy");
    if (start_session->summary.project != id || start_session->summary.lifecycle != SessionLifecycle::Open ||
        start_session->summary.binding_revision != agent_revision) {
      co_return Fail<Receipt<TaskUpdated>>(ErrorCode::Conflict, "The claimant session changed");
    }
    auto input = task.spec.input;
    input.context = task.resolved_inputs;
    running = &RecordRun(*start_session, actor.Value().principal, std::move(input), *binding, RunTrigger::ProjectTask);
    running->view.task = TaskExecutionRef{id, task.id, *task.attempt};
    goal_home = AttachGoalRun(*running, task.attempt_goal);
    task.run = running->view.id;
  }
  ++task.revision;
  stored = std::move(prepared);
  ++project.revision;
  RefreshTaskReadiness(project);
  auto receipt = co_await CommitProject(project, actor.Value(), std::move(command), std::move(key),
      TaskUpdated{stored.snapshot.id, stored.snapshot.revision, stored.snapshot.attempt}, start_session, goal_home);
  if (receipt.Succeeded() && stage == Stage::Started) {
    if (running && !running->stop.stop_requested()) Launch(*running);
    if (cancel && !Terminal(cancel->view.state) && !cancel->stop.stop_requested()) Stop(*cancel, RunEndReason::UserCancelled);
  }
  co_return receipt;
}

ProjectData* RuntimeState::FinishProjectRun(const RunSnapshot& run) {
  if (!run.task || (run.state != RunState::Failed && run.state != RunState::Cancelled)) return nullptr;
  auto project = projects.find(run.task->project);
  if (project == projects.end()) return nullptr;
  auto entry = project->second.tasks.find(run.task->task);
  if (entry == project->second.tasks.end()) return nullptr;
  auto& task = entry->second;
  if (task.snapshot.attempt != run.task->attempt || task.snapshot.run != run.id || task.snapshot.state != ProjectTaskState::Running) {
    return nullptr;
  }
  SetOutcome(task, project->first, run.state == RunState::Failed ? ProjectTaskState::Failed : ProjectTaskState::Cancelled,
      {}, {}, run.failure.value_or(Error(ErrorCode::AlreadyClosed, "Task run cancelled")));
  ++task.snapshot.revision;
  ++project->second.revision;
  RefreshTaskReadiness(project->second);
  return &project->second;
}

bool RuntimeState::CurrentWorkClaim(const ProjectTaskSnapshot& task, const TaskExecutionRef& execution) const {
  auto project = projects.find(execution.project);
  if (project == projects.end()) return false;
  auto found = project->second.tasks.find(execution.task);
  if (found == project->second.tasks.end()) return false;
  const auto& current = found->second.snapshot;
  if (current.state != ProjectTaskState::Assigned || current.attempt != execution.attempt ||
      current.revision != task.revision || current.claimant != task.claimant || !current.claimant ||
      (current.spec.goal && !current.attempt_goal) ||
      !CheckGoalDispatch(sessions.at(*current.claimant), current.attempt_goal).Succeeded()) return false;
  auto inputs = TaskInputs(*this, project->second, current);
  return inputs.Succeeded() && inputs.Value() == task.resolved_inputs;
}

Async<ProjectTaskSnapshot> RuntimeState::ResolveWorkTask(const SessionData& target, WorkRequest work, const ActionParty& actor) {
  using Result = AgentResult<ProjectTaskSnapshot>;
  const auto& execution = *work.task;
  if (target.summary.project != execution.project) co_return Fail<ProjectTaskSnapshot>(ErrorCode::PermissionDenied, "Work task belongs to another project");
  auto authorized = co_await ProjectAccess(execution.project, actor.run, Capability::ManageTask);
  if (!authorized.Succeeded()) co_return Result::Failure(authorized.Error());
  if (authorized.Value().principal != actor.principal) co_return Fail<ProjectTaskSnapshot>(ErrorCode::PermissionDenied, "The acting principal changed");
  auto request = ProjectPolicy(actor, execution.project, Capability::ManageTask);
  request.target.task = execution.task;
  request.target.session = target.summary.id;
  auto permit = co_await Require(std::move(request));
  if (!permit.Succeeded()) co_return Result::Failure(permit.Error());
  const auto* project = PublishedProject(execution.project);
  if (!project || !project->tasks.contains(execution.task)) co_return Fail<ProjectTaskSnapshot>(ErrorCode::NotFound, "Task unavailable");
  auto task = project->tasks.at(execution.task).snapshot;
  if (task.claimant != target.summary.id || !CurrentWorkClaim(task, execution)) {
    co_return Fail<ProjectTaskSnapshot>(ErrorCode::Conflict, "Task claim is stale or belongs to a different session");
  }
  if (task.spec.input != work.input) co_return Fail<ProjectTaskSnapshot>(ErrorCode::Conflict, "Work input does not match the claimed task");
  if (work.scope && std::ranges::any_of(task.resolved_inputs, [&](const auto& source) {
        return std::ranges::find(work.scope->inputs, source) == work.scope->inputs.end();
      })) co_return Fail<ProjectTaskSnapshot>(ErrorCode::PermissionDenied, "Work scope excludes a claimed source");
  auto authorized_input = co_await ValidateProjectSources(execution.project, actor, task.resolved_inputs);
  if (!authorized_input.Succeeded()) co_return Result::Failure(authorized_input.Error());
  if (!CurrentWorkClaim(task, execution)) co_return Fail<ProjectTaskSnapshot>(ErrorCode::Conflict, "Task claim changed");
  co_return Result::Success(std::move(task));
}

ProjectData& RuntimeState::BindWorkTask(const TaskExecutionRef& execution, RunExecution& run) {
  auto& project = projects.at(execution.project);
  auto& task = project.tasks.at(execution.task).snapshot;
  task.run = run.view.id;
  task.state = ProjectTaskState::Running;
  ++task.revision;
  ++project.revision;
  run.view.task = execution;
  run.view.trigger = RunTrigger::ProjectTask;
  return project;
}

ProjectData* RuntimeState::ReleaseGoalClaims(const SessionData& home, const GoalRef& ref) {
  if (!home.summary.project || !projects.contains(*home.summary.project)) return nullptr;
  auto& project = projects.at(*home.summary.project);
  bool released = false;
  for (auto& [id, entry] : project.tasks) {
    auto& task = entry.snapshot;
    if (task.state != ProjectTaskState::Assigned || task.attempt_goal != ref || !task.attempt) continue;
    SetOutcome(entry, project.id, ProjectTaskState::Cancelled, {}, {}, Error(ErrorCode::Conflict, "The goal definition changed"));
    ResetAttempt(task);
    ++task.revision;
    released = true;
  }
  if (!released) return nullptr;
  ++project.revision;
  RefreshTaskReadiness(project);
  return &project;
}

} // namespace huxerui::agentkit::detail
