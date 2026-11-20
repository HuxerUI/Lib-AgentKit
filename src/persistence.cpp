#include "runtime_internal.h"

#include <cstring>
#include <nlohmann/json.hpp>

namespace nlohmann {

template<class T> struct adl_serializer<std::optional<T>> {
  static void to_json(json& value, const std::optional<T>& input) {
    if (input) value = *input;
    else value = nullptr;
  }

  static void from_json(const json& value, std::optional<T>& output) {
    if (value.is_null()) output.reset();
    else output = value.get<T>();
  }
};

template<class... T> struct adl_serializer<std::variant<T...>> {
  static void to_json(json& value, const std::variant<T...>& input) {
    value = json::array({input.index(), std::visit([](const auto& item) { return json(item); }, input)});
  }

  template<std::size_t Index = 0>
  static void Select(std::size_t tag, const json& value, std::variant<T...>& output) {
    if constexpr (Index == sizeof...(T)) {
      throw std::invalid_argument("Unknown stored variant");
    } else if (tag == Index) {
      output.template emplace<Index>(value.get<std::variant_alternative_t<Index, std::variant<T...>>>());
    } else {
      Select<Index + 1>(tag, value, output);
    }
  }

  static void from_json(const json& value, std::variant<T...>& output) {
    if (!value.is_array() || value.size() != 2) throw std::invalid_argument("Invalid stored variant");
    Select(value.at(0).get<std::size_t>(), value.at(1), output);
  }
};

template<> struct adl_serializer<std::monostate> {
  static void to_json(json& value, std::monostate) { value = nullptr; }

  static void from_json(const json& value, std::monostate&) {
    if (!value.is_null()) throw std::invalid_argument("Invalid empty field");
  }
};

template<> struct adl_serializer<huxerui::agentkit::Duration> {
  using Duration = huxerui::agentkit::Duration;
  static void to_json(json& value, Duration input) { value = input.count(); }

  static void from_json(const json& value, Duration& output) { output = Duration(value.get<Duration::rep>()); }
};

template<> struct adl_serializer<huxerui::agentkit::TimePoint> {
  using TimePoint = huxerui::agentkit::TimePoint;
  static void to_json(json& value, TimePoint input) { value = input.time_since_epoch().count(); }

  static void from_json(const json& value, TimePoint& output) {
    output = TimePoint(TimePoint::duration(value.get<TimePoint::rep>()));
  }
};

} // namespace nlohmann

namespace huxerui::agentkit {

template<class Tag> void to_json(nlohmann::json& value, const Id<Tag>& input) { value = input.value; }

template<class Tag> void from_json(const nlohmann::json& value, Id<Tag>& output) { value.get_to(output.value); }

#define AGENTKIT_EMPTY_RECORD(Type)                                                         \
  inline void to_json(nlohmann::json& value, Type) { value = nullptr; }                       \
  inline void from_json(const nlohmann::json& value, Type&) {                                \
    if (!value.is_null()) throw std::invalid_argument("Invalid empty record");               \
  }

AGENTKIT_EMPTY_RECORD(ArchiveSession)
AGENTKIT_EMPTY_RECORD(UnarchiveSession)
AGENTKIT_EMPTY_RECORD(DeleteSession)
AGENTKIT_EMPTY_RECORD(DismissForm)
AGENTKIT_EMPTY_RECORD(CancelTask)
AGENTKIT_EMPTY_RECORD(RetryTask)
AGENTKIT_EMPTY_RECORD(RequestContextReview)
AGENTKIT_EMPTY_RECORD(PublishContext)
AGENTKIT_EMPTY_RECORD(WithdrawContext)
AGENTKIT_EMPTY_RECORD(EnableGoal)
AGENTKIT_EMPTY_RECORD(PauseGoal)
AGENTKIT_EMPTY_RECORD(ResumeGoal)
AGENTKIT_EMPTY_RECORD(CancelGoal)
#undef AGENTKIT_EMPTY_RECORD

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(JsonValue, value)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(JsonSchema, document)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScopeExpansion, affected_parts, whole_artifact, summary)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FieldError, path, code, message)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AgentError, code, message, fields, current_revision, retry_after,
    provider_code, proposed_scope)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CommitStamp, command, commit, session, session_sequence, project,
    project_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PendingApproval, session, interaction, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionCreated, session, initial_run, initial_request)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunAccepted, run)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InputQueued, item)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunSteered, run, message)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InteractionConsumed, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InputRequested, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolCallAccepted, call)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MessageAccepted, message, delivery, target_run, approval)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalUpdated, goal, revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskUpdated, task, revision, attempt)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SharedContextRef, project, item, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChangePlanAccepted, run, call, queue_item, approval)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForkReviewCreated, review, branches)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CommandProgress, accepted, state, resolution, result, approval, failure)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AgentRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelProviderRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExecutionProfileRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UserRunOptions, model, execution_profile)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunStartBinding, model, execution_profile)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolRef, id, version, catalog)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SkillRef, source, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArtifactRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MessageRef, session, message, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ContentTypeRef, id, schema_version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceTextRange, leaf, begin_utf8, end_utf8)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceSlice, source, parts, text_range)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SideChatConclusion, text, sources)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SideChatSeed, source, selected)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueuedConclusionRef, item, index)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UnappliedSteerConclusionRef, command, index)

void to_json(nlohmann::json& value, const SendInput& input) {
  value = {{"text", input.text}, {"context", input.context}, {"attachments", input.attachments},
           {"skills", input.skills}, {"side_chat_conclusions", input.side_chat_conclusions}};
  if (!input.recovered_conclusions.empty()) value["recovered_conclusions"] = input.recovered_conclusions;
  if (!input.recovered_steer_conclusions.empty()) value["recovered_steer_conclusions"] = input.recovered_steer_conclusions;
  if (!input.staged_attachments.empty()) value["staged_attachments"] = input.staged_attachments;
}

void from_json(const nlohmann::json& value, SendInput& input) {
  value.at("text").get_to(input.text);
  value.at("context").get_to(input.context);
  value.at("attachments").get_to(input.attachments);
  value.at("skills").get_to(input.skills);
  value.at("side_chat_conclusions").get_to(input.side_chat_conclusions);
  input.recovered_conclusions = value.value("recovered_conclusions", std::vector<QueuedConclusionRef>{});
  input.recovered_steer_conclusions = value.value("recovered_steer_conclusions", std::vector<UnappliedSteerConclusionRef>{});
  input.staged_attachments = value.value("staged_attachments", std::vector<ArtifactId>{});
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionOptions, agent, project, workspace, title, retention)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(HistoryBoundary, session, anchor, through_sequence)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorkspaceCopyPlanRef, id, version)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorkspaceChoice, kind, workspace, copy_plan)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskExecutionRef, project, task, attempt)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionSummary, id, title, title_source, agent, binding_revision,
    metadata_revision, project, workspace, lifecycle, last_activity, active_run, run_state, pending_interactions,
    queued_inputs)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TextBlock, text, markdown)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AgentTransferred, from, to, binding_revision, handoff_summary)
void to_json(nlohmann::json& value, const ArtifactBlock& block) {
  value = {{"artifact", block.artifact}};
  if (!block.name.empty()) value["name"] = block.name;
}

void from_json(const nlohmann::json& value, ArtifactBlock& block) {
  value.at("artifact").get_to(block.artifact);
  block.name = value.value("name", std::string{});
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InteractionBlock, interaction)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolBlock, call)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CustomBlock, type, data, fallback, interactions, artifacts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CustomBlockRef, message, block)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CustomBlockAction, target, action, payload)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArtifactAction, artifact, action, payload)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ContentBlock, id, body)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Message, ref, sequence, run, author, state, blocks)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RenameSession, title)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunWait, kind, subject, deadline)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Usage, input_tokens, output_tokens, model_turns, tool_calls)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Budget, tokens, model_turns, tool_calls, context_bytes, deadline)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunCompletion, summary, structured, artifacts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SkillActivation, skill, trigger)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunSteerSnapshot, message, state, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunSnapshot, id, session, agent, model, trigger, parent, retry_of,
    work_request, task, goal, state, waits, steering, active_skills, usage, end_reason, result, failure,
    execution_profile)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormRequest, title, schema, presentation, expires_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TargetBaseline, target, expected)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GoalEditImpact, previous, retiring_run, in_flight_calls, continuing_runs,
    continuing_attempts, withdrawn_requests, released_attempts, invalidated_interactions, resets_completion_review, retiring_linked_runs)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ApprovalPreview, title, summary, artifacts, targets, goal_edit)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolApproval, call, plan, preview)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OperationApproval, pending_command, preview)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InteractionSnapshot, id, version, session, run, recipient, state, body,
    expires_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InteractionReply, id, expected_version, response)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(QueuedInput, id, input, state, run, user_start, conclusions, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UndoRef, handle, support)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceEffect, resource, state, before, after, undo, error)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolReviewRecord, plan, reviewer, reviewer_version, outcome, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ToolCallSnapshot, id, run, tool, plan, state, review, progress, effects,
    artifacts, reason, reverts, source_changes, issued_targets, reconciliation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ActionParty, kind, principal, session, run, call, agent)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionOrigin, kind, creator, source_session, fork_point, side_chat_selection)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TransferRequest, next, expected_binding, handoff_summary)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolRequest, tool, arguments)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArtifactPart, id, label)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArtifactDependency, output_part, input)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceReplacement, previous, replacement)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReworkRequest, session, baseline, parts, targets, replacements, accept_whole_artifact_scope)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RevertRequest, session, original_call, expected_current)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ArtifactDraft, id, expected_latest, type, name, storage_handle, size_bytes,
    parts, dependencies, changed_parts, published_effect)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArtifactMetadata, ref, type, name, size_bytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ArtifactRecord, metadata, session, run, call, availability, parts, dependencies, source_change)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolResourceClaim, key, mode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreparedChange, preview, preview_artifacts, targets, inputs,
    conflict_keys, execution_data, undo_support)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreparedToolCall, call, plan, tool, request, change)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelContent, role, body)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelToolCall, provider_call_id, name, arguments)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelToolResult, provider_call_id, call, value, error)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelResumeState, provider, format_version, opaque)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelContinuation, state, prefix_items)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunCheckpoint, stage, schema_version, state, steer_through)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolResult, call, outcome, model_value, blocks, existing_artifacts,
    published_artifacts, effects, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReplyExpectation, schema, artifact_types, reply_by)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorkScope, inputs, writable_resources, tools)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorkRequest, input, scope, request_start, reply, task, cancel_on_goal_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Notice, input)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SteerRequest, expected_run, input)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionMessageRequest, to, body, related, deliver_by)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionReply, status, summary, value, artifacts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReplyOutcome, request, end, reply, target_run)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CollaborationSummary, id, from, to, in_reply_to, delivery, request, target_run)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CollaborationRecord, summary, request, reply, outcome, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MessageDelta, message, block, utf8)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ArtifactRegistered, artifact)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SessionEvent, session, sequence, commit, command, body)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Evidence, values, sources)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(VerificationResult, status, reason, verified)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalDefinition, objective, evidence_schema, completion_conditions, completion)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalSpec, definition, budget)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalCompletionReview, approval, definition, verification_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalSnapshot, ref, session, revision, spec, state, usage, tasks,
    runs, evidence, verification, completion_review, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProposeGoal, spec)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EditGoal, definition, preview_token)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SetGoalBudget, budget)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SetGoalTasks, tasks)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SubmitGoalEvidence, goal, evidence)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChangeGoal, goal, expected, change)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskDependency, upstream, required_artifact_types, accept_partial)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProjectTaskSpec, title, input, goal, dependencies, completion_conditions)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskOutcome, execution, goal, state, artifacts, evidence, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProjectTaskSnapshot, id, revision, spec, state, assignee, attempt,
    attempt_goal, claimant, run, resolved_inputs, outcome, blocked_reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CreateTask, expected_project, spec)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EditTask, spec, expected_project)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AssignTask, assignee)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ClaimTask, session)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReleaseTask, attempt)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StartTask, attempt)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompleteTask, attempt, evidence, artifacts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FailTask, attempt, reason, artifacts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChangeTask, task, expected, change)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SharedContextDraft, title, content, sources, access_profile)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SharedContextItem, ref, publisher, state, data)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProposeSharedContext, expected_project, draft)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EditSharedContext, draft)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChangeSharedContext, expected, change)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SourceChange, before, after, kind)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CandidateRequest, session, workspace, task)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForkReviewRequest, from, project, inputs, candidates, comparison_criteria, shared_budget)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CandidateVerification, model, workspace, workspace_revision, conditions, evidence)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForkCandidate, id, session, run, state, artifacts, validation, usage, elapsed, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ForkReviewSnapshot, id, revision, state, request, candidates, selected, adoption_call,
    adoption_state, adoption_reconciled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AdoptCandidateRequest, review, expected_review, candidate, candidate_artifact, target_session, parts, targets)

namespace detail {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PreparedRenderedAction, request, artifact, content_type)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CandidateApprovalBinding, session, agent, start, confirmation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ComparisonApprovalBinding, review, candidates)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolReconciliation, call, outcome, artifacts, effects, reason)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CommandInput, operation, session, run, queue_item, options, input, start,
    reply, metadata, transfer, expected, message, delivery, final_reply, work, fork, goal, workspace_choice, side_chat,
    rendered_action, skill, rework, revert, adoption, comparison, reconciliation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OperationPlan, confirmation, target_binding, new_agent, run_start, goal_edit,
    rendered_action, comparison)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ArtifactChangePlan, run, call, change, source_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RetiredComparisonWorkspace, copy, workspace)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CommandRecord, input, progress, accepted, plan, original_approval,
    retired_workspace, artifact_change, retired_comparison_workspaces, rendered_steer_input)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PendingModelCall, call, tool, error, budget_reserved, local_call, result, terminal_error, delegation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ToolPlanState, prepared, approval, source_revision)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ModelSkillResourceUse, skill, path, bytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FormRecord, request, stamp, result)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(HostToolRecord, request, ref, stamp, call, result, failure)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StoredArtifact, record, handle)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StoredCollaboration, record, auto_start, scope, steer_message, goal)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DelegatedRun, budget, expected_result, started)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(HostedModelStep, input_index, allowed_tools, published)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StoredRun, view, principal, input, deadline, prepared_tool,
    tool_approval, tool_plans, items, pending_steers, continuation, continuation_missing, pending_calls, model_boundary,
    checkpoint, form_requests, tool_requests, separate_inherited_history, model_skill_resources, host_skill_input, artifact_action,
    delegate, delegations, host_model_step)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StoredCommand, principal, record)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GoalVerification, command, actor, revision, dependencies)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StoredGoal, view, owner, enabled_at, definitions, retired, verification)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForkBranchBinding, review, candidate, run, budget, started, isolated)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionData, summary, retention, origin, sequence, messages,
    runs, tools, interactions, queue, queue_owners, replies, artifacts, collaboration, current_goal, goals, fork_branch,
    event_sequence)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StoredSession, data, runs, commands)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StoredSessionEvent, event, command_owner, pending_recipients)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(StoredProjectTask, snapshot, attempts)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CloseForkReview, review, expected)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProjectCommandRecord, principal, actor, input, progress, source_change)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ForkAdoptionBinding, session, principal, command)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProjectData, id, revision, tasks, context, commands, fork_reviews, fork_adoptions)

namespace {

constexpr std::string_view collection = "agentkit.session";
constexpr std::string_view project_collection = "agentkit.project";
constexpr std::string_view event_collection = "agentkit.session-events";
constexpr unsigned project_schema_version = 2;
constexpr unsigned session_schema_version = 2;

template<class T>
AgentResult<Bytes> EncodeStoreRecord(std::string_view field, const T& image, unsigned schema) {
  try {
    auto cbor = nlohmann::json::to_cbor({{"schema", schema}, {std::string(field), image}});
    Bytes bytes(cbor.size());
    std::memcpy(bytes.data(), cbor.data(), bytes.size());
    return AgentResult<Bytes>::Success(std::move(bytes));
  } catch (const std::exception& error) {
    return Fail<Bytes>(ErrorCode::PersistenceFailure, error.what());
  }
}

bool ValidContextSourceChange(const ProjectData& project, const ProjectCommandRecord& command) {
  const auto& change = *command.source_change;
  const auto* input = std::get_if<SharedContextCommand>(&command.input);
  const auto* transition = input ? std::get_if<ChangeSharedContext>(input) : nullptr;
  const auto* before = std::get_if<SharedContextRef>(&change.before.source);
  if (!transition || !before || !change.before.parts.empty() || change.before.text_range ||
      before->project != project.id || transition->expected != std::get<SharedContextRef>(*command.progress.result) ||
      before->item != transition->expected.item || !before->version) return false;
  const auto versions = project.context.find(before->item);
  if (versions == project.context.end() || transition->expected.project != project.id ||
      transition->expected.version > versions->second.size()) return false;
  if (change.kind == SourceChangeKind::Modified) {
    return std::holds_alternative<PublishContext>(transition->change) && before->version < transition->expected.version &&
        change.after == SourceRef{transition->expected};
  }
  return change.kind == SourceChangeKind::Withdrawn && std::holds_alternative<WithdrawContext>(transition->change) &&
      *before == transition->expected && !change.after &&
      versions->second[before->version - 1].state == SharedContextState::Withdrawn;
}

AgentResult<ProjectData> DecodeProject(const StoreValue& row) {
  try {
    const auto* start = reinterpret_cast<const std::uint8_t*>(row.data.data());
    auto record = nlohmann::json::from_cbor(start, start + row.data.size());
    const auto schema = record.at("schema").get<unsigned>();
    if (schema != 1 && schema != project_schema_version) {
      return Fail<ProjectData>(ErrorCode::Unsupported, "Unknown AgentKit project schema");
    }
    auto project = record.at("project").get<ProjectData>();
    if (project.id.value.empty() || project.id.value != row.key || !project.revision) {
      return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored project identity");
    }
    for (const auto& [id, task] : project.tasks) {
      if (id != task.snapshot.id || !task.snapshot.revision ||
          std::ranges::any_of(task.snapshot.spec.dependencies, [&](const auto& dep) { return !project.tasks.contains(dep.upstream); })) {
        return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored task graph");
      }
    }
    for (const auto& [id, versions] : project.context) {
      for (std::size_t i = 0; i < versions.size(); ++i) {
        if (versions[i].ref != SharedContextRef{project.id, id, i + 1}) {
          return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored shared context version");
        }
      }
    }
    for (const auto& [id, review] : project.fork_reviews) {
      std::set<SessionId> branches;
      std::set<CandidateId> candidates;
      if (schema == 1 || id != review.id || id.value.empty() || !review.revision || review.request.project != project.id ||
          review.candidates.size() < 2 || review.candidates.size() != review.request.candidates.size() ||
          std::ranges::any_of(review.candidates, [&](const auto& candidate) {
            return candidate.session.value.empty() || candidate.id.value.empty() || !candidate.run || candidate.run->value.empty() ||
                !branches.insert(candidate.session).second || !candidates.insert(candidate.id).second;
          }) || bool(review.selected) != bool(review.adoption_call) || bool(review.selected) != bool(review.adoption_state) ||
          bool(review.selected) != project.fork_adoptions.contains(id) ||
          (review.selected && !candidates.contains(*review.selected))) {
        return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored comparison");
      }
    }
    for (const auto& [id, adoption] : project.fork_adoptions) {
      if (!project.fork_reviews.contains(id) || adoption.session.value.empty() || adoption.principal.value.empty() ||
          adoption.command.value.empty()) return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored adoption");
    }
    for (const auto& command : project.commands) {
      const bool simple = std::holds_alternative<SourceChange>(command.input) || std::holds_alternative<CloseForkReview>(command.input);
      if (command.progress.accepted.project != project.id || command.principal.value.empty() ||
          command.progress.accepted.command.value.empty() || command.progress.accepted.commit.value.empty() ||
          !command.progress.result ||
          (schema == 1 && (simple || std::holds_alternative<ForkReviewRequest>(command.input))) ||
          (std::holds_alternative<TaskCommand>(command.input) != std::holds_alternative<TaskUpdated>(*command.progress.result)) ||
          (std::holds_alternative<SharedContextCommand>(command.input) != std::holds_alternative<SharedContextRef>(*command.progress.result)) ||
          (std::holds_alternative<ForkReviewRequest>(command.input) != std::holds_alternative<ForkReviewCreated>(*command.progress.result)) ||
          (simple != std::holds_alternative<std::monostate>(*command.progress.result))) {
        return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored project command");
      }
      if (command.source_change && (schema == 1 || !ValidContextSourceChange(project, command))) {
        return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid stored context source change");
      }
      if (const auto* created = std::get_if<ForkReviewCreated>(&*command.progress.result)) {
        const auto found = project.fork_reviews.find(created->review);
        if (found == project.fork_reviews.end() || found->second.request != std::get<ForkReviewRequest>(command.input) ||
            !std::ranges::equal(created->branches, found->second.candidates, {}, std::identity{}, &ForkCandidate::session)) {
          return Fail<ProjectData>(ErrorCode::PersistenceFailure, "Invalid comparison receipt");
        }
      }
    }
    return AgentResult<ProjectData>::Success(std::move(project));
  } catch (const std::exception& error) {
    return Fail<ProjectData>(ErrorCode::PersistenceFailure, error.what());
  }
}

bool ValidStoredSession(const StoredSession& image, const std::string& key) {
  const auto& data = image.data;
  if (key.empty() || data.summary.id.value != key || data.retention != SessionRetention::Stored ||
      !data.summary.metadata_revision || !data.summary.binding_revision || !data.sequence ||
      data.runs.size() != image.runs.size()) return false;
  std::set<RunId> run_ids;
  std::map<RunId, RunId> delegated;
  for (std::size_t index = 0; index < image.runs.size(); ++index) {
    const auto& stored = image.runs[index];
    const auto& run = stored.view;
    if ((run.trigger == RunTrigger::Delegation) != stored.delegate.has_value() ||
        run.parent.has_value() != stored.delegate.has_value()) return false;
    if (run.parent) {
      if (!run_ids.contains(*run.parent) || data.summary.active_run == run.id) return false;
      const auto expected = delegated.find(run.id);
      if (expected == delegated.end() || expected->second != *run.parent) return false;
      delegated.erase(expected);
    }
    for (const auto& [command, children] : stored.delegations) {
      if (command.value.empty() || children.empty()) return false;
      for (const auto& child : children) {
        if (child == run.id || run_ids.contains(child) || !delegated.emplace(child, run.id).second) return false;
      }
    }
    for (const auto& wait : run.waits) {
      if (wait.kind != WaitKind::Delegation) continue;
      const auto* command = std::get_if<CommandId>(&wait.subject);
      if (!command || !stored.delegations.contains(*command)) return false;
    }
    if (run.id != data.runs[index] || run.session != data.summary.id || run.id.value.empty() ||
        !run_ids.insert(run.id).second) return false;
  }
  if (!delegated.empty()) return false;
  if (data.summary.active_run && !run_ids.contains(*data.summary.active_run)) return false;
  if (data.fork_branch && (!data.summary.project || data.fork_branch->review.value.empty() ||
      data.fork_branch->candidate.value.empty() || !run_ids.contains(data.fork_branch->run) || !data.origin.fork_point)) return false;
  for (const auto& item : data.queue) {
    if (!data.queue_owners.contains(item.id)) return false;
    if (const auto* message = std::get_if<CollaborationMessageId>(&item.input)) {
      if (!data.collaboration.contains(*message)) return false;
    } else if (const auto* change = std::get_if<CommandId>(&item.input)) {
      if (std::ranges::none_of(image.commands, [&](const auto& command) {
            const auto* accepted = std::get_if<ChangePlanAccepted>(&command.record.accepted);
            const auto& input = command.record.input;
            const bool change_operation = (input.operation == Operation::Rework && input.rework && !input.revert && !input.adoption) ||
                (input.operation == Operation::Revert && input.revert && !input.rework && !input.adoption) ||
                (input.operation == Operation::AdoptCandidate && input.adoption && !input.rework && !input.revert &&
                    input.adoption->target_session == data.summary.id);
            return command.principal == data.queue_owners.at(item.id) && command.record.progress.accepted.command == *change &&
                change_operation && command.record.artifact_change && accepted &&
                accepted->queue_item == item.id;
          })) return false;
    } else if (!item.user_start || !std::holds_alternative<SendInput>(item.input)) return false;
  }
  for (const auto& [id, entry] : data.collaboration) {
    if (id != entry.record.summary.id || id.value.empty() ||
        (entry.record.summary.target_run && !run_ids.contains(*entry.record.summary.target_run))) return false;
  }
  for (const auto& run : image.runs) {
    if (run.view.work_request && !data.collaboration.contains(*run.view.work_request)) return false;
  }
  if (data.current_goal && !data.goals.contains(*data.current_goal)) return false;
  for (const auto& [id, goal] : data.goals) {
    const auto& view = goal.view;
    if (id != view.ref.id || id.value.empty() || view.session != data.summary.id || !view.revision ||
        goal.owner.value.empty() || !view.ref.version || !goal.definitions.contains(view.ref.version) ||
        goal.definitions.at(view.ref.version) != view.spec.definition) return false;
    const bool review = view.state == GoalState::AwaitingCompletionApproval;
    if (review != view.completion_review.has_value() ||
        (review || view.state == GoalState::Verifying) != goal.verification.has_value()) return false;
    if (goal.verification) {
      if (!view.evidence || !goal.verification->revision || goal.verification->revision > view.revision ||
          std::ranges::none_of(image.commands, [&](const auto& item) {
            return item.principal == goal.verification->actor.principal &&
                item.record.progress.accepted.command == goal.verification->command && item.record.input.operation == Operation::Goal &&
                item.record.input.goal.has_value();
          })) return false;
    }
    if (review && (!view.verification || view.completion_review->definition != view.ref ||
        !view.completion_review->verification_revision || view.completion_review->verification_revision > view.revision ||
        view.completion_review->approval.session != data.summary.id)) return false;
  }
  std::set<std::pair<PrincipalId, CommandId>> command_ids;
  for (const auto& item : image.commands) {
    const auto& stamp = item.record.progress.accepted;
    if (!stamp.session || *stamp.session != data.summary.id || item.principal.value.empty() ||
        stamp.command.value.empty() || stamp.commit.value.empty() || !command_ids.emplace(item.principal, stamp.command).second) {
      return false;
    }
    const auto& record = item.record;
    if (record.input.operation == Operation::RenderedAction &&
        (!record.input.rendered_action || record.input.session != data.summary.id)) return false;
    if (record.input.operation == Operation::CreateComparison && !record.plan) return false;
    if (record.input.operation == Operation::ReconcileTool && (!record.input.reconciliation || record.input.session != data.summary.id ||
        record.progress.state != CommandState::Applied || std::ranges::none_of(data.tools, [&](const auto& call) {
          return call.id == record.input.reconciliation->call && call.run == record.input.run && call.reconciliation == stamp.command;
        }))) return false;
    if (!record.retired_comparison_workspaces.empty() && record.input.operation != Operation::CreateComparison) return false;
    if (record.plan) {
      const InteractionSnapshot* interaction = nullptr;
      if (const auto& pending = record.progress.approval; pending) {
        if (pending->session != data.summary.id) return false;
        const auto found = std::ranges::find(data.interactions, pending->interaction, &InteractionSnapshot::id);
        if (found == data.interactions.end() || found->recipient != item.principal || found->version < pending->version) return false;
        const auto* approval = std::get_if<OperationApproval>(&found->body);
        if (!approval || approval->pending_command != stamp.command) return false;
        interaction = &*found;
      } else if (record.input.operation != Operation::CreateComparison || record.progress.state == CommandState::AwaitingApproval) {
        return false;
      }
      switch (record.input.operation) {
      case Operation::ActivateSkill:
        if (!record.input.skill || !ValidSkillRef(record.input.skill->skill) || interaction->run != record.input.run) return false;
        break;
      case Operation::Create:
        if (!record.input.options || !record.plan->new_agent) return false;
        break;
      case Operation::Fork:
        if (!record.input.options || !record.input.fork || !record.plan->new_agent || !record.plan->target_binding) return false;
        break;
      case Operation::CreateComparison: {
        const auto& request = record.input.comparison;
        const auto& plan = record.plan->comparison;
        if (!request || !plan || !record.plan->target_binding || plan->review.value.empty() ||
            record.input.session != data.summary.id || request->from.session != data.summary.id ||
            request->project != data.summary.project || request->candidates.size() < 2 ||
            plan->candidates.size() != request->candidates.size()) return false;
        std::set<SessionId> reserved;
        for (std::size_t i = 0; i < plan->candidates.size(); ++i) {
          const auto& pinned = plan->candidates[i];
          if (pinned.session.value.empty() || !reserved.insert(pinned.session).second || !pinned.agent.version ||
              pinned.agent.id != request->candidates[i].session.agent || !pinned.start.model.version) return false;
        }
        if (record.progress.state == CommandState::Applied) {
          const auto* result = record.progress.result ? std::get_if<ForkReviewCreated>(&*record.progress.result) : nullptr;
          if (!result || result->review != plan->review || !std::ranges::equal(result->branches, plan->candidates, {},
              std::identity{}, &CandidateApprovalBinding::session)) return false;
        }
        if (!record.retired_comparison_workspaces.empty()) {
          if (record.progress.state == CommandState::AwaitingApproval || record.progress.state == CommandState::Scheduled ||
              record.progress.state == CommandState::Applied) return false;
          std::set<WorkspaceCopyPlanId> retired;
          std::set<WorkspaceId> workspaces;
          for (const auto& copy : record.retired_comparison_workspaces) {
            if (copy.workspace.value.empty() || !retired.insert(copy.copy.id).second || !workspaces.insert(copy.workspace).second ||
                std::ranges::none_of(request->candidates, [&](const auto& candidate) {
                  return candidate.workspace.copy_plan == copy.copy && candidate.workspace.workspace != copy.workspace;
                })) return false;
          }
        }
        break;
      }
      case Operation::SendMessage:
        if (!record.input.delivery || !record.plan->target_binding) return false;
        break;
      case Operation::Reply:
        if (!record.input.final_reply || !record.input.message || !record.plan->target_binding) return false;
        break;
      case Operation::StartRequest:
        if (!record.input.message || !record.plan->target_binding || !record.plan->run_start) return false;
        break;
      case Operation::Goal:
        if (!record.input.goal || record.input.session != data.summary.id) return false;
        break;
      case Operation::RenderedAction:
        if (!record.plan->rendered_action || !record.plan->target_binding) return false;
        break;
      default:
        return false;
      }
    }
  }
  return true;
}

bool MatchesRecoveredEffect(const std::vector<ResourceEffect>& effects, const ResourceEffect& recovered) {
  if (effects.empty()) return true;
  if (effects.size() != 1 || (effects.front().undo && effects.front().undo != recovered.undo)) return false;
  auto prior = effects.front();
  prior.undo = recovered.undo;
  return prior == recovered;
}

void RecoverPublishedEffects(StoredSession& image,
    const std::map<ToolId, std::shared_ptr<const ToolDefinition>>& tools) {
  auto& session = image.data;
  const auto available = [&](const StoredArtifact& stored, const ArtifactRef& ref) {
    return stored.record.metadata.ref == ref && stored.record.session == session.summary.id &&
        stored.record.availability == ArtifactAvailability::Available && !stored.handle.empty();
  };
  for (auto& call : session.tools) {
    if (call.state != ToolCallState::Succeeded || call.reason || call.tool.catalog || call.artifacts.size() != 1 ||
        (call.effects.size() == 1 && call.effects.front().undo)) continue;
    const auto tool = tools.find(call.tool.id);
    if (tool == tools.end() || tool->second->version != call.tool.version ||
        tool->second->effect != ToolEffect::StageArtifact || !tool->second->recover_published_effect) continue;
    const auto& ref = call.artifacts.front();
    if (ref.version < 2) continue;
    const ArtifactRef previous{ref.id, ref.version - 1};
    const auto published = std::ranges::find_if(session.artifacts, [&](const auto& entry) { return available(entry, ref); });
    if (published == session.artifacts.end() || published->record.run != call.run || published->record.call != call.id ||
        std::ranges::none_of(published->record.dependencies, [&](const ArtifactDependency& dependency) {
          return !dependency.output_part && dependency.input == SourceSlice{previous};
        })) continue;
    const auto base = std::ranges::find_if(session.artifacts, [&](const auto& entry) { return available(entry, previous); });
    if (base == session.artifacts.end() || base->record.metadata.type != published->record.metadata.type) continue;
    const auto run = std::ranges::find_if(image.runs, [&](const StoredRun& entry) { return entry.view.id == call.run; });
    if (run == image.runs.end()) continue;
    const auto request = std::ranges::find_if(run->tool_requests, [&](const auto& entry) { return entry.second.call == call.id; });
    ToolResult* result = nullptr;
    if (request != run->tool_requests.end()) {
      auto& record = request->second;
      if (record.failure || record.ref != call.tool || !record.result || record.result->call != call.id ||
          record.result->outcome != ToolOutcome::Succeeded || record.result->reason ||
          std::ranges::find(record.result->published_artifacts, ref) == record.result->published_artifacts.end()) continue;
      result = &*record.result;
    }
    auto effect = tool->second->recover_published_effect(base->record, published->record);
    if (!effect || effect->resource.value.empty() || effect->state != ResourceEffectState::Applied || effect->error ||
        !effect->before || !effect->after || effect->before->id != effect->resource || effect->after->id != effect->resource ||
        effect->before->version.empty() || effect->after->version.empty() || effect->before == effect->after ||
        !effect->undo || effect->undo->support != UndoSupport::RestoreVersion || effect->undo->handle.empty() ||
        !MatchesRecoveredEffect(call.effects, *effect) || (result && !MatchesRecoveredEffect(result->effects, *effect))) continue;
    call.effects = {*effect};
    if (result) result->effects = {std::move(*effect)};
  }
}

AgentResult<StoredSession> DecodeStoredSession(const StoreValue& value,
    const std::map<ToolId, std::shared_ptr<const ToolDefinition>>& tools) {
  try {
    auto bytes = std::span<const std::byte>(value.data);
    const auto as_unsigned = reinterpret_cast<const std::uint8_t*>(bytes.data());
    auto document = nlohmann::json::from_cbor(as_unsigned, as_unsigned + bytes.size());
    const auto schema = document.at("schema").get<unsigned>();
    if (schema != 1 && schema != session_schema_version) {
      return Fail<StoredSession>(ErrorCode::Unsupported, "Unknown AgentKit session schema");
    }
    auto& data = document.at("session");
    for (auto& command : data.at("commands")) {
      auto& record = command.at("record");
      if (!record.contains("artifact_change") && record.contains("rework")) {
        auto prior = std::move(record.at("rework"));
        record["artifact_change"] = std::move(prior);
      }
    }
    auto image = data.get<StoredSession>();
    for (auto& run : image.runs) {
      if (!run.prepared_tool) continue;
      const auto call = run.prepared_tool->call;
      const bool hosted = std::ranges::any_of(run.tool_requests, [&](const auto& item) { return item.second.call == call; });
      if (!hosted && !run.pending_calls.empty() && !run.pending_calls.front().local_call) {
        run.pending_calls.front().local_call = call;
        run.pending_calls.front().budget_reserved = true;
      }
      run.tool_plans.try_emplace(call, ToolPlanState{std::move(run.prepared_tool), std::move(run.tool_approval)});
      run.prepared_tool.reset();
      run.tool_approval.reset();
    }
    if (!ValidStoredSession(image, value.key)) {
      return Fail<StoredSession>(ErrorCode::PersistenceFailure, "Invalid stored session record");
    }
    RecoverPublishedEffects(image, tools);
    return AgentResult<StoredSession>::Success(std::move(image));
  } catch (const std::exception& error) {
    return Fail<StoredSession>(ErrorCode::PersistenceFailure, error.what());
  }
}

void Complete(const std::shared_ptr<SaveTicket>& ticket, AgentResult<void> result) {
  ticket->result = std::move(result);
  for (const auto& waiter : std::exchange(ticket->waiters, {})) waiter.Complete(*ticket->result);
}

template<class Range, class Key, class Value, class Add>
void AppendChanges(const Range* before, const Range& after, Key key, Value value, const Add& add) {
  using Entry = typename Range::value_type;
  std::map<std::decay_t<decltype(key(std::declval<const Entry&>()))>, const Entry*> prior;
  if (before) for (const auto& item : *before) prior.emplace(key(item), &item);
  for (const auto& item : after) {
    auto old = prior.find(key(item));
    if (old == prior.end() || nlohmann::json(value(*old->second)) != nlohmann::json(value(item))) add(value(item));
  }
}

void RecordEvents(StoredSession& image, const StoredSession* before, const CommitId& commit,
    const std::optional<CommandId>& command, const std::optional<PrincipalId>& owner,
    std::vector<StoredSessionEvent>& events) {
  auto& current = image.data;
  const auto* prior = before ? &before->data : nullptr;
  current.event_sequence = prior ? prior->event_sequence : 0;
  const auto initial = current.event_sequence;
  const auto add = [&](SessionEventBody body) {
    std::vector<PrincipalId> recipients;
    if (std::holds_alternative<SessionSummary>(body)) {
      for (const auto& item : current.interactions) {
        if (item.state == InteractionState::Pending) recipients.push_back(item.recipient);
      }
    }
    events.push_back({{current.summary.id, ++current.event_sequence, commit, command, std::move(body)}, owner,
        std::move(recipients)});
  };
  if (!prior) add(current.origin);
  if (!prior || current.summary != prior->summary) add(current.summary);
  auto previous_message = prior ? prior->messages.begin() : current.messages.end();
  for (const auto& message : current.messages) {
    if (!prior || previous_message == prior->messages.end() || previous_message->sequence != message.sequence) {
      add(message);
      continue;
    }
    const auto& previous = *previous_message++;
    if (previous.ref != message.ref || previous.state != message.state) {
      add(message);
    } else if (message.state == MessageState::Streaming) {
      const auto& block = message.blocks.front();
      const auto& text = std::get<TextBlock>(block.body).text;
      const auto& old_text = std::get<TextBlock>(previous.blocks.front().body).text;
      if (text.size() > old_text.size()) add(MessageDelta{message.ref, block.id, text.substr(old_text.size())});
    }
  }
  AppendChanges(before ? &before->runs : nullptr, image.runs,
      [](const StoredRun& run) { return run.view.id; }, [](const StoredRun& run) -> const auto& { return run.view; }, add);
  const auto identity = [](const auto& item) -> const auto& { return item; };
  const auto key = [](const auto& item) -> const auto& { return item.id; };
  AppendChanges(prior ? &prior->tools : nullptr, current.tools, key, identity, add);
  AppendChanges(prior ? &prior->interactions : nullptr, current.interactions, key, identity, add);
  AppendChanges(prior ? &prior->queue : nullptr, current.queue, key, identity, add);
  for (const auto& [id, item] : current.collaboration) {
    const auto& summary = item.record.summary;
    const auto old = prior ? prior->collaboration.find(id) : current.collaboration.end();
    if (!prior || old == prior->collaboration.end() || nlohmann::json(old->second.record.summary) != nlohmann::json(summary))
      add(summary);
  }
  const auto first_artifact = prior ? std::min(prior->artifacts.size(), current.artifacts.size()) : 0;
  for (std::size_t i = first_artifact; i < current.artifacts.size(); ++i) {
    add(ArtifactRegistered{current.artifacts[i].record.metadata.ref});
  }
  // A legacy image supplies the replay baseline. A later commit starts recording from that state.
  if (prior && current.event_sequence == initial && (command || current.sequence != prior->sequence)) add(current.summary);
}

Task<void> SettleInterrupted(StoredSession& stored, RuntimeState& runtime) {
  auto& session = stored.data;
  const auto reason = Error(ErrorCode::InvalidState, "Run interrupted before a resumable checkpoint");
  const auto unknown = Error(ErrorCode::ResultUnknown, "Reconcile the original tool invocation before retrying");
  for (auto& run : stored.runs) {
    const auto parent = run.view.parent ? std::ranges::find_if(stored.runs,
        [&](const auto& candidate) { return candidate.view.id == run.view.parent; }) : stored.runs.end();
    const bool parent_closed = parent != stored.runs.end() && Terminal(parent->view.state);
    if (Terminal(run.view.state) || (!parent_closed && runtime.CanRestore(run, session))) continue;
    auto uncertain = false;
    for (auto& call : session.tools) {
      if (call.run != run.view.id) continue;
      if (call.state == ToolCallState::Executing) {
        const auto definition = runtime.ResolveTool(call.tool);
        if (definition && definition->lookup_result) {
          try {
            auto known = co_await definition->lookup_result(call.id);
            if (known.Succeeded() && known.Value() && known.Value()->outcome != ToolOutcome::ResultUnknown &&
                known.Value()->new_artifacts.empty()) {
              auto result = std::move(*known.Value());
              call.state = StateForOutcome(result.outcome);
              call.effects = std::move(result.effects);
              call.artifacts = std::move(result.existing_artifacts);
              call.reason = std::move(result.reason);
              continue;
            }
          } catch (...) {
            runtime.ReportException(std::current_exception());
          }
        }
        const bool may_write = !definition || definition->effect != ToolEffect::Read;
        call.state = may_write ? ToolCallState::ResultUnknown : ToolCallState::Cancelled;
        if (may_write) { uncertain = true; call.reason = unknown; }
      } else if (call.state == ToolCallState::Preparing || call.state == ToolCallState::AwaitingApproval ||
                 call.state == ToolCallState::Ready || call.state == ToolCallState::Reviewing) {
        if (call.review && (call.review->outcome == ToolReviewOutcome::Pending ||
                            call.review->outcome == ToolReviewOutcome::Approved)) {
          call.review->outcome = ToolReviewOutcome::Invalidated;
          call.review->reason = "The reviewed plan could not be restored";
        }
        call.state = ToolCallState::Cancelled;
      }
    }
    for (auto& interaction : session.interactions) {
      if (interaction.run == run.view.id && interaction.state == InteractionState::Pending) {
        runtime.CloseInteraction(session, interaction, InteractionState::Cancelled);
      }
    }
    for (auto& item : stored.commands) {
      if (item.record.input.operation == Operation::ActivateSkill && item.record.input.run == run.view.id &&
          (item.record.progress.state == CommandState::Scheduled || item.record.progress.state == CommandState::AwaitingApproval)) {
        runtime.Resolve(item.record, session, CommandState::Withdrawn, reason);
        item.record.progress.resolution->commit = {};
      }
    }
    for (auto& message : session.messages) {
      if (message.run == run.view.id && message.state == MessageState::Streaming) {
        message.state = MessageState::Interrupted;
        ++message.ref.version;
      }
    }
    for (auto& steer : run.view.steering) {
      if (steer.state != SteerState::Pending) continue;
      steer.state = SteerState::NotApplied;
      steer.reason = reason;
      runtime.SettleCollaborationSteer(session, steer, false);
      for (auto& item : stored.commands) {
        auto* accepted = std::get_if<RunSteered>(&item.record.accepted);
        if (accepted && accepted->message == steer.message) {
          runtime.Resolve(item.record, session, CommandState::Failed, reason);
        }
      }
    }
    const bool revised = run.view.end_reason == RunEndReason::GoalRevised;
    run.view.state = revised || parent_closed ? RunState::Cancelled : RunState::Failed;
    runtime.FinishWork(session, run.view);
    if (parent_closed) run.view.end_reason = RunEndReason::ParentCancelled;
    else if (!revised) run.view.end_reason = uncertain ? RunEndReason::ToolOutcomeUnknown : RunEndReason::Interrupted;
    if (!revised || uncertain) run.view.failure = uncertain ? unknown : reason;
    run.view.waits.clear();
    run.tool_plans.clear();
    run.items.clear();
    run.pending_steers.clear();
    run.continuation.reset();
    run.pending_calls.clear();
    run.host_skill_input.reset();
    run.host_model_step.reset();
    run.model_boundary = false;
    for (auto& [command, record] : run.tool_requests) {
      if (!record.result && !record.failure) record.failure = uncertain ? unknown : reason;
      if (ArtifactChangeTool(record.ref.id)) {
        const auto owner = std::ranges::find_if(stored.commands, [&](const auto& item) {
          return item.principal == run.principal && item.record.progress.accepted.command == command;
        });
        if (owner != stored.commands.end()) {
          runtime.SettleArtifactChange(owner->record, session, record);
          if (owner->record.progress.resolution) owner->record.progress.resolution->commit = {};
        }
      }
    }
    if (session.summary.active_run == run.view.id) {
      session.summary.active_run.reset();
      session.summary.run_state = run.view.state;
    }
    ++session.sequence;
  }
}

} // namespace

StoredSession RuntimeState::Capture(const SessionData& session) const {
  StoredSession image{.data = session};
  image.data.waiters.clear();
  image.data.interaction_timers.clear();
  image.data.consuming = false;
  for (const auto& id : session.runs) {
    const auto& run = runs.at(id);
    auto model = models.find(run.view.model.id);
    const bool keep_resume = model != models.end() && model->second.ref == run.view.model &&
                             model->second.allow_persist_resume_state;
    std::map<ToolCallId, ToolPlanState> tool_plans;
    for (const auto& [call, attempt] : run.tool_attempts) {
      if (attempt.plan.prepared || attempt.plan.source_revision) tool_plans.emplace(call, attempt.plan);
    }
    image.runs.push_back({.view = run.view, .principal = run.principal, .input = run.input,
        .deadline = run.deadline, .tool_plans = std::move(tool_plans),
        .items = run.items, .separate_inherited_history = run.separate_inherited_history, .pending_steers = run.pending_steers,
        .continuation = keep_resume ? run.continuation : std::nullopt,
        .continuation_missing = !keep_resume && run.continuation.has_value(), .pending_calls = run.pending_calls,
        .model_boundary = run.model_boundary, .checkpoint = run.checkpoint, .form_requests = run.form_requests,
        .tool_requests = run.tool_requests, .model_skill_resources = run.model_skill_resources, .host_model_step = run.host_model_step,
        .artifact_action = run.artifact_action, .delegate = run.delegate, .delegations = run.delegations});
  }
  for (const auto& [key, record] : commands) {
    if (record.progress.accepted.session == session.summary.id) image.commands.push_back({key.first, record});
  }
  return image;
}

bool RuntimeState::CanRestore(const StoredRun& run, const SessionData& session) const {
  if (Terminal(run.view.state) || run.view.state == RunState::Cancelling ||
      session.summary.lifecycle != SessionLifecycle::Open ||
      (run.delegate ? !session.summary.active_run : session.summary.active_run != run.view.id)) return false;
  auto agent = agents.find(run.view.agent.id);
  auto model = models.find(run.view.model.id);
  if (agent == agents.end() || agent->second.ref != run.view.agent || model == models.end() ||
      model->second.ref != run.view.model || run.continuation_missing ||
      (run.continuation && (!model->second.allow_persist_resume_state ||
          run.continuation->prefix_items > run.items.size() || run.continuation->state.provider != run.view.model))) {
    return false;
  }
  if (std::ranges::any_of(session.tools, [&](const ToolCallSnapshot& call) {
        return call.run == run.view.id && call.state == ToolCallState::Executing;
      }) || std::ranges::any_of(session.messages, [&](const Message& message) {
        return message.run == run.view.id && message.state == MessageState::Streaming;
      })) return false;
  if (run.view.trigger == RunTrigger::ArtifactChange) {
    if (run.tool_requests.size() != 1) return false;
    const auto& tool = run.tool_requests.begin()->second;
    return ArtifactChangeTool(tool.ref.id) && ResolveTool(tool.ref) &&
        (tool.result || tool.failure || std::ranges::any_of(session.tools, [&](const auto& snapshot) {
          if (snapshot.id != tool.call || snapshot.run != run.view.id || snapshot.tool != tool.ref) return false;
          return snapshot.state == ToolCallState::Preparing || snapshot.state == ToolCallState::Reviewing ||
                 snapshot.state == ToolCallState::AwaitingApproval || snapshot.state == ToolCallState::Ready;
        }));
  }
  const auto workflow = workflows.find(run.view.agent.id);
  if (run.delegate && !run.delegate->started) return true;
  if (workflow != workflows.end()) {
    if ((!run.input.skills.empty() || run.artifact_action || run.view.trigger == RunTrigger::ForkComparison) &&
        run.model_boundary && !run.checkpoint && !run.view.usage.model_turns &&
        run.form_requests.empty() && run.tool_requests.empty()) return true;
    return workflow->second.restore && run.checkpoint && !run.checkpoint->stage.empty() && run.checkpoint->schema_version;
  }
  bool registered_tool = false;
  for (const auto& pending : run.pending_calls) {
    if (!pending.local_call) continue;
    registered_tool = true;
    const auto call = std::ranges::find(session.tools, *pending.local_call, &ToolCallSnapshot::id);
    if (call == session.tools.end() || call->run != run.view.id || call->tool != pending.tool || !pending.budget_reserved)
      return false;
    if (pending.terminal_error) continue;
    if (pending.result) {
      if (pending.result->call != pending.local_call || pending.result->provider_call_id != pending.call.provider_call_id)
        return false;
      continue;
    }
    if (call->state == ToolCallState::Preparing) continue;
    const auto plan = run.tool_plans.find(call->id);
    if (plan == run.tool_plans.end() || !plan->second.prepared || call->plan != plan->second.prepared->plan) return false;
    if (call->state == ToolCallState::Ready) continue;
    if (call->state == ToolCallState::AwaitingApproval && plan->second.approval) continue;
    if (call->state != ToolCallState::Reviewing || !call->review || call->review->plan != call->plan) return false;
  }
  return (run.model_boundary || registered_tool) && !run.items.empty();
}

std::shared_ptr<SaveTicket> RuntimeState::Persist(const SessionData& session, bool order, CommitId id,
    const SessionData* other, const ProjectData* project, std::span<const SessionData* const> additional) {
  if (id.value.empty()) id = CommitId{MakeId()};
  std::map<SessionId, const SessionData*> affected{{session.summary.id, &session}};
  if (other) affected.emplace(other->summary.id, other);
  for (const auto* entry : additional) affected.emplace(entry->summary.id, entry);
  std::optional<std::pair<PrincipalId, CommandId>> command_scope;
  bool ambiguous_command = false;
  const auto associate = [&](const PrincipalId& principal, const CommandId& command) {
    if (!command_scope) command_scope = {principal, command};
    else if (*command_scope != std::pair{principal, command}) ambiguous_command = true;
  };
  for (auto& [key, command] : commands) {
    auto& resolution = command.progress.resolution;
    if (resolution && resolution->commit.value.empty() && resolution->session && affected.contains(*resolution->session)) {
      resolution->commit = id;
      resolution->session_sequence = affected.at(*resolution->session)->sequence;
    }
    if (command.progress.accepted.commit == id || (resolution && resolution->commit == id)) associate(key.first, key.second);
  }
  std::vector<StoredSession> snapshots;
  std::vector<StoredSessionEvent> events;
  std::map<ProjectId, const ProjectData*> changed_projects;
  if (project) changed_projects.emplace(project->id, project);
  for (const auto& [session_id, data] : affected) {
    if (auto* updated = RefreshForkAdoptions(*data)) changed_projects.insert_or_assign(updated->id, updated);
    if (auto pending = pending_source_changes.extract(session_id); !pending.empty()) {
      const auto& project_id = *data->summary.project;
      auto& target = projects.try_emplace(project_id, ProjectData{.id = project_id}).first->second;
      ++target.revision;
      for (auto& event : pending.mapped()) {
        CommitStamp stamp{.command = std::move(event.command), .commit = id,
            .project = project_id, .project_revision = target.revision};
        target.commands.push_back({std::move(event.principal), std::move(event.run), std::move(event.change),
            {.accepted = std::move(stamp), .state = CommandState::Applied, .result = CommandValue{std::monostate{}}}});
      }
      changed_projects.insert_or_assign(project_id, &target);
    }
    snapshots.push_back(Capture(*data));
  }
  for (const auto& snapshot : snapshots) {
    for (const auto& run : snapshot.runs) {
      for (const auto& [command, form] : run.form_requests)
        if (form.stamp.commit == id) associate(run.principal, command);
      for (const auto& [command, tool] : run.tool_requests)
        if (tool.stamp.commit == id) associate(run.principal, command);
    }
  }
  for (const auto& [project_id, data] : changed_projects) {
    for (const auto& command : data->commands) {
      if (command.progress.accepted.commit == id || (command.progress.resolution && command.progress.resolution->commit == id))
        associate(command.principal, command.progress.accepted.command);
    }
  }
  const auto command = command_scope && !ambiguous_command ? std::optional{command_scope->second} : std::nullopt;
  const auto command_owner = command ? std::optional{command_scope->first} : std::nullopt;
  for (auto& snapshot : snapshots) {
    const auto& session_id = snapshot.data.summary.id;
    const StoredSession* prior = nullptr;
    for (auto pending = saves.rbegin(); pending != saves.rend() && !prior; ++pending) {
      const auto found = std::ranges::find_if(pending->images,
          [&](const StoredSession& image) { return image.data.summary.id == session_id; });
      if (found != pending->images.end()) prior = &*found;
    }
    if (!prior) {
      if (auto saved = saved_sessions.find(session_id); saved != saved_sessions.end()) prior = &saved->second.image;
      else if (auto saved = ephemeral_sessions.find(session_id); saved != ephemeral_sessions.end()) prior = &saved->second;
    }
    RecordEvents(snapshot, prior, id, command, command_owner, events);
  }
  std::vector<ProjectData> project_snapshots;
  for (const auto& [project_id, data] : changed_projects) {
    if (options.store) project_snapshots.push_back(*data);
    else InvalidateSourcePlans(project_id);
  }
  return EnqueueSave({std::move(id), std::move(snapshots), order, {}, std::move(project_snapshots), std::move(events)});
}

std::shared_ptr<SaveTicket> RuntimeState::PersistProject(const ProjectData& project, CommitId id, const SessionData* session) {
  if (session) return Persist(*session, true, std::move(id), nullptr, &project);
  if (!options.store) InvalidateSourcePlans(project.id);
  return EnqueueSave({std::move(id), {}, false, {}, options.store ? std::vector{project} : std::vector<ProjectData>{}});
}

std::shared_ptr<SaveTicket> RuntimeState::EnqueueSave(SaveChange change) {
  const bool durable = !change.projects.empty() || std::ranges::any_of(change.images,
      [](const StoredSession& image) { return image.data.retention == SessionRetention::Stored; });
  const bool ordered = !durable && std::ranges::any_of(saves, [&](const SaveChange& pending) {
    return std::ranges::any_of(change.images, [&](const StoredSession& image) {
      return std::ranges::any_of(pending.images,
          [&](const StoredSession& prior) { return prior.data.summary.id == image.data.summary.id; });
    });
  });
  if (!durable && !ordered) {
    PublishSessions(change);
    Changed(change.order);
    return {};
  }
  auto ticket = std::make_shared<SaveTicket>();
  if (storage_error) {
    Complete(ticket, AgentResult<void>::Failure(*storage_error));
    return ticket;
  }
  change.ticket = ticket;
  saves.push_back(std::move(change));
  if (!saving) {
    saving = true;
    options.tasks.Launch([self = shared_from_this()] { return self->WritePending(); });
  }
  return ticket;
}

Async<void> RuntimeState::WhenSaved(std::shared_ptr<SaveTicket> ticket) {
  if (!ticket) co_return AgentResult<void>::Success();
  if (ticket->result) co_return *ticket->result;
  auto await_commit = [ticket](CallbackCompletion<AgentResult<void>> ready) {
    if (ticket->result) ready.Complete(*ticket->result);
    else ticket->waiters.push_back(std::move(ready));
    return std::function<void()>{};
  };
  co_return co_await AwaitCallback<AgentResult<void>>(std::move(await_commit));
}

Task<void> RuntimeState::WritePending() {
  while (!saves.empty() && !storage_error) {
    auto& pending = saves.front();
    StoreTransaction request{pending.id, store_revision, {}};
    std::set<SessionId> durable_sessions;
    for (const auto& image : pending.images) {
      if (image.data.retention != SessionRetention::Stored) continue;
      const auto& session_id = image.data.summary.id;
      durable_sessions.insert(session_id);
      auto encoded = EncodeStoreRecord("session", image, session_schema_version);
      if (!encoded.Succeeded()) { storage_error = encoded.Error(); break; }
      auto previously_saved = saved_sessions.find(session_id);
      auto prior = previously_saved == saved_sessions.end() ? std::nullopt
          : std::optional<Revision>(previously_saved->second.revision);
      request.writes.push_back({std::string(collection), session_id.value, prior, std::move(encoded).Value()});
    }
    if (!storage_error && !durable_sessions.empty()) {
      std::vector<StoredSessionEvent> events;
      for (const auto& item : pending.events) {
        if (durable_sessions.contains(item.event.session)) events.push_back(item);
      }
      if (!events.empty()) {
        auto encoded = EncodeStoreRecord("events", events, 1);
        if (!encoded.Succeeded()) storage_error = encoded.Error();
        else request.writes.push_back({std::string(event_collection), pending.id.value, {}, std::move(encoded).Value()});
      }
    }
    for (const auto& project : pending.projects) {
      if (storage_error) break;
      auto encoded = EncodeStoreRecord("project", project, project_schema_version);
      if (!encoded.Succeeded()) { storage_error = encoded.Error(); break; }
      auto prior = saved_projects.find(project.id);
      auto expected = prior == saved_projects.end() ? std::nullopt : std::optional{prior->second.revision};
      request.writes.push_back({std::string(project_collection), project.id.value, expected, std::move(encoded).Value()});
    }
    if (storage_error) break;
    if (!request.writes.empty()) {
      auto result = Fail<Revision>(ErrorCode::CommitUncertain, "Store did not confirm the transaction");
      try {
        result = co_await options.store->CommitAsync(std::move(request));
      } catch (...) {
        ReportException(std::current_exception());
      }
      try {
        if (!result.Succeeded() && result.Error().code == ErrorCode::CommitUncertain) {
          auto found = co_await options.store->FindCommitAsync(pending.id);
          if (found.Succeeded() && found.Value()) result = AgentResult<Revision>::Success(*found.Value());
        }
      } catch (...) {
        ReportException(std::current_exception());
      }
      if (!result.Succeeded() || result.Value() <= store_revision) {
        storage_error = result.Succeeded() ? Error(ErrorCode::PersistenceFailure, "Store returned a stale commit revision")
                                          : result.Error();
        break;
      }
      store_revision = result.Value();
    }
    auto completed = std::move(pending);
    saves.pop_front();
    PublishSessions(completed);
    std::vector<ProjectId> affected_projects;
    for (auto& project : completed.projects) {
      affected_projects.push_back(project.id);
      saved_projects.insert_or_assign(affected_projects.back(), SavedProject{store_revision, std::move(project)});
    }
    for (const auto& project : affected_projects) InvalidateSourcePlans(project);
    Changed(completed.order);
    Complete(completed.ticket, AgentResult<void>::Success());
  }
  if (storage_error) {
    auto error = AgentResult<void>::Failure(*storage_error);
    auto pending = std::exchange(saves, {});
    for (const auto& change : pending) Complete(change.ticket, error);
  }
  saving = false;
}

void RuntimeState::PublishSessions(SaveChange& change) {
  for (auto& image : change.images) {
    const auto id = image.data.summary.id;
    if (image.data.retention == SessionRetention::Stored)
      saved_sessions.insert_or_assign(id, SavedSession{store_revision, std::move(image)});
    else
      ephemeral_sessions.insert_or_assign(id, std::move(image));
  }

  std::set<SessionId> recheck;
  for (auto& item : change.events) {
    const bool references_artifacts = std::visit([](const auto& body) {
      using T = std::remove_cvref_t<decltype(body)>;
      if constexpr (std::is_same_v<T, ArtifactRegistered>) return true;
      else if constexpr (std::is_same_v<T, ToolCallSnapshot>) return !body.artifacts.empty();
      else if constexpr (std::is_same_v<T, RunSnapshot>) return body.result && !body.result->artifacts.empty();
      else if constexpr (std::is_same_v<T, Message>) {
        ArtifactVersions refs;
        for (const auto& block : body.blocks) CollectArtifactRefs(refs, block);
        return !refs.empty();
      } else if constexpr (std::is_same_v<T, InteractionSnapshot>) {
        if (const auto* tool = std::get_if<ToolApproval>(&body.body)) return !tool->preview.artifacts.empty();
        if (const auto* operation = std::get_if<OperationApproval>(&body.body)) return !operation->preview.artifacts.empty();
      } else if constexpr (std::is_same_v<T, QueuedInput>) {
        const auto* input = std::get_if<SendInput>(&body.input);
        return input && (!input->attachments.empty() || !input->context.empty() || !input->side_chat_conclusions.empty());
      }
      return false;
    }, item.event.body);
    if (references_artifacts) recheck.insert(item.event.session);
    session_events[item.event.session].push_back(std::move(item));
  }

  // Refresh visibility after the committed images and journal entries are available together.
  for (const auto& [scope, sequence] : checked_session_artifacts)
    if (recheck.contains(scope.second)) ScheduleSessionArtifactCheck(scope.second, scope.first);
}

const SessionData* RuntimeState::Published(const SessionData& session) const {
  if (session.retention == SessionRetention::Ephemeral) {
    auto published = ephemeral_sessions.find(session.summary.id);
    return published == ephemeral_sessions.end() ? nullptr : &published->second.data;
  }
  auto saved = saved_sessions.find(session.summary.id);
  return saved == saved_sessions.end() ? nullptr : &saved->second.image.data;
}

Async<void> RuntimeState::LoadStored() {
  if (!options.store) co_return AgentResult<void>::Success();
  auto stored_image = co_await options.store->LoadAsync();
  if (!stored_image.Succeeded()) co_return AgentResult<void>::Failure(stored_image.Error());
  std::map<SessionId, SavedSession> recovered;
  std::map<SessionId, std::vector<StoredSessionEvent>> recovered_events;
  std::set<std::pair<PrincipalId, CommandId>> command_keys;
  std::set<RunId> run_ids;
  for (const auto& row : stored_image.Value().records) {
    if (row.collection != collection && row.collection != project_collection && row.collection != event_collection) continue;
    if (!row.revision || row.revision > stored_image.Value().revision || row.data.empty()) {
      co_return Fail<void>(ErrorCode::PersistenceFailure, "Invalid stored record revision or data");
    }
    if (row.collection == event_collection) {
      try {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(row.data.data());
        const auto document = nlohmann::json::from_cbor(begin, begin + row.data.size());
        if (document.at("schema").get<unsigned>() != 1)
          co_return Fail<void>(ErrorCode::Unsupported, "Unknown AgentKit event journal schema");
        for (auto& item : document.at("events").get<std::vector<StoredSessionEvent>>()) {
          const auto& event = item.event;
          if (row.key.empty() || event.commit.value != row.key || event.session.value.empty() || !event.sequence ||
              event.command.has_value() != item.command_owner.has_value())
            co_return Fail<void>(ErrorCode::PersistenceFailure, "Invalid stored session event");
          recovered_events[event.session].push_back(std::move(item));
        }
      } catch (const std::exception& error) {
        co_return Fail<void>(ErrorCode::PersistenceFailure, error.what());
      }
      continue;
    }
    if (row.collection == project_collection) {
      auto decoded = DecodeProject(row);
      if (!decoded.Succeeded()) co_return AgentResult<void>::Failure(decoded.Error());
      auto project = std::move(decoded).Value();
      for (const auto& command : project.commands) {
        if (!command_keys.emplace(command.principal, command.progress.accepted.command).second) {
          co_return Fail<void>(ErrorCode::PersistenceFailure, "Duplicate stored command");
        }
      }
      auto id = project.id;
      projects.emplace(id, project);
      if (!saved_projects.emplace(id, SavedProject{row.revision, std::move(project)}).second) {
        co_return Fail<void>(ErrorCode::PersistenceFailure, "Duplicate stored project");
      }
      continue;
    }
    auto session = DecodeStoredSession(row, tools);
    if (!session.Succeeded()) co_return AgentResult<void>::Failure(session.Error());
    for (const auto& run : session.Value().runs) {
      if (!run_ids.insert(run.view.id).second) co_return Fail<void>(ErrorCode::PersistenceFailure, "Duplicate stored run");
    }
    for (const auto& item : session.Value().commands) {
      if (!command_keys.emplace(item.principal, item.record.progress.accepted.command).second) {
        co_return Fail<void>(ErrorCode::PersistenceFailure, "Duplicate stored command");
      }
    }
    if (!recovered.emplace(SessionId{row.key}, SavedSession{row.revision, std::move(session).Value()}).second) {
      co_return Fail<void>(ErrorCode::PersistenceFailure, "Duplicate stored session");
    }
  }
  for (auto& [id, events] : recovered_events) {
    const auto session = recovered.find(id);
    if (session == recovered.end()) co_return Fail<void>(ErrorCode::PersistenceFailure, "Event journal has no session");
    const auto sequence = [](const StoredSessionEvent& item) { return item.event.sequence; };
    std::ranges::sort(events, {}, sequence);
    if (sequence(events.back()) > session->second.image.data.event_sequence ||
        std::ranges::adjacent_find(events, {}, sequence) != events.end())
      co_return Fail<void>(ErrorCode::PersistenceFailure, "Invalid event journal sequence");
  }
  store_revision = stored_image.Value().revision;
  saved_sessions = std::move(recovered);
  session_events = std::move(recovered_events);
  for (const auto& [id, saved] : saved_sessions) {
    const auto& session = saved.image.data;
    for (const auto& stored : saved.image.commands) {
      const auto& command = stored.record;
      if (command.input.operation != Operation::CreateComparison || command.progress.state != CommandState::Applied) continue;
      const auto& request = *command.input.comparison;
      const auto& created = std::get<ForkReviewCreated>(*command.progress.result);
      const auto project = projects.find(request.project);
      if (project == projects.end()) co_return Fail<void>(ErrorCode::PersistenceFailure, "Approved comparison has no project");
      const auto review = project->second.fork_reviews.find(created.review);
      if (review == project->second.fork_reviews.end() || review->second.request != request ||
          !std::ranges::equal(created.branches, review->second.candidates, {}, std::identity{}, &ForkCandidate::session))
        co_return Fail<void>(ErrorCode::PersistenceFailure, "Approved comparison does not match its command");
    }
    if (!session.fork_branch) continue;
    const auto project = projects.find(*session.summary.project);
    if (project == projects.end()) co_return Fail<void>(ErrorCode::PersistenceFailure, "Comparison branch has no project");
    const auto review = project->second.fork_reviews.find(session.fork_branch->review);
    if (review == project->second.fork_reviews.end() || std::ranges::none_of(review->second.candidates, [&](const auto& candidate) {
          return candidate.id == session.fork_branch->candidate && candidate.session == id && candidate.run == session.fork_branch->run;
        })) co_return Fail<void>(ErrorCode::PersistenceFailure, "Comparison branch has no registered candidate");
  }
  for (const auto& [id, project] : projects) {
    for (const auto& [review, adoption] : project.fork_adoptions) {
      const auto stored = saved_sessions.find(adoption.session);
      const auto& comparison = project.fork_reviews.at(review);
      if (stored == saved_sessions.end() || stored->second.image.data.summary.project != id ||
          std::ranges::none_of(stored->second.image.commands, [&](const auto& command) {
            const auto& record = command.record;
            return command.principal == adoption.principal && record.progress.accepted.command == adoption.command &&
                record.input.operation == Operation::AdoptCandidate && record.input.adoption && record.artifact_change &&
                record.input.adoption->review == review && record.input.adoption->candidate == comparison.selected &&
                record.input.adoption->target_session == adoption.session && record.artifact_change->call == comparison.adoption_call;
          })) co_return Fail<void>(ErrorCode::PersistenceFailure, "Comparison adoption has no matching command");
    }
  }
  std::vector<std::pair<SessionId, std::optional<RunId>>> interrupted;
  for (auto& [id, saved] : saved_sessions) {
    auto image = saved.image;
    if (std::ranges::any_of(image.runs, [&](const auto& run) {
          return !Terminal(run.view.state) && !CanRestore(run, image.data);
        })) {
      interrupted.emplace_back(id, image.data.summary.active_run);
      co_await SettleInterrupted(image, *this);
    }
    sessions.emplace(id, std::move(image.data));
    for (auto& entry : image.runs) {
      auto run_id = entry.view.id;
      const auto restoring = CanRestore(entry, sessions.at(id));
      std::map<ToolCallId, ToolAttempt> attempts;
      for (auto& [call, plan] : entry.tool_plans) attempts.emplace(call, ToolAttempt{std::move(plan)});
      if (!entry.host_model_step && entry.host_skill_input)
        entry.host_model_step = HostedModelStep{.input_index = entry.host_skill_input};
      runs.emplace(std::move(run_id), RunExecution{
          .view = std::move(entry.view), .principal = std::move(entry.principal), .input = std::move(entry.input),
          .artifact_action = std::move(entry.artifact_action),
          .items = std::move(entry.items), .separate_inherited_history = entry.separate_inherited_history,
          .pending_steers = std::move(entry.pending_steers),
          .continuation = std::move(entry.continuation), .pending_calls = std::move(entry.pending_calls),
          .model_boundary = entry.model_boundary, .deadline = entry.deadline,
          .model_skill_resources = std::move(entry.model_skill_resources),
          .tool_attempts = std::move(attempts),
          .checkpoint = std::move(entry.checkpoint), .form_requests = std::move(entry.form_requests),
          .tool_requests = std::move(entry.tool_requests),
          .restoring = restoring, .host_model_step = std::move(entry.host_model_step),
          .delegate = std::move(entry.delegate), .delegations = std::move(entry.delegations)});
    }
    for (auto& entry : image.commands) {
      auto key = std::pair(std::move(entry.principal), entry.record.progress.accepted.command);
      commands.emplace(std::move(key), std::move(entry.record));
    }
  }
  for (const auto& [id, active] : interrupted) {
    auto& session = sessions.at(id);
    const auto* owner = active && Terminal(runs.at(*active).view.state) ? FinishGoalRun(runs.at(*active).view) : nullptr;
    for (const auto& entry : saved_sessions.at(id).image.runs) {
      if (!entry.delegate || Terminal(entry.view.state)) continue;
      const auto& child = runs.at(entry.view.id).view;
      if (Terminal(child.state)) {
        if (auto* goal = FinishGoalRun(child)) owner = goal;
      }
    }
    ProjectData* affected = nullptr;
    for (const auto& run : session.runs) {
      if (auto project = FinishProjectRun(runs.at(run).view)) affected = project;
      if (auto project = FinishForkCandidate(runs.at(run).view)) affected = project;
    }
    auto committed = co_await WhenSaved(Persist(session, false, {}, owner, affected));
    if (!committed.Succeeded()) co_return committed;
  }
  co_return AgentResult<void>::Success();
}

} // namespace detail
} // namespace huxerui::agentkit
