#include "tool_turn.h"

#include <algorithm>

namespace huxerui::agentkit::detail {

ToolCallState StateForOutcome(ToolOutcome outcome) {
  switch (outcome) {
  case ToolOutcome::Succeeded: return ToolCallState::Succeeded;
  case ToolOutcome::Failed: return ToolCallState::Failed;
  case ToolOutcome::PartiallyApplied: return ToolCallState::PartiallyApplied;
  case ToolOutcome::Cancelled: return ToolCallState::Cancelled;
  case ToolOutcome::ResultUnknown: return ToolCallState::ResultUnknown;
  }
  return ToolCallState::Failed;
}

namespace {
bool ExternalEffect(ToolEffect effect) {
  return effect == ToolEffect::WriteResource || effect == ToolEffect::ExternalSubmit;
}

bool CoversClaim(const std::vector<ToolResourceClaim>& claims, const ResourceId& key, ResourceAccessMode mode) {
  return std::ranges::any_of(claims, [&](const auto& claim) {
    return claim.key == key && (claim.mode == ResourceAccessMode::Write || mode == ResourceAccessMode::Read);
  });
}

bool ValidClaims(const ToolDefinition& definition, const PreparedChange& change) {
  if (definition.concurrency == ToolConcurrency::Serial || !change.conflict_keys) return true;
  const auto& claims = *change.conflict_keys;
  std::set<ResourceId> keys;
  for (const auto& claim : claims) {
    if (claim.key.value.empty() || !keys.insert(claim.key).second) return false;
    if (claim.mode == ResourceAccessMode::Write && definition.effect == ToolEffect::Read) return false;
  }
  if (!ExternalEffect(definition.effect)) return true;
  return std::ranges::all_of(change.targets, [&](const auto& target) {
    return CoversClaim(claims, target.target, ResourceAccessMode::Write);
  });
}

AgentResult<std::vector<ResourceEffect>> PublicationEffects(const ToolExecutionResult& result, const ToolInvocation& invocation) {
  std::vector<ResourceEffect> effects;
  for (const auto& draft : result.new_artifacts) {
    if (!draft.published_effect) continue;
    const auto& effect = *draft.published_effect;
    const auto matches = [&](const auto& value) { return value.resource == effect.resource; };
    const auto target = std::ranges::find(invocation.prepared_targets, effect.resource, &TargetBaseline::target);
    if ((result.outcome != ToolOutcome::Succeeded && result.outcome != ToolOutcome::PartiallyApplied) ||
        !draft.id || !draft.expected_latest || effect.resource.value.empty() ||
        effect.state != ResourceEffectState::Applied || effect.error || !effect.before || !effect.after ||
        effect.before->id != effect.resource || effect.after->id != effect.resource || effect.before->version.empty() ||
        effect.after->version.empty() || effect.before == effect.after || std::ranges::any_of(effects, matches) ||
        std::ranges::any_of(result.effects, matches) || (ExternalEffect(invocation.effect) &&
            (target == invocation.prepared_targets.end() || target->expected != effect.before))) {
      return Fail<std::vector<ResourceEffect>>(ErrorCode::InvalidArgument, "Invalid resource effect for an artifact publication");
    }
    effects.push_back(effect);
  }
  return AgentResult<std::vector<ResourceEffect>>::Success(std::move(effects));
}

AgentResult<void> CheckReconciliation(const ToolReconciliation& observed, const ToolCallSnapshot& call,
                                      const std::vector<TargetBaseline>& targets) {
  std::set<ResourceId> seen;
  std::size_t applied = 0;
  for (const auto& effect : observed.effects) {
    const auto target = std::ranges::find(targets, effect.resource, &TargetBaseline::target);
    const auto prior = std::ranges::find(call.effects, effect.resource, &ResourceEffect::resource);
    const auto versioned = [&](const auto& ref) { return !ref || (ref->id == effect.resource && !ref->version.empty()); };
    if (effect.resource.value.empty() || !seen.insert(effect.resource).second ||
        !versioned(effect.before) || !versioned(effect.after) || effect.state == ResourceEffectState::Unknown ||
        (target == targets.end() ? prior == call.effects.end() : target->expected != effect.before) ||
        (prior != call.effects.end() && prior->state == ResourceEffectState::Applied && *prior != effect)) {
      return Fail<void>(ErrorCode::Conflict, "The observed effect does not match the original call or its approved scope");
    }
    if (effect.state == ResourceEffectState::Applied) {
      if (effect.before == effect.after || effect.error ||
          (effect.undo && (effect.undo->support == UndoSupport::None || effect.undo->handle.empty())))
        return Fail<void>(ErrorCode::InvalidArgument, "An applied effect requires a confirmed change and a valid restore reference");
      ++applied;
    } else if (effect.undo || (effect.after && effect.after != effect.before)) {
      return Fail<void>(ErrorCode::InvalidArgument, "An unchanged effect must retain the approved baseline");
    }
  }
  if (std::ranges::any_of(targets, [&](const auto& target) { return !seen.contains(target.target); }) ||
      std::ranges::any_of(call.effects, [&](const auto& effect) { return !seen.contains(effect.resource); }))
    return Fail<void>(ErrorCode::ResultUnknown, "Verify every resource from the original call before settling its result");
  if ((observed.outcome == ToolOutcome::PartiallyApplied && (!applied || applied == seen.size())) ||
      ((observed.outcome == ToolOutcome::Cancelled || observed.outcome == ToolOutcome::Failed) && applied) ||
      (observed.outcome == ToolOutcome::Succeeded && applied != seen.size()))
    return Fail<void>(ErrorCode::InvalidArgument, "The outcome does not match the resource effects");
  return AgentResult<void>::Success();
}

Async<ToolResult> PublishToolResult(RuntimeState& runtime, const ToolInvocation& invocation, std::size_t tool_index,
                                    const std::optional<JsonSchema>& output_schema, ToolExecutionResult entry) {
  auto& session = runtime.sessions.at(invocation.session);
  const auto& run = runtime.runs.at(invocation.run);
  const auto tool = [&]() -> ToolCallSnapshot& { return session.tools.at(tool_index); };
  auto publication_effects = PublicationEffects(entry, invocation);
  tool().effects = entry.effects;
  if (invocation.reverts) runtime.RecordRevertEffects(session, tool(), invocation.actor);
  tool().reason = entry.reason;
  tool().state = StateForOutcome(entry.outcome);
  ++session.sequence;
  ToolResult output{
      .call = invocation.call, .outcome = entry.outcome, .model_value = std::move(entry.model_value),
      .blocks = std::move(entry.blocks), .existing_artifacts = std::move(entry.existing_artifacts),
      .effects = std::move(entry.effects), .reason = std::move(entry.reason)};
  const auto publication_failed = [&](AgentError error) {
    const bool applied = std::ranges::any_of(output.effects, [](const auto& effect) { return effect.state == ResourceEffectState::Applied; });
    const bool unknown = output.outcome == ToolOutcome::ResultUnknown || std::ranges::any_of(output.effects,
        [](const auto& effect) { return effect.state == ResourceEffectState::Unknown; });
    output.outcome = unknown ? ToolOutcome::ResultUnknown : applied ? ToolOutcome::PartiallyApplied : ToolOutcome::Failed;
    if (unknown) error.code = ErrorCode::ResultUnknown;
    output.reason = error;
    output.model_value.reset();
    output.blocks.clear();
    output.existing_artifacts.clear();
    tool().reason = std::move(error);
    tool().state = !applied && !unknown && tool().reason->code == ErrorCode::Conflict
        ? ToolCallState::Conflict : StateForOutcome(output.outcome);
    ++session.sequence;
    runtime.Persist(session);
    return AgentResult<ToolResult>::Success(std::move(output));
  };
  if (!publication_effects.Succeeded()) co_return publication_failed(publication_effects.Error());
  if (output.model_value && output_schema) {
    auto checked = ValidateJson(*output.model_value, *output_schema);
    if (!checked.Succeeded()) co_return AgentResult<ToolResult>::Failure(checked.Error());
  }
  for (const auto& existing : output.existing_artifacts) {
    if (std::ranges::none_of(session.artifacts, [&](const auto& item) {
          return item.record.metadata.ref == existing && item.record.availability == ArtifactAvailability::Available;
        })) co_return Fail<ToolResult>(ErrorCode::NotFound, "Referenced tool artifact is unavailable in this session");
    auto permitted = RuntimeState::Request(run.principal, Capability::ReadArtifact, &session, &run);
    permitted.sources.push_back({existing});
    auto allowed = co_await runtime.Require(std::move(permitted));
    if (!allowed.Succeeded()) co_return AgentResult<ToolResult>::Failure(allowed.Error());
  }
  auto valid_blocks = co_await runtime.ValidateToolBlocks(session, run, output.blocks);
  if (!valid_blocks.Succeeded()) {
    tool().reason = valid_blocks.Error();
    if (tool().state == ToolCallState::Succeeded) {
      tool().state = std::ranges::any_of(tool().effects, [](const auto& effect) {
        return effect.state == ResourceEffectState::Applied;
      }) ? ToolCallState::PartiallyApplied : ToolCallState::Failed;
    }
    ++session.sequence;
    auto saved = co_await RuntimeState::WhenSaved(runtime.Persist(session));
    if (!saved.Succeeded()) co_return AgentResult<ToolResult>::Failure(saved.Error());
    co_return AgentResult<ToolResult>::Failure(valid_blocks.Error());
  }
  auto published = co_await runtime.PublishArtifacts(invocation.run, invocation.call, std::move(entry.new_artifacts),
      invocation.actor, invocation.reverts.has_value());
  if (!published.Succeeded()) co_return publication_failed(published.Error());
  output.published_artifacts = std::move(published).Value();
  if (!publication_effects.Value().empty()) {
    auto& effects = publication_effects.Value();
    std::ranges::move(effects, std::back_inserter(output.effects));
    tool().effects = output.effects;
  }
  tool().artifacts = output.existing_artifacts;
  tool().artifacts.insert(tool().artifacts.end(), output.published_artifacts.begin(), output.published_artifacts.end());
  for (const auto& artifact : output.published_artifacts) {
    output.blocks.push_back({BlockId{MakeId()}, ArtifactBlock{artifact}});
    if (invocation.reverts) {
      const auto stored = std::ranges::find_if(session.artifacts, [&](const auto& entry) { return entry.record.metadata.ref == artifact; });
      if (stored->record.source_change) tool().source_changes.push_back(*stored->record.source_change);
    }
  }
  if (!output.blocks.empty()) {
    session.messages.push_back(Message{
        .ref = {session.summary.id, MessageId{MakeId()}, 1}, .sequence = ++session.sequence, .run = invocation.run,
        .author = MessageAuthor::Tool, .state = MessageState::Complete, .blocks = output.blocks});
  }
  if (output.outcome != ToolOutcome::Succeeded && !output.reason) {
    const auto code = output.outcome == ToolOutcome::ResultUnknown ? ErrorCode::ResultUnknown : ErrorCode::InvalidState;
    output.reason = Error(code, "Tool did not complete");
  }
  co_return AgentResult<ToolResult>::Success(std::move(output));
}

} // namespace

Task<std::optional<ToolReviewResult>> RuntimeState::ReviewTool(ToolAttempt& invocation, ToolReviewRequest request) {
  using Decision = std::optional<ToolReviewResult>;
  auto attempt = std::make_shared<PendingToolReview>();
  invocation.review = attempt;
  request.stop = attempt->stop.get_token();
  auto start = [self = shared_from_this(), attempt, request = std::move(request)](
                   CallbackCompletion<Decision> complete) mutable {
    attempt->completion = complete;
    const auto timeout = std::max(Duration::zero(), request.deadline - self->Now());
    attempt->worker = self->options.tasks.Launch([self, request = std::move(request), complete]() mutable -> Task<void> {
      const auto deadline = request.deadline;
      if (request.stop.stop_requested()) co_return;
      if (self->Now() >= deadline) {
        complete.Complete(ToolReviewResult{ToolReviewDecision::DeferToUser, "Tool review timed out"});
        co_return;
      }
      try {
        auto decision = co_await self->options.tool_reviewer->review(std::move(request));
        if (self->Now() >= deadline) {
          complete.Complete(ToolReviewResult{ToolReviewDecision::DeferToUser, "Tool review timed out"});
        } else if (decision.Succeeded() && decision.Value().reason.size() <= 4096 && ValidUtf8(decision.Value().reason)) {
          complete.Complete(std::move(decision).Value());
        } else {
          complete.Complete(ToolReviewResult{ToolReviewDecision::DeferToUser, "Tool review did not return a valid decision"});
        }
      } catch (...) {
        self->ReportException(std::current_exception());
        complete.Complete(ToolReviewResult{ToolReviewDecision::DeferToUser, "Tool review failed"});
      }
    });
    attempt->timer = self->options.tasks.Launch([complete, timeout]() -> Task<void> {
      co_await Delay(timeout);
      complete.Complete(ToolReviewResult{ToolReviewDecision::DeferToUser, "Tool review timed out"});
    });
    return std::function<void()>{};
  };
  co_return co_await AwaitCallback<Decision>(std::move(start));
}

Async<ToolResult> RuntimeState::ExecuteTool(RunId id, ToolRef ref, ToolRequest request, ToolCallId call_id) {
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  auto& attempt = run.tool_attempts[call_id];
  auto& saved_plan = attempt.plan.prepared;
  auto& tool_approval = attempt.plan.approval;
  if (!attempt.plan.source_revision) attempt.plan.source_revision = ProjectSourceRevision(session);
  const bool restoring_plan = run.restoring && saved_plan.has_value();
  const auto turn = run.tool_turn;
  const auto index = std::ranges::find(session.tools, call_id, &ToolCallSnapshot::id) - session.tools.begin();
  auto tool = [&]() -> ToolCallSnapshot& { return session.tools.at(index); };
  auto publish = [&](ToolCallState state) {
    tool().state = state;
    ++session.sequence;
    return Persist(session);
  };
  bool execution_started = false;
  auto invalidate_review = [&](std::string reason) {
    if (auto& review = tool().review; review && (review->outcome == ToolReviewOutcome::Pending ||
                                               review->outcome == ToolReviewOutcome::Approved)) {
      review->outcome = ToolReviewOutcome::Invalidated;
      review->reason = std::move(reason);
    }
  };
  auto failure = [&](ToolCallState state, AgentError error) {
    tool().reason = error;
    if (!execution_started) invalidate_review(error.message);
    if (tool_approval) {
      auto approval = std::ranges::find(session.interactions, *tool_approval, &InteractionSnapshot::id);
      if (approval != session.interactions.end()) CloseInteraction(session, *approval, InteractionState::Invalidated);
    }
    saved_plan.reset();
    tool_approval.reset();
    publish(state);
    const auto outcome = state == ToolCallState::ResultUnknown ? ToolOutcome::ResultUnknown :
        state == ToolCallState::Cancelled ? ToolOutcome::Cancelled : ToolOutcome::Failed;
    return AgentResult<ToolResult>::Success({.call = call_id, .outcome = outcome, .reason = std::move(error)});
  };
  auto changed = [&]() {
    return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "New user input superseded the pending plan"));
  };
  const auto stopping = [&]() { return run.stop.stop_requested() || (turn && turn->failure); };
  auto stopped = [&]() {
    return failure(ToolCallState::Cancelled, Error(ErrorCode::AlreadyClosed, "Run stopped before tool execution"));
  };
  auto stale = [&]() {
    return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "Tool revision is unavailable"));
  };
  auto stale_sources = [&]() {
    return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "A source used by the prepared plan changed"));
  };
  const auto pinned = ResolveTool(ref);
  if (!pinned) co_return stale();
  const auto current = [&]() { return !ref.catalog || ResolveTool(ref) == pinned; };
  const auto& definition = *pinned;
  if (session.fork_branch && !session.fork_branch->isolated && ExternalEffect(definition.effect)) {
    co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, "A shared comparison workspace may only stage candidate artifacts"));
  }
  auto valid = ValidateJson(request.arguments, definition.input_schema);
  if (!valid.Succeeded()) co_return failure(ToolCallState::Failed, valid.Error());
  if (stopping()) co_return stopped();
  if (!run.pending_steers.empty()) co_return changed();

  ToolInvocation invocation{
      .actor = Request(run.principal, Capability::PrepareTool, &session, &run).actor,
      .phase = ToolAccessPhase::Prepare, .session = session.summary.id, .run = id, .call = call_id,
      .project = session.summary.project, .workspace = session.summary.workspace, .goal = run.view.goal,
      .tool = ref, .effect = definition.effect, .arguments = request.arguments};
  if (ArtifactChangeTool(ref.id)) {
    auto bound = BindArtifactChangeInvocation(invocation);
    if (!bound.Succeeded()) co_return failure(ToolCallState::Denied, bound.Error());
  }
  invocation.actor.call = call_id;
  PreparedToolCall plan;
  auto policy_for = [&](Capability capability, std::optional<PlanId> plan_id = {}) {
    auto request = Request(run.principal, capability, &session, &run);
    request.actor = invocation.actor;
    request.tool = invocation.tool;
    request.invocation = invocation;
    request.plan = std::move(plan_id);
    request.sources = capability == Capability::PrepareTool ? run.input.context : plan.change.inputs;
    request.artifact_action = run.artifact_action;
    for (const auto& target : invocation.prepared_targets) request.target.resources.push_back(target.target);
    return request;
  };
  if (restoring_plan) {
    plan = *saved_plan;
    if (tool().run != id || tool().plan != plan.plan || plan.tool != ref || plan.request.tool != ref.id ||
        plan.request.arguments != request.arguments) {
      co_return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "Approval does not match the restored plan"));
    }
    if (tool_approval) {
      auto approval = std::ranges::find(session.interactions, *tool_approval, &InteractionSnapshot::id);
      const auto* binding = approval == session.interactions.end() ? nullptr : std::get_if<ToolApproval>(&approval->body);
      if (!binding || approval->run != id || approval->recipient != run.principal || binding->call != call_id ||
          binding->plan != plan.plan) {
        co_return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "Approval does not match the restored plan"));
      }
    } else if (tool().review && tool().review->plan != plan.plan) {
      co_return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "Review does not match the restored plan"));
    }
  } else {
    publish(ToolCallState::Preparing);
    auto policy = co_await Require(policy_for(Capability::PrepareTool));
    if (!policy.Succeeded()) co_return failure(ToolCallState::Denied, policy.Error());
    std::vector<ResourceAccess> readable;
    if (options.tool_resources) {
      auto resources = co_await ResolveRunToolResources(run, invocation);
      if (!resources.Succeeded()) co_return failure(resources.Error().code == ErrorCode::Conflict
          ? ToolCallState::Conflict : ToolCallState::Denied, resources.Error());
      readable = std::move(resources).Value();
      if (std::ranges::any_of(readable, [](const auto& access) { return access.mode == ResourceAccessMode::Write; })) {
        co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, "Preparation requires read-only access"));
      }
    }
    if (stopping()) co_return stopped();
    if (!run.pending_steers.empty()) co_return changed();
    auto sources = co_await ValidateInputSources(session, invocation.actor, run.input.context);
    if (!sources.Succeeded()) co_return failure(ToolCallState::Denied, sources.Error());
    if (!current()) co_return stale();
    auto prepared = co_await definition.prepare(ToolPrepareContext{
        invocation.actor, call_id, std::move(readable), run.stop.get_token(), run.input.context, run.artifact_action}, request);
    if (stopping()) co_return stopped();
    if (!current()) co_return stale();
    if (!prepared.Succeeded()) co_return failure(prepared.Error().code == ErrorCode::Conflict
        ? ToolCallState::Conflict : ToolCallState::Failed, prepared.Error());
    if (!run.pending_steers.empty()) co_return changed();
    plan = PreparedToolCall{call_id, PlanId{MakeId()}, ref, std::move(request), std::move(prepared).Value()};
  }

  if (!ValidClaims(definition, plan.change)) {
    co_return failure(ToolCallState::Failed, Error(ErrorCode::InvalidArgument, "Tool resource claims contradict the prepared plan"));
  }
  const auto sources_changed = [&] {
    return attempt.plan.source_revision && SourcesChanged(session, *attempt.plan.source_revision, plan.change.inputs);
  };
  if (sources_changed()) co_return stale_sources();
  if (!plan.change.preview_artifacts.empty()) {
    if (std::ranges::any_of(plan.change.preview_artifacts, [](const auto& draft) { return draft.published_effect.has_value(); })) {
      co_return failure(ToolCallState::Failed, Error(ErrorCode::InvalidArgument, "A preview cannot record an applied effect"));
    }
    auto published = co_await PublishArtifacts(id, call_id, std::move(plan.change.preview_artifacts), invocation.actor);
    if (!published.Succeeded()) co_return failure(ToolCallState::Failed, published.Error());
    auto& refs = published.Value();
    plan.change.preview.artifacts.insert(plan.change.preview.artifacts.end(), refs.begin(), refs.end());
  }
  if (!current()) co_return stale();
  if (const auto* scope = Scope(run); scope && ExternalEffect(definition.effect) &&
      (plan.change.targets.empty() || std::ranges::any_of(plan.change.targets, [&](const auto& target) {
        return std::ranges::find(scope->writable_resources, target.target) == scope->writable_resources.end();
      }))) {
    co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, "Tool plan exceeds the work request's write scope"));
  }
  tool().plan = plan.plan;
  invocation.phase = ToolAccessPhase::Execute;
  invocation.prepared_targets = plan.change.targets;
  plan.change.preview.targets = plan.change.targets;
  saved_plan = plan;
  auto decision_epoch = access_revision;
  auto decision = co_await DecideToolExecution(policy_for(Capability::ExecuteTool, plan.plan));
  if (stopping()) co_return stopped();
  if (!run.pending_steers.empty()) co_return changed();
  if (!current()) co_return stale();
  if (sources_changed()) co_return stale_sources();
  if (!decision.Succeeded()) co_return failure(ToolCallState::Denied, decision.Error());
  if (decision.Value().decision == AccessDecision::Deny) {
    co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, decision.Value().reason));
  }
  const auto initial_decision = decision.Value().decision;
  bool ask_user = tool_approval.has_value() || initial_decision == AccessDecision::RequireConfirmation;
  std::optional<Revision> reviewed_access;
  const auto& reviewer = options.tool_reviewer;
  if (restoring_plan && !tool_approval && tool().review) {
    auto& review = *tool().review;
    if (review.outcome == ToolReviewOutcome::Approved && initial_decision == AccessDecision::AllowReview &&
        reviewer && review.reviewer == reviewer->id && review.reviewer_version == reviewer->version) {
      reviewed_access = decision_epoch;
    } else {
      if (review.outcome == ToolReviewOutcome::Pending) {
        review.outcome = ToolReviewOutcome::Escalated;
        review.reason = "Review was interrupted before a decision was saved";
      } else {
        invalidate_review("The saved decision no longer meets the review policy");
      }
      ask_user = true;
    }
  } else if (!ask_user && initial_decision == AccessDecision::AllowReview) {
    ask_user = true;
    if (reviewer) {
      const auto review_epoch = decision_epoch;
      auto sources = co_await ValidateInputSources(session, invocation.actor, plan.change.inputs);
      if (!sources.Succeeded()) co_return failure(ToolCallState::Denied, sources.Error());
      if (sources_changed()) co_return stale_sources();
      const auto now = Now();
      const auto review_by = now < TimePoint::max() - reviewer->timeout ? now + reviewer->timeout : TimePoint::max();
      const auto deadline = std::min(review_by, run.deadline.value_or(TimePoint::max()));
      tool().review = ToolReviewRecord{plan.plan, reviewer->id, reviewer->version, ToolReviewOutcome::Pending, {}};
      auto saved = co_await WhenSaved(publish(ToolCallState::Reviewing));
      if (!saved.Succeeded()) co_return AgentResult<ToolResult>::Failure(saved.Error());
      if (stopping()) co_return stopped();
      if (!run.pending_steers.empty()) co_return changed();
      std::optional<ToolReviewResult> review;
      if (!current()) co_return stale();
      if (sources_changed()) co_return stale_sources();
      if (access_revision == review_epoch) {
        review = co_await ReviewTool(attempt, {invocation, plan.plan, plan.change.preview, plan.change.inputs, deadline, {}});
      }
      if (run.deadline && Now() >= *run.deadline && !run.stop.stop_requested()) Stop(run, RunEndReason::Deadline);
      if (stopping()) co_return stopped();
      if (!run.pending_steers.empty()) co_return changed();
      if (!current()) co_return stale();
      if (sources_changed()) co_return stale_sources();
      if (access_revision != review_epoch || !review) {
        invalidate_review("Authorization changed during tool review");
      } else {
        auto& record = *tool().review;
        record.reason = std::move(review->reason);
        if (review->decision == ToolReviewDecision::Deny) {
          record.outcome = ToolReviewOutcome::Denied;
          co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied,
              record.reason.empty() ? "The tool reviewer denied the plan" : record.reason));
        }
        if (review->decision == ToolReviewDecision::Approve) {
          reviewed_access = review_epoch;
          ask_user = false;
        } else {
          record.outcome = ToolReviewOutcome::Escalated;
        }
      }
    }
  }

  std::vector<ResourceAccess> grants;
  bool approved_by_user = false;
  bool admitted = false;
  const auto claims = definition.concurrency == ToolConcurrency::Serial ? std::nullopt : plan.change.conflict_keys;
  Revision execution_epoch;
  for (;;) {
    if (stopping()) co_return stopped();
    if (!current()) co_return stale();
    if (sources_changed()) co_return stale_sources();
    if (ask_user && !approved_by_user) {
      if (decision_epoch != access_revision) {
        decision_epoch = access_revision;
        decision = co_await DecideToolExecution(policy_for(Capability::ExecuteTool, plan.plan));
        if (!decision.Succeeded()) co_return failure(ToolCallState::Denied, decision.Error());
        if (decision.Value().decision == AccessDecision::Deny) {
          co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, decision.Value().reason));
        }
        if (decision_epoch != access_revision) continue;
      }
      auto user_access = co_await Require(Request(run.principal, Capability::SubmitInteraction, &session));
      if (!user_access.Succeeded()) co_return failure(ToolCallState::Denied, user_access.Error());
      if (stopping()) co_return stopped();
      if (!run.pending_steers.empty()) co_return changed();
      if (!current()) co_return stale();
      if (sources_changed()) co_return stale_sources();
      if (!tool_approval) {
        tool_approval = InteractionId{MakeId()};
        session.interactions.push_back(InteractionSnapshot{
            .id = *tool_approval, .version = 1, .session = session.summary.id, .run = id,
            .recipient = run.principal, .state = InteractionState::Pending,
            .body = ToolApproval{call_id, plan.plan, plan.change.preview}, .expires_at = run.deadline});
        ++session.summary.pending_interactions;
        publish(ToolCallState::AwaitingApproval);
      }
      auto answer = co_await WaitForInteraction(id, *tool_approval, bool(turn));
      if (stopping()) co_return stopped();
      if (!run.pending_steers.empty()) co_return changed();
      if (!current()) co_return stale();
      if (sources_changed()) co_return stale_sources();
      if (!answer.Succeeded()) co_return failure(ToolCallState::Failed, answer.Error());
      if (answer.Value().state != InteractionState::Approved) {
        co_return failure(ToolCallState::Denied, Error(
            answer.Value().state == InteractionState::Expired ? ErrorCode::DeadlineExceeded : ErrorCode::PermissionDenied,
            "Tool approval was not granted"));
      }
      approved_by_user = true;
      auto saved = co_await WhenSaved(publish(ToolCallState::Ready));
      if (!saved.Succeeded()) co_return AgentResult<ToolResult>::Failure(saved.Error());
    }

    if (turn && !admitted) {
      if (!co_await turn->Acquire(call_id, claims) || stopping()) co_return stopped();
      admitted = true;
      attempt.parallel = claims.has_value();
    }
    if (!current()) co_return stale();
    if (!run.pending_steers.empty()) co_return changed();
    execution_epoch = access_revision;
    if (!run.view.active_skills.empty()) {
      auto skills = co_await VerifyActiveSkills(run, !workflows.contains(run.view.agent.id));
      if (stopping()) co_return stopped();
      if (!skills.Succeeded()) co_return failure(ToolCallState::Denied, skills.Error());
    }
    auto sources = co_await ValidateInputSources(session, invocation.actor, plan.change.inputs);
    if (!sources.Succeeded()) co_return failure(ToolCallState::Denied, sources.Error());
    if (options.tool_resources) {
      auto resources = co_await ResolveRunToolResources(run, invocation);
      if (!resources.Succeeded()) co_return failure(resources.Error().code == ErrorCode::Conflict
          ? ToolCallState::Conflict : ToolCallState::Denied, resources.Error());
      grants = std::move(resources).Value();
    }
    if (claims && std::ranges::any_of(grants, [&](const auto& grant) {
          return !CoversClaim(*claims, grant.resource, grant.mode);
        })) {
      co_return failure(ToolCallState::Failed, Error(ErrorCode::InvalidArgument, "Execution grant exceeds the tool resource claims"));
    }
    for (const auto& target : plan.change.targets) {
      const auto found = std::ranges::find(grants, target.target, &ResourceAccess::resource);
      if (found == grants.end() || (ExternalEffect(definition.effect) && found->mode != ResourceAccessMode::Write)) {
        co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, "Target no longer has execution access"));
      }
      if (found->current != target.expected) {
        co_return failure(ToolCallState::Conflict, Error(ErrorCode::Conflict, "Resource baseline changed"));
      }
    }
    if (std::ranges::any_of(grants, [&](const auto& grant) {
          return grant.mode == ResourceAccessMode::Write && std::ranges::none_of(plan.change.targets, [&](const auto& target) {
            return target.target == grant.resource;
          });
        })) {
      co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, "Execution grant exceeds the plan"));
    }
    decision_epoch = execution_epoch;
    decision = co_await DecideToolExecution(policy_for(Capability::ExecuteTool, plan.plan));
    if (stopping()) co_return stopped();
    if (!run.pending_steers.empty()) co_return changed();
    if (!decision.Succeeded()) co_return failure(ToolCallState::Denied, decision.Error());
    if (decision.Value().decision == AccessDecision::Deny) {
      co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, decision.Value().reason));
    }
    if (execution_epoch != access_revision) continue;
    if (reviewed_access && (*reviewed_access != execution_epoch || decision.Value().decision != AccessDecision::AllowReview)) {
      invalidate_review("The reviewed plan requires a fresh approval");
      reviewed_access.reset();
      ask_user = true;
    }
    if (!approved_by_user && !reviewed_access && (ask_user || decision.Value().decision != AccessDecision::Allow)) {
      if (admitted) {
        turn->Release(call_id);
        admitted = false;
      }
      ask_user = true;
      continue;
    }
    if (reviewed_access) tool().review->outcome = ToolReviewOutcome::Approved;
    if (tool().state != ToolCallState::Ready) {
      auto saved = co_await WhenSaved(publish(ToolCallState::Ready));
      if (!saved.Succeeded()) co_return AgentResult<ToolResult>::Failure(saved.Error());
    }
    if (execution_epoch == access_revision) break;
  }
  if (run.deadline && Now() >= *run.deadline && !run.stop.stop_requested()) Stop(run, RunEndReason::Deadline);
  if (stopping()) co_return stopped();
  if (!run.pending_steers.empty()) co_return changed();
  if (!current()) co_return stale();
  if (sources_changed()) co_return stale_sources();
  attempt.executing = definition.effect;
  if (ExternalEffect(definition.effect)) tool().issued_targets = invocation.prepared_targets;
  auto committed = co_await WhenSaved(publish(ToolCallState::Executing));
  if (!committed.Succeeded()) {
    attempt.executing.reset();
    saved_plan.reset();
    co_return AgentResult<ToolResult>::Failure(committed.Error());
  }
  if (run.deadline && Now() >= *run.deadline && !run.stop.stop_requested()) Stop(run, RunEndReason::Deadline);
  if (stopping()) {
    attempt.executing.reset();
    co_return stopped();
  }
  if (!current()) {
    attempt.executing.reset();
    co_return stale();
  }
  if (sources_changed()) {
    attempt.executing.reset();
    co_return stale_sources();
  }
  if (execution_epoch != access_revision || !run.pending_steers.empty()) {
    attempt.executing.reset();
    if (!run.pending_steers.empty()) co_return changed();
    co_return failure(ToolCallState::Denied, Error(ErrorCode::PermissionDenied, "Authorization changed before execution"));
  }

  execution_started = true;
  attempt.issued = true;
  auto result = co_await definition.execute(
      ToolContext(shared_from_this(), session.summary.id, id, call_id, run.stop.get_token(), std::move(grants)),
      std::move(plan));
  attempt.executing.reset();
  saved_plan.reset();
  tool_approval.reset();
  if (!result.Succeeded()) {
    const bool unknown = ExternalEffect(definition.effect);
    auto error = result.Error();
    if (unknown) error.code = ErrorCode::ResultUnknown;
    co_return failure(unknown ? ToolCallState::ResultUnknown : ToolCallState::Failed, std::move(error));
  }
  // Separate result publication so MSVC can emit the coroutine's tail transfers in Release.
  co_return co_await PublishToolResult(*this, invocation, index, definition.output_schema, std::move(result).Value());
}

Async<CommandReceipt> RuntimeState::ReconcileTool(CommandId command, SessionId id, ToolCallId call_id, ToolExecutionResult result) {
  using Result = AgentResult<CommandReceipt>;
  if (command.value.empty() || id.value.empty() || call_id.value.empty() || result.model_value ||
      !result.blocks.empty() || !result.new_artifacts.empty() || result.outcome == ToolOutcome::ResultUnknown)
    co_return Fail<CommandReceipt>(ErrorCode::InvalidArgument, "Reconciliation requires a known outcome and existing evidence");
  auto user = User();
  if (!user.Succeeded()) co_return Result::Failure(user.Error());
  const auto epoch = access_revision;
  auto allowed = co_await WithSession(id, user.Value(), Capability::ReadSession);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const auto found = sessions.find(id);
  const auto* image = found == sessions.end() ? nullptr : Published(found->second);
  if (!image || image->summary.lifecycle == SessionLifecycle::Deleting)
    co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Session unavailable");
  const auto observed = std::ranges::find(image->tools, call_id, &ToolCallSnapshot::id);
  if (observed == image->tools.end()) co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Tool call unavailable");
  const auto binding = image->summary.binding_revision;
  const auto run_id = observed->run;
  auto policy = Request(user.Value(), Capability::ReconcileTool, image);
  policy.actor.kind = ActionParty::Kind::Host;
  policy.target.run = run_id;
  policy.actor.call = call_id;
  policy.tool = observed->tool;
  policy.plan = observed->plan;
  for (const auto& effect : result.effects) policy.target.resources.push_back(effect.resource);
  for (const auto& artifact : result.existing_artifacts) policy.sources.push_back({artifact});
  allowed = co_await Require(std::move(policy));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const auto target = sessions.find(id);
  if (target == sessions.end()) co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Session unavailable");
  auto& session = target->second;
  auto current = User();
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (user.Value() != current.Value() || epoch != access_revision || binding != session.summary.binding_revision ||
      session.summary.lifecycle == SessionLifecycle::Deleting)
    co_return Fail<CommandReceipt>(ErrorCode::Conflict, "Conversation access changed while reconciling the call");
  CommandInput key{.operation = Operation::ReconcileTool, .session = id, .run = run_id,
      .reconciliation = ToolReconciliation{call_id, result.outcome, std::move(result.existing_artifacts),
          std::move(result.effects), std::move(result.reason)}};
  auto duplicate = Find(user.Value(), command, key);
  if (!duplicate.Succeeded()) co_return Result::Failure(duplicate.Error());
  if (duplicate.Value()) co_return co_await ReplayReceipt(ReceiptFor<std::monostate>(user.Value(), command));
  if (storage_error && session.retention == SessionRetention::Stored) co_return Result::Failure(*storage_error);
  auto tool = std::ranges::find(session.tools, call_id, &ToolCallSnapshot::id);
  if (tool == session.tools.end()) co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Tool call unavailable");
  auto& run = runs.at(run_id);
  if (!Terminal(run.view.state)) co_return Fail<CommandReceipt>(ErrorCode::Busy, "The tool run is still active");
  if (tool->reconciliation || (tool->state != ToolCallState::ResultUnknown && tool->state != ToolCallState::PartiallyApplied))
    co_return Fail<CommandReceipt>(ErrorCode::AlreadyClosed, "The original tool outcome has been settled");
  auto host = std::ranges::find_if(run.tool_requests, [&](const auto& entry) { return entry.second.call == call_id; });
  CommandRecord* change = nullptr;
  if (host != run.tool_requests.end() && ArtifactChangeTool(host->second.ref.id)) {
    const auto original = commands.find({run.principal, host->first});
    if (original != commands.end()) change = &original->second;
  }
  const auto& evidence = *key.reconciliation;
  const std::vector<TargetBaseline> unrecorded;
  const auto& targets = tool->issued_targets ? *tool->issued_targets : change && change->artifact_change
      ? change->artifact_change->change.targets : unrecorded;
  auto checked = CheckReconciliation(evidence, *tool, targets);
  if (!checked.Succeeded()) co_return Result::Failure(checked.Error());
  for (const auto& ref : evidence.artifacts) {
    if (std::ranges::none_of(session.artifacts, [&](const auto& item) {
          return item.record.metadata.ref == ref && item.record.call == call_id &&
              item.record.availability == ArtifactAvailability::Available;
        })) co_return Fail<CommandReceipt>(ErrorCode::NotFound, "The tool has no such committed artifact");
  }
  for (const auto& ref : evidence.artifacts) {
    if (std::ranges::find(tool->artifacts, ref) == tool->artifacts.end()) tool->artifacts.push_back(ref);
  }
  if (tool->reverts) {
    ToolCallSnapshot recovered{.run = run_id};
    for (const auto& effect : evidence.effects) {
      if (effect.state == ResourceEffectState::Applied && std::ranges::find(tool->effects, effect) == tool->effects.end())
        recovered.effects.push_back(effect);
    }
    RecordRevertEffects(session, recovered, {.kind = ActionParty::Kind::Host, .principal = user.Value()});
    std::ranges::move(recovered.source_changes, std::back_inserter(tool->source_changes));
  }
  tool->state = StateForOutcome(evidence.outcome);
  tool->effects = evidence.effects;
  tool->reason = evidence.reason;
  tool->reconciliation = command;
  if (!tool->reason && evidence.outcome != ToolOutcome::Succeeded)
    tool->reason = Error(ErrorCode::InvalidState, "The original tool did not apply all of its changes");
  if (host != run.tool_requests.end()) {
    auto receipt = host->second.result.value_or(ToolResult{.call = call_id});
    receipt.outcome = evidence.outcome;
    receipt.effects = tool->effects;
    receipt.reason = tool->reason;
    for (const auto& ref : evidence.artifacts) {
      if (std::ranges::find(receipt.published_artifacts, ref) == receipt.published_artifacts.end()) receipt.published_artifacts.push_back(ref);
    }
    host->second.result = std::move(receipt);
    host->second.failure.reset();
  }
  ++session.sequence;
  if (change) {
    Resolve(*change, session, evidence.outcome == ToolOutcome::Succeeded ? CommandState::Applied : CommandState::Failed, tool->reason);
    change->progress.resolution->commit = {};
  }
  Accept(user.Value(), command, std::move(key), session, std::monostate{});
  co_return co_await SaveReceipt<std::monostate>(session, user.Value(), command);
}

void RuntimeState::SettleToolException(RunExecution& run, const ToolCallId& call) {
  auto& session = sessions.at(run.view.session);
  const auto snapshot = std::ranges::find(session.tools, call, &ToolCallSnapshot::id);
  const auto attempt = run.tool_attempts.find(call);
  if (snapshot == session.tools.end() || attempt == run.tool_attempts.end()) return;
  if (attempt->second.plan.approval) {
    const auto approval = std::ranges::find(session.interactions, *attempt->second.plan.approval, &InteractionSnapshot::id);
    if (approval != session.interactions.end()) CloseInteraction(session, *approval, InteractionState::Invalidated);
  }
  const auto effect = attempt->second.executing;
  const bool unknown = attempt->second.issued && effect && ExternalEffect(*effect);
  if (snapshot->state == ToolCallState::Executing || !attempt->second.issued) {
    snapshot->state = unknown ? ToolCallState::ResultUnknown : ToolCallState::Failed;
    snapshot->reason = Error(unknown ? ErrorCode::ResultUnknown : ErrorCode::InvalidState, "Tool handler failed");
  }
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

Async<CommandReceipt> AgentRuntime::ReconcileToolAsync(CommandId command, SessionId session, ToolCallId call, ToolExecutionResult result) {
  auto state = state_;
  co_return co_await state->ReconcileTool(std::move(command), std::move(session), std::move(call), std::move(result));
}

} // namespace huxerui::agentkit
