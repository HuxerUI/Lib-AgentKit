#include "runtime_internal.h"
#include "json_wire.h"

#include <limits>
#include <nlohmann/json.hpp>

namespace huxerui::agentkit {
namespace detail {
namespace {

using GoalReceipt = AgentResult<Receipt<GoalUpdated>>;

bool Closed(GoalState state) {
  return state == GoalState::Completed || state == GoalState::Cancelled;
}

AgentResult<void> CheckDefinition(const GoalDefinition& definition) {
  if (Title(definition.objective).empty()) return Fail<void>(ErrorCode::InvalidArgument, "A UTF-8 objective is required");
  return ValidateJson(definition.completion_conditions, JsonSchema{JsonValue{true}});
}

AgentResult<void> CheckBudget(const Budget& budget) {
  if (budget.deadline && *budget.deadline <= Duration::zero()) {
    return Fail<void>(ErrorCode::InvalidArgument, "The goal duration must be positive");
  }
  return AgentResult<void>::Success();
}

AgentError Stale(const GoalSnapshot& goal) {
  auto error = Error(ErrorCode::Conflict, "The goal changed");
  error.current_revision = goal.revision;
  return error;
}

std::optional<TimePoint> GoalDeadline(const StoredGoal& goal) {
  if (!goal.enabled_at || !goal.view.spec.budget.deadline) return {};
  const auto room = TimePoint::max() - *goal.enabled_at;
  return *goal.enabled_at + std::min(room, *goal.view.spec.budget.deadline);
}

std::uint64_t Add(std::uint64_t left, std::uint64_t right) {
  return left + std::min(right, std::numeric_limits<std::uint64_t>::max() - left);
}

void AddUsage(Usage& used, const Usage& more) {
  used.input_tokens = Add(used.input_tokens, more.input_tokens);
  used.output_tokens = Add(used.output_tokens, more.output_tokens);
  used.model_turns = Add(used.model_turns, more.model_turns);
  used.tool_calls = Add(used.tool_calls, more.tool_calls);
}

Usage GoalUsage(const RuntimeState& state, const StoredGoal& goal, const RunId* excluded = nullptr) {
  auto used = goal.view.usage;
  for (const auto& id : goal.view.runs) {
    if (excluded && id == *excluded) continue;
    auto run = state.runs.find(id);
    if (run != state.runs.end() && !Terminal(run->second.view.state)) AddUsage(used, run->second.view.usage);
  }
  return used;
}

bool Exhausted(const StoredGoal& goal, TimePoint now, const Usage& used) {
  const auto& budget = goal.view.spec.budget;
  const auto deadline = GoalDeadline(goal);
  return (deadline && now >= *deadline) ||
      (budget.tokens && (used.input_tokens >= *budget.tokens || used.output_tokens >= *budget.tokens - used.input_tokens)) ||
      (budget.model_turns && used.model_turns >= *budget.model_turns) ||
      (budget.tool_calls && used.tool_calls >= *budget.tool_calls) ||
      (budget.context_bytes && *budget.context_bytes == 0);
}

bool Exhausted(const StoredGoal& goal, TimePoint now) { return Exhausted(goal, now, goal.view.usage); }

GoalEditImpact EditImpact(const RuntimeState& state, const SessionData& session, const StoredGoal& goal) {
  GoalEditImpact result{.previous = goal.view.ref, .resets_completion_review = goal.verification.has_value()};
  if (session.summary.active_run) {
    const auto& run = state.runs.at(*session.summary.active_run);
    if (run.view.goal == goal.view.ref && run.view.state != RunState::Cancelling) {
      result.retiring_run = run.view.id;
      for (const auto& call : session.tools) {
        if (call.run == run.view.id && call.state == ToolCallState::Executing) result.in_flight_calls.push_back(call.id);
      }
      for (const auto& interaction : session.interactions) {
        if (interaction.run == run.view.id && interaction.state == InteractionState::Pending) {
          result.invalidated_interactions.push_back(interaction.id);
        }
      }
    }
  }
  if (goal.view.completion_review) result.invalidated_interactions.push_back(goal.view.completion_review->approval.interaction);
  for (const auto& [id, run] : state.runs) {
    if (run.view.session == session.summary.id || run.view.goal != goal.view.ref || Terminal(run.view.state)) continue;
    if (!state.RetireWorkWithGoal(run)) { result.continuing_runs.push_back(id); continue; }
    if (run.view.state == RunState::Cancelling) continue;
    result.retiring_linked_runs.push_back(id);
    const auto& target = state.sessions.at(run.view.session);
    for (const auto& call : target.tools) {
      if (call.run == id && call.state == ToolCallState::Executing) result.in_flight_calls.push_back(call.id);
    }
    for (const auto& item : target.interactions) {
      if (item.run == id && item.state == InteractionState::Pending) result.invalidated_interactions.push_back(item.id);
    }
  }
  for (const auto& [id, target] : state.sessions) {
    for (const auto& [message, item] : target.collaboration) {
      if (item.goal == goal.view.ref && item.record.summary.delivery == DeliveryState::Queued) {
        result.withdrawn_requests.push_back(message);
      }
    }
  }
  for (const auto& [key, command] : state.commands) {
    if (command.input.operation == Operation::StartRequest && command.input.message && command.progress.approval &&
        (command.progress.state == CommandState::AwaitingApproval || command.progress.state == CommandState::Scheduled) &&
        std::ranges::find(result.withdrawn_requests, *command.input.message) != result.withdrawn_requests.end()) {
      result.invalidated_interactions.push_back(command.progress.approval->interaction);
    }
  }
  if (session.summary.project && state.projects.contains(*session.summary.project)) {
    const auto& project = state.projects.at(*session.summary.project);
    for (const auto& [id, entry] : project.tasks) {
      const auto& task = entry.snapshot;
      if (task.attempt_goal != goal.view.ref || !task.attempt) continue;
      if (task.state == ProjectTaskState::Assigned) result.released_attempts.push_back({project.id, id, *task.attempt});
      if (task.state == ProjectTaskState::Running && task.claimant != session.summary.id && task.run &&
          !state.RetireWorkWithGoal(state.runs.at(*task.run))) {
        result.continuing_attempts.push_back({project.id, id, *task.attempt});
      }
    }
  }
  return result;
}

template<class T>
bool ContainsAll(const std::vector<T>& before, const std::vector<T>& after) {
  return std::ranges::all_of(after, [&](const T& item) { return std::ranges::find(before, item) != before.end(); });
}

bool SameImpact(const GoalEditImpact& before, const GoalEditImpact& after) {
  return before.previous == after.previous && (!after.retiring_run || before.retiring_run == after.retiring_run) &&
      (!after.resets_completion_review || before.resets_completion_review) &&
      ContainsAll(before.in_flight_calls, after.in_flight_calls) &&
      ContainsAll(before.retiring_linked_runs, after.retiring_linked_runs) &&
      ContainsAll(before.continuing_runs, after.continuing_runs) &&
      ContainsAll(before.continuing_attempts, after.continuing_attempts) &&
      ContainsAll(before.withdrawn_requests, after.withdrawn_requests) &&
      ContainsAll(before.released_attempts, after.released_attempts) &&
      ContainsAll(before.invalidated_interactions, after.invalidated_interactions);
}

Task<GoalEditImpact> VisibleImpact(RuntimeState& state, const ActionParty& actor, const SessionData& home, GoalEditImpact full) {
  GoalEditImpact result{.previous = full.previous, .retiring_run = full.retiring_run,
      .resets_completion_review = full.resets_completion_review};
  for (const auto& [id, session] : state.sessions) {
    if (&session != &home) {
      const auto relevant = std::ranges::any_of(full.continuing_runs, [&](const auto& run) {
        return state.runs.at(run).view.session == id;
      }) || std::ranges::any_of(full.retiring_linked_runs, [&](const auto& run) {
        return state.runs.at(run).view.session == id;
      }) || std::ranges::any_of(full.withdrawn_requests, [&](const auto& message) { return session.collaboration.contains(message); });
      if (!relevant || !(co_await state.Require(state.CollaborationAccess(actor, Capability::ReadSession, session))).Succeeded()) continue;
    }
    for (const auto& run : full.continuing_runs) {
      if (state.runs.at(run).view.session == id) result.continuing_runs.push_back(run);
    }
    for (const auto& run : full.retiring_linked_runs) {
      if (state.runs.at(run).view.session == id) result.retiring_linked_runs.push_back(run);
    }
    for (const auto& call : session.tools) {
      if (std::ranges::find(full.in_flight_calls, call.id) != full.in_flight_calls.end()) result.in_flight_calls.push_back(call.id);
    }
    for (const auto& message : full.withdrawn_requests) {
      if (session.collaboration.contains(message)) result.withdrawn_requests.push_back(message);
    }
    for (const auto& interaction : session.interactions) {
      if (interaction.recipient == actor.principal &&
          std::ranges::find(full.invalidated_interactions, interaction.id) != full.invalidated_interactions.end()) {
        result.invalidated_interactions.push_back(interaction.id);
      }
    }
  }
  auto task_visible = [&](const TaskExecutionRef& ref) {
    auto request = state.ProjectPolicy(actor, ref.project, Capability::ReadProject);
    request.target.task = ref.task;
    return state.Require(std::move(request));
  };
  for (const auto& attempt : full.released_attempts) {
    if ((co_await task_visible(attempt)).Succeeded()) result.released_attempts.push_back(attempt);
  }
  for (const auto& attempt : full.continuing_attempts) {
    if ((co_await task_visible(attempt)).Succeeded()) result.continuing_attempts.push_back(attempt);
  }
  co_return result;
}

bool EvidenceState(GoalState state) {
  return state == GoalState::Active || state == GoalState::Blocked || state == GoalState::Verifying ||
      state == GoalState::AwaitingCompletionApproval;
}

Capability ChangeCapability(const ChangeGoal* change) {
  if (change && std::holds_alternative<EditGoal>(change->change)) return Capability::EditGoalDefinition;
  if (change && std::holds_alternative<SubmitGoalEvidence>(change->change)) return Capability::CompleteGoal;
  return Capability::ManageGoal;
}

PolicyRequest GoalPolicy(const RuntimeState& state, const SessionData& session, const ActionParty& party,
                         Capability capability, std::optional<GoalId> goal) {
  auto policy = state.CollaborationAccess(party, capability, session);
  policy.target.goal = goal;
  if (goal && session.goals.contains(*goal)) policy.goal = session.goals.at(*goal).view.ref;
  return policy;
}

AgentResult<ActionParty> GoalParty(const RuntimeState& state, const PrincipalId& principal, const SessionId& id,
    std::optional<RunId> bound, Capability capability, bool detached = false) {
  if (!state.sessions.contains(id)) return Fail<ActionParty>(ErrorCode::NotFound, "Goal session unavailable");
  if (detached && bound) {
    auto user = state.User();
    const auto run = state.runs.find(*bound);
    if (!user.Succeeded() || user.Value() != principal || run == state.runs.end() ||
        run->second.principal != principal || run->second.view.session != id) {
      return Fail<ActionParty>(ErrorCode::PermissionDenied, "The goal request owner is unavailable");
    }
    const auto agent = state.agents.find(run->second.view.agent.id);
    if (agent == state.agents.end() || agent->second.ref != run->second.view.agent ||
        !state.AllowsAgentOperation(run->second, capability)) {
      return Fail<ActionParty>(ErrorCode::PermissionDenied, "The agent's goal capability has changed");
    }
    return AgentResult<ActionParty>::Success(RuntimeState::Request(principal, capability, &state.sessions.at(id), &run->second).actor);
  }
  auto actor = state.Actor(bound, capability);
  if (!actor.Succeeded()) return actor;
  if (actor.Value().principal != principal || (bound && actor.Value().session != id)) {
    return Fail<ActionParty>(ErrorCode::PermissionDenied, "The goal belongs to another session or principal");
  }
  return actor;
}

} // namespace

Async<ActionParty> RuntimeState::GoalActor(SessionId id, PrincipalId principal, std::optional<RunId> bound,
    Capability capability, std::optional<GoalId> goal) {
  auto party = GoalParty(*this, principal, id, bound, capability);
  if (!party.Succeeded()) co_return party;
  auto& session = sessions.at(id);
  auto permission = co_await WithSession(id, principal, Capability::ReadSession);
  if (!permission.Succeeded()) co_return AgentResult<ActionParty>::Failure(permission.Error());
  permission = co_await Require(GoalPolicy(*this, session, party.Value(), capability, goal));
  if (!permission.Succeeded()) co_return AgentResult<ActionParty>::Failure(permission.Error());
  co_return GoalParty(*this, principal, id, bound, capability);
}

Async<std::optional<GoalSnapshot>> RuntimeState::ReadGoal(SessionId id, PrincipalId principal, std::optional<RunId> bound) {
  using Result = AgentResult<std::optional<GoalSnapshot>>;
  auto actor = co_await GoalActor(id, principal, bound, Capability::ReadGoal);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  const auto* visible = Published(sessions.at(id));
  if (!visible || !visible->current_goal) co_return Result::Success(std::nullopt);
  const auto current = *visible->current_goal;
  actor = co_await GoalActor(id, principal, bound, Capability::ReadGoal, current);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  visible = Published(sessions.at(id));
  if (!visible || visible->current_goal != current) co_return Fail<std::optional<GoalSnapshot>>(ErrorCode::CursorExpired, "The visible goal changed");
  auto result = visible->goals.at(current).view;
  std::vector<RunId> readable_runs;
  for (const auto& id : result.runs) {
    auto run = runs.find(id);
    if (run == runs.end()) continue;
    const auto& target = sessions.at(run->second.view.session);
    const auto* image = Published(target);
    if (!image) continue;
    if (image->retention == SessionRetention::Ephemeral) {
      if (!Terminal(run->second.view.state)) AddUsage(result.usage, run->second.view.usage);
    } else {
      const auto& entries = saved_sessions.at(image->summary.id).image.runs;
      auto saved = std::ranges::find_if(entries, [&](const auto& item) { return item.view.id == id; });
      if (saved != entries.end() && !Terminal(saved->view.state)) AddUsage(result.usage, saved->view.usage);
    }
    if (target.summary.id == result.session ||
        (co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, target))).Succeeded()) readable_runs.push_back(id);
  }
  result.runs = std::move(readable_runs);
  if (result.evidence || result.verification) {
    std::vector<SourceSlice> sources;
    if (result.evidence) sources = result.evidence->sources;
    if (result.verification) sources.insert(sources.end(), result.verification->verified.begin(), result.verification->verified.end());
    auto access = co_await ValidateInputSources(sessions.at(id), actor.Value(), sources);
    if (!access.Succeeded()) {
      result.evidence.reset();
      result.verification.reset();
    }
  }
  co_return Result::Success(std::move(result));
}

Async<GoalDefinition> RuntimeState::ReadGoalDefinition(SessionId id, PrincipalId principal, std::optional<RunId> bound, GoalRef ref) {
  auto actor = GoalParty(*this, principal, id, bound, Capability::ReadGoal);
  if (!actor.Succeeded()) co_return AgentResult<GoalDefinition>::Failure(actor.Error());
  auto access = co_await WithSession(id, principal, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<GoalDefinition>::Failure(access.Error());
  const auto* owner = &sessions.at(id);
  if (!owner->goals.contains(ref.id) && bound && runs.at(*bound).view.goal == ref) owner = GoalHome(ref.id);
  if (!owner || owner->summary.lifecycle == SessionLifecycle::Deleting) {
    co_return Fail<GoalDefinition>(ErrorCode::NotFound, "Goal definition unavailable");
  }
  auto policy = GoalPolicy(*this, *owner, actor.Value(), Capability::ReadGoal, ref.id);
  policy.goal = ref;
  access = co_await Require(std::move(policy));
  if (!access.Succeeded()) co_return AgentResult<GoalDefinition>::Failure(access.Error());
  actor = GoalParty(*this, principal, id, bound, Capability::ReadGoal);
  if (!actor.Succeeded()) co_return AgentResult<GoalDefinition>::Failure(actor.Error());
  const auto* visible = Published(*owner);
  if (visible && visible->goals.contains(ref.id)) {
    const auto& definitions = visible->goals.at(ref.id).definitions;
    if (auto found = definitions.find(ref.version); found != definitions.end()) {
      co_return AgentResult<GoalDefinition>::Success(found->second);
    }
  }
  co_return Fail<GoalDefinition>(ErrorCode::NotFound, "Goal definition unavailable");
}

Async<GoalEditPreview> RuntimeState::PreviewGoalEdit(SessionId id, PrincipalId principal, std::optional<RunId> bound,
    GoalId goal, Revision expected, GoalDefinition definition) {
  auto valid = CheckDefinition(definition);
  if (!valid.Succeeded()) co_return AgentResult<GoalEditPreview>::Failure(valid.Error());
  auto actor = GoalParty(*this, principal, id, bound, Capability::EditGoalDefinition);
  if (!actor.Succeeded()) co_return AgentResult<GoalEditPreview>::Failure(actor.Error());
  auto access = co_await WithSession(id, principal, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<GoalEditPreview>::Failure(access.Error());
  const auto& session = sessions.at(id);
  std::vector<Capability> confirmation;
  access = co_await CheckOperation(GoalPolicy(*this, session, actor.Value(), Capability::EditGoalDefinition, goal), confirmation, {});
  if (!access.Succeeded()) co_return AgentResult<GoalEditPreview>::Failure(access.Error());
  if (!session.goals.contains(goal)) co_return Fail<GoalEditPreview>(ErrorCode::NotFound, "Goal unavailable");
  const auto& stored = session.goals.at(goal);
  if (Closed(stored.view.state)) co_return Fail<GoalEditPreview>(ErrorCode::AlreadyClosed, "Goal is closed");
  if (stored.view.revision != expected) co_return AgentResult<GoalEditPreview>::Failure(Stale(stored.view));
  const auto now = Now();
  std::erase_if(goal_edits, [&](const auto& entry) { return entry.second.expires <= now; });
  auto impact = EditImpact(*this, session, stored);
  auto visible = co_await VisibleImpact(*this, actor.Value(), session, impact);
  if (stored.view.revision != expected || !SameImpact(impact, EditImpact(*this, session, stored))) {
    co_return AgentResult<GoalEditPreview>::Failure(Stale(stored.view));
  }
  auto token = MakeId();
  goal_edits.emplace(token, PreparedGoalEdit{id, actor.Value(), expected, std::move(definition), impact, now + Duration(300000)});
  co_return AgentResult<GoalEditPreview>::Success({std::move(visible), std::move(token)});
}

Async<Receipt<GoalUpdated>> RuntimeState::UpdateGoal(SessionId id, PrincipalId principal, std::optional<RunId> bound,
    CommandId command, GoalCommand input, std::optional<OperationPlan> approved) {
  const auto* change = std::get_if<ChangeGoal>(&input);
  const auto* edit = change ? std::get_if<EditGoal>(&change->change) : nullptr;
  const auto* evidence = change ? std::get_if<SubmitGoalEvidence>(&change->change) : nullptr;
  const auto* membership = change ? std::get_if<SetGoalTasks>(&change->change) : nullptr;
  const auto capability = ChangeCapability(change);
  auto actor = GoalParty(*this, principal, id, bound, capability, approved.has_value());
  if (!actor.Succeeded()) co_return GoalReceipt::Failure(actor.Error());
  auto readable = co_await WithSession(id, principal, Capability::ReadSession);
  if (!readable.Succeeded()) co_return GoalReceipt::Failure(readable.Error());
  auto& session = sessions.at(id);
  const CommandInput key{.operation = Operation::Goal, .session = id, .run = bound.value_or(RunId{}), .goal = input};
  auto check = Find(principal, command, key);
  if (!check.Succeeded()) co_return GoalReceipt::Failure(check.Error());
  if (check.Value() && !approved) co_return co_await ReplayReceipt(ReceiptFor<GoalUpdated>(principal, command));
  auto lookup = [&]() -> AgentResult<StoredGoal*> {
    if (stage != Stage::Started || session.summary.lifecycle != SessionLifecycle::Open) {
      return Fail<StoredGoal*>(ErrorCode::InvalidState, "The goal session is unavailable");
    }
    if (storage_error && session.retention == SessionRetention::Stored) return AgentResult<StoredGoal*>::Failure(*storage_error);
    if (!change) {
      if (session.current_goal && !Closed(session.goals.at(*session.current_goal).view.state)) {
        return Fail<StoredGoal*>(ErrorCode::Busy, "This session already has a goal");
      }
      return AgentResult<StoredGoal*>::Success(nullptr);
    }
    if (!session.goals.contains(change->goal)) return Fail<StoredGoal*>(ErrorCode::NotFound, "Goal unavailable");
    auto& current = session.goals.at(change->goal);
    if (Closed(current.view.state)) return Fail<StoredGoal*>(ErrorCode::AlreadyClosed, "Goal is closed");
    if (current.view.revision != change->expected || (evidence && evidence->goal != current.view.ref)) {
      return AgentResult<StoredGoal*>::Failure(Stale(current.view));
    }
    return AgentResult<StoredGoal*>::Success(&current);
  };
  auto target = lookup();
  if (!target.Succeeded()) co_return GoalReceipt::Failure(target.Error());
  auto request = GoalPolicy(*this, session, actor.Value(), capability, change ? std::optional{change->goal} : std::nullopt);
  if (evidence) request.sources = evidence->evidence.sources;
  if (edit) request.operation = JsonValue{edit->definition.objective};
  std::vector<Capability> confirmation;
  auto permission = co_await CheckOperation(std::move(request), confirmation, approved);
  if (!permission.Succeeded()) co_return GoalReceipt::Failure(permission.Error());
  if (edit && bound && !approved && confirmation.empty()) confirmation.push_back(capability);
  if (evidence) {
    const auto snapshot = target.Value()->view;
    if (Exhausted(*target.Value(), Now())) co_return Fail<Receipt<GoalUpdated>>(ErrorCode::BudgetExceeded, "Goal budget exhausted");
    if (bound && runs.at(*bound).view.goal != evidence->goal) {
      co_return Fail<Receipt<GoalUpdated>>(ErrorCode::Conflict, "The submitting run uses another goal version");
    }
    if (!options.verify) co_return Fail<Receipt<GoalUpdated>>(ErrorCode::Unsupported, "A goal verifier is required");
    auto valid = ValidateJson(evidence->evidence.values, snapshot.spec.definition.evidence_schema.value_or(JsonSchema{JsonValue{true}}));
    if (valid.Succeeded()) valid = co_await ValidateInputSources(session, actor.Value(), evidence->evidence.sources);
    if (!valid.Succeeded()) co_return GoalReceipt::Failure(valid.Error());
  }
  if (membership && (!membership->tasks.empty() || !target.Value()->view.tasks.empty())) {
    if (!session.summary.project || session.retention != SessionRetention::Stored) {
      co_return Fail<Receipt<GoalUpdated>>(ErrorCode::InvalidState, "Goal tasks require a stored project session");
    }
    permission = co_await Require(ProjectPolicy(actor.Value(), *session.summary.project, Capability::ManageTask));
    if (!permission.Succeeded()) co_return GoalReceipt::Failure(permission.Error());
  }
  actor = GoalParty(*this, principal, id, bound, capability, approved.has_value());
  if (!actor.Succeeded()) co_return GoalReceipt::Failure(actor.Error());
  check = Find(principal, command, key);
  if (!check.Succeeded()) co_return GoalReceipt::Failure(check.Error());
  if (check.Value() && !approved) co_return co_await ReplayReceipt(ReceiptFor<GoalUpdated>(principal, command));
  target = lookup();
  if (!target.Succeeded()) co_return GoalReceipt::Failure(target.Error());

  StoredGoal* goal = target.Value();
  GoalEditImpact impact{};
  std::optional<TimePoint> approval_deadline;
  if (const auto* proposal = std::get_if<ProposeGoal>(&input)) {
    auto valid = CheckDefinition(proposal->spec.definition);
    if (valid.Succeeded()) valid = CheckBudget(proposal->spec.budget);
    if (!valid.Succeeded()) co_return GoalReceipt::Failure(valid.Error());
  } else if (edit) {
    auto valid = CheckDefinition(edit->definition);
    if (!valid.Succeeded()) co_return GoalReceipt::Failure(valid.Error());
    if (edit->definition == goal->view.spec.definition) {
      co_return co_await FinishOperation(session, principal, command, key,
          GoalUpdated{goal->view.ref, goal->view.revision}, false, approved.has_value());
    }
    impact = EditImpact(*this, session, *goal);
    if (approved) {
      if (!approved->goal_edit || !SameImpact(*approved->goal_edit, impact)) co_return GoalReceipt::Failure(Stale(goal->view));
    } else if (!bound) {
      auto prepared = edit->preview_token ? goal_edits.find(*edit->preview_token) : goal_edits.end();
      if (prepared == goal_edits.end() || prepared->second.expires <= Now() || prepared->second.session != id ||
          prepared->second.actor.principal != principal || prepared->second.actor.run != bound ||
          prepared->second.expected != change->expected || prepared->second.definition != edit->definition ||
          !SameImpact(prepared->second.impact, impact)) co_return GoalReceipt::Failure(Stale(goal->view));
      approval_deadline = prepared->second.expires;
    }
  } else if (evidence) {
    if (!EvidenceState(goal->view.state)) co_return Fail<Receipt<GoalUpdated>>(ErrorCode::InvalidState, "The goal does not currently accept evidence");
    if (Exhausted(*goal, Now())) co_return Fail<Receipt<GoalUpdated>>(ErrorCode::BudgetExceeded, "Goal budget exhausted");
  }

  if (!confirmation.empty()) {
    if (!approval_deadline) approval_deadline = Now() + Duration(300000);
    std::optional<GoalEditImpact> visible;
    if (edit) {
      visible = co_await VisibleImpact(*this, actor.Value(), session, impact);
      if (goal->view.revision != change->expected || !SameImpact(impact, EditImpact(*this, session, *goal))) {
        co_return GoalReceipt::Failure(Stale(goal->view));
      }
    }
    auto accepted = co_await StageOperation(principal, command, key, id,
        {.confirmation = std::move(confirmation), .goal_edit = edit ? std::optional{impact} : std::nullopt},
        {.title = edit ? "Edit goal" : evidence ? "Submit goal evidence" : "Update goal",
         .summary = edit ? edit->definition.objective : goal ? goal->view.spec.definition.objective : std::get<ProposeGoal>(input).spec.definition.objective,
         .goal_edit = std::move(visible)}, approval_deadline);
    if (!accepted.Succeeded()) co_return GoalReceipt::Failure(accepted.Error());
    co_return co_await ReplayReceipt(ReceiptFor<GoalUpdated>(principal, command));
  }

  ProjectData* project = nullptr;
  std::vector<const SessionData*> affected;
  if (const auto* proposal = std::get_if<ProposeGoal>(&input)) {
    GoalId next{MakeId()};
    session.current_goal = next;
    goal = &session.goals.emplace(next, StoredGoal{
        .view = {.ref = {next, 1}, .session = id, .revision = 1, .spec = proposal->spec, .state = GoalState::Proposed, .usage = {}},
        .owner = principal, .definitions = {{1, proposal->spec.definition}}}).first->second;
  } else {
    auto& view = goal->view;
    if (edit) {
      goal->retired[view.ref.version] = view;
      project = ReleaseGoalClaims(session, view.ref);
      affected = WithdrawGoalWork(view.ref);
      ClearGoalVerification(session, *goal);
      view.evidence.reset();
      view.verification.reset();
      view.reason.reset();
      ++view.ref.version;
      view.spec.definition = edit->definition;
      goal->definitions.emplace(view.ref.version, edit->definition);
      if (view.state != GoalState::Proposed && view.state != GoalState::Paused) view.state = GoalState::Active;
      if (impact.retiring_run) {
        auto& run = runs.at(*impact.retiring_run);
        run.view.end_reason = RunEndReason::GoalRevised;
        Stop(run, RunEndReason::GoalRevised);
      }
      for (const auto& retired : impact.retiring_linked_runs) {
        auto& run = runs.at(retired);
        run.view.end_reason = RunEndReason::GoalRevised;
        Stop(run, RunEndReason::GoalRevised);
        const auto* target = &sessions.at(run.view.session);
        if (std::ranges::find(affected, target) == affected.end()) affected.push_back(target);
      }
    } else if (evidence) {
      ClearGoalVerification(session, *goal);
      view.evidence = evidence->evidence;
      view.verification.reset();
      view.reason.reset();
      view.state = GoalState::Verifying;
      goal->verification_stop = std::stop_source{};
      goal->verification = GoalVerification{command, actor.Value(), view.revision + 1};
    } else if (const auto* budget = std::get_if<SetGoalBudget>(&change->change)) {
      auto valid = CheckBudget(budget->budget);
      if (!valid.Succeeded()) co_return GoalReceipt::Failure(valid.Error());
      if (bound) co_return Fail<Receipt<GoalUpdated>>(ErrorCode::PermissionDenied, "A user must change the goal budget");
      view.spec.budget = budget->budget;
    } else if (membership) {
      auto selected = membership->tasks;
      std::ranges::sort(selected);
      selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
      if (selected != view.tasks) {
        if (!session.summary.project || !projects.contains(*session.summary.project)) {
          co_return Fail<Receipt<GoalUpdated>>(ErrorCode::NotFound, "The goal project is unavailable");
        }
        project = &projects.at(*session.summary.project);
        for (const auto& task : selected) {
          const auto found = project->tasks.find(task);
          if (found == project->tasks.end() || (found->second.snapshot.spec.goal && found->second.snapshot.spec.goal != view.ref.id) ||
              (found->second.snapshot.attempt && found->second.snapshot.spec.goal != view.ref.id)) {
            co_return Fail<Receipt<GoalUpdated>>(ErrorCode::Conflict, "A task is unavailable for this goal");
          }
        }
        for (const auto& task : view.tasks) {
          auto& entry = project->tasks.at(task).snapshot;
          if (std::ranges::find(selected, task) != selected.end()) continue;
          if (entry.state == ProjectTaskState::Running || entry.state == ProjectTaskState::Assigned) {
            co_return Fail<Receipt<GoalUpdated>>(ErrorCode::Conflict, "Release the active attempt before changing membership");
          }
        }
        for (const auto& task : view.tasks) {
          if (std::ranges::find(selected, task) != selected.end()) continue;
          auto& entry = project->tasks.at(task).snapshot;
          entry.spec.goal.reset();
          ++entry.revision;
        }
        for (const auto& task : selected) {
          auto& entry = project->tasks.at(task).snapshot;
          if (entry.spec.goal == view.ref.id) continue;
          entry.spec.goal = view.ref.id;
          ++entry.revision;
        }
        view.tasks = std::move(selected);
        ++project->revision;
        ClearGoalVerification(session, *goal);
        if (EvidenceState(view.state)) view.state = GoalState::Active;
      }
    } else {
      if (bound) co_return Fail<Receipt<GoalUpdated>>(ErrorCode::PermissionDenied, "A user must control this goal");
      const bool enable = std::holds_alternative<EnableGoal>(change->change) && view.state == GoalState::Proposed;
      const bool resume = std::holds_alternative<ResumeGoal>(change->change) &&
          (view.state == GoalState::Blocked || view.state == GoalState::Paused || view.state == GoalState::BudgetLimited);
      const bool pause = std::holds_alternative<PauseGoal>(change->change) && view.state != GoalState::Proposed;
      const bool cancel = std::holds_alternative<CancelGoal>(change->change);
      if (!enable && !resume && !pause && !cancel) {
        co_return Fail<Receipt<GoalUpdated>>(ErrorCode::InvalidState, "The goal transition is unavailable");
      }
      ClearGoalVerification(session, *goal);
      if (enable) {
        goal->enabled_at = Now();
        goal->owner = principal;
      }
      view.state = cancel ? GoalState::Cancelled : pause ? GoalState::Paused : GoalState::Active;
      if (cancel) {
        project = ReleaseGoalClaims(session, view.ref);
        affected = WithdrawGoalWork(view.ref);
      }
      view.reason.reset();
    }
    ++view.revision;
  }
  auto& view = goal->view;
  if (!evidence && view.state != GoalState::Proposed && view.state != GoalState::Paused && !Closed(view.state) && Exhausted(*goal, Now())) {
    ClearGoalVerification(session, *goal);
    view.state = GoalState::BudgetLimited;
    view.reason = "Goal budget exhausted";
    if (session.summary.active_run) {
      auto& run = runs.at(*session.summary.active_run);
      if (run.view.goal == view.ref && run.view.state != RunState::Cancelling) Stop(run, RunEndReason::Budget);
    }
  }
  ++session.sequence;
  const auto result = GoalUpdated{view.ref, view.revision};
  if (evidence && goal->verification) {
    if (approved) {
      auto& record = commands.at({principal, command});
      record.original_approval = record.progress.approval;
      record.progress.approval.reset();
      record.plan.reset();
      record.progress.result = result;
    } else {
      Accept(principal, command, key, session, result, CommandState::Scheduled);
    }
    const auto receipt = ReceiptFor<GoalUpdated>(principal, command);
    auto saved = co_await WhenSaved(Persist(session, false, approved ? CommitId{} : receipt.Value().stamp.commit));
    if (!saved.Succeeded()) co_return GoalReceipt::Failure(saved.Error());
    if (goal->verification && goal->verification->revision == result.revision) ScheduleGoalVerification(id, *goal);
    co_return receipt;
  }
  auto receipt = co_await FinishOperation(session, principal, command, key, result, false, approved.has_value(),
      project, std::move(affected));
  if (receipt.Succeeded() && view.state == GoalState::Active) {
    for (const auto& [target_id, target] : sessions) {
      if (target.summary.lifecycle != SessionLifecycle::Open || target.summary.active_run) continue;
      if (target_id != id && std::ranges::none_of(target.collaboration, [&](const auto& item) {
            const auto& delivery = item.second;
            return delivery.goal == view.ref && delivery.auto_start && delivery.record.summary.delivery == DeliveryState::Queued;
          })) continue;
      options.tasks.Launch([self = shared_from_this(), target_id] { return self->ConsumeQueue(target_id); });
    }
  }
  co_return receipt;
}

void RuntimeState::ClearGoalVerification(SessionData& session, StoredGoal& goal) {
  if (!goal.verification) return;
  goal.verification_stop.request_stop();
  const auto pending = goal.view.completion_review;
  const auto key = std::pair(goal.verification->actor.principal, goal.verification->command);
  goal.verification.reset();
  goal.view.completion_review.reset();
  if (pending) {
    auto interaction = std::ranges::find(session.interactions, pending->approval.interaction, &InteractionSnapshot::id);
    if (interaction != session.interactions.end()) CloseInteraction(session, *interaction, InteractionState::Invalidated);
  }
  auto command = commands.find(key);
  if (command != commands.end() && (command->second.progress.state == CommandState::Scheduled ||
      command->second.progress.state == CommandState::AwaitingApproval)) {
    Resolve(command->second, session, CommandState::Withdrawn, Error(ErrorCode::Conflict, "The goal verification changed"));
    command->second.progress.resolution->commit = {};
  }
}

void RuntimeState::UpdateGoalReview(SessionData& session, const InteractionSnapshot& interaction, InteractionState decision) {
  if (!session.current_goal || decision == InteractionState::Approved) return;
  auto& goal = session.goals.at(*session.current_goal);
  if (!goal.view.completion_review || goal.view.completion_review->approval.interaction != interaction.id) return;
  goal.verification.reset();
  goal.view.completion_review.reset();
  const bool exhausted = Exhausted(goal, Now());
  goal.view.state = exhausted ? GoalState::BudgetLimited : GoalState::Blocked;
  goal.view.reason = exhausted ? "Goal budget exhausted" :
      decision == InteractionState::Denied ? "Goal completion was denied" : "Goal completion approval closed";
  ++goal.view.revision;
}

void RuntimeState::ScheduleGoalVerification(SessionId id, const StoredGoal& goal) {
  if (stage != Stage::Started || goal.view.state != GoalState::Verifying || !goal.verification) return;
  options.tasks.Launch([self = shared_from_this(), id, ref = goal.view.ref.id, version = goal.verification->revision] {
    return self->VerifyGoal(id, ref, version);
  });
}

Task<void> RuntimeState::VerifyGoal(SessionId id, GoalId goal_id, Revision version, bool approved) {
  auto result = co_await CheckGoalCompletion(std::move(id), std::move(goal_id), version, approved);
  if (!result.Succeeded()) co_return;
}

Async<Receipt<InteractionConsumed>> RuntimeState::ApproveGoal(SessionId id, GoalId goal_id, PrincipalId user,
    CommandId command, InteractionReply reply) {
  const auto replay = [&]() -> Async<Receipt<InteractionConsumed>> {
    CommandInput key{.operation = Operation::Submit, .session = id, .reply = reply};
    auto found = Find(user, command, key);
    if (!found.Succeeded()) co_return AgentResult<Receipt<InteractionConsumed>>::Failure(found.Error());
    if (found.Value()) co_return co_await ReplayReceipt(ReceiptFor<InteractionConsumed>(user, command));
    co_return Fail<Receipt<InteractionConsumed>>(ErrorCode::AlreadyClosed, "Goal approval is no longer active");
  };
  const auto& goal = sessions.at(id).goals.at(goal_id);
  if (!goal.verification || goal.verification->actor.principal != user) {
    co_return co_await replay();
  }
  auto result = co_await CheckGoalCompletion(id, std::move(goal_id), goal.verification->revision, true, command, reply);
  if (!result.Succeeded()) co_return AgentResult<Receipt<InteractionConsumed>>::Failure(result.Error());
  if (!result.Value()) co_return co_await replay();
  co_return AgentResult<Receipt<InteractionConsumed>>::Success(std::move(*result.Value()));
}

Async<std::optional<Receipt<InteractionConsumed>>> RuntimeState::CheckGoalCompletion(SessionId id, GoalId goal_id,
    Revision version, bool approved, std::optional<CommandId> decision, std::optional<InteractionReply> reply) {
  using Result = AgentResult<std::optional<Receipt<InteractionConsumed>>>;
  auto& session = sessions.at(id);
  auto& goal = session.goals.at(goal_id);
  const auto expected_state = approved ? GoalState::AwaitingCompletionApproval : GoalState::Verifying;
  auto current = [&] {
    return stage == Stage::Started && session.summary.lifecycle == SessionLifecycle::Open &&
        goal.verification && goal.verification->revision == version && goal.view.state == expected_state;
  };
  if (!current()) co_return Result::Success(std::nullopt);
  const auto job = *goal.verification;
  const auto snapshot = goal.view;
  const auto key = std::pair(job.actor.principal, job.command);
  auto& command = commands.at(key);
  const auto project_id = session.summary.project;
  auto dependencies = [&]() -> AgentResult<std::map<ProjectTaskId, Revision>> {
    std::map<ProjectTaskId, Revision> revisions;
    if (snapshot.tasks.empty()) return AgentResult<decltype(revisions)>::Success({});
    const auto project = project_id ? projects.find(*project_id) : projects.end();
    for (const auto& task : snapshot.tasks) {
      if (project == projects.end() || !project->second.tasks.contains(task)) {
        return Fail<decltype(revisions)>(ErrorCode::Conflict, "A goal task is unavailable");
      }
      const auto& entry = project->second.tasks.at(task).snapshot;
      if (entry.state != ProjectTaskState::Succeeded || entry.spec.goal != goal_id) {
        return Fail<decltype(revisions)>(ErrorCode::InvalidState, "Goal dependencies have not completed");
      }
      revisions.emplace(task, entry.revision);
    }
    return AgentResult<decltype(revisions)>::Success(std::move(revisions));
  };
  const auto accepted_dependencies = dependencies();
  auto dependency_sources = snapshot.evidence->sources;
  if (accepted_dependencies.Succeeded() && project_id) {
    for (const auto& [task, unused] : accepted_dependencies.Value()) {
      const auto& outcome = projects.at(*project_id).tasks.at(task).snapshot.outcome;
      if (!outcome) continue;
      for (const auto& artifact : outcome->artifacts) dependency_sources.push_back({artifact});
    }
  }
  auto validated = accepted_dependencies.Succeeded()
      ? co_await ValidateInputSources(session, job.actor, dependency_sources)
      : AgentResult<void>::Failure(accepted_dependencies.Error());
  if (!current()) co_return Result::Success(std::nullopt);
  auto checked = Fail<VerificationResult>(ErrorCode::Unsupported, "A goal verifier is required");
  if (!validated.Succeeded()) {
    checked = AgentResult<VerificationResult>::Failure(validated.Error());
  } else if (approved) {
    if (!snapshot.verification || !snapshot.completion_review || !command.progress.approval ||
        snapshot.completion_review->approval.interaction != command.progress.approval->interaction ||
        snapshot.completion_review->definition != snapshot.ref || accepted_dependencies.Value() != job.dependencies) {
      checked = Fail<VerificationResult>(ErrorCode::Conflict, "The reviewed evidence or dependencies changed");
    } else {
      checked = AgentResult<VerificationResult>::Success(*snapshot.verification);
    }
  } else if (options.verify) {
    try {
      checked = co_await options.verify({project_id, id, snapshot.ref, version, snapshot.spec.definition.completion_conditions,
          *snapshot.evidence, goal.verification_stop.get_token()});
    } catch (...) {
      ReportException(std::current_exception());
      checked = Fail<VerificationResult>(ErrorCode::InvalidState, "Goal verifier failed");
    }
  }
  if (!current()) co_return Result::Success(std::nullopt);

  const auto original = GoalParty(*this, job.actor.principal, id, job.actor.run, Capability::CompleteGoal, true);
  std::optional<AgentError> failure;
  bool review = false;
  if (!checked.Succeeded()) failure = checked.Error();
  else if (checked.Value().status != VerificationStatus::Passed) {
    failure = Error(ErrorCode::InvalidState, checked.Value().reason.empty() ? "Goal evidence did not pass verification" : checked.Value().reason);
  } else if (!original.Succeeded()) {
    failure = original.Error();
  } else {
    dependency_sources.insert(dependency_sources.end(), checked.Value().verified.begin(), checked.Value().verified.end());
    auto policy = GoalPolicy(*this, session, original.Value(), Capability::CompleteGoal, goal_id);
    policy.sources = dependency_sources;
    auto decision = co_await Decide(std::move(policy));
    if (!current()) co_return Result::Success(std::nullopt);
    if (!decision.Succeeded()) failure = decision.Error();
    else if (decision.Value().decision == AccessDecision::Deny || decision.Value().decision == AccessDecision::AllowReview) {
      failure = Error(ErrorCode::PermissionDenied, decision.Value().reason);
    } else {
      review = !approved && (snapshot.spec.definition.completion == GoalCompletionPolicy::RequireConfirmation ||
          decision.Value().decision == AccessDecision::RequireConfirmation);
      auto sources = co_await ValidateInputSources(session, job.actor, dependency_sources);
      if (!sources.Succeeded()) failure = sources.Error();
    }
  }
  if (review && !failure) {
    auto access = co_await WithSession(id, job.actor.principal, Capability::SubmitInteraction);
    if (!access.Succeeded()) failure = access.Error();
  }
  if (decision && reply) {
    auto permit = co_await WithSession(id, job.actor.principal, Capability::SubmitInteraction);
    if (!permit.Succeeded()) co_return Result::Failure(permit.Error());
  }
  const CommandInput submitted{.operation = Operation::Submit, .session = id, .reply = reply};
  if (decision) {
    auto replay = Find(job.actor.principal, *decision, submitted);
    if (!replay.Succeeded()) co_return Result::Failure(replay.Error());
    if (replay.Value()) {
      auto receipt = co_await ReplayReceipt(ReceiptFor<InteractionConsumed>(job.actor.principal, *decision));
      if (!receipt.Succeeded()) co_return Result::Failure(receipt.Error());
      co_return Result::Success(std::move(receipt).Value());
    }
  }
  if (!current()) co_return Result::Success(std::nullopt);
  const auto latest_dependencies = dependencies();
  if (!failure && (!latest_dependencies.Succeeded() || latest_dependencies.Value() != accepted_dependencies.Value())) {
    failure = Error(ErrorCode::Conflict, "Goal dependencies changed during verification");
  }
  const bool exhausted = Exhausted(goal, Now(), GoalUsage(*this, goal));
  if (exhausted) failure = Error(ErrorCode::BudgetExceeded, "Goal budget exhausted");

  std::optional<InteractionConsumed> consumed;
  if (reply) {
    auto interaction = std::ranges::find(session.interactions, reply->id, &InteractionSnapshot::id);
    if (interaction == session.interactions.end() || interaction->state != InteractionState::Pending ||
        interaction->recipient != job.actor.principal) co_return Result::Success(std::nullopt);
    if (interaction->version != reply->expected_version ||
        goal.view.completion_review->approval.interaction != reply->id ||
        command.progress.state != CommandState::AwaitingApproval) {
      co_return Fail<std::optional<Receipt<InteractionConsumed>>>(ErrorCode::Conflict, "The goal review changed");
    }
    if (interaction->expires_at && Now() >= *interaction->expires_at) {
      CloseInteraction(session, *interaction, InteractionState::Expired);
      auto saved = co_await WhenSaved(Persist(session));
      if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
      co_return Fail<std::optional<Receipt<InteractionConsumed>>>(ErrorCode::DeadlineExceeded, "Goal review expired");
    }
    if (failure) goal.view.completion_review.reset();
    CloseInteraction(session, *interaction, failure ? InteractionState::Invalidated : InteractionState::Approved);
    if (!failure) consumed = InteractionConsumed{reply->id, interaction->version};
  }

  auto& view = goal.view;
  ++view.revision;
  ++session.sequence;
  if (checked.Succeeded()) view.verification = checked.Value();
  if (failure) {
    view.state = exhausted ? GoalState::BudgetLimited : GoalState::Blocked;
    view.reason = failure->message;
    view.completion_review.reset();
    goal.verification.reset();
    Resolve(command, session, CommandState::Failed, *failure);
  } else if (review) {
    const auto deadline = std::min(GoalDeadline(goal).value_or(TimePoint::max()), Now() + Duration(300000));
    PendingApproval pending{id, InteractionId{MakeId()}, 1};
    ApprovalPreview preview{.title = "Complete goal", .summary = view.spec.definition.objective};
    for (const auto& source : dependency_sources) {
      if (const auto* artifact = std::get_if<ArtifactRef>(&source.source)) {
        if (std::ranges::find(preview.artifacts, *artifact) == preview.artifacts.end()) preview.artifacts.push_back(*artifact);
      }
    }
    session.interactions.push_back({.id = pending.interaction, .version = pending.version, .session = id,
        .recipient = job.actor.principal, .state = InteractionState::Pending,
        .body = OperationApproval{job.command, std::move(preview)}, .expires_at = deadline});
    ++session.summary.pending_interactions;
    view.state = GoalState::AwaitingCompletionApproval;
    view.completion_review = GoalCompletionReview{pending, view.ref, view.revision};
    goal.verification->dependencies = latest_dependencies.Value();
    command.progress.state = CommandState::AwaitingApproval;
    command.progress.approval = pending;
    command.plan = OperationPlan{.confirmation = {Capability::CompleteGoal}};
    auto saved = co_await WhenSaved(Persist(session));
    if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
    if (stage == Stage::Started) ScheduleOperationExpiry(id, pending.interaction, deadline);
    co_return Result::Success(std::nullopt);
  } else {
    view.state = GoalState::Completed;
    view.reason.reset();
    view.completion_review.reset();
    goal.verification.reset();
    Resolve(command, session, CommandState::Applied);
  }
  if (consumed) {
    Accept(job.actor.principal, *decision, submitted, session, *consumed);
    command.progress.resolution->commit = {};
    auto receipt = co_await SaveReceipt<InteractionConsumed>(session, job.actor.principal, *decision);
    if (!receipt.Succeeded()) co_return Result::Failure(receipt.Error());
    co_return Result::Success(std::move(receipt).Value());
  }
  auto saved = co_await WhenSaved(Persist(session, false, command.progress.resolution->commit));
  if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
  if (decision && failure) co_return Result::Failure(std::move(*failure));
  co_return Result::Success(std::nullopt);
}

Task<void> RuntimeState::StartGoalRun(SessionId id) {
  auto& session = sessions.at(id);
  if (!session.current_goal) co_return;
  auto& goal = session.goals.at(*session.current_goal);
  auto& view = goal.view;
  const auto version = view.revision;
  const auto session_binding = session.summary.binding_revision;
  auto ready = [&] {
    return stage == Stage::Started && !(storage_error && session.retention == SessionRetention::Stored) &&
        session.summary.lifecycle == SessionLifecycle::Open && !session.summary.active_run &&
        session.current_goal == view.ref.id && view.state == GoalState::Active && view.revision == version;
  };
  if (!ready()) co_return;
  auto allowed = co_await WithSession(id, goal.owner, Capability::SubmitInput);
  if (!ready()) co_return;
  if (allowed.Succeeded()) {
    auto policy = Request(goal.owner, Capability::ManageGoal, &session);
    policy.goal = view.ref;
    policy.target.goal = view.ref.id;
    allowed = co_await Require(std::move(policy));
  }
  if (!ready()) co_return;
  auto binding = allowed.Succeeded() ? co_await ResolveStart(session, goal.owner, {})
                                     : AgentResult<RunStartBinding>::Failure(allowed.Error());
  if (!ready() || session_binding != session.summary.binding_revision || std::ranges::any_of(session.queue, [&](const auto& item) {
        if (item.state != QueueItemState::Queued) return false;
        const auto* message = std::get_if<CollaborationMessageId>(&item.input);
        if (!message) return true;
        const auto& entry = session.collaboration.at(*message);
        return entry.auto_start && CheckGoalDispatch(session, entry.goal).Succeeded();
      })) co_return;
  const bool exhausted = Exhausted(goal, Now());
  if (exhausted || !binding.Succeeded()) {
    view.state = exhausted ? GoalState::BudgetLimited : GoalState::Blocked;
    view.reason = exhausted ? "Goal budget exhausted" : binding.Error().message;
    ++view.revision;
    ++session.sequence;
    auto saved = co_await WhenSaved(Persist(session));
    if (!saved.Succeeded()) co_return;
    co_return;
  }
  auto& run = RecordRun(session, goal.owner, SendInput{.text = "Continue the active goal."}, binding.Value(), RunTrigger::Goal);
  AttachGoalRun(run, view.ref);
  ++view.revision;
  ++session.sequence;
  auto saved = co_await WhenSaved(Persist(session, true));
  if (saved.Succeeded() && stage == Stage::Started && !run.stop.stop_requested()) Launch(run);
}

const SessionData* RuntimeState::GoalHome(const GoalId& goal) const {
  auto home = std::ranges::find_if(sessions, [&](const auto& entry) { return entry.second.goals.contains(goal); });
  return home == sessions.end() ? nullptr : &home->second;
}

std::optional<GoalRef> RuntimeState::ActorGoal(const ActionParty& actor) const {
  auto run = actor.run ? runs.find(*actor.run) : runs.end();
  return run == runs.end() ? std::nullopt : run->second.view.goal;
}

AgentResult<void> RuntimeState::CheckGoalDispatch(const SessionData& target, const std::optional<GoalRef>& ref) const {
  if (!ref) return AgentResult<void>::Success();
  const auto* home = GoalHome(ref->id);
  if (!home || home->summary.lifecycle != SessionLifecycle::Open) {
    return Fail<void>(ErrorCode::NotFound, "The goal is unavailable");
  }
  if (target.retention == SessionRetention::Stored && home->retention != SessionRetention::Stored) {
    return Fail<void>(ErrorCode::InvalidArgument, "A stored request requires a stored goal definition");
  }
  const auto& goal = home->goals.at(ref->id);
  if (goal.view.ref != ref || home->current_goal != ref->id) {
    return Fail<void>(ErrorCode::Conflict, "The goal definition changed");
  }
  if (Exhausted(goal, Now(), GoalUsage(*this, goal))) return Fail<void>(ErrorCode::BudgetExceeded, "Goal budget exhausted");
  if (goal.view.state != GoalState::Active && goal.view.state != GoalState::Blocked) {
    return Fail<void>(ErrorCode::InvalidState, "The goal is not dispatching work");
  }
  return AgentResult<void>::Success();
}

Async<void> RuntimeState::AuthorizeGoalDispatch(const SessionData& target, ActionParty actor, std::optional<GoalRef> goal) {
  auto valid = CheckGoalDispatch(target, goal);
  if (!valid.Succeeded() || !goal) co_return valid;
  const auto* home = GoalHome(goal->id);
  auto policy = GoalPolicy(*this, *home, actor, Capability::ReadGoal, goal->id);
  policy.goal = goal;
  auto access = co_await Require(std::move(policy));
  if (!access.Succeeded()) co_return access;
  co_return CheckGoalDispatch(target, goal);
}

SessionData* RuntimeState::AttachGoalRun(RunExecution& run, std::optional<GoalRef> ref) {
  if (!ref) return nullptr;
  auto& owner = sessions.at(GoalHome(ref->id)->summary.id);
  auto& goal = owner.goals.at(ref->id);
  run.view.goal = ref;
  goal.view.runs.push_back(run.view.id);
  if (auto deadline = GoalDeadline(goal); deadline && (!run.deadline || *deadline < *run.deadline)) run.deadline = deadline;
  ++owner.sequence;
  return &owner;
}

SessionData* RuntimeState::FinishGoalRun(const RunSnapshot& run) {
  if (!run.goal) return nullptr;
  const auto* home = GoalHome(run.goal->id);
  if (!home) return nullptr;
  auto& session = sessions.at(home->summary.id);
  auto& goal = session.goals.at(run.goal->id);
  auto& view = goal.view;
  AddUsage(view.usage, run.usage);
  ++session.sequence;
  if (Closed(view.state) || view.state == GoalState::Proposed || view.state == GoalState::Paused) return &session;
  if (Exhausted(goal, Now(), GoalUsage(*this, goal))) {
    ClearGoalVerification(session, goal);
    view.state = GoalState::BudgetLimited;
    view.reason = "Goal budget exhausted";
    ++view.revision;
  } else if (!run.parent && run.goal == view.ref && view.state == GoalState::Active && run.session == session.summary.id) {
    view.state = GoalState::Blocked;
    view.reason = run.failure ? run.failure->message : "Goal is waiting for evidence or additional input";
    ++view.revision;
  }
  return &session;
}

Budget RuntimeState::RunBudget(const RunExecution& run) const {
  auto budget = agents.at(run.view.agent.id).budget;
  auto restrict = [](auto& cap, auto allocated, std::uint64_t spent) {
    if (!allocated) return;
    const auto remaining = *allocated - std::min(*allocated, spent);
    cap = std::min(cap.value_or(remaining), remaining);
  };
  const auto& branch = sessions.at(run.view.session).fork_branch;
  if (branch && branch->run == run.view.id) {
    restrict(budget.tokens, branch->budget.tokens, 0);
    restrict(budget.tool_calls, branch->budget.tool_calls, 0);
    restrict(budget.model_turns, branch->budget.model_turns, 0);
    restrict(budget.context_bytes, branch->budget.context_bytes, 0);
  }
  ConstrainDelegationBudget(run, budget);
  if (!run.view.goal) return budget;
  const auto* home = GoalHome(run.view.goal->id);
  if (!home) return budget;
  const auto& goal = home->goals.at(run.view.goal->id);
  const auto& cumulative = goal.view.spec.budget;
  const auto used = GoalUsage(*this, goal, &run.view.id);
  const auto tokens = Add(used.input_tokens, used.output_tokens);
  restrict(budget.tokens, cumulative.tokens, tokens);
  restrict(budget.tool_calls, cumulative.tool_calls, used.tool_calls);
  restrict(budget.model_turns, cumulative.model_turns, used.model_turns);
  restrict(budget.context_bytes, cumulative.context_bytes, 0);
  return budget;
}

Async<std::optional<ModelContent>> RuntimeState::GoalContext(const RunExecution& run) {
  using Result = AgentResult<std::optional<ModelContent>>;
  if (!run.view.goal || run.delegate) co_return Result::Success(std::nullopt);
  const auto& session = sessions.at(run.view.session);
  const auto& ref = *run.view.goal;
  auto definition = co_await ReadGoalDefinition(session.summary.id, run.principal, run.view.id, ref);
  if (!definition.Succeeded()) co_return Result::Failure(definition.Error());
  auto policy = Request(run.principal, Capability::SendToModel, &session, &run);
  policy.goal = ref;
  policy.target.goal = ref.id;
  policy.recipient_model = run.view.model.id;
  auto allowed = co_await Require(std::move(policy));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto content = ToWireJson(definition.Value().completion_conditions).dump();
  co_return Result::Success(ModelContent{ModelContent::Role::Context,
      "Goal (" + ref.id.value + ", revision " + std::to_string(ref.version) + "): " +
      definition.Value().objective + "\nCompletion conditions: " + std::move(content)});
}

} // namespace detail

SessionGoals::SessionGoals(std::shared_ptr<detail::RuntimeState> runtime, SessionId session, PrincipalId principal,
    std::optional<RunId> actor)
    : runtime_(std::move(runtime)), session_(std::move(session)), principal_(std::move(principal)), actor_(std::move(actor)) {}

SessionGoals Conversation::Goals() { return SessionGoals(state_, id_, principal_); }

SessionGoals RunContext::Goals() {
  const auto& view = runtime_->runs.at(run_);
  return SessionGoals(runtime_, view.view.session, view.principal, run_);
}

Async<std::optional<GoalSnapshot>> SessionGoals::ReadAsync() {
  return [](SessionGoals service) -> Async<std::optional<GoalSnapshot>> {
    co_return co_await service.runtime_->ReadGoal(service.session_, service.principal_, service.actor_);
  }(*this);
}

Async<GoalDefinition> SessionGoals::ReadDefinitionAsync(GoalRef goal) {
  return [](SessionGoals service, GoalRef ref) -> Async<GoalDefinition> {
    co_return co_await service.runtime_->ReadGoalDefinition(service.session_, service.principal_, service.actor_, std::move(ref));
  }(*this, std::move(goal));
}

Async<GoalEditPreview> SessionGoals::PreviewEditAsync(GoalId goal, Revision expected, GoalDefinition definition) {
  return [](SessionGoals service, GoalId id, Revision revision, GoalDefinition draft) -> Async<GoalEditPreview> {
    co_return co_await service.runtime_->PreviewGoalEdit(service.session_, service.principal_, service.actor_, std::move(id), revision, std::move(draft));
  }(*this, std::move(goal), expected, std::move(definition));
}

Async<Subscription> SessionGoals::ObserveAsync(Observer<std::optional<GoalSnapshot>> observer) {
  return detail::RuntimeState::Observe(runtime_, std::move(observer),
      [service = *this](detail::RuntimeState&) mutable { return service.ReadAsync(); });
}

Async<Receipt<GoalUpdated>> SessionGoals::UpdateAsync(CommandId command, GoalCommand request) {
  return [](SessionGoals service, CommandId id, GoalCommand input) -> Async<Receipt<GoalUpdated>> {
    co_return co_await service.runtime_->UpdateGoal(service.session_, service.principal_, service.actor_, std::move(id), std::move(input));
  }(*this, std::move(command), std::move(request));
}

} // namespace huxerui::agentkit
