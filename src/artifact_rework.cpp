#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

const ToolId rework_stage{"agentkit/rework"};
const ToolId rework_write{"agentkit/rework-write"};
const ToolId revert_write{"agentkit/revert"};
const ToolId adopt_write{"agentkit/adopt-candidate"};

std::optional<RunId> BoundActor(const CommandInput& input) {
  return input.run.value.empty() ? std::nullopt : std::optional{input.run};
}

const std::vector<TargetBaseline>& ChangeTargets(const CommandInput& input) {
  if (input.adoption) return input.adoption->targets;
  return input.rework ? input.rework->targets : input.revert->expected_current;
}

Capability ChangeCapability(const CommandInput& input) {
  if (input.adoption) return Capability::AdoptCandidate;
  return input.rework ? Capability::ReworkArtifact : Capability::RevertChange;
}

const ToolId& ToolFor(const CommandInput& input) {
  if (input.adoption) return adopt_write;
  if (input.revert) return revert_write;
  return input.rework->targets.empty() ? rework_stage : rework_write;
}

std::vector<SourceSlice> ReworkSources(const ReworkRequest& request) {
  std::vector<SourceSlice> result{{request.baseline, request.parts}};
  for (const auto& replacement : request.replacements) result.push_back(replacement.replacement);
  return result;
}

std::vector<SourceSlice> ChangeSources(const CommandInput& input) {
  if (input.rework) return ReworkSources(*input.rework);
  std::vector<SourceSlice> result;
  if (input.adoption) result.push_back({input.adoption->candidate_artifact, input.adoption->parts});
  for (const auto& target : ChangeTargets(input)) {
    if (target.expected) result.push_back({*target.expected});
  }
  return result;
}

bool Latest(const SessionData& session, const ArtifactRef& baseline) {
  return std::ranges::none_of(session.artifacts, [&](const auto& entry) {
    const auto& ref = entry.record.metadata.ref;
    return ref.id == baseline.id && ref.version > baseline.version;
  });
}

bool AvailableRun(const RunExecution& run) {
  return !run.stop.stop_requested() && run.view.state == RunState::Running && !run.stream && !run.host_model_busy &&
      run.pending_calls.empty() && !run.dispatched_host_tool && run.tool_attempts.empty() &&
      std::ranges::none_of(run.tool_requests, [](const auto& entry) { return !entry.second.result && !entry.second.failure; });
}

AgentResult<void> ValidateShape(const CommandInput& input) {
  std::set<ResourceId> targets;
  for (const auto& target : ChangeTargets(input)) {
    if (target.target.value.empty() || !targets.insert(target.target).second ||
        (target.expected && (target.expected->id != target.target || target.expected->version.empty())))
      return Fail<void>(ErrorCode::InvalidArgument, "Invalid change target or baseline");
  }
  if (input.adoption) {
    const auto& request = *input.adoption;
    const auto distinct = std::set<PartId>(request.parts.begin(), request.parts.end());
    if (request.review.value.empty() || !request.expected_review || request.candidate.value.empty() ||
        request.candidate_artifact.id.value.empty() || !request.candidate_artifact.version ||
        request.target_session.value.empty() || targets.empty() || distinct.size() != request.parts.size() ||
        std::ranges::any_of(request.parts, [](const auto& part) { return part.value.empty(); })) {
      return Fail<void>(ErrorCode::InvalidArgument, "Specify a comparison revision, candidate version and explicit target baselines");
    }
    return AgentResult<void>::Success();
  }
  if (input.revert) {
    if (input.session.value.empty() || input.revert->original_call.value.empty() || targets.empty())
      return Fail<void>(ErrorCode::InvalidArgument, "Revert requires the original call and explicit resource baselines");
    return AgentResult<void>::Success();
  }
  const auto& request = *input.rework;
  const auto part_set = std::set<PartId>(request.parts.begin(), request.parts.end());
  if (request.session.value.empty() || request.baseline.id.value.empty() || !request.baseline.version ||
      request.replacements.empty() || request.parts.size() != part_set.size() ||
      std::ranges::any_of(request.parts, [](const auto& part) { return part.value.empty(); })) {
    return Fail<void>(ErrorCode::InvalidArgument, "Provide a baseline, selected scope and distinct source replacements");
  }
  for (std::size_t i = 0; i < request.replacements.size(); ++i) {
    const auto& replacement = request.replacements[i];
    if (replacement.previous == replacement.replacement ||
        std::any_of(request.replacements.begin(), request.replacements.begin() + i, [&](const auto& prior) {
          return prior.previous == replacement.previous;
        })) return Fail<void>(ErrorCode::InvalidArgument, "Each prior source must have one different replacement");
  }
  return AgentResult<void>::Success();
}

AgentResult<std::vector<ResourceEffect>> RevertEffects(const RuntimeState& runtime, const RevertRequest& request) {
  using Result = AgentResult<std::vector<ResourceEffect>>;
  const auto* image = runtime.Published(runtime.sessions.at(request.session));
  if (!image) return Fail<std::vector<ResourceEffect>>(ErrorCode::NotFound, "The original call is unavailable");
  const auto call = std::ranges::find(image->tools, request.original_call, &ToolCallSnapshot::id);
  if (call == image->tools.end()) return Fail<std::vector<ResourceEffect>>(ErrorCode::NotFound, "The original call is unavailable");
  if (call->state == ToolCallState::Executing || call->state == ToolCallState::ResultUnknown) {
    return Fail<std::vector<ResourceEffect>>(ErrorCode::ResultUnknown, "Reconcile the original call before preparing a revert");
  }
  if (call->state != ToolCallState::Succeeded && call->state != ToolCallState::PartiallyApplied) {
    return Fail<std::vector<ResourceEffect>>(ErrorCode::Unsupported, "The original call has no known applied effects");
  }
  std::vector<ResourceEffect> effects;
  for (const auto& target : request.expected_current) {
    if (std::ranges::any_of(image->tools, [&](const auto& reversal) {
          return reversal.reverts == request.original_call && reversal.state == ToolCallState::ResultUnknown &&
              (!reversal.issued_targets || std::ranges::any_of(*reversal.issued_targets, [&](const auto& issued) {
                return issued.target == target.target;
              }));
        })) {
      return Fail<std::vector<ResourceEffect>>(ErrorCode::ResultUnknown, "Reconcile the previous revert for this resource before preparing another");
    }
    const auto effect = std::ranges::find(call->effects, target.target, &ResourceEffect::resource);
    if (effect != call->effects.end() && effect->state == ResourceEffectState::Unknown) {
      return Fail<std::vector<ResourceEffect>>(ErrorCode::ResultUnknown, "Reconcile the selected resource before preparing a revert");
    }
    if (effect == call->effects.end() || effect->state != ResourceEffectState::Applied || !effect->undo ||
        effect->undo->support == UndoSupport::None || effect->undo->handle.empty()) {
      return Fail<std::vector<ResourceEffect>>(ErrorCode::Unsupported, "The selected resource has no supported restore record");
    }
    if (std::ranges::count(call->effects, target.target, &ResourceEffect::resource) != 1 ||
        (effect->before && (effect->before->id != target.target || effect->before->version.empty()))) {
      return Fail<std::vector<ResourceEffect>>(ErrorCode::ResultUnknown, "The original resource effect is inconsistent");
    }
    if (effect->after != target.expected) {
      return Fail<std::vector<ResourceEffect>>(ErrorCode::Conflict, "The selected baseline differs from the original call's applied version");
    }
    effects.push_back(*effect);
  }
  return Result::Success(std::move(effects));
}

AgentResult<void> CheckChangeGrants(const std::vector<TargetBaseline>& targets,
                                  const std::vector<ResourceAccess>& grants) {
  for (const auto& target : targets) {
    const auto granted = std::ranges::find(grants, target.target, &ResourceAccess::resource);
    if (granted == grants.end()) return Fail<void>(ErrorCode::PermissionDenied, "The change target is unavailable");
    if (granted->current != target.expected) return Fail<void>(ErrorCode::Conflict, "The change target baseline changed");
  }
  return AgentResult<void>::Success();
}

AgentError WholeArtifact(const ArtifactRecord& artifact) {
  auto error = Error(ErrorCode::ScopeExpansionRequired, "The source affects the entire artifact");
  error.proposed_scope = ScopeExpansion{.whole_artifact = true, .summary = "Confirm regeneration of the entire artifact"};
  for (const auto& part : artifact.parts) error.proposed_scope->affected_parts.push_back(part.id);
  return error;
}

AgentResult<std::vector<ArtifactDependency>> ReworkLineage(const ReworkRequest& request, const ArtifactRecord& artifact) {
  using Result = AgentResult<std::vector<ArtifactDependency>>;
  const bool whole = request.parts.empty() && request.accept_whole_artifact_scope;
  if (request.parts.empty() && !whole) return Result::Failure(WholeArtifact(artifact));
  std::vector<bool> matched(request.replacements.size());
  auto dependencies = artifact.dependencies;
  for (auto& dependency : dependencies) {
    const auto replacement = std::ranges::find(request.replacements, dependency.input, &SourceReplacement::previous);
    if (replacement == request.replacements.end()) continue;
    if (!whole && !dependency.output_part) return Result::Failure(WholeArtifact(artifact));
    if (!whole && std::ranges::find(request.parts, *dependency.output_part) == request.parts.end()) continue;
    matched[static_cast<std::size_t>(replacement - request.replacements.begin())] = true;
    dependency.input = replacement->replacement;
  }
  if (!std::ranges::all_of(matched, [](bool matched) { return matched; }))
    return Fail<std::vector<ArtifactDependency>>(ErrorCode::Conflict, "A previous source is outside the selected artifact scope");
  return Result::Success(std::move(dependencies));
}

Async<ArtifactRecord> ReworkBaseline(RuntimeState& state, const ReworkRequest& request, const ActionParty& actor) {
  using Result = AgentResult<ArtifactRecord>;
  auto artifact = co_await state.ReadArtifact(request.baseline, actor);
  if (!artifact.Succeeded()) co_return Result::Failure(artifact.Error());
  if (artifact.Value().record.session != request.session)
    co_return Fail<ArtifactRecord>(ErrorCode::PermissionDenied, "Rework must target the artifact's owning conversation");
  const auto& session = state.sessions.at(request.session);
  const auto epoch = state.access_revision;
  auto readable = co_await state.ValidateInputSources(session, actor, ReworkSources(request));
  if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
  auto& record = artifact.Value().record;
  if (std::ranges::any_of(request.parts, [&](const auto& id) {
        return std::ranges::find(record.parts, id, &ArtifactPart::id) == record.parts.end();
      })) co_return Fail<ArtifactRecord>(ErrorCode::NotFound, "The selected artifact part is unavailable");
  if (epoch != state.access_revision || !Latest(session, request.baseline))
    co_return Fail<ArtifactRecord>(ErrorCode::Conflict, "The artifact baseline or source access changed");
  co_return Result::Success(std::move(record));
}

const CommandRecord* ChangeCommand(const RuntimeState& state, const PrincipalId& user, const JsonValue& arguments,
                                   const ToolCallId& call, const ToolId& tool) {
  const auto* command = std::get_if<std::string>(&arguments.value);
  if (!command) return nullptr;
  const auto found = state.commands.find({user, CommandId{*command}});
  if (found == state.commands.end() || !found->second.artifact_change ||
      (!found->second.input.rework && !found->second.input.revert && !found->second.input.adoption) ||
      found->second.artifact_change->call != call || found->second.progress.state != CommandState::Scheduled ||
      ToolFor(found->second.input) != tool) return nullptr;
  return &found->second;
}

bool ValidReplacementDraft(const ArtifactDraft& draft, const ArtifactRecord& baseline, const ReworkRequest& request) {
  if (draft.id != request.baseline.id || draft.expected_latest != request.baseline.version ||
      draft.type != baseline.metadata.type || draft.parts.size() != baseline.parts.size()) return false;
  for (std::size_t index = 0; index < baseline.parts.size(); ++index) {
    const auto& original = baseline.parts[index];
    if (draft.parts[index].id != original.id) return false;
    const bool selected = request.parts.empty() || std::ranges::find(request.parts, original.id) != request.parts.end();
    if (!selected && draft.parts[index].label != original.label) return false;
  }
  return true;
}

void CheckWriteResults(ToolExecutionResult& result, const std::vector<TargetBaseline>& targets) {
  std::vector<ResourceEffect*> effects;
  for (auto& effect : result.effects) effects.push_back(&effect);
  for (auto& draft : result.new_artifacts) {
    if (draft.published_effect) effects.push_back(&*draft.published_effect);
  }
  std::set<ResourceId> accounted;
  bool invalid = false;
  for (auto* value : effects) {
    auto& effect = *value;
    const auto target = std::ranges::find(targets, effect.resource, &TargetBaseline::target);
    accounted.insert(effect.resource);
    const bool valid = target != targets.end() && std::ranges::count(effects, effect.resource, &ResourceEffect::resource) == 1 &&
        effect.before == target->expected &&
        (!effect.after || (effect.after->id == effect.resource && !effect.after->version.empty()));
    if (!valid) effect.state = ResourceEffectState::Unknown;
    invalid = invalid || !valid || (result.outcome == ToolOutcome::Succeeded && effect.state != ResourceEffectState::Applied);
  }
  if (invalid || (result.outcome == ToolOutcome::Succeeded && accounted.size() != targets.size())) {
    result.new_artifacts.clear();
    const bool unknown = !result.effects.empty();
    result.outcome = unknown ? ToolOutcome::ResultUnknown : ToolOutcome::Failed;
    result.reason = Error(unknown ? ErrorCode::ResultUnknown : ErrorCode::InvalidArgument,
        "The change did not account for its approved write scope");
  }
}

} // namespace

bool ArtifactChangeTool(const ToolId& id) { return id == rework_stage || id == rework_write || id == revert_write || id == adopt_write; }

AgentResult<void> RuntimeState::BindArtifactChangeInvocation(ToolInvocation& invocation) const {
  const auto* record = ChangeCommand(*this, invocation.actor.principal, invocation.arguments, invocation.call, invocation.tool.id);
  if (!record) return Fail<void>(ErrorCode::AlreadyClosed, "The artifact change has ended");
  auto actor = Actor(BoundActor(record->input), ChangeCapability(record->input));
  if (!actor.Succeeded()) return AgentResult<void>::Failure(actor.Error());
  if (actor.Value().principal != invocation.actor.principal)
    return Fail<void>(ErrorCode::PermissionDenied, "The acting principal changed");
  invocation.actor = std::move(actor).Value();
  invocation.prepared_targets = ChangeTargets(record->input);
  if (record->input.revert) invocation.reverts = record->input.revert->original_call;
  if (record->input.adoption) invocation.candidate_artifact = record->input.adoption->candidate_artifact;
  return AgentResult<void>::Success();
}

Async<PolicyDecision> RuntimeState::ArtifactChangeAccess(CommandInput request,
                                                 std::optional<PlanId> plan) {
  using Result = AgentResult<PolicyDecision>;
  const auto bound = BoundActor(request);
  auto actor = Actor(bound, ChangeCapability(request));
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  const auto found = sessions.find(request.session);
  if (found == sessions.end() || found->second.summary.lifecycle != SessionLifecycle::Open)
    co_return Fail<PolicyDecision>(ErrorCode::NotFound, "The target conversation is unavailable");
  auto permission = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, found->second));
  if (!permission.Succeeded()) co_return Result::Failure(permission.Error());
  if (bound) {
    const auto* scope = Scope(runs.at(*bound));
    if (scope && std::ranges::any_of(ChangeTargets(request), [&](const auto& target) {
          return std::ranges::find(scope->writable_resources, target.target) == scope->writable_resources.end();
        })) co_return Fail<PolicyDecision>(ErrorCode::PermissionDenied, "The artifact change exceeds the work request's write scope");
  }
  auto policy = CollaborationAccess(actor.Value(), ChangeCapability(request), found->second);
  policy.plan = std::move(plan);
  policy.sources = ChangeSources(request);
  if (request.revert) policy.operation = JsonValue{request.revert->original_call.value};
  if (request.adoption) {
    policy.target.review = request.adoption->review;
    policy.operation = JsonValue{request.adoption->candidate.value};
  }
  for (const auto& target : ChangeTargets(request)) policy.target.resources.push_back(target.target);
  co_return co_await Decide(std::move(policy));
}

Async<PolicyDecision> RuntimeState::DecideToolExecution(PolicyRequest request) {
  if (!request.tool || !ArtifactChangeTool(request.tool->id)) co_return co_await Decide(std::move(request));
  if (!request.invocation) co_return Fail<PolicyDecision>(ErrorCode::InvalidArgument, "The artifact change invocation is required");
  const auto& invocation = *request.invocation;
  const auto* record = ChangeCommand(*this, request.actor.principal, invocation.arguments, invocation.call, request.tool->id);
  if (!record) co_return Fail<PolicyDecision>(ErrorCode::AlreadyClosed, "The artifact change has ended");
  if (record->input.adoption) {
    const auto& adoption = *record->input.adoption;
    if (!AdoptionBound(sessions.at(adoption.target_session), adoption, invocation.call))
      co_return Fail<PolicyDecision>(ErrorCode::Conflict, "The comparison no longer selects this adoption");
    auto source = co_await ReadAdoptionCandidate(adoption, invocation.actor);
    if (!source.Succeeded()) co_return AgentResult<PolicyDecision>::Failure(source.Error());
  }
  auto access = co_await ArtifactChangeAccess(record->input, request.plan);
  if (!access.Succeeded() || access.Value().decision == AccessDecision::Deny) co_return access;
  auto checked = co_await Decide(std::move(request));
  if (!checked.Succeeded() || checked.Value().decision == AccessDecision::Deny) co_return checked;
  if (access.Value().decision == AccessDecision::RequireConfirmation ||
      (access.Value().decision == AccessDecision::AllowReview && checked.Value().decision == AccessDecision::Allow))
    co_return access;
  if (record->input.adoption && checked.Value().decision == AccessDecision::Allow) {
    co_return AgentResult<PolicyDecision>::Success({AccessDecision::RequireConfirmation,
        "Review the selected candidate against the target baseline"});
  }
  co_return checked;
}

void RuntimeState::InstallArtifactChangeTools() {
  for (const auto& id : {rework_stage, rework_write, revert_write, adopt_write}) {
    if ((id == adopt_write && !options.changes.prepare_adoption) || (id == revert_write && !options.changes.prepare_revert) ||
        ((id == rework_stage || id == rework_write) && !options.changes.prepare_rework)) continue;
    auto tool = ToolDefinition{
        .id = id, .version = 1, .description = id == adopt_write ? "Apply the reviewed candidate" :
            id == revert_write ? "Apply the reviewed reverse change" : "Apply the reviewed artifact rework",
        .input_schema = {JsonValue{JsonValue::Object{{"type", JsonValue{std::string("string")}}}}},
        .effect = id == rework_stage ? ToolEffect::StageArtifact : ToolEffect::WriteResource,
        .prepare = [weak = weak_from_this(), id](ToolPrepareContext context, ToolRequest request) -> Async<PreparedChange> {
          auto self = weak.lock();
          const auto* record = self ? ChangeCommand(*self, context.actor.principal, request.arguments, context.call, id) : nullptr;
          if (!record) co_return Fail<PreparedChange>(ErrorCode::AlreadyClosed, "The artifact change has ended");
          auto actor = self->Actor(BoundActor(record->input), ChangeCapability(record->input));
          if (!actor.Succeeded()) co_return AgentResult<PreparedChange>::Failure(actor.Error());
          if (record->input.adoption) {
            const auto& request = *record->input.adoption;
            if (!self->AdoptionBound(self->sessions.at(request.target_session), request, context.call))
              co_return Fail<PreparedChange>(ErrorCode::Conflict, "The comparison no longer selects this adoption");
            auto artifact = co_await self->ReadAdoptionCandidate(request, actor.Value());
            if (!artifact.Succeeded()) co_return AgentResult<PreparedChange>::Failure(artifact.Error());
            auto grants = CheckChangeGrants(request.targets, context.readable);
            if (!grants.Succeeded()) co_return AgentResult<PreparedChange>::Failure(grants.Error());
          } else if (record->input.revert) {
            auto effects = RevertEffects(*self, *record->input.revert);
            if (!effects.Succeeded()) co_return AgentResult<PreparedChange>::Failure(effects.Error());
            auto grants = CheckChangeGrants(record->input.revert->expected_current, context.readable);
            if (!grants.Succeeded()) co_return AgentResult<PreparedChange>::Failure(grants.Error());
          } else {
            auto baseline = co_await ReworkBaseline(*self, *record->input.rework, actor.Value());
            if (!baseline.Succeeded()) co_return AgentResult<PreparedChange>::Failure(baseline.Error());
          }
          co_return AgentResult<PreparedChange>::Success(record->artifact_change->change);
        },
        .execute = [weak = weak_from_this()](ToolContext context, PreparedToolCall call) -> Async<ToolExecutionResult> {
          using Result = AgentResult<ToolExecutionResult>;
          const auto reject = [](AgentError reason) {
            return Result::Success({.outcome = ToolOutcome::Failed, .reason = std::move(reason)});
          };
          auto self = weak.lock();
          const auto user = self ? self->User() : Fail<PrincipalId>(ErrorCode::RuntimeClosed, "Agent runtime closed");
          if (!user.Succeeded()) co_return reject(user.Error());
          const auto* record = ChangeCommand(*self, user.Value(), call.request.arguments, call.call, call.tool.id);
          if (!record) co_return reject(Error(ErrorCode::AlreadyClosed, "The artifact change has ended"));
          if (record->input.adoption) {
            const auto request = *record->input.adoption;
            if (!self->AdoptionBound(self->sessions.at(request.target_session), request, call.call))
              co_return reject(Error(ErrorCode::Conflict, "The comparison no longer selects this adoption"));
            auto actor = self->Actor(BoundActor(record->input), Capability::AdoptCandidate);
            if (!actor.Succeeded()) co_return reject(actor.Error());
            auto source = co_await self->ReadAdoptionCandidate(request, actor.Value());
            if (!source.Succeeded()) co_return reject(source.Error());
            if (context.StopToken().stop_requested()) co_return Result::Success({.outcome = ToolOutcome::Cancelled});
            auto output = co_await self->options.changes.execute_adoption(std::move(context), std::move(call));
            if (!output.Succeeded()) co_return output;
            auto& result = output.Value();
            const SourceSlice selected{request.candidate_artifact, request.parts};
            for (auto& draft : result.new_artifacts) {
              if (std::ranges::none_of(draft.dependencies, [&](const auto& dependency) { return dependency.input == selected; }))
                draft.dependencies.push_back({{}, selected});
            }
            CheckWriteResults(result, request.targets);
            co_return output;
          }
          if (record->input.revert) {
            const auto request = *record->input.revert;
            auto effects = RevertEffects(*self, request);
            if (!effects.Succeeded()) co_return reject(effects.Error());
            if (context.StopToken().stop_requested()) co_return Result::Success({.outcome = ToolOutcome::Cancelled});
            auto output = co_await self->options.changes.execute_revert(std::move(context), std::move(call));
            if (output.Succeeded()) CheckWriteResults(output.Value(), request.expected_current);
            co_return output;
          }
          const auto& request = *record->input.rework;
          auto actor = self->Actor(BoundActor(record->input), Capability::ReworkArtifact);
          if (!actor.Succeeded()) co_return reject(actor.Error());
          auto baseline = co_await ReworkBaseline(*self, request, actor.Value());
          if (!baseline.Succeeded()) co_return reject(baseline.Error());
          auto lineage = ReworkLineage(request, baseline.Value());
          if (!lineage.Succeeded()) co_return reject(lineage.Error());
          if (context.StopToken().stop_requested()) co_return Result::Success({.outcome = ToolOutcome::Cancelled});
          auto output = co_await self->options.changes.execute_rework(std::move(context), std::move(call));
          if (!output.Succeeded() || output.Value().outcome != ToolOutcome::Succeeded) co_return output;
          auto& drafts = output.Value().new_artifacts;
          if (drafts.size() != 1 || !ValidReplacementDraft(drafts.front(), baseline.Value(), request))
            co_return Fail<ToolExecutionResult>(ErrorCode::InvalidArgument, "Rework must publish the reviewed artifact version and stable parts");
          drafts.front().dependencies = std::move(lineage).Value();
          drafts.front().changed_parts = request.parts;
          co_return output;
        }};
    tools.emplace(id, std::make_shared<const ToolDefinition>(std::move(tool)));
  }
}

void RuntimeState::StartArtifactChangeTool(RunExecution& run, const CommandId& command, const CommandRecord& record) {
  auto& session = sessions.at(run.view.session);
  const auto& plan = *record.artifact_change;
  const auto tool = ToolFor(record.input);
  const ToolRef ref{tool, 1};
  if (plan.source_revision) run.tool_attempts[plan.call].plan.source_revision = plan.source_revision;
  ++run.view.usage.tool_calls;
  run.tool_requests.emplace(command, HostToolRecord{{tool, JsonValue{command.value}}, ref, record.progress.accepted, plan.call});
  session.tools.push_back({.id = plan.call, .run = run.view.id, .tool = ref, .state = ToolCallState::Preparing,
      .registered_sequence = ++session.sequence});
  if (record.input.revert) session.tools.back().reverts = record.input.revert->original_call;
}

void RuntimeState::RecordRevertEffects(const SessionData& session, ToolCallSnapshot& call, const ActionParty& actor) {
  if (!session.summary.project || (options.store && session.retention == SessionRetention::Ephemeral)) return;
  for (const auto& effect : call.effects) {
    if (effect.state != ResourceEffectState::Applied || !effect.before || effect.before == effect.after) continue;
    SourceChange change{.before = {*effect.before}, .kind = SourceChangeKind::Reverted};
    if (effect.after) change.after = SourceRef{*effect.after};
    auto command = NewCommandId();
    call.source_changes.push_back(command);
    pending_source_changes[session.summary.id].push_back({std::move(command), actor.principal, call.run, std::move(change)});
  }
}

Async<Receipt<ChangePlanAccepted>> RuntimeState::PrepareArtifactChange(CommandId command, CommandInput key) {
  using Result = AgentResult<Receipt<ChangePlanAccepted>>;
  const auto capability = ChangeCapability(key);
  const auto bound = BoundActor(key);
  const auto target = key.session;
  auto actor = Actor(bound, capability);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  const auto user = actor.Value().principal;
  auto shape = ValidateShape(key);
  if (!shape.Succeeded()) co_return Result::Failure(shape.Error());
  auto readable = co_await WithSession(target, user, Capability::ReadSession);
  if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
  std::optional<ArtifactRecord> candidate;
  if (key.adoption) {
    auto selected = co_await ReadAdoptionCandidate(*key.adoption, actor.Value());
    if (!selected.Succeeded()) co_return Result::Failure(selected.Error());
    candidate = std::move(selected).Value();
  }
  auto duplicate = Find(user, command, key);
  if (!duplicate.Succeeded()) co_return Result::Failure(duplicate.Error());
  if (duplicate.Value()) {
    auto replay = co_await ReplayReceipt(ReceiptFor<ChangePlanAccepted>(user, command));
    if (replay.Succeeded() && bound) {
      const auto& accepted = std::get<ChangePlanAccepted>(replay.Value().outcome);
      if (accepted.run == bound && accepted.call) ScheduleHostedTool(runs.at(*bound), *accepted.call);
    }
    co_return replay;
  }
  auto [preparing, inserted] = preparing_actions.emplace(std::pair{user, command}, key);
  if (!inserted) {
    co_return Fail<Receipt<ChangePlanAccepted>>(preparing->second == key ? ErrorCode::Busy : ErrorCode::Conflict,
                                                "This command is already being prepared");
  }
  const auto release = std::unique_ptr<const std::pair<PrincipalId, CommandId>,
      std::function<void(const std::pair<PrincipalId, CommandId>*)>>(
          &preparing->first, [this](const auto* entry) { preparing_actions.erase(*entry); });
  if ((key.rework && !options.changes.prepare_rework) ||
      (key.revert && (!options.changes.prepare_revert || !options.tool_resources)) ||
      (key.adoption && (!options.changes.prepare_adoption || !options.tool_resources)))
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::Unsupported, "The host does not support this artifact change");
  auto access = co_await ArtifactChangeAccess(key);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  if (access.Value().decision == AccessDecision::Deny)
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::PermissionDenied, access.Value().reason);
  auto& session = sessions.at(target);
  const auto source_revision = ProjectSourceRevision(session);
  const auto same_run = bound && session.summary.active_run == bound;
  if (same_run && !AvailableRun(runs.at(*bound)))
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::Busy, "The current run already has work in progress");
  const auto run = same_run ? *bound : RunId{MakeId()};
  const ToolCallId call{MakeId()};
  const auto tool = ToolFor(key);
  ToolInvocation invocation{.actor = actor.Value(), .phase = ToolAccessPhase::Prepare, .session = target,
      .run = run, .call = call, .project = session.summary.project, .workspace = session.summary.workspace,
      .tool = {tool, 1}, .effect = ChangeTargets(key).empty() ? ToolEffect::StageArtifact : ToolEffect::WriteResource,
      .arguments = JsonValue{command.value}, .prepared_targets = ChangeTargets(key)};
  if (key.revert) invocation.reverts = key.revert->original_call;
  if (key.adoption) invocation.candidate_artifact = key.adoption->candidate_artifact;
  invocation.actor.call = call;
  auto prepare = CollaborationAccess(actor.Value(), Capability::PrepareTool, session);
  prepare.tool = invocation.tool;
  const auto sources = ChangeSources(key);
  prepare.sources = sources;
  for (const auto& resource : ChangeTargets(key)) prepare.target.resources.push_back(resource.target);
  prepare.invocation = invocation;
  const auto revision = access_revision;
  auto permitted = co_await Require(std::move(prepare));
  if (!permitted.Succeeded()) co_return Result::Failure(permitted.Error());
  ChangePrepareContext material;
  if (key.adoption) {
    auto checked = CheckAdoptionSlot(session, *key.adoption);
    if (!checked.Succeeded()) co_return Result::Failure(checked.Error());
    material.artifact = std::move(candidate);
  } else if (key.rework) {
    auto baseline = co_await ReworkBaseline(*this, *key.rework, actor.Value());
    if (!baseline.Succeeded()) co_return Result::Failure(baseline.Error());
    auto lineage = ReworkLineage(*key.rework, baseline.Value());
    if (!lineage.Succeeded()) co_return Result::Failure(lineage.Error());
    material.artifact = std::move(baseline).Value();
  } else {
    auto visible = co_await ValidateInputSources(session, actor.Value(), sources);
    if (!visible.Succeeded()) co_return Result::Failure(visible.Error());
    auto effects = RevertEffects(*this, *key.revert);
    if (!effects.Succeeded()) co_return Result::Failure(effects.Error());
    material.original_effects = std::move(effects).Value();
  }
  std::vector<ResourceAccess> granted;
  if (options.tool_resources) {
    auto resources = co_await options.tool_resources(invocation);
    if (!resources.Succeeded()) co_return Result::Failure(resources.Error());
    granted = std::move(resources).Value();
    if (std::ranges::any_of(granted, [](const auto& entry) { return entry.mode == ResourceAccessMode::Write; }))
      co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::PermissionDenied, "Preparation requires read-only access");
  }
  auto inputs_to_review = sources;
  if (key.rework) {
    for (const auto& replacement : key.rework->replacements) {
      std::vector<SourceSlice> previous{replacement.previous};
      auto visible = co_await ValidateInputSources(session, actor.Value(), previous);
      if (visible.Succeeded()) inputs_to_review.push_back(replacement.previous);
      else if (visible.Error().code != ErrorCode::PermissionDenied && visible.Error().code != ErrorCode::NotFound)
        co_return Result::Failure(visible.Error());
    }
  } else {
    auto current = CheckChangeGrants(ChangeTargets(key), granted);
    if (!current.Succeeded()) co_return Result::Failure(current.Error());
  }
  const auto stop = bound ? runs.at(*bound).stop.get_token() : std::stop_token{};
  material.access = {invocation.actor, call, std::move(granted), stop, std::move(inputs_to_review)};
  auto build = [&]() {
    if (key.adoption) return options.changes.prepare_adoption(std::move(material), *key.adoption);
    if (key.rework) return options.changes.prepare_rework(std::move(material), *key.rework);
    return options.changes.prepare_revert(std::move(material), *key.revert);
  };
  auto prepared = co_await build();
  if (!prepared.Succeeded()) co_return Result::Failure(prepared.Error());
  if (prepared.Value().targets != ChangeTargets(key))
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::Conflict, "The plan changed the requested write scope");
  auto& inputs = prepared.Value().inputs;
  for (const auto& source : sources) {
    if (std::ranges::find(inputs, source) == inputs.end()) inputs.push_back(source);
  }
  permitted = co_await ValidateInputSources(session, actor.Value(), inputs);
  if (!permitted.Succeeded()) co_return Result::Failure(permitted.Error());
  if (source_revision && SourcesChanged(session, *source_revision, inputs))
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::Conflict, "A source used by the change plan changed");
  auto current = Actor(bound, capability);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != user || revision != access_revision || (key.rework && !Latest(session, key.rework->baseline)) ||
      session.summary.lifecycle != SessionLifecycle::Open)
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::Conflict, "The target or authorization changed during preparation");
  duplicate = Find(user, command, key);
  if (!duplicate.Succeeded()) co_return Result::Failure(duplicate.Error());
  if (duplicate.Value()) co_return co_await ReplayReceipt(ReceiptFor<ChangePlanAccepted>(user, command));
  if (same_run && (session.summary.active_run != bound || !AvailableRun(runs.at(*bound))))
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::Busy, "The current run already has work in progress");
  if (same_run && runs.at(*bound).view.usage.tool_calls >= RunBudget(runs.at(*bound)).tool_calls.value_or(128))
    co_return Fail<Receipt<ChangePlanAccepted>>(ErrorCode::BudgetExceeded, "Run reached its tool budget");
  const auto item = same_run ? std::optional<QueueItemId>{} : std::optional{QueueItemId{MakeId()}};
  ProjectData* comparison = nullptr;
  if (key.adoption) {
    auto checked = CheckAdoptionSlot(session, *key.adoption);
    if (!checked.Succeeded()) co_return Result::Failure(checked.Error());
    comparison = &projects.at(*session.summary.project);
    auto& review = comparison->fork_reviews.at(key.adoption->review);
    review.selected = key.adoption->candidate;
    review.adoption_call = call;
    review.adoption_state = ToolCallState::Preparing;
    review.adoption_reconciled = false;
    comparison->fork_adoptions.insert_or_assign(review.id, ForkAdoptionBinding{target, user, command});
    ++review.revision;
    ++comparison->revision;
  }
  ++session.sequence;
  Accept(user, command, std::move(key), session,
      same_run ? ChangePlanAccepted{run, call} : ChangePlanAccepted{.queue_item = item}, CommandState::Scheduled);
  auto& record = commands.at({user, command});
  record.artifact_change = ArtifactChangePlan{run, call, std::move(prepared).Value(), source_revision};
  if (same_run) StartArtifactChangeTool(runs.at(run), command, record);
  else {
    session.queue.push_back({.id = *item, .input = command, .state = QueueItemState::Queued});
    session.queue_owners.emplace(*item, user);
    ++session.summary.queued_inputs;
  }
  auto receipt = ReceiptFor<ChangePlanAccepted>(user, command);
  auto saved = co_await WhenSaved(Persist(session, !same_run, receipt.Value().stamp.commit, nullptr, comparison));
  if (!saved.Succeeded()) co_return Result::Failure(saved.Error());
  if (receipt.Succeeded() && same_run) ScheduleHostedTool(runs.at(run), call);
  if (receipt.Succeeded() && !same_run)
    options.tasks.Launch([self = shared_from_this(), target] { return self->ConsumeQueue(target); });
  co_return receipt;
}

Task<void> RuntimeState::StartQueuedArtifactChange(SessionId id, QueueItemId item, CommandId command) {
  auto& session = sessions.at(id);
  const auto user = session.queue_owners.at(item);
  auto& record = commands.at({user, command});
  const auto& input = record.input;
  auto binding = co_await ResolveStart(session, user, {});
  auto access = co_await ArtifactChangeAccess(input);
  const auto queued = std::ranges::find(session.queue, item, &QueuedInput::id);
  if (stage != Stage::Started || session.summary.active_run || session.summary.lifecycle != SessionLifecycle::Open ||
      queued->state != QueueItemState::Queued) co_return;
  --session.summary.queued_inputs;
  ++session.sequence;
  const auto& plan = *record.artifact_change;
  const bool stale_sources = plan.source_revision && SourcesChanged(session, *plan.source_revision, plan.change.inputs);
  if (!binding.Succeeded() || !access.Succeeded() || access.Value().decision == AccessDecision::Deny || stale_sources ||
      (input.rework && !Latest(session, input.rework->baseline))) {
    queued->state = QueueItemState::Rejected;
    queued->reason = !binding.Succeeded() ? binding.Error() : !access.Succeeded() ? access.Error() :
        access.Value().decision == AccessDecision::Deny ? Error(ErrorCode::PermissionDenied, access.Value().reason) :
        Error(ErrorCode::Conflict, stale_sources ? "A source used by the change plan changed" : "The artifact baseline changed while queued");
    Resolve(record, session, CommandState::Failed, queued->reason);
    Persist(session, false, record.progress.resolution->commit);
    co_return;
  }
  auto& run = RecordRun(session, user, SendInput{.text = input.adoption ? "Adopt the selected candidate" :
                       input.revert ? "Revert approved changes" : "Rework selected artifact parts"},
                       binding.Value(), RunTrigger::ArtifactChange, {}, {}, plan.run);
  queued->state = QueueItemState::Started;
  queued->run = run.view.id;
  record.progress.result = ChangePlanAccepted{run.view.id, plan.call, item};
  StartArtifactChangeTool(run, command, record);
  auto saved = co_await WhenSaved(Persist(session, true));
  if (saved.Succeeded() && stage == Stage::Started && !run.stop.stop_requested()) Launch(run);
  else if (!run.stop.stop_requested()) Finish(run, RunState::Failed, RunEndReason::Interrupted, saved.Succeeded()
      ? Error(ErrorCode::RuntimeClosed, "Agent runtime closed") : saved.Error());
}

void RuntimeState::SettleArtifactChange(CommandRecord& record, SessionData& session, const HostToolRecord& tool) {
  if (record.progress.state != CommandState::Scheduled) return;
  if (tool.result && tool.result->outcome == ToolOutcome::Succeeded)
    Resolve(record, session, CommandState::Applied);
  else {
    const auto reason = tool.failure ? *tool.failure : tool.result && tool.result->reason ? *tool.result->reason :
        Error(ErrorCode::InvalidState, "The artifact change did not complete");
    Resolve(record, session, CommandState::Failed, reason);
  }
}

Task<void> RuntimeState::DriveArtifactChange(RunId id) {
  auto& run = runs.at(id);
  const auto call = run.tool_requests.begin()->second.call;
  auto result = co_await WaitForHostedTool(id, call);
  if (run.stop.stop_requested()) co_return;
  if (result.Succeeded() && result.Value().outcome == ToolOutcome::Succeeded) {
    Finish(run, RunState::Succeeded, RunEndReason::Completed, {},
           RunCompletion{"", {}, result.Value().published_artifacts});
  } else {
    auto reason = result.Succeeded() ? result.Value().reason.value_or(Error(ErrorCode::InvalidState, "The artifact change did not complete"))
                                     : result.Error();
    Finish(run, RunState::Failed, reason.code == ErrorCode::ResultUnknown ? RunEndReason::ToolOutcomeUnknown : RunEndReason::Failed,
           std::move(reason));
  }
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

Async<Receipt<ChangePlanAccepted>> ArtifactChanges::PrepareReworkAsync(CommandId command, ReworkRequest request) {
  return [](std::shared_ptr<detail::RuntimeState> state, CommandId command, ReworkRequest request,
            std::optional<RunId> actor) -> Async<Receipt<ChangePlanAccepted>> {
    detail::CommandInput input{.operation = detail::Operation::Rework, .session = request.session,
        .run = actor.value_or(RunId{}), .rework = std::move(request)};
    co_return co_await state->PrepareArtifactChange(std::move(command), std::move(input));
  }(state_, std::move(command), std::move(request), actor_);
}

Async<Receipt<ChangePlanAccepted>> ArtifactChanges::PrepareRevertAsync(CommandId command, RevertRequest request) {
  return [](std::shared_ptr<detail::RuntimeState> state, CommandId command, RevertRequest request,
            std::optional<RunId> actor) -> Async<Receipt<ChangePlanAccepted>> {
    detail::CommandInput input{.operation = detail::Operation::Revert, .session = request.session,
        .run = actor.value_or(RunId{}), .revert = std::move(request)};
    co_return co_await state->PrepareArtifactChange(std::move(command), std::move(input));
  }(state_, std::move(command), std::move(request), actor_);
}

} // namespace huxerui::agentkit
