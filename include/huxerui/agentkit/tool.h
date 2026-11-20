#pragma once

/// @file
/// @brief Tool registration, preparation, resource grants and approval policy.
///
/// The runtime validates input, prepares an immutable plan, checks approval and grants, then invokes ToolHandler.
/// Use ToolContext only for the current logical call. Report actual effects even when a tool fails or is cancelled.

#include <huxerui/agentkit/model.h>
#include <huxerui/file.h>

namespace huxerui::agentkit {

struct ArtifactRecord;

/// @brief Logical tool invocation supplied by a host or assembled from a provider call.
struct ToolRequest {
  /// Registered tool ID.
  ToolId tool;
  /// Owned JSON arguments checked against ToolDefinition::input_schema.
  JsonValue arguments;
};

/// @brief Public description of an exact published artifact version.
struct ArtifactMetadata {
  /// Artifact identity and version.
  ArtifactRef ref;
  /// Host artifact type, or MIME type for an inline attachment.
  std::string type;
  /// Human-readable display name.
  std::string name;
  /// Known stored byte length, when available.
  std::optional<std::uint64_t> size_bytes;
};

/// @brief Stable named part of an artifact version.
struct ArtifactPart {
  /// Part ID used in SourceSlice and rework requests.
  PartId id;
  /// Human-readable part label for review.
  std::string label;
};

/// @brief Input provenance for an output artifact or one of its parts.
struct ArtifactDependency {
  /// Affected output part, or empty for the entire artifact.
  std::optional<PartId> output_part;
  /// Exact input dependency and optional selection scope.
  SourceSlice input;
};

/// @brief Host description of content to register during an accepted operation.
struct ArtifactDraft {
  /// Existing artifact identity when publishing another version. Empty allocates a new identity.
  std::optional<ArtifactId> id;
  /// Expected latest revision when publishing to an existing identity.
  std::optional<Revision> expected_latest;
  /// Host-defined type used by the artifact store and renderer registry.
  std::string type;
  /// Display name of the new version.
  std::string name;
  /// Opaque host reference for later preview or byte reads.
  std::string storage_handle;
  /// Known byte length, required when accepting staged inline attachments.
  std::optional<std::uint64_t> size_bytes;
  /// Selectable parts of the version to publish.
  std::vector<ArtifactPart> parts;
  /// Exact sources used by the output and its parts.
  std::vector<ArtifactDependency> dependencies;
  /// Parts changed from the previous version, when known.
  std::vector<PartId> changed_parts;
  /// Optional resource effect associated with publishing this version.
  std::optional<ResourceEffect> published_effect;
};

/// @brief Read or write access for resource grants and conflict declarations.
enum class ResourceAccessMode {
  /// Allows reading and can share the key with other readers.
  Read,
  /// Allows the declared write and conflicts with readers and writers of the same key.
  Write
};

/// @brief Logical lock declaration used for concurrent tool scheduling.
struct ToolResourceClaim {
  /// Resource identity shared by tools that can conflict.
  ResourceId key;
  /// Access mode for the claim.
  ResourceAccessMode mode;
};

/// @brief Immutable plan produced by ToolPrepare before approval and execution.
struct PreparedChange {
  /// Human-readable details and artifact references for review.
  ApprovalPreview preview;
  /// Draft review artifacts to register with the prepared plan.
  std::vector<ArtifactDraft> preview_artifacts;
  /// Expected target baselines to recheck at execution.
  std::vector<TargetBaseline> targets;
  /// Versioned sources used when preparing the plan.
  std::vector<SourceSlice> inputs;
  /// Known resource claims. Empty optional means unknown scope and requires exclusive scheduling.
  std::optional<std::vector<ToolResourceClaim>> conflict_keys;
  /// Host-defined payload persisted and returned unchanged to ToolHandler.
  JsonValue execution_data;
  /// Compensation supported for effects resulting from the plan.
  UndoSupport undo_support;
};

/// @brief Exact invocation and approved or authorized plan passed to ToolHandler.
struct PreparedToolCall {
  /// Logical call ID for tracking, idempotency and reconciliation.
  ToolCallId call;
  /// ID of the immutable prepared plan.
  PlanId plan;
  /// Tool registration and catalog version bound to the call.
  ToolRef tool;
  /// Validated logical request.
  ToolRequest request;
  /// Prepared inputs, execution payload and target baselines.
  PreparedChange change;
};

/// @brief Actual tool completion state reported after execution or reconciliation.
enum class ToolOutcome {
  /// The tool reports successful completion.
  Succeeded,
  /// The tool reports failure.
  Failed,
  /// Some of the intended effects are confirmed.
  PartiallyApplied,
  /// The tool stopped with a known cancellation outcome.
  Cancelled,
  /// The tool may have changed state and requires explicit reconciliation.
  ResultUnknown
};

/// @brief Callback result before runtime validation, publication and commit.
struct ToolExecutionResult {
  /// Known or unknown execution outcome.
  ToolOutcome outcome;
  /// Optional protocol-visible result checked against the tool output schema.
  std::optional<JsonValue> model_value;
  /// TextBlock or registered CustomBlock values for the transcript.
  std::vector<ContentBlock> blocks;
  /// Already-published versions to associate with the result.
  std::vector<ArtifactRef> existing_artifacts;
  /// Draft versions to register with the result.
  std::vector<ArtifactDraft> new_artifacts;
  /// Per-resource record of actual effects and compensation handles.
  std::vector<ResourceEffect> effects;
  /// Failure or partial-result diagnostic, when available.
  std::optional<AgentError> reason;
};

/// @brief Committed tool result returned by RunContext::WaitForToolAsync() and RunContext::CallToolAsync().
///
/// Receipt reads project artifact references against the caller's current permissions.
struct ToolResult {
  /// Original logical tool call.
  ToolCallId call;
  /// Committed completion state.
  ToolOutcome outcome;
  /// Validated response used for model continuation. A receipt omits it when any associated artifact is hidden.
  std::optional<JsonValue> model_value;
  /// Committed transcript content from the call.
  std::vector<ContentBlock> blocks;
  /// Previously published versions associated with the call.
  std::vector<ArtifactRef> existing_artifacts;
  /// Exact versions newly registered with this result.
  std::vector<ArtifactRef> published_artifacts;
  /// Committed resource effects of the call.
  std::vector<ResourceEffect> effects;
  /// Failure or partial-result diagnostic, when available.
  std::optional<AgentError> reason;
};

/// @brief Concrete grant resolved for a tool phase.
struct ResourceAccess {
  /// Logical resource identity used for policy and conflict checks.
  ResourceId resource;
  /// Current version when the host can resolve one.
  std::optional<ResourceRef> current;
  /// Resource access offered for this phase.
  ResourceAccessMode mode;
  /// Optional host file capability for the resolved resource.
  std::optional<huxerui::FileReference> file;
};

/// @brief Phase for which the host resolves the resource grants.
enum class ToolAccessPhase {
  /// Resolve inputs for reviewable preparation.
  Prepare,
  /// Resolve current resources and baselines before issuing the approved call.
  Execute
};

/// @brief Invocation identity, arguments and context passed to resource and access policy.
struct ToolInvocation {
  /// Authorized caller with principal and Run identity as applicable.
  ActionParty actor;
  /// Preparation or execution boundary.
  ToolAccessPhase phase;
  /// Session owning the call.
  SessionId session;
  /// Run owning the call.
  RunId run;
  /// Original logical call ID.
  ToolCallId call;
  /// Project scope when bound.
  std::optional<ProjectId> project;
  /// Workspace scope when bound.
  std::optional<WorkspaceId> workspace;
  /// Goal definition version associated with this Run, when present.
  std::optional<GoalRef> goal;
  /// Exact registration of the requested tool.
  ToolRef tool;
  /// Tool's declared effect category.
  ToolEffect effect;
  /// Validated argument payload for this call.
  JsonValue arguments;
  /// Targets and expected baselines from the plan, when available.
  std::vector<TargetBaseline> prepared_targets;
  /// Original call targeted by a revert, when applicable.
  std::optional<ToolCallId> reverts;
  /// Exact candidate version targeted by an adoption, when applicable.
  std::optional<ArtifactRef> candidate_artifact;
};

/// @brief Resolves concrete resource identities, versions and handles for each phase.
using ToolResourceResolver =
    std::function<Async<std::vector<ResourceAccess>>(ToolInvocation)>;

/// @brief Identity, readable inputs and cancellation available before execution.
struct ToolPrepareContext {
  /// Authorized caller of this operation.
  ActionParty actor;
  /// Logical call that owns the preparation.
  ToolCallId call;
  /// Resolved grants passed to preparation.
  std::vector<ResourceAccess> readable;
  /// Cancellation signal for this call.
  std::stop_token stop;
  /// Versioned inputs available to the plan.
  std::vector<SourceSlice> sources;
  /// Versioned artifact action associated with the Run, when applicable.
  std::optional<ArtifactAction> artifact_action;
};

/// @brief Execution-scoped access handle provided to ToolHandler.
class ToolContext {
public:
  /// @brief Returns the logical call ID for deduplication and effect accounting.
  /// @return The identity of this tool attempt, useful as an idempotency key in the host.
  ToolCallId CallId() const;

  /// @brief Returns the call's cancellation signal.
  /// @return The cooperative cancellation token for this attempt.
  std::stop_token StopToken() const;

  /// @brief Returns grants resolved and authorized for execution.
  /// @return The resource handles and versions resolved for this execution phase.
  std::vector<ResourceAccess> Resources() const;

  /// @brief Updates visible progress for this logical call.
  /// @param text User-facing progress to attach to this call.
  /// @return Success while the call accepts progress, or an error once the attempt is closed.
  AgentResult<void> ReportProgress(std::string text);

  /// @brief Requests and waits for an additional form response within the call's Run.
  /// @param request Form schema, display content and optional expiration.
  /// @return The settled interaction outcome.
  /// @note Tool execution may ask for input only when the current attempt permits it. Parallel attempts return InvalidState.
  Async<InteractionOutcome> AskAsync(FormRequest request);

private:
  ToolContext(std::shared_ptr<detail::RuntimeState> state, SessionId session,
              RunId run, ToolCallId call, std::stop_token stop,
              std::vector<ResourceAccess> resources);
  std::shared_ptr<detail::RuntimeState> state_;
  SessionId session_;
  RunId run_;
  ToolCallId call_;
  std::stop_token stop_;
  std::vector<ResourceAccess> resources_;
  friend class detail::RuntimeState;
};

/// @brief Builds an immutable plan from validated arguments and preparation grants.
using ToolPrepare = std::function<Async<PreparedChange>(ToolPrepareContext, ToolRequest)>;
/// @brief Executes an issued immutable plan and reports actual results and effects.
using ToolHandler = std::function<Async<ToolExecutionResult>(ToolContext, PreparedToolCall)>;

/// @brief Additional declaration governing which tool calls can run together.
enum class ToolConcurrency {
  /// Runs exclusively.
  Serial,
  /// Allows compatible reads and requires ToolEffect::Read.
  ReadOnly,
  /// Allows concurrency when declared resource claims do not conflict.
  DisjointResources
};

/// @brief Registered schema, effect policy and callbacks for one tool revision.
struct ToolDefinition {
  /// Stable logical tool ID.
  ToolId id;
  /// Positive registration revision used to bind pending calls.
  Revision version;
  /// Description sent to the model when the tool is offered.
  std::string description;
  /// Schema used to validate input before preparation.
  JsonSchema input_schema;
  /// Optional schema used to validate structured results.
  std::optional<JsonSchema> output_schema;
  /// Category used to route approvals, grants and failure handling.
  ToolEffect effect;
  /// Allowed scheduling mode.
  ToolConcurrency concurrency = ToolConcurrency::Serial;
  /// Declares host-side deduplication by ToolCallId. Catalog equality includes this declaration.
  /// Recovery currently relies on result lookup or reconciliation for already-issued calls.
  bool idempotent_by_call_id = false;
  /// Builds the immutable approval and execution plan.
  ToolPrepare prepare;
  /// Receives the authorized plan and returns actual effects.
  ToolHandler execute;
  /// Optionally queries durable host evidence for a previously issued call.
  std::function<Async<std::optional<ToolExecutionResult>>(ToolCallId)> lookup_result;
  /// Optionally reconstructs a resource effect from previous and newly recovered artifact records.
  std::function<std::optional<ResourceEffect>(const ArtifactRecord& before, const ArtifactRecord& after)>
      recover_published_effect;
};

/// @brief New revision and complete definition set for a dynamic tool catalog.
struct ToolCatalogSnapshot {
  /// Positive catalog snapshot revision, increasing across changes.
  Revision revision;
  /// Tools to expose in the published snapshot.
  std::vector<ToolDefinition> tools;
};

/// @brief Operation classifications passed to host policy and bound to agent definitions.
enum class Capability {
  /// Lists or discovers visible sessions.
  DiscoverSession,
  /// Opens a session or reads its visible projection.
  ReadSession,
  /// Creates a session and its optional initial work.
  CreateSession,
  /// Creates a session from a committed history boundary.
  ForkSession,
  /// Changes the bound agent of a session.
  TransferAgent,
  /// Submits or queues user input.
  SubmitInput,
  /// Cancels a Run.
  CancelRun,
  /// Submits a form reply or approval decision.
  SubmitInteraction,
  /// Uses a selected model and execution profile.
  UseRunConfiguration,
  /// Sends a cross-session message.
  SendMessage,
  /// Starts a Run in a target session.
  StartSessionRun,
  /// Applies supplemental input to an exact Run.
  SteerRun,
  /// Submits a reply to a collaboration request.
  Reply,
  /// Withdraws an undelivered collaboration message.
  WithdrawMessage,
  /// Waits for a collaboration reply or task outcome.
  WaitForResult,
  /// Updates session metadata or lifecycle.
  UpdateSession,
  /// Resolves a referenced source selection.
  ReadContext,
  /// Shares authorized data with the exact selected model.
  SendToModel,
  /// Invokes a custom message block action.
  UseContentAction,
  /// Invokes a versioned artifact action.
  UseArtifactAction,
  /// Discovers or exposes an eligible tool.
  DiscoverTool,
  /// Prepares a tool invocation.
  PrepareTool,
  /// Issues a prepared tool after approval and resource revalidation.
  ExecuteTool,
  /// Lists visible skill summaries.
  DiscoverSkill,
  /// Loads and activates a versioned skill.
  ActivateSkill,
  /// Reads a resource from an active skill.
  ReadSkillResource,
  /// Reads an exact artifact version.
  ReadArtifact,
  /// Publishes or modifies an artifact.
  WriteArtifact,
  /// Prepares or performs a compensating change.
  RevertChange,
  /// Creates a comparison and its candidate branches.
  CreateComparison,
  /// Adopts an artifact from a comparison.
  AdoptCandidate,
  /// Prepares or performs artifact rework.
  ReworkArtifact,
  /// Reads a session goal.
  ReadGoal,
  /// Changes goal scheduling, limits or associations.
  ManageGoal,
  /// Changes a versioned goal definition.
  EditGoalDefinition,
  /// Completes a goal against verified evidence.
  CompleteGoal,
  /// Creates, claims or advances a project task.
  ManageTask,
  /// Manages publication of project shared context.
  PublishSharedContext,
  /// Registers a change to a versioned input source.
  RecordSourceChange,
  /// Records evidence for an unknown tool outcome.
  ReconcileTool,
  /// Reads the authorized project view.
  ReadProject,
  /// Starts a child Run in the current session.
  DelegateRun
};

/// @brief Portable outcome from the host access policy.
enum class AccessDecision {
  /// Allows the requested operation subject to runtime validation.
  Allow,
  /// Rejects the requested operation.
  Deny,
  /// Routes supported operations to the applicable approval interaction.
  RequireConfirmation,
  /// Allows an automatic review for a prepared ExecuteTool request.
  AllowReview
};

/// @brief Known target identifiers supplied when evaluating an operation.
struct OperationTarget {
  /// Target session, when applicable.
  std::optional<SessionId> session;
  /// Target Run, when applicable.
  std::optional<RunId> run;
  /// Target collaboration message, when applicable.
  std::optional<CollaborationMessageId> message;
  /// Target goal ID, when applicable.
  std::optional<GoalId> goal;
  /// Target project task, when applicable.
  std::optional<ProjectTaskId> task;
  /// Target comparison, when applicable.
  std::optional<ForkReviewId> review;
  /// Known logical resource IDs targeted by the operation.
  std::vector<ResourceId> resources;
  /// Target form or approval, when applicable.
  std::optional<InteractionId> interaction;
};

/// @brief Owned policy input for one capability check.
struct PolicyRequest {
  /// Principal and optional execution identity requesting the operation.
  ActionParty actor;
  /// Capability being evaluated.
  Capability capability;
  /// Versioned agent associated with the operation, when applicable.
  std::optional<AgentRef> agent;
  /// Project scope of the request, when applicable.
  std::optional<ProjectId> project;
  /// Workspace scope of the request, when applicable.
  std::optional<WorkspaceId> workspace;
  /// Target identities and resolved resource keys.
  OperationTarget target;
  /// Replacement or delegated agent being authorized, when applicable.
  std::optional<AgentId> new_agent;
  /// Exact tool registration, when applicable.
  std::optional<ToolRef> tool;
  /// Exact skill version, when applicable.
  std::optional<SkillRef> skill;
  /// Requested package-relative resource path, when applicable.
  std::optional<std::string> skill_resource;
  /// Model that would receive the data, when applicable.
  std::optional<ModelId> recipient_model;
  /// Applicable goal definition version, when available.
  std::optional<GoalRef> goal;
  /// Versioned message component action, when applicable.
  std::optional<CustomBlockAction> content_action;
  /// Operation-specific JSON details, when supplied.
  JsonValue operation;
  /// Runtime-produced tool invocation context, when applicable.
  std::optional<ToolInvocation> invocation;
  /// ID of a prepared plan being checked, when applicable.
  std::optional<PlanId> plan;
  /// Referenced versions and selections relevant to the operation.
  std::vector<SourceSlice> sources;
  /// Effective model and execution-profile binding under evaluation.
  std::optional<RunStartBinding> run_configuration;
  /// Versioned artifact action under evaluation.
  std::optional<ArtifactAction> artifact_action;
};

/// @brief Host authorization disposition and optional explanation.
struct PolicyDecision {
  /// Authorization action for the requested capability.
  AccessDecision decision;
  /// Human-readable explanation of that decision, when supplied.
  std::string reason;
};

/// @brief Required host callback evaluating the current actor and operation.
using AccessPolicy = std::function<Async<PolicyDecision>(PolicyRequest)>;

/// @brief Immutable, authorized input for automatic review of a prepared tool call.
struct ToolReviewRequest {
  /// Validated invocation and targets for the plan under review.
  ToolInvocation invocation;
  /// Exact plan ID to which the decision will be bound.
  PlanId plan;
  /// Authorized review data for the plan.
  ApprovalPreview preview;
  /// Authorized user-intent slices available to the reviewer.
  std::vector<SourceSlice> authorized_intent;
  /// Absolute review deadline.
  TimePoint deadline;
  /// Cancellation signal for this review.
  std::stop_token stop;
};

/// @brief Automatic review disposition and its explanation.
struct ToolReviewResult {
  /// Whether to approve, deny or escalate to the user.
  ToolReviewDecision decision;
  /// Human-readable reason for the decision.
  std::string reason;
};

/// @brief Versioned host automatic-review registration.
struct ToolReviewer {
  /// Stable reviewer identifier.
  std::string id;
  /// Positive revision binding decisions to the reviewer configuration.
  Revision version;
  /// Positive maximum duration of a review.
  Duration timeout;
  /// Returns an automatic decision or defers the exact plan to the user.
  std::function<Async<ToolReviewResult>(ToolReviewRequest)> review;
};

} // namespace huxerui::agentkit
