#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

template<class T>
std::optional<AgentResult<Receipt<T>>> Replay(RuntimeState& state, const ActionParty& actor,
                                             const CommandId& command, const CommandInput& input) {
  auto entry = state.Find(actor.principal, command, input);
  if (!entry.Succeeded()) return AgentResult<Receipt<T>>::Failure(entry.Error());
  if (entry.Value()) return state.ReceiptFor<T>(actor.principal, command);
  return {};
}

const SendInput& Payload(const SessionMessageRequest& request) {
  return std::visit([](const auto& body) -> const SendInput& { return body.input; }, request.body);
}

const WorkRequest* Work(const CollaborationRecord& record) {
  return record.request ? std::get_if<WorkRequest>(&record.request->body) : nullptr;
}

std::optional<std::pair<TimePoint, ReplyEnd>> Expiry(const CollaborationRecord& record) {
  if (record.outcome || !record.request) return {};
  auto deadline = record.summary.delivery == DeliveryState::Queued ? record.request->deliver_by : std::nullopt;
  if (const auto* work = Work(record); work && work->reply && work->reply->reply_by) {
    if (!deadline || *work->reply->reply_by < *deadline) deadline = work->reply->reply_by;
  }
  return deadline ? std::optional(std::pair{*deadline, ReplyEnd::Expired}) : std::nullopt;
}

MessageAccepted ReceiptValue(const CollaborationSummary& value) {
  return {value.id, value.delivery, value.target_run, {}};
}

std::optional<WorkScope> LimitScope(const std::optional<WorkScope>& requested, std::optional<WorkScope> allowed) {
  if (!requested) return allowed;
  if (!allowed) return requested;
  auto permit = [](auto& items, const auto& other) {
    std::erase_if(items, [&](const auto& item) { return std::ranges::find(other, item) == other.end(); });
  };
  permit(allowed->inputs, requested->inputs);
  permit(allowed->writable_resources, requested->writable_resources);
  permit(allowed->tools, requested->tools);
  return allowed;
}

void RemoveQueue(SessionData& session, CollaborationMessageId id, QueueItemState state,
                 std::optional<RunId> run = {}, std::optional<AgentError> reason = {}) {
  auto entry = std::ranges::find_if(session.queue, [&](const auto& item) {
    const auto* message = std::get_if<CollaborationMessageId>(&item.input);
    return message && *message == id && item.state == QueueItemState::Queued;
  });
  if (entry == session.queue.end()) return;
  entry->state = state;
  entry->run = std::move(run);
  entry->reason = std::move(reason);
  --session.summary.queued_inputs;
}

void EndRequest(SessionData& session, StoredCollaboration& entry, ReplyEnd end, RequestState state) {
  auto& record = entry.record;
  record.summary.request = state;
  record.outcome = ReplyOutcome{record.summary.id, end, {}, record.summary.target_run};
  ++session.sequence;
}

} // namespace

AgentResult<ActionParty> RuntimeState::Actor(const std::optional<RunId>& id, Capability capability) const {
  if (stage != Stage::Started) return Fail<ActionParty>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  auto principal = User();
  if (!principal.Succeeded()) return AgentResult<ActionParty>::Failure(principal.Error());
  if (!id) return AgentResult<ActionParty>::Success({.kind = ActionParty::Kind::User, .principal = principal.Value()});
  auto run = runs.find(*id);
  if (run == runs.end() || Terminal(run->second.view.state) || run->second.stop.stop_requested()) {
    return Fail<ActionParty>(ErrorCode::AlreadyClosed, "The acting run has ended");
  }
  if (run->second.principal != principal.Value()) return Fail<ActionParty>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (!AllowsAgentOperation(run->second, capability)) {
    return Fail<ActionParty>(ErrorCode::PermissionDenied, "The agent has not been granted this collaboration operation");
  }
  return AgentResult<ActionParty>::Success(Request(principal.Value(), capability,
      &sessions.at(run->second.view.session), &run->second).actor);
}

PolicyRequest RuntimeState::CollaborationAccess(const ActionParty& actor, Capability capability,
                                                const SessionData& target) const {
  auto request = Request(actor.principal, capability, &target);
  request.actor = actor;
  return request;
}

Async<ReceiveDecision> RuntimeState::Receive(ActionParty sender, const SessionData& session, CollaborationMessageId id,
    std::variant<SessionMessageRequest, SessionReply> payload, std::optional<GoalRef> goal) {
  if (!options.receive) co_return AgentResult<ReceiveDecision>::Success({ReceiveAction::Allow, true});
  co_return co_await options.receive({std::move(sender), session.summary, std::move(id), std::move(payload), goal});
}

AgentResult<void> RuntimeState::CheckWork(const WorkRequest& work) const {
  if (work.scope && std::ranges::any_of(work.input.context, [&](const auto& slice) {
        return std::ranges::find(work.scope->inputs, slice) == work.scope->inputs.end();
      })) return Fail<void>(ErrorCode::PermissionDenied, "The work input exceeds its declared source scope");
  return CheckInput(work.input);
}

void RuntimeState::EnqueueMessage(SessionData& target, CollaborationRecord record, ReceiveDecision decision,
    std::optional<GoalRef> goal) {
  const auto* work = Work(record);
  const auto id = record.summary.id;
  if (decision.action == ReceiveAction::Deny) {
    record.summary.delivery = DeliveryState::Rejected;
    record.reason = Error(ErrorCode::PermissionDenied, std::move(decision.reason));
    if (work) {
      record.summary.request = RequestState::Failed;
      record.outcome = ReplyOutcome{id, ReplyEnd::Rejected};
    }
  } else if (work) {
    record.summary.request = RequestState::AwaitingReply;
    record.summary.delivery = DeliveryState::Queued;
    const QueueItemId item{MakeId()};
    target.queue.push_back({.id = item, .input = id, .state = QueueItemState::Queued});
    target.queue_owners.emplace(item, record.summary.from.principal);
    ++target.summary.queued_inputs;
  } else {
    record.summary.delivery = decision.action == ReceiveAction::Allow ? DeliveryState::Delivered : DeliveryState::AwaitingApproval;
  }
  auto scope = work ? LimitScope(work->scope, std::move(decision.allowed_work_scope)) : std::nullopt;
  const auto automatic = work && work->request_start && decision.allow_auto_start && decision.action == ReceiveAction::Allow;
  target.collaboration.emplace(id, StoredCollaboration{std::move(record), automatic, std::move(scope), {}, std::move(goal)});
  ++target.sequence;
}

const WorkScope* RuntimeState::Scope(const RunExecution& run) const {
  if (run.view.parent) return Scope(runs.at(*run.view.parent));
  if (!run.view.work_request) return nullptr;
  const auto& records = sessions.at(run.view.session).collaboration;
  auto entry = records.find(*run.view.work_request);
  return entry != records.end() && entry->second.scope ? &*entry->second.scope : nullptr;
}

RunExecution& RuntimeState::RecordWork(SessionData& target, CollaborationMessageId id, PrincipalId user, RunStartBinding binding,
                                       std::vector<SourceSlice> task_inputs) {
  auto& record = target.collaboration.at(id).record;
  auto input = Work(record)->input;
  if (Work(record)->task) input.context = std::move(task_inputs);
  auto& run = RecordRun(target, std::move(user), std::move(input), std::move(binding), RunTrigger::Collaboration);
  run.view.work_request = id;
  record.summary.delivery = DeliveryState::Delivered;
  record.summary.target_run = run.view.id;
  RemoveQueue(target, id, QueueItemState::Started, run.view.id);
  return run;
}

std::optional<std::pair<SessionId, CollaborationRecord>> RuntimeState::FindMessage(CollaborationMessageId id) const {
  for (const auto& [session_id, session] : sessions) {
    const auto* image = Published(session);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting) continue;
    const auto record = image->collaboration.find(id);
    if (record != image->collaboration.end()) return std::pair{session_id, record->second.record};
  }
  return {};
}

Async<CollaborationRecord> RuntimeState::ReadMessage(CollaborationMessageId id, std::optional<RunId> bound,
                                                    Capability capability, std::vector<Capability>* confirmation,
                                                    std::optional<OperationPlan> approved) {
  using Result = AgentResult<CollaborationRecord>;
  auto actor = Actor(bound, capability);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto found = FindMessage(id);
  if (!found || found->second.summary.from.principal != actor.Value().principal) {
    co_return Fail<CollaborationRecord>(ErrorCode::NotFound, "Message unavailable");
  }
  if (bound && found->second.summary.from.run != bound && found->second.summary.target_run != bound) {
    co_return Fail<CollaborationRecord>(ErrorCode::NotFound, "Message is outside the acting run");
  }
  auto& target = sessions.at(found->first);
  auto permitted = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, target));
  if (!permitted.Succeeded()) co_return Result::Failure(permitted.Error());
  auto access = CollaborationAccess(actor.Value(), capability, target);
  access.target.message = id;
  if (confirmation) {
    permitted = co_await CheckOperation(std::move(access), *confirmation, approved);
  } else {
    permitted = co_await Require(std::move(access));
  }
  if (!permitted.Succeeded()) co_return Result::Failure(permitted.Error());
  actor = Actor(bound, capability);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  found = FindMessage(id);
  if (!found) co_return Fail<CollaborationRecord>(ErrorCode::NotFound, "Message unavailable");
  auto& answer = found->second;
  if (auto expiry = Expiry(answer); expiry && Now() >= expiry->first && Work(answer)) {
    auto& latest = target.collaboration.at(id);
    auto current_expiry = Expiry(latest.record);
    if (current_expiry && Now() >= current_expiry->first) {
      if (latest.record.summary.delivery == DeliveryState::Queued) {
        latest.record.summary.delivery = DeliveryState::Expired;
        RemoveQueue(target, id, QueueItemState::Rejected, {}, Error(ErrorCode::DeadlineExceeded, "Message expired"));
      }
      EndRequest(target, latest, ReplyEnd::Expired, RequestState::Failed);
      auto saved = co_await WhenSaved(Persist(target));
      if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
    }
    found = FindMessage(id);
    if (!found) co_return Fail<CollaborationRecord>(ErrorCode::NotFound, "Message unavailable");
  }
  co_return Result::Success(std::move(found->second));
}

Async<Receipt<MessageAccepted>> RuntimeState::SendMessage(CommandId command, SessionMessageRequest request,
                                                          std::optional<RunId> bound, std::optional<OperationPlan> approved) {
  using Result = AgentResult<Receipt<MessageAccepted>>;
  auto actor = Actor(bound, Capability::SendMessage);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto target = sessions.find(request.to);
  if (target == sessions.end() || !Published(target->second)) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::NotFound, "Target session unavailable");
  }
  auto& session = target->second;
  if (session.origin.kind == SessionOriginKind::SideChat)
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::NotFound, "Target session unavailable");
  const auto principal = actor.Value().principal;
  const auto agent_revision = session.summary.binding_revision;
  if (approved && approved->target_binding != session.summary.binding_revision) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The approved target session changed");
  }
  auto readable = co_await Require(CollaborationAccess(actor.Value(), Capability::DiscoverSession, session));
  if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
  if (bound) {
    const auto& agent = agents.at(actor.Value().agent->id);
    if (std::ranges::find(agent.collaboration_targets, session.summary.agent.id) == agent.collaboration_targets.end()) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "Target agent is outside the declared collaboration scope");
    }
  }
  CommandInput key{.operation = Operation::SendMessage, .session = request.to, .run = bound.value_or(RunId{}), .delivery = request};
  if (!approved) {
    if (auto replay = Replay<MessageAccepted>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*replay));
  }
  std::vector<Capability> confirmation;
  auto permitted = co_await CheckOperation(CollaborationAccess(actor.Value(), Capability::SendMessage, session), confirmation, approved);
  if (!permitted.Succeeded()) co_return Result::Failure(permitted.Error());
  auto valid = std::holds_alternative<WorkRequest>(request.body) ? CheckWork(std::get<WorkRequest>(request.body)) : CheckInput(Payload(request));
  if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  valid = co_await ValidateInputSources(session, actor.Value(), Payload(request).context);
  if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  const auto* task_request = std::get_if<WorkRequest>(&request.body);
  std::optional<GoalRef> goal = task_request ? ActorGoal(actor.Value()) : std::nullopt;
  std::optional<ProjectTaskSnapshot> task;
  if (task_request && task_request->task) {
    auto claim = co_await ResolveWorkTask(session, *task_request, actor.Value());
    if (!claim.Succeeded()) co_return Result::Failure(claim.Error());
    task = std::move(claim).Value();
    if (task->attempt_goal) {
      if (goal && goal != task->attempt_goal) {
        co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The claim and requesting run use different goals");
      }
      goal = task->attempt_goal;
    }
  }
  auto dispatch = co_await AuthorizeGoalDispatch(session, actor.Value(), goal);
  if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
  const auto* steer = std::get_if<SteerRequest>(&request.body);
  if (steer) {
    auto permitted_actor = Actor(bound, Capability::SteerRun);
    if (!permitted_actor.Succeeded()) co_return Result::Failure(permitted_actor.Error());
    auto steering = CollaborationAccess(actor.Value(), Capability::SteerRun, session);
    steering.target.run = steer->expected_run;
    auto access = co_await CheckOperation(std::move(steering), confirmation, approved);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
  }
  if (request.related) {
    auto related = FindMessage(*request.related);
    if (!related || related->second.summary.from.principal != actor.Value().principal ||
        (related->first != request.to && related->second.summary.from.session != request.to)) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::NotFound, "Related message unavailable");
    }
  }
  const CollaborationMessageId id{MakeId()};
  auto allowed = co_await Receive(actor.Value(), session, id, request, goal);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  actor = Actor(bound, Capability::SendMessage);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  if (actor.Value().principal != principal) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (!approved) {
    if (auto replay = Replay<MessageAccepted>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*replay));
  }
  if (session.summary.lifecycle != SessionLifecycle::Open || session.summary.binding_revision != agent_revision) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "Target session changed");
  }
  if (request.deliver_by && Now() >= *request.deliver_by) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::DeadlineExceeded, "Delivery deadline passed");
  }
  dispatch = CheckGoalDispatch(session, goal);
  if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
  if (task && !CurrentWorkClaim(*task, *task_request->task)) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The work claim changed before delivery");
  }
  RunExecution* steer_target = nullptr;
  if (steer) {
    auto run = runs.find(steer->expected_run);
    if (run == runs.end() || run->second.view.session != session.summary.id) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::NotFound, "Target run unavailable");
    }
    if (Terminal(run->second.view.state)) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::AlreadyClosed, "Target run closed");
    if (session.summary.active_run != steer->expected_run) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "Active run changed");
    }
    if (run->second.view.state == RunState::Cancelling) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::InvalidState, "Target run is cancelling");
    }
    steer_target = &run->second;
  }
  if (!confirmation.empty()) {
    auto home = actor.Value().session.value_or(session.summary.id);
    auto summary = "Conversation: " + session.summary.title + "\n\n" + Payload(request).text;
    auto consent = co_await StageOperation(principal, command, key, home, {std::move(confirmation), agent_revision},
        {.title = "Approve conversation message", .summary = std::move(summary)}, request.deliver_by);
    if (!consent.Succeeded()) co_return Result::Failure(consent.Error());
    co_return co_await ReplayReceipt(ReceiptFor<MessageAccepted>(principal, command));
  }
  if (steer_target && allowed.Value().action == ReceiveAction::Allow) {
    const auto message = RecordSteer(session, *steer_target, steer->input, MessageAuthor::Collaboration);
    CollaborationSummary summary{.id = id, .from = actor.Value(), .to = request.to,
        .in_reply_to = request.related, .delivery = DeliveryState::Queued, .target_run = steer->expected_run};
    session.collaboration.emplace(id, StoredCollaboration{.record = {.summary = summary, .request = std::move(request)},
                                                         .steer_message = message});
    co_return co_await FinishOperation(session, principal, command, key, ReceiptValue(summary), true, approved.has_value());
  }
  EnqueueMessage(session, {.summary = {.id = id, .from = actor.Value(), .to = request.to, .in_reply_to = request.related},
                           .request = std::move(request)}, std::move(allowed).Value(), goal);
  const auto& entry = session.collaboration.at(id);
  auto receipt = co_await FinishOperation(session, principal, command, key, ReceiptValue(entry.record.summary), false,
                                          approved.has_value());
  if (receipt.Succeeded() && entry.auto_start) {
    options.tasks.Launch([self = shared_from_this(), to = session.summary.id] { return self->ConsumeQueue(to); });
  }
  co_return receipt;
}

Async<Receipt<MessageAccepted>> RuntimeState::StartMessage(CommandId command, CollaborationMessageId id, bool automatic,
                                                          std::optional<OperationPlan> approved) {
  using Result = AgentResult<Receipt<MessageAccepted>>;
  auto actor = Actor({}, Capability::StartSessionRun);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  std::vector<Capability> confirmation;
  auto read = co_await ReadMessage(id, {}, Capability::StartSessionRun, &confirmation, approved);
  if (!read.Succeeded()) co_return Result::Failure(read.Error());
  const auto target_id = read.Value().summary.to;
  auto& target = sessions.at(target_id);
  auto& entry = target.collaboration.at(id);
  CommandInput key{.operation = Operation::StartRequest, .session = target_id, .message = id};
  if (!approved) {
    if (auto prior = Replay<MessageAccepted>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*prior));
  }
  const auto* work = Work(entry.record);
  if (!work || entry.record.summary.delivery != DeliveryState::Queued) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::AlreadyClosed, "The work request is no longer queued");
  }
  if (automatic && !entry.auto_start) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Busy, "The work request requires a manual start");
  if (target.summary.active_run) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Busy, "Target session is busy");
  const auto goal = entry.goal;
  auto dispatch = co_await AuthorizeGoalDispatch(target, entry.record.summary.from, goal);
  if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
  if (goal) {
    dispatch = co_await AuthorizeGoalDispatch(target, actor.Value(), goal);
    if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
  }
  const auto binding_revision = target.summary.binding_revision;
  if (approved && approved->target_binding != binding_revision) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The approved target session changed");
  }
  auto options = approved && approved->run_start
      ? UserRunOptions{approved->run_start->model, approved->run_start->execution_profile} : UserRunOptions{};
  auto binding = co_await ResolveStart(target, actor.Value().principal, options);
  if (!binding.Succeeded()) co_return Result::Failure(binding.Error());
  auto receive = co_await Receive(entry.record.summary.from, target, id, *entry.record.request, goal);
  if (!receive.Succeeded()) co_return Result::Failure(receive.Error());
  auto sources = co_await ValidateInputSources(target, entry.record.summary.from, work->input.context);
  if (!sources.Succeeded()) co_return Result::Failure(sources.Error());
  auto scope = LimitScope(entry.scope, receive.Value().allowed_work_scope);
  std::optional<ProjectTaskSnapshot> task;
  if (work->task && receive.Value().action != ReceiveAction::Deny) {
    auto input = *work;
    input.scope = scope;
    auto claim = co_await ResolveWorkTask(target, std::move(input), actor.Value());
    if (!claim.Succeeded()) co_return Result::Failure(claim.Error());
    task = std::move(claim).Value();
    if (task->attempt_goal && task->attempt_goal != goal) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The accepted work has a different goal version");
    }
  }
  if (!approved) {
    if (auto prior = Replay<MessageAccepted>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*prior));
  }
  auto current = Actor({}, Capability::StartSessionRun);
  if (!current.Succeeded() || current.Value().principal != actor.Value().principal) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "The acting principal changed");
  }
  if (entry.record.summary.delivery != DeliveryState::Queued || entry.record.outcome) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::AlreadyClosed, "The work request is no longer queued");
  }
  if (!approved && PendingStart(id)) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Busy, "This work request already has a start approval");
  }
  if (target.summary.active_run || (automatic && std::ranges::any_of(target.queue, [](const auto& item) {
        return item.state == QueueItemState::Queued && std::holds_alternative<SendInput>(item.input);
      }))) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Busy, "User input has priority");
  if (target.summary.binding_revision != binding_revision || target.summary.lifecycle != SessionLifecycle::Open) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "Target session changed");
  }
  if (auto expiry = Expiry(entry.record); expiry && Now() >= expiry->first) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::DeadlineExceeded, "The work request expired");
  }
  dispatch = CheckGoalDispatch(target, goal);
  if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
  if (!confirmation.empty() && receive.Value().action != ReceiveAction::Deny) {
    auto expiry = Expiry(entry.record);
    auto consent = co_await StageOperation(actor.Value().principal, command, key, target_id,
        {std::move(confirmation), binding_revision, {}, binding.Value()},
        {.title = "Start collaboration request", .summary = work->input.text},
        expiry ? std::optional{expiry->first} : std::nullopt);
    if (!consent.Succeeded()) co_return Result::Failure(consent.Error());
    co_return co_await ReplayReceipt(ReceiptFor<MessageAccepted>(actor.Value().principal, command));
  }
  ProjectData* project = nullptr;
  std::vector<const SessionData*> goal_home;
  if (receive.Value().action == ReceiveAction::Deny) {
    entry.record.summary.delivery = DeliveryState::Rejected;
    entry.record.reason = Error(ErrorCode::PermissionDenied, receive.Value().reason);
    RemoveQueue(target, id, QueueItemState::Rejected, {}, entry.record.reason);
    EndRequest(target, entry, ReplyEnd::Rejected, RequestState::Failed);
  } else if (automatic && (receive.Value().action == ReceiveAction::Hold || !receive.Value().allow_auto_start)) {
    entry.auto_start = false;
    ++target.sequence;
    auto saved = co_await WhenSaved(Persist(target));
    if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Busy, "The work request requires a manual start");
  } else {
    if (task && !CurrentWorkClaim(*task, *work->task)) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "Task claim changed before work started");
    }
    entry.scope = std::move(scope);
    auto& run = RecordWork(target, id, actor.Value().principal, binding.Value(), task ? task->resolved_inputs : std::vector<SourceSlice>{});
    if (task) project = &BindWorkTask(*work->task, run);
    if (auto* home = AttachGoalRun(run, goal)) goal_home.push_back(home);
  }
  const auto accepted = ReceiptValue(entry.record.summary);
  auto receipt = co_await FinishOperation(target, actor.Value().principal, command, key, accepted,
      accepted.target_run.has_value(), approved.has_value(), project, std::move(goal_home));
  if (receipt.Succeeded() && accepted.target_run && stage == Stage::Started) {
    auto& run = runs.at(*accepted.target_run);
    if (!run.stop.stop_requested()) Launch(run);
  }
  co_return receipt;
}

bool RuntimeState::PendingStart(CollaborationMessageId id) const {
  return std::ranges::any_of(commands, [&](const auto& item) {
    const auto& command = item.second;
    return command.input.operation == Operation::StartRequest && command.input.message == id && command.plan &&
          (command.progress.state == CommandState::AwaitingApproval || command.progress.state == CommandState::Scheduled);
  });
}

void RuntimeState::ClosePendingStart(SessionData& target, CollaborationMessageId id, DeliveryState delivery, AgentError reason) {
  auto found = target.collaboration.find(id);
  if (found == target.collaboration.end() || found->second.record.summary.delivery != DeliveryState::Queued) return;
  auto& entry = found->second;
  entry.auto_start = false;
  entry.record.summary.delivery = delivery;
  entry.record.reason = reason;
  const bool withdrawn = delivery == DeliveryState::Withdrawn;
  RemoveQueue(target, id, withdrawn ? QueueItemState::Withdrawn : QueueItemState::Rejected, {}, std::move(reason));
  const auto outcome = withdrawn ? ReplyEnd::Withdrawn : delivery == DeliveryState::Expired ? ReplyEnd::Expired : ReplyEnd::Rejected;
  EndRequest(target, entry, outcome, withdrawn ? RequestState::Cancelled : RequestState::Failed);
}

bool RuntimeState::RetireWorkWithGoal(const RunExecution& run) const {
  if (!run.view.work_request) return false;
  const auto& requests = sessions.at(run.view.session).collaboration;
  auto found = requests.find(*run.view.work_request);
  const auto* work = found == requests.end() ? nullptr : Work(found->second.record);
  return work && work->cancel_on_goal_revision;
}

std::vector<const SessionData*> RuntimeState::WithdrawGoalWork(const GoalRef& ref) {
  std::vector<const SessionData*> affected;
  const auto reason = Error(ErrorCode::Conflict, "The goal definition changed");
  for (auto& [id, target] : sessions) {
    bool changed = false;
    for (const auto& [message, entry] : target.collaboration) {
      if (entry.goal != ref || entry.record.summary.delivery != DeliveryState::Queued) continue;
      ClosePendingStart(target, message, DeliveryState::Withdrawn, reason);
      changed = true;
      for (auto& [key, command] : commands) {
        if (command.input.operation != Operation::StartRequest || command.input.message != message ||
            !command.progress.approval || command.progress.approval->session != id ||
            (command.progress.state != CommandState::AwaitingApproval && command.progress.state != CommandState::Scheduled)) continue;
        auto interaction = std::ranges::find(target.interactions, command.progress.approval->interaction, &InteractionSnapshot::id);
        if (interaction != target.interactions.end()) CloseInteraction(target, *interaction, InteractionState::Invalidated);
        Resolve(command, target, CommandState::Withdrawn, reason);
        command.progress.resolution->commit = {};
      }
    }
    if (changed) affected.push_back(&target);
  }
  return affected;
}

bool RuntimeState::ReceivedArtifact(const ActionParty& actor, const ArtifactRecord& artifact) const {
  const auto* source = Published(sessions.at(artifact.session));
  return source && actor.run && std::ranges::any_of(source->collaboration, [&](const auto& entry) {
    const auto& transcript = entry.second.record;
    return transcript.summary.from.run == actor.run && transcript.summary.from.principal == actor.principal &&
        transcript.outcome && transcript.outcome->end == ReplyEnd::Received && transcript.reply &&
        std::ranges::find(transcript.reply->artifacts, artifact.metadata.ref) != transcript.reply->artifacts.end();
  });
}

Async<Receipt<MessageAccepted>> RuntimeState::ReplyMessage(CommandId command, CollaborationMessageId id, SessionReply reply,
                                                           std::optional<RunId> bound, std::optional<OperationPlan> approved) {
  using Result = AgentResult<Receipt<MessageAccepted>>;
  auto actor = Actor(bound, Capability::Reply);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  const auto principal = actor.Value().principal;
  std::vector<Capability> confirmation;
  auto read = co_await ReadMessage(id, bound, Capability::Reply, &confirmation, approved);
  if (!read.Succeeded()) co_return Result::Failure(read.Error());
  const auto& record = read.Value();
  auto& target = sessions.at(record.summary.to);
  const auto target_binding = target.summary.binding_revision;
  if (approved && approved->target_binding != target_binding) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The approved receiving session changed");
  }
  auto& entry = target.collaboration.at(id);
  if (bound && record.summary.target_run != bound) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "Only the receiving run can submit the reply");
  }
  CommandInput key{.operation = Operation::Reply, .session = target.summary.id, .run = bound.value_or(RunId{}),
                   .message = id, .final_reply = reply};
  if (!approved) {
    if (auto prior = Replay<MessageAccepted>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*prior));
  }
  const auto* work = Work(record);
  if (!work || record.outcome || record.summary.delivery != DeliveryState::Delivered) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::AlreadyClosed, "This request is not awaiting an active reply");
  }
  if (reply.summary.size() > (1 << 20) || !ValidUtf8(reply.summary)) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::InvalidArgument, "Invalid reply summary");
  }
  if (reply.value) {
    auto parsed = ValidateJson(*reply.value, work->reply && work->reply->schema && reply.status == ReplyStatus::Succeeded
        ? *work->reply->schema : JsonSchema{JsonValue{true}});
    if (!parsed.Succeeded()) co_return Result::Failure(parsed.Error());
  } else if (work->reply && work->reply->schema && reply.status == ReplyStatus::Succeeded) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::InvalidArgument, "A structured reply is required");
  }
  std::set<std::string> types;
  for (const auto& ref : reply.artifacts) {
    auto found = co_await ReadArtifact(ref);
    if (!found.Succeeded()) co_return Result::Failure(found.Error());
    if (found.Value().record.session != target.summary.id || (bound && found.Value().record.run != *bound)) {
      const auto* source = Published(sessions.at(found.Value().record.session));
      if (!source || !target.summary.project || source->summary.project != target.summary.project ||
          !ReceivedArtifact(actor.Value(), found.Value().record)) {
        co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "Reply artifact is outside the receiving run");
      }
    }
    const std::vector<SourceSlice> sources{{ref}};
    auto readable = co_await ValidateInputSources(target, actor.Value(), sources);
    if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
    types.insert(found.Value().record.metadata.type);
  }
  if (work->reply && reply.status == ReplyStatus::Succeeded &&
      std::ranges::any_of(work->reply->artifact_types, [&](const auto& type) { return !types.contains(type); })) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::InvalidArgument, "Required reply artifacts are missing");
  }
  const CollaborationMessageId message_id{MakeId()};
  auto sender = record.summary.from.session ? sessions.find(*record.summary.from.session) : sessions.end();
  if (sender != sessions.end()) {
    auto allowed = co_await Receive(actor.Value(), sender->second, message_id, reply, entry.goal);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    if (allowed.Value().action != ReceiveAction::Allow) {
      co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "The requesting session did not accept the reply");
    }
  }
  actor = Actor(bound, Capability::Reply);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  if (actor.Value().principal != principal) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (!approved) {
    if (auto prior = Replay<MessageAccepted>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*prior));
  }
  if (target.summary.binding_revision != target_binding) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::Conflict, "The receiving session changed");
  }
  if (entry.record.outcome) co_return Fail<Receipt<MessageAccepted>>(ErrorCode::AlreadyClosed, "The request is already resolved");
  if (auto expiry = Expiry(entry.record); expiry && Now() >= expiry->first) {
    co_return Fail<Receipt<MessageAccepted>>(ErrorCode::DeadlineExceeded, "The reply deadline passed");
  }
  if (!confirmation.empty()) {
    auto expiry = Expiry(entry.record);
    auto consent = co_await StageOperation(principal, command, key, target.summary.id, {std::move(confirmation), target_binding},
        {.title = "Approve collaboration reply", .summary = reply.summary, .artifacts = reply.artifacts},
        expiry ? std::optional{expiry->first} : std::nullopt);
    if (!consent.Succeeded()) co_return Result::Failure(consent.Error());
    co_return co_await ReplayReceipt(ReceiptFor<MessageAccepted>(principal, command));
  }
  auto summary = CollaborationSummary{.id = message_id, .from = actor.Value(),
      .to = record.summary.from.session.value_or(target.summary.id), .in_reply_to = id, .delivery = DeliveryState::Delivered};
  target.collaboration.emplace(message_id, StoredCollaboration{CollaborationRecord{.summary = summary, .reply = reply}});
  entry.record.reply = reply;
  EndRequest(target, entry, ReplyEnd::Received, RequestState::Replied);
  entry.record.outcome->reply = std::move(reply);
  co_return co_await FinishOperation(target, principal, command, key, ReceiptValue(summary), false, approved.has_value());
}

Async<CommandReceipt> RuntimeState::WithdrawMessage(CommandId command, CollaborationMessageId id, std::optional<RunId> bound) {
  using Result = AgentResult<CommandReceipt>;
  auto actor = Actor(bound, Capability::WithdrawMessage);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto read = co_await ReadMessage(id, bound, Capability::WithdrawMessage);
  if (!read.Succeeded()) co_return Result::Failure(read.Error());
  auto& session = sessions.at(read.Value().summary.to);
  auto& entry = session.collaboration.at(id);
  if (entry.record.summary.from.run != bound && bound) {
    co_return Fail<CommandReceipt>(ErrorCode::PermissionDenied, "The acting run does not own the message");
  }
  CommandInput key{.operation = Operation::WithdrawMessage, .session = session.summary.id,
                   .run = bound.value_or(RunId{}), .message = id};
  if (auto prior = Replay<std::monostate>(*this, actor.Value(), command, key)) co_return co_await ReplayReceipt(std::move(*prior));
  if (entry.record.summary.delivery != DeliveryState::Queued && entry.record.summary.delivery != DeliveryState::AwaitingApproval) {
    co_return Fail<CommandReceipt>(ErrorCode::AlreadyClosed, "The message has already taken effect");
  }
  entry.record.summary.delivery = DeliveryState::Withdrawn;
  if (entry.steer_message && entry.record.summary.target_run) {
    auto& run = runs.at(*entry.record.summary.target_run);
    std::erase_if(run.pending_steers, [&](const auto& item) { return item.first == *entry.steer_message; });
    auto state = std::ranges::find(run.view.steering, *entry.steer_message, &RunSteerSnapshot::message);
    if (state != run.view.steering.end()) {
      state->state = SteerState::NotApplied;
      state->reason = Error(ErrorCode::AlreadyClosed, "Collaboration input was withdrawn");
    }
  }
  RemoveQueue(session, id, QueueItemState::Withdrawn);
  EndRequest(session, entry, ReplyEnd::Withdrawn, RequestState::Cancelled);
  Accept(actor.Value().principal, command, std::move(key), session, std::monostate{});
  co_return co_await SaveReceipt<std::monostate>(session, actor.Value().principal, command);
}

void RuntimeState::FinishWork(SessionData& session, RunSnapshot& run) {
  if (!run.work_request) return;
  auto& request = session.collaboration.at(*run.work_request);
  if (request.record.outcome) return;
  if (auto expiry = Expiry(request.record); expiry && Now() >= expiry->first) {
    EndRequest(session, request, ReplyEnd::Expired, RequestState::Failed);
  } else if (run.state == RunState::Cancelled) {
    EndRequest(session, request, ReplyEnd::TargetCancelled, RequestState::Cancelled);
  } else if (run.state == RunState::Succeeded) {
    EndRequest(session, request, ReplyEnd::MissingReply, RequestState::Failed);
  } else {
    EndRequest(session, request, ReplyEnd::TargetFailed, RequestState::Failed);
  }
}

void RuntimeState::SettleCollaborationSteer(SessionData& session, RunSteerSnapshot& steer, bool apply) {
  auto entry = std::ranges::find_if(session.collaboration, [&](const auto& item) {
    return item.second.steer_message == steer.message;
  });
  if (entry == session.collaboration.end()) return;
  auto& record = entry->second.record;
  if (apply && record.request->deliver_by && Now() >= *record.request->deliver_by) {
    steer.state = SteerState::NotApplied;
    steer.reason = Error(ErrorCode::DeadlineExceeded, "Collaboration input expired before application");
  }
  record.summary.delivery = steer.state == SteerState::Applied ? DeliveryState::Delivered : DeliveryState::Rejected;
  record.reason = steer.reason;
}

} // namespace huxerui::agentkit::detail
