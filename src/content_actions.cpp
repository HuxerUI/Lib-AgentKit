#include "runtime_internal.h"

#include "json_wire.h"

#include <nlohmann/json.hpp>

namespace huxerui::agentkit::detail {
namespace {

template<class T>
AgentResult<CommandReceipt> ActionReceipt(AgentResult<Receipt<T>> receipt) {
  if (!receipt.Succeeded()) return AgentResult<CommandReceipt>::Failure(receipt.Error());
  auto value = std::move(receipt).Value();
  CommandReceipt result{.stamp = std::move(value.stamp)};
  if (const auto* approval = std::get_if<PendingApproval>(&value.outcome)) result.outcome = *approval;
  return AgentResult<CommandReceipt>::Success(std::move(result));
}

CommandReceipt OriginalReceipt(const CommandRecord& record) {
  CommandReceipt result{.stamp = record.progress.accepted};
  if (record.progress.approval) result.outcome = *record.progress.approval;
  return result;
}

AgentResult<void> CheckPayload(const JsonValue& value, const JsonSchema& schema) {
  auto valid = ValidateJson(value, schema);
  if (!valid.Succeeded()) return valid;
  if (ToWireJson(value).dump().size() > (256 << 10))
    return Fail<void>(ErrorCode::BudgetExceeded, "Action payload exceeds 256 KiB");
  return AgentResult<void>::Success();
}

const ContentActionRegistration* FindContentAction(const RuntimeState& state, const CustomBlock& block, std::string_view id) {
  const auto component = state.content_components.find({block.type.id, block.type.schema_version});
  if (component == state.content_components.end()) return nullptr;
  const auto& actions = component->second.actions;
  auto registered = std::ranges::find(actions, id, &ContentActionRegistration::id);
  return registered == actions.end() ? nullptr : &*registered;
}

PolicyRequest ContentPermit(const SessionData& session, const PrincipalId& user, const CustomBlockAction& action) {
  auto request = RuntimeState::Request(user, Capability::UseContentAction, &session);
  request.content_action = action;
  request.sources.push_back({action.target.message});
  return request;
}

PolicyRequest ArtifactPermit(const SessionData& session, const PrincipalId& user, const ArtifactAction& action) {
  auto request = RuntimeState::Request(user, Capability::UseArtifactAction, &session);
  request.artifact_action = action;
  request.sources.push_back({action.artifact});
  return request;
}

AgentResult<void> CheckBindings(const PreparedRenderedAction& plan, const CustomBlockAction* action, const CustomBlock* block) {
  if (const auto* reply = std::get_if<InteractionReply>(&plan.request)) {
    if (!block || std::ranges::find(block->interactions, reply->id) == block->interactions.end()) {
      return Fail<void>(ErrorCode::PermissionDenied, "Interaction is outside the component bindings");
    }
  }
  if (plan.artifact && block && std::ranges::find(block->artifacts, plan.artifact->artifact) == block->artifacts.end()) {
    return Fail<void>(ErrorCode::PermissionDenied, "Artifact is outside the component bindings");
  }
  const auto* input = std::get_if<SendInput>(&plan.request);
  if (const auto* steer = std::get_if<SteerRequest>(&plan.request)) input = &steer->input;
  if (!input) return AgentResult<void>::Success();
  const auto bound_artifact = [&](const ArtifactRef& ref) {
    return (block && std::ranges::find(block->artifacts, ref) != block->artifacts.end()) ||
        (plan.artifact && plan.artifact->artifact == ref);
  };
  if (!input->side_chat_conclusions.empty() || !input->skills.empty() ||
      std::ranges::any_of(input->attachments, [&](const auto& ref) { return !bound_artifact(ref); }) ||
      std::ranges::any_of(input->context, [&](const auto& slice) {
        if (const auto* ref = std::get_if<ArtifactRef>(&slice.source)) return !bound_artifact(*ref);
        return !action || slice.source != SourceRef{action->target.message};
      })) {
    return Fail<void>(ErrorCode::PermissionDenied, "Input references are outside the action bindings");
  }
  return AgentResult<void>::Success();
}

} // namespace

Async<std::shared_ptr<const CustomBlock>> RuntimeState::ResolveRenderedBlock(SessionId target, PrincipalId user, CustomBlockRef ref) {
  using Result = AgentResult<std::shared_ptr<const CustomBlock>>;
  const auto epoch = access_revision;
  auto access = co_await WithSession(target, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  const auto& session = sessions.at(target);
  auto resolve = [&]() -> Result {
    const auto* image = Published(session);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting)
      return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::NotFound, "Conversation unavailable");
    const auto message = std::ranges::find_if(image->messages, [&](const auto& entry) {
      return entry.ref.session == ref.message.session && entry.ref.message == ref.message.message;
    });
    if (message == image->messages.end()) return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::NotFound, "Message unavailable");
    if (message->ref.version != ref.message.version)
      return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::Conflict, "Component message version changed");
    if (message->state != MessageState::Complete)
      return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::InvalidState, "Component message is incomplete");
    const auto block = std::ranges::find(message->blocks, ref.block, &ContentBlock::id);
    const auto* custom = block == message->blocks.end() ? nullptr : std::get_if<CustomBlock>(&block->body);
    if (!custom) return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::NotFound, "Component block unavailable");
    auto visible = std::make_shared<CustomBlock>(*custom);
    std::erase_if(visible->interactions, [&](const auto& id) {
      const auto found = std::ranges::find(image->interactions, id, &InteractionSnapshot::id);
      return ref.message.session != target || found == image->interactions.end() || found->recipient != user;
    });
    return Result::Success(std::move(visible));
  };
  auto block = resolve();
  if (!block.Succeeded()) co_return block;
  if (ref.message.session != target) {
    access = co_await WithSession(ref.message.session, user, Capability::ReadSession);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    auto policy = Request(user, Capability::ReadContext, &session);
    policy.sources.push_back({ref.message});
    access = co_await Require(std::move(policy));
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
  }
  for (const auto& artifact : block.Value()->artifacts) {
    auto allowed = co_await ReadActionArtifact(target, user, artifact);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  }
  if (epoch != access_revision)
    co_return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::Conflict, "Component access changed during resolution");
  const auto current = User();
  if (!current.Succeeded() || current.Value() != user)
    co_return Fail<std::shared_ptr<const CustomBlock>>(ErrorCode::PermissionDenied, "The acting principal changed");
  co_return resolve();
}

Async<CommandReceipt> RuntimeState::DispatchRenderedAction(SessionId id, PrincipalId user, CommandId command,
                                                            RenderedActionOrigin action) {
  using Result = AgentResult<CommandReceipt>;
  auto access = co_await WithSession(id, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  CommandInput key{.operation = Operation::RenderedAction, .session = id, .rendered_action = action};
  auto prior = Find(user, command, key);
  if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
  if (prior.Value()) co_return co_await ReplayReceipt(Result::Success(OriginalReceipt(*prior.Value())));
  auto [pending, inserted] = preparing_actions.emplace(std::pair(user, command), key);
  if (!inserted) {
    if (pending->second != key) co_return Fail<CommandReceipt>(ErrorCode::Conflict, "The command ID is bound to a different action");
    co_return Fail<CommandReceipt>(ErrorCode::Busy, "The component action is being prepared");
  }
  const auto release = std::unique_ptr<const std::pair<PrincipalId, CommandId>, std::function<void(const std::pair<PrincipalId, CommandId>*)>>(
      &pending->first, [this](const auto* scope) { preparing_actions.erase(*scope); });
  auto& session = sessions.at(id);
  const auto epoch = access_revision;
  const auto binding = session.summary.binding_revision;
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = user, .session = id};
  std::vector<Capability> confirmation;
  PreparedRenderedAction prepared;
  const auto* custom = std::get_if<CustomBlockAction>(&action);
  std::shared_ptr<const CustomBlock> block;
  try {
    if (custom) {
      access = co_await CheckOperation(ContentPermit(session, user, *custom), confirmation, {});
      if (!access.Succeeded()) co_return Result::Failure(access.Error());
      auto resolved = co_await ResolveRenderedBlock(id, user, custom->target);
      if (!resolved.Succeeded()) co_return Result::Failure(resolved.Error());
      block = std::move(resolved).Value();
      const auto* registration = FindContentAction(*this, *block, custom->action);
      if (!registration) co_return Fail<CommandReceipt>(ErrorCode::InvalidArgument, "Unknown component action");
      auto valid = CheckPayload(custom->payload, registration->payload_schema);
      if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
      const ContentActionContext context{actor, command, custom->target, block};
      auto plan = co_await registration->prepare(context, custom->payload);
      if (!plan.Succeeded()) co_return Result::Failure(plan.Error());
      prepared.content_type = block->type;
      std::visit([&](auto&& effect) {
        using T = std::decay_t<decltype(effect)>;
        if constexpr (std::is_same_v<T, ArtifactAction>) prepared.artifact = std::move(effect);
        else prepared.request = std::move(effect);
      }, std::move(plan).Value());
    } else {
      prepared.artifact = std::get<ArtifactAction>(action);
    }
    if (prepared.artifact) {
      auto bound = CheckBindings(prepared, custom, block.get());
      if (!bound.Succeeded()) co_return Result::Failure(bound.Error());
      const auto& target = *prepared.artifact;
      auto artifact = co_await ReadActionArtifact(id, user, target.artifact);
      if (!artifact.Succeeded()) co_return Result::Failure(artifact.Error());
      access = co_await CheckOperation(ArtifactPermit(session, user, target), confirmation, {});
      if (!access.Succeeded()) co_return Result::Failure(access.Error());
      const auto registration = artifact_actions.find({artifact.Value().record.metadata.type, target.action});
      if (registration == artifact_actions.end()) co_return Fail<CommandReceipt>(ErrorCode::InvalidArgument, "Unknown artifact action");
      auto valid = CheckPayload(target.payload, registration->second.payload_schema);
      if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
      const ArtifactActionContext context{actor, command, id, target.artifact};
      auto plan = co_await registration->second.prepare(context, target.payload);
      if (!plan.Succeeded()) co_return Result::Failure(plan.Error());
      prepared.request = std::move(plan).Value();
    }
  } catch (...) {
    ReportException(std::current_exception());
    co_return Fail<CommandReceipt>(ErrorCode::InvalidState, "The host could not prepare the action");
  }
  if (epoch != access_revision || binding != session.summary.binding_revision)
    co_return Fail<CommandReceipt>(ErrorCode::Conflict, "Conversation changed while preparing the action");
  auto execution = std::make_shared<const RenderedExecution>(RenderedExecution{std::move(key), std::move(prepared), {}});
  co_return co_await ApplyRenderedAction(std::move(user), std::move(command), std::move(execution));
}

Async<CommandReceipt> RuntimeState::ApplyRenderedAction(PrincipalId user, CommandId command,
                                                         std::shared_ptr<const RenderedExecution> execution) {
  const auto id = execution->key.session;
  const auto& request = execution->prepared.request;
  if (const auto* input = std::get_if<SendInput>(&request)) {
    co_return ActionReceipt(co_await Send(shared_from_this(), id, user, command, *input, {}, execution));
  }
  if (const auto* steer = std::get_if<SteerRequest>(&request)) {
    co_return ActionReceipt(co_await Steer(shared_from_this(), id, user, command, steer->expected_run, steer->input, execution));
  }
  co_return ActionReceipt(co_await Submit(shared_from_this(), id, user, command, std::get<InteractionReply>(request), execution));
}

Async<bool> RuntimeState::CheckRenderedCommit(const RenderedExecution& execution, const PrincipalId& user,
                                                const CommandId& command, PolicyRequest target,
                                                std::optional<RunStartBinding> start) {
  using Result = AgentResult<bool>;
  const auto& key = execution.key;
  const auto& prepared = execution.prepared;
  const auto& approved = execution.approved;
  auto& session = sessions.at(key.session);
  const auto epoch = access_revision;
  const auto binding = session.summary.binding_revision;
  std::vector<Capability> confirmation;
  const auto* custom = std::get_if<CustomBlockAction>(&*key.rendered_action);
  std::shared_ptr<const CustomBlock> block;
  if (custom) {
    auto resolved = co_await ResolveRenderedBlock(key.session, user, custom->target);
    if (!resolved.Succeeded()) co_return Result::Failure(resolved.Error());
    block = std::move(resolved).Value();
    if (block->type != prepared.content_type)
      co_return Fail<bool>(ErrorCode::Conflict, "Component type version changed");
    const auto* registration = FindContentAction(*this, *block, custom->action);
    if (!registration) co_return Fail<bool>(ErrorCode::Conflict, "Component action registration changed");
    auto valid = CheckPayload(custom->payload, registration->payload_schema);
    if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
    auto access = co_await CheckOperation(ContentPermit(session, user, *custom), confirmation, approved);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    target.content_action = *custom;
  }
  auto bound = CheckBindings(prepared, custom, block.get());
  if (!bound.Succeeded()) co_return Result::Failure(bound.Error());
  if (prepared.artifact) {
    const auto& action = *prepared.artifact;
    if (!custom && std::get<ArtifactAction>(*key.rendered_action) != action)
      co_return Fail<bool>(ErrorCode::Conflict, "Artifact action no longer matches its command");
    auto artifact = co_await ReadActionArtifact(key.session, user, action.artifact);
    if (!artifact.Succeeded()) co_return Result::Failure(artifact.Error());
    const auto registration = artifact_actions.find({artifact.Value().record.metadata.type, action.action});
    if (registration == artifact_actions.end())
      co_return Fail<bool>(ErrorCode::Conflict, "Artifact action registration changed");
    auto valid = CheckPayload(action.payload, registration->second.payload_schema);
    if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
    auto access = co_await CheckOperation(ArtifactPermit(session, user, action), confirmation, approved);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    target.artifact_action = action;
  }
  auto access = co_await CheckOperation(std::move(target), confirmation, approved);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  const auto current = User();
  if (!current.Succeeded() || current.Value() != user) co_return Fail<bool>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (epoch != access_revision || binding != session.summary.binding_revision || session.summary.lifecycle != SessionLifecycle::Open ||
      (approved && (approved->target_binding != binding || approved->run_start != start))) {
    co_return Fail<bool>(ErrorCode::Conflict, "The action target changed");
  }
  auto prior = Find(user, command, key);
  if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
  if (approved && (!prior.Value() || prior.Value()->progress.state != CommandState::Scheduled || prior.Value()->progress.result))
    co_return Fail<bool>(ErrorCode::AlreadyClosed, "The approved action is no longer scheduled");
  if (!approved && prior.Value()) co_return Result::Success(false);

  if (!confirmation.empty()) {
    ApprovalPreview preview{.title = "Confirm action",
        .summary = custom ? custom->action : prepared.artifact->action};
    if (prepared.artifact) preview.artifacts = {prepared.artifact->artifact};
    std::optional<TimePoint> expires;
    if (const auto* input = std::get_if<SendInput>(&prepared.request)) {
      if (session.summary.active_run || session.consuming || session.summary.queued_inputs)
        co_return Fail<bool>(ErrorCode::Busy, "Session has active or queued work");
      preview.summary += "\n" + input->text;
    } else if (const auto* steer = std::get_if<SteerRequest>(&prepared.request)) {
      const auto& run = runs.at(steer->expected_run);
      if (Terminal(run.view.state)) co_return Fail<bool>(ErrorCode::AlreadyClosed, "Target run closed");
      if (session.summary.active_run != steer->expected_run) co_return Fail<bool>(ErrorCode::Conflict, "Active run changed");
      if (run.view.state == RunState::Cancelling) co_return Fail<bool>(ErrorCode::InvalidState, "Target run is cancelling");
      expires = run.deadline;
      preview.summary += "\n" + steer->input.text;
    } else if (const auto* reply = std::get_if<InteractionReply>(&prepared.request)) {
      const auto form = std::ranges::find(session.interactions, reply->id, &InteractionSnapshot::id);
      if (form == session.interactions.end() || form->state != InteractionState::Pending || form->version != reply->expected_version)
        co_return Fail<bool>(ErrorCode::AlreadyClosed, "The bound interaction is no longer pending");
      expires = form->expires_at;
      if (const auto* request = std::get_if<FormRequest>(&form->body)) preview.summary += "\n" + request->title;
      if (const auto* value = std::get_if<JsonValue>(&reply->response)) preview.summary += "\n" + ToWireJson(*value).dump();
    }
    OperationPlan plan{std::move(confirmation), binding, {}, start, {}, prepared};
    auto staged = co_await StageOperation(user, command, key, key.session, std::move(plan), std::move(preview), expires);
    if (!staged.Succeeded()) co_return Result::Failure(staged.Error());
    co_return Result::Success(false);
  }
  co_return Result::Success(true);
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

Async<CommandReceipt> Conversation::DispatchContentActionAsync(CommandId command, CustomBlockAction action) {
  return [](std::shared_ptr<detail::RuntimeState> owner, SessionId session, PrincipalId user,
            CommandId command, CustomBlockAction action) -> Async<CommandReceipt> {
    co_return co_await owner->DispatchRenderedAction(session, user, std::move(command), std::move(action));
  }(state_, id_, principal_, std::move(command), std::move(action));
}

Async<Receipt<RunAccepted>> Conversation::DispatchArtifactActionAsync(CommandId command, ArtifactAction action) {
  return [](std::shared_ptr<detail::RuntimeState> owner, SessionId session, PrincipalId user,
            CommandId command, ArtifactAction action) -> Async<Receipt<RunAccepted>> {
    auto result = co_await owner->DispatchRenderedAction(session, user, command, std::move(action));
    if (!result.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(result.Error());
    co_return owner->ReceiptFor<RunAccepted>(user, command);
  }(state_, id_, principal_, std::move(command), std::move(action));
}

} // namespace huxerui::agentkit
