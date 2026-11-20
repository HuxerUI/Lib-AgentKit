#include "runtime_internal.h"

namespace huxerui::agentkit::detail {

Async<void> RuntimeState::CheckOperation(PolicyRequest request, std::vector<Capability>& confirmation,
                                         const std::optional<OperationPlan>& approved) {
  const auto capability = request.capability;
  auto decision = co_await Decide(std::move(request));
  if (!decision.Succeeded()) co_return AgentResult<void>::Failure(decision.Error());
  switch (decision.Value().decision) {
  case AccessDecision::Allow:
    break;
  case AccessDecision::RequireConfirmation:
    if (approved) {
      if (std::ranges::find(approved->confirmation, capability) == approved->confirmation.end()) {
        co_return Fail<void>(ErrorCode::Conflict, "The operation now needs a different approval");
      }
    } else if (std::ranges::find(confirmation, capability) == confirmation.end()) {
      confirmation.push_back(capability);
    }
    break;
  case AccessDecision::AllowReview:
    co_return Fail<void>(ErrorCode::PermissionDenied, "Operation approval requires a human decision");
  case AccessDecision::Deny:
    co_return Fail<void>(ErrorCode::PermissionDenied, decision.Value().reason);
  }
  co_return AgentResult<void>::Success();
}

Async<void> RuntimeState::StageOperation(PrincipalId principal, CommandId command, CommandInput input, SessionId home,
                                         OperationPlan plan, ApprovalPreview preview, std::optional<TimePoint> expires) {
  auto access = co_await WithSession(home, principal,
      input.operation == Operation::RenderedAction ? Capability::ReadSession : Capability::SubmitInteraction);
  if (!access.Succeeded()) co_return access;
  auto prior = Find(principal, command, input);
  if (!prior.Succeeded()) co_return AgentResult<void>::Failure(prior.Error());
  if (prior.Value()) co_return AgentResult<void>::Success();

  auto& session = sessions.at(home);
  std::optional<RunId> run;
  if (!input.run.value.empty()) {
    const auto source = runs.find(input.run);
    if (source == runs.end() || Terminal(source->second.view.state) || source->second.stop.stop_requested()) {
      co_return Fail<void>(ErrorCode::AlreadyClosed, "The requesting run has ended");
    }
    if (source->second.view.session == home && input.operation != Operation::Goal) {
      run = input.run;
      auto deadline = source->second.deadline;
      if (deadline && (!expires || *deadline < *expires)) expires = deadline;
    }
  }
  if (expires && Now() >= *expires) co_return Fail<void>(ErrorCode::DeadlineExceeded, "Operation approval expired");
  if (input.operation == Operation::StartRequest) {
    auto request = session.collaboration.find(*input.message);
    if (request == session.collaboration.end() || request->second.record.summary.delivery != DeliveryState::Queued) {
      co_return Fail<void>(ErrorCode::AlreadyClosed, "The work request is no longer queued");
    }
    if (PendingStart(*input.message)) co_return Fail<void>(ErrorCode::Busy, "This work request already has a start approval");
    request->second.auto_start = false;
  }

  PendingApproval pending{home, InteractionId{MakeId()}, 1};
  session.interactions.push_back({.id = pending.interaction, .version = pending.version, .session = home,
      .run = run, .recipient = principal, .state = InteractionState::Pending,
      .body = OperationApproval{command, std::move(preview)}, .expires_at = expires});
  ++session.summary.pending_interactions;
  ++session.sequence;
  Accept(principal, command, std::move(input), session, std::monostate{}, CommandState::AwaitingApproval);
  auto& record = commands.at({principal, command});
  record.progress.result.reset();
  record.progress.approval = pending;
  record.plan = std::move(plan);
  auto committed = co_await WhenSaved(Persist(session, false, record.progress.accepted.commit));
  if (!committed.Succeeded()) co_return committed;
  if (expires) ScheduleOperationExpiry(home, pending.interaction, *expires);
  co_return AgentResult<void>::Success();
}

void RuntimeState::DecideOperation(SessionData& session, const InteractionSnapshot& interaction,
                                   InteractionState decision) {
  const auto* approval = std::get_if<OperationApproval>(&interaction.body);
  if (!approval) return;
  const auto command = commands.find({interaction.recipient, approval->pending_command});
  if (command == commands.end() || command->second.progress.state != CommandState::AwaitingApproval) return;
  auto& record = command->second;
  switch (decision) {
  case InteractionState::Approved:
    record.progress.state = CommandState::Scheduled;
    return;
  case InteractionState::Denied:
    Resolve(record, session, CommandState::Rejected, Error(ErrorCode::PermissionDenied, "Operation was denied"));
    if (record.input.operation == Operation::StartRequest) {
      ClosePendingStart(session, *record.input.message, DeliveryState::Rejected, *record.progress.failure);
    }
    break;
  case InteractionState::Expired:
    Resolve(record, session, CommandState::Expired, Error(ErrorCode::DeadlineExceeded, "Operation approval expired"));
    if (record.input.operation == Operation::StartRequest) {
      ClosePendingStart(session, *record.input.message, DeliveryState::Expired, *record.progress.failure);
    }
    break;
  case InteractionState::Cancelled:
  case InteractionState::Invalidated:
    Resolve(record, session, CommandState::Withdrawn, Error(ErrorCode::AlreadyClosed, "Approval is no longer active"));
    break;
  default:
    return;
  }
  record.progress.resolution->commit = {};
  if (record.input.operation == Operation::Goal) UpdateGoalReview(session, interaction, decision);
}

void RuntimeState::ScheduleOperation(PrincipalId principal, CommandId command, std::optional<CallbackCompletion<void>> completed) {
  const auto saved = commands.find({principal, command});
  if (saved != commands.end() && saved->second.input.operation == Operation::ActivateSkill) return;
  if (stage != Stage::Started || !executing_operations.emplace(principal, command).second) {
    if (completed) completed->Complete();
    return;
  }
  options.tasks.Launch([self = shared_from_this(), principal = std::move(principal), command = std::move(command),
                        completed = std::move(completed)]() -> Task<void> {
    co_await self->ExecuteOperation(principal, command);
    if (completed) completed->Complete();
  });
}

Task<void> RuntimeState::ExecuteOperation(PrincipalId principal, CommandId command) {
  const auto key = std::pair(principal, command);
  auto registered = executing_operations.find(key);
  auto cleanup = std::unique_ptr<const std::pair<PrincipalId, CommandId>, std::function<void(const std::pair<PrincipalId, CommandId>*)>>(
      &*registered, [this](const auto* entry) { executing_operations.erase(*entry); });
  auto& record = commands.at(key);
  if (stage != Stage::Started || record.progress.state != CommandState::Scheduled || !record.plan) co_return;
  if (record.input.operation == Operation::RenderedAction && record.progress.result) co_return;
  const auto& approval = record.progress.approval;
  if (!approval && record.input.operation != Operation::CreateComparison) co_return;
  auto& home = sessions.at(*record.progress.accepted.session);
  std::optional<TimePoint> expires;
  InteractionId interaction_id;
  if (approval) {
    const auto found = std::ranges::find(home.interactions, approval->interaction, &InteractionSnapshot::id);
    if (found == home.interactions.end() || found->state != InteractionState::Approved) co_return;
    expires = found->expires_at;
    interaction_id = found->id;
  }

  const auto input = record.input;
  const auto plan = record.plan;
  const auto bound = input.run.value.empty() ? std::optional<RunId>{} : std::optional{input.run};
  auto resume = [&]() -> Async<void> {
    auto permission = co_await WithSession(home.summary.id, principal, approval ? Capability::SubmitInteraction : Capability::ReadSession);
    if (!permission.Succeeded()) co_return permission;
    if (expires && Now() >= *expires) co_return Fail<void>(ErrorCode::DeadlineExceeded, "The approved operation expired");
    switch (input.operation) {
    case Operation::Create:
    case Operation::Fork: {
      CreateSessionRequest request{.options = *input.options, .user_start = input.start};
      if (input.work) request.initial = *input.work;
      else if (input.input) request.initial = *input.input;
      auto result = co_await Create(shared_from_this(), command, std::move(request), bound, input.fork, plan,
                                    input.workspace_choice);
      if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      break;
    }
    case Operation::CreateComparison: {
      auto result = co_await CreateForkReview(command, *input.comparison, bound, plan);
      if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      break;
    }
    case Operation::SendMessage: {
      auto result = co_await SendMessage(command, *input.delivery, bound, plan);
      if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      break;
    }
    case Operation::Reply: {
      auto result = co_await ReplyMessage(command, *input.message, *input.final_reply, bound, plan);
      if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      break;
    }
    case Operation::StartRequest: {
      auto result = co_await StartMessage(command, *input.message, false, plan);
      if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      break;
    }
    case Operation::Goal: {
      auto match = std::ranges::find_if(home.goals, [&](const auto& entry) {
        const auto& goal = entry.second;
        return goal.view.completion_review && goal.verification &&
            goal.view.completion_review->approval.interaction == record.progress.approval->interaction;
      });
      if (match != home.goals.end()) {
        co_await VerifyGoal(home.summary.id, match->first, match->second.verification->revision, true);
      } else {
        auto result = co_await UpdateGoal(input.session, principal, bound, command, *input.goal, plan);
        if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      }
      break;
    }
    case Operation::RenderedAction: {
      auto execution = std::make_shared<const RenderedExecution>(RenderedExecution{input, *plan->rendered_action, plan});
      auto result = co_await ApplyRenderedAction(principal, command, std::move(execution));
      if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
      break;
    }
    default:
      co_return Fail<void>(ErrorCode::Unsupported, "Unknown approved operation");
    }
    co_return AgentResult<void>::Success();
  };
  auto result = Fail<void>(ErrorCode::InvalidState, "Approved operation failed");
  try { result = co_await resume(); }
  catch (...) { ReportException(std::current_exception()); }
  if (stage != Stage::Started || result.Succeeded() || record.progress.state != CommandState::Scheduled) co_return;
  ++home.sequence;
  const auto status = result.Error().code == ErrorCode::DeadlineExceeded ? CommandState::Expired : CommandState::Failed;
  Resolve(record, home, status, result.Error());
  if (input.operation == Operation::Goal) {
    auto interaction = std::ranges::find(home.interactions, interaction_id, &InteractionSnapshot::id);
    if (interaction != home.interactions.end()) UpdateGoalReview(home, *interaction,
        status == CommandState::Expired ? InteractionState::Expired : InteractionState::Invalidated);
  }
  if (input.operation == Operation::StartRequest &&
      (result.Error().code == ErrorCode::PermissionDenied || result.Error().code == ErrorCode::DeadlineExceeded)) {
    ClosePendingStart(home, *input.message, status == CommandState::Expired ? DeliveryState::Expired : DeliveryState::Rejected,
                      result.Error());
  }
  auto saved = co_await WhenSaved(Persist(home, false, record.progress.resolution->commit));
  if (!saved.Succeeded()) co_return;
}

void RuntimeState::ScheduleOperationExpiry(SessionId id, InteractionId interaction, TimePoint deadline) {
  auto& session = sessions.at(id);
  const auto entry = std::ranges::find(session.interactions, interaction, &InteractionSnapshot::id);
  if (session.interaction_timers.contains(interaction) || entry == session.interactions.end() ||
      entry->state != InteractionState::Pending) return;
  session.interaction_timers.emplace(interaction, options.tasks.Launch(
      [self = shared_from_this(), id, interaction, deadline] { return self->ExpireOperation(id, interaction, deadline); }));
}

Task<void> RuntimeState::ExpireOperation(SessionId id, InteractionId interaction, TimePoint deadline) {
  while (Now() < deadline) co_await Delay(std::max(Duration(1), deadline - Now()));
  auto& session = sessions.at(id);
  session.interaction_timers.erase(interaction);
  auto found = std::ranges::find(session.interactions, interaction, &InteractionSnapshot::id);
  if (found == session.interactions.end() || found->state != InteractionState::Pending || stage != Stage::Started) co_return;
  CloseInteraction(session, *found, InteractionState::Expired);
  Persist(session);
}

void RuntimeState::ResumeOperations() {
  for (auto& [key, record] : commands) {
    if (!record.plan) continue;
    if (record.progress.state == CommandState::Scheduled) {
      if (record.input.operation == Operation::RenderedAction && record.progress.result) continue;
      ScheduleOperation(key.first, key.second);
      continue;
    }
    if (record.progress.state != CommandState::AwaitingApproval || !record.progress.approval) continue;
    const auto& approval = *record.progress.approval;
    auto& session = sessions.at(approval.session);
    auto interaction = std::ranges::find(session.interactions, approval.interaction, &InteractionSnapshot::id);
    if (interaction == session.interactions.end()) continue;
    if (interaction->state != InteractionState::Pending) {
      DecideOperation(session, *interaction, interaction->state);
      Persist(session);
    } else if (interaction->expires_at) {
      ScheduleOperationExpiry(approval.session, approval.interaction, *interaction->expires_at);
    }
  }
}

} // namespace huxerui::agentkit::detail
