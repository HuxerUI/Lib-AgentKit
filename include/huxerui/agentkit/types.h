#pragma once

/// @file
/// @brief Shared identifiers, errors, command receipts and subscription handles.
/// Include <huxerui/agentkit/types.h> for these values. Link HuxerUI::AgentKit for NewCommandId() and CommandService.
/// Keep a command ID until its outcome is known. Use CommandService to recover an approval or commit whose outcome is pending.

#include <huxerui/data.h>
#include <huxerui/task.h>

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <variant>

namespace huxerui::agentkit {

/// @brief Opaque string identifier whose tag separates otherwise identical ID domains.
/// @tparam Tag Type-level identity domain.
template<class Tag>
struct Id {
  /// Identifier payload. Runtime APIs may require a nonempty value.
  std::string value;
  /// Orders values lexicographically by their declared fields.
  auto operator<=>(const Id&) const = default;
};

/// @brief Identity of an AgentDefinition across revisions.
using AgentId = Id<struct AgentTag>;
/// @brief Identity of a registered model configuration across revisions.
using ModelId = Id<struct ModelTag>;
/// @brief Identity of a host tool within the runtime's tool namespace.
using ToolId = Id<struct ToolTag>;
/// @brief Identity of a replaceable tool catalog.
using ToolCatalogId = Id<struct ToolCatalogTag>;
/// @brief Identity of a registered SkillSource.
using SkillSourceId = Id<struct SkillSourceTag>;
/// @brief Identity of a skill within its source.
using SkillId = Id<struct SkillTag>;
/// @brief Identity of a registered ContextProvider.
using ContextProviderId = Id<struct ContextProviderTag>;
/// @brief Stable identity used to open a Conversation.
using SessionId = Id<struct SessionTag>;
/// @brief Identity of one execution attempt in a session.
using RunId = Id<struct RunTag>;
/// @brief Identity of one provider model turn in a Run.
using TurnId = Id<struct TurnTag>;
/// @brief Stable identity of a transcript message across visible versions.
using MessageId = Id<struct MessageTag>;
/// @brief Identity of a content block inside a message version.
using BlockId = Id<struct BlockTag>;
/// @brief Identity of a scheduled input accepted into a session queue.
using QueueItemId = Id<struct QueueItemTag>;
/// @brief Identity of a form or approval across state revisions.
using InteractionId = Id<struct InteractionTag>;
/// @brief Identity of a logical tool call, including recovery and reconciliation.
using ToolCallId = Id<struct ToolCallTag>;
/// @brief Identity of the immutable prepared plan associated with a tool call.
using PlanId = Id<struct PlanTag>;
/// @brief Caller-supplied idempotency key for one logical command.
using CommandId = Id<struct CommandTag>;
/// @brief Identity of a committed store transaction or runtime commit.
using CommitId = Id<struct CommitTag>;
/// @brief Authenticated user or host principal resolved by RuntimeOptions::principal.
using PrincipalId = Id<struct PrincipalTag>;
/// @brief Identity of a project that scopes tasks and shared context.
using ProjectId = Id<struct ProjectTag>;
/// @brief Identity of a host-managed workspace.
using WorkspaceId = Id<struct WorkspaceTag>;
/// @brief Identity of a host-prepared workspace copy plan.
using WorkspaceCopyPlanId = Id<struct WorkspaceCopyPlanTag>;
/// @brief Identity of a host-selected execution profile.
using ExecutionProfileId = Id<struct ExecutionProfileTag>;
/// @brief Host-owned identity of a resource for permission and conflict checks.
using ResourceId = Id<struct ResourceTag>;
/// @brief Stable identity of an artifact across published versions.
using ArtifactId = Id<struct ArtifactTag>;
/// @brief Identity of a selectable part within an artifact version.
using PartId = Id<struct PartTag>;
/// @brief Identity of a request or reply sent between sessions.
using CollaborationMessageId = Id<struct CollaborationMessageTag>;
/// @brief Stable identity of a session goal across definition versions.
using GoalId = Id<struct GoalTag>;
/// @brief Identity of a project task.
using ProjectTaskId = Id<struct ProjectTaskTag>;
/// @brief Identity of one claim and execution attempt for a project task.
using AttemptId = Id<struct AttemptTag>;
/// @brief Identity of a shared project item across versions.
using SharedContextId = Id<struct SharedContextTag>;
/// @brief Identity of an alternative comparison managed by ForkReviewService.
using ForkReviewId = Id<struct ForkReviewTag>;
/// @brief Identity of an alternative within a comparison.
using CandidateId = Id<struct CandidateTag>;

/// @brief Unsigned revision counter. Its scope is defined by the owning object or snapshot.
using Revision = std::uint64_t;
/// @brief Unsigned position in one session's committed timeline.
using Sequence = std::uint64_t;
/// @brief Time span measured in milliseconds.
using Duration = std::chrono::milliseconds;
/// @brief Absolute system-clock time with millisecond precision.
using TimePoint = std::chrono::sys_time<Duration>;

/// @brief Identifies a goal definition at one exact version.
struct GoalRef {
  /// Stable goal identity.
  GoalId id;
  /// Definition version, distinct from the mutable GoalSnapshot::revision.
  Revision version;
  /// Orders values lexicographically by their declared fields.
  auto operator<=>(const GoalRef&) const = default;
};

/// @brief Owned JSON data used for schemas, payloads, checkpoints and structured results.
struct JsonValue {
  /// @brief Ordered array of JSON values.
  using Array = std::vector<JsonValue>;
  /// @brief Object keyed by UTF-8 strings with transparent key comparison.
  using Object = std::map<std::string, JsonValue, std::less<>>;
  /// Active JSON representation. The default is JSON null.
  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> value = nullptr;
  /// Compares the stored fields for equality.
  bool operator==(const JsonValue&) const = default;
};

/// @brief Owned schema document interpreted by the relevant runtime or provider validator.
struct JsonSchema {
  /// JSON schema to validate against. Supported keywords depend on the receiving API.
  JsonValue document;
  /// Compares the stored fields for equality.
  bool operator==(const JsonSchema&) const = default;
};

/// @brief Portable failure categories returned in AgentError.
enum class ErrorCode {
  /// An input, field, schema or range fails validation.
  InvalidArgument,
  /// The operation is incompatible with the current state.
  InvalidState,
  /// The requested visible object or exact version could not be resolved.
  NotFound,
  /// The current principal, agent or recipient lacks authorization.
  PermissionDenied,
  /// The required adapter or capability is unavailable.
  Unsupported,
  /// A revision, command payload or resource baseline no longer matches.
  Conflict,
  /// An active operation prevents immediate acceptance.
  Busy,
  /// The page cursor no longer matches the current view.
  CursorExpired,
  /// The addressed Run, interaction, or other operation has already settled.
  AlreadyClosed,
  /// An applicable deadline elapsed.
  DeadlineExceeded,
  /// The provider or runtime rate-limited the operation.
  RateLimited,
  /// The request or execution exceeds its applicable budget.
  BudgetExceeded,
  /// Registering or continuing the wait would form a dependency cycle.
  WaitCycle,
  /// The model or transport could not complete the operation.
  ProviderFailure,
  /// An adapter returned an invalid or incomplete protocol response.
  ProtocolError,
  /// The runtime or store could not read or commit durable state.
  PersistenceFailure,
  /// Commit completion is unknown. Query the original command before resubmitting.
  CommitUncertain,
  /// An issued tool may have changed external state. Reconcile the original call before retrying.
  ResultUnknown,
  /// A rework adapter requires a larger scope. Inspect AgentError::proposed_scope.
  ScopeExpansionRequired,
  /// The runtime that owns the operation has closed.
  RuntimeClosed
};

/// @brief Broader rework scope proposed for explicit user confirmation.
struct ScopeExpansion {
  /// Artifact parts covered by the proposal, when available.
  std::vector<PartId> affected_parts;
  /// Whether the adapter requires regenerating the entire artifact.
  bool whole_artifact;
  /// Human-readable explanation of the scope.
  std::string summary;
  /// Compares the stored fields for equality.
  bool operator==(const ScopeExpansion&) const = default;
};

/// @brief Validation diagnostic associated with a particular input field.
struct FieldError {
  /// Field or JSON location reported by the validating API.
  std::string path;
  /// Machine-readable validation rule identifier.
  std::string code;
  /// Human-readable explanation suitable for the caller.
  std::string message;
  /// Compares the stored fields for equality.
  bool operator==(const FieldError&) const = default;
};

/// @brief Owned operation failure with optional validation, revision or provider details.
struct AgentError {
  /// Portable category used for decisions.
  ErrorCode code;
  /// Human-readable diagnostic.
  std::string message;
  /// Field-level validation details, if available.
  std::vector<FieldError> fields;
  /// Current revision on a conflict, when available and visible.
  std::optional<Revision> current_revision;
  /// Optional retry delay supplied by a host adapter. Built-in HTTP adapters currently leave it empty.
  std::optional<Duration> retry_after;
  /// Optional provider or protocol-specific diagnostic code.
  /// Built-in model adapters use first_response_timeout, stream_idle_timeout, request_deadline,
  /// output_token_limit and context_window_exceeded for typed model turn failures.
  std::optional<std::string> provider_code;
  /// Proposed scope for a ScopeExpansionRequired result.
  std::optional<ScopeExpansion> proposed_scope;
  /// Compares the stored fields for equality.
  bool operator==(const AgentError&) const = default;
};

/// @brief Result of an AgentKit operation. Check Succeeded() before using Value() or Error().
/// @tparam T Success payload. Use void when completion needs no value.
template<class T> using AgentResult = huxerui::Result<T, AgentError>;
/// @brief Lazy HuxerUI task that returns an AgentResult.
/// @tparam T Success payload returned when the task completes.
template<class T> using Async = huxerui::Task<AgentResult<T>>;

/// @brief Identifies the committed acceptance or resolution of a command.
struct CommitStamp {
  /// Original logical command ID.
  CommandId command;
  /// Commit at which this result was accepted or resolved.
  CommitId commit;
  /// Associated session, when applicable and visible.
  std::optional<SessionId> session;
  /// Sequence after the commit in that session, when applicable.
  std::optional<Sequence> session_sequence;
  /// Associated project, when applicable and visible.
  std::optional<ProjectId> project;
  /// Revision after the commit in that project, when applicable.
  std::optional<Revision> project_revision;
};

/// @brief Identity and version of an operation approval presented to a user.
struct PendingApproval {
  /// Session through which the interaction is visible.
  SessionId session;
  /// Interaction to resolve with Conversation::SubmitAsync().
  InteractionId interaction;
  /// Version to supply in InteractionReply::expected_version.
  Revision version;
};

/// @brief Durable acceptance of a command, with its typed outcome or pending approval.
/// @tparam T Command-specific acceptance payload.
template<class T>
struct Receipt {
  /// Commit that accepted the original command.
  CommitStamp stamp;
  /// Accepted payload, or a PendingApproval that gates the operation.
  std::variant<T, PendingApproval> outcome;
};

/// @brief A command receipt with no extra success payload.
using CommandReceipt = Receipt<std::monostate>;

/// @brief Receipt data for a newly accepted session.
struct SessionCreated {
  /// Created session ID to pass to SessionService::OpenAsync().
  SessionId session;
  /// Initial Run ID when creation started a Run.
  std::optional<RunId> initial_run;
  /// Initial work request ID, when creation registered one.
  std::optional<CollaborationMessageId> initial_request;
};

/// @brief State of a collaboration delivery independently of a reply wait.
enum class DeliveryState {
  /// The delivery is waiting for approval.
  AwaitingApproval,
  /// Accepted and waiting to become effective.
  Queued,
  /// The receiving action took effect.
  Delivered,
  /// The receiving action was rejected or failed validation.
  Rejected,
  /// The delivery reached its deadline.
  Expired,
  /// The sender withdrew the delivery before it took effect.
  Withdrawn
};

/// @brief Acceptance data for a Run. Use snapshots to observe execution or completion.
struct RunAccepted {
  /// Stable Run ID.
  RunId run;
};
/// @brief Acceptance data for an input queued for later execution.
struct InputQueued {
  /// Queue entry ID for observation, recovery or withdrawal.
  QueueItemId item;
};
/// @brief Acceptance data for a supplement addressed to an existing Run.
struct RunSteered {
  /// Run targeted by the supplement.
  RunId run;
  /// Transcript message that owns the supplemental input.
  MessageId message;
};
/// @brief Receipt data for a committed form submission or approval decision.
struct InteractionConsumed {
  /// Resolved interaction ID.
  InteractionId id;
  /// Resulting interaction version.
  Revision version;
};
/// @brief Receipt data for a form created by a Run.
struct InputRequested {
  /// New interaction ID, for example to pass to WaitForInputAsync().
  InteractionId id;
  /// Initial interaction version.
  Revision version;
};
/// @brief Receipt data for a prepared or scheduled tool call.
struct ToolCallAccepted {
  /// Logical call ID to pass to RunContext::WaitForToolAsync().
  ToolCallId call;
};

/// @brief Receipt data for a collaboration send, reply or start.
struct MessageAccepted {
  /// Accepted collaboration message ID.
  CollaborationMessageId message;
  /// Delivery state at acceptance.
  DeliveryState delivery;
  /// Run started by the accepted request, when known and visible.
  std::optional<RunId> target_run;
  /// Associated interaction, when the delivery has an approval.
  std::optional<InteractionId> approval;
};

/// @brief Receipt data for a goal update.
struct GoalUpdated {
  /// Applicable goal definition version.
  GoalRef goal;
  /// Mutable goal state revision after the update.
  Revision revision;
};

/// @brief Receipt data for a project task update.
struct TaskUpdated {
  /// Updated task ID.
  ProjectTaskId task;
  /// Task revision after the update.
  Revision revision;
  /// Current attempt identity, when assigned.
  std::optional<AttemptId> attempt;
};

/// @brief Exact version of one shared project item.
struct SharedContextRef {
  /// Project that owns the shared item.
  ProjectId project;
  /// Item identity within the project.
  SharedContextId item;
  /// Exact item revision to resolve.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const SharedContextRef&) const = default;
};

/// @brief Receipt data for rework, revert or candidate adoption.
struct ChangePlanAccepted {
  /// Run that owns the operation, when scheduled and visible.
  std::optional<RunId> run;
  /// Tool call that owns the plan or actual change, when available.
  std::optional<ToolCallId> call;
  /// Queue ID when the operation is waiting to start.
  std::optional<QueueItemId> queue_item;
  /// Interaction ID if approval is already available.
  std::optional<InteractionId> approval;
};

/// @brief Receipt data for an accepted comparison.
struct ForkReviewCreated {
  /// Comparison ID.
  ForkReviewId review;
  /// Created candidate session IDs visible to the caller.
  std::vector<SessionId> branches;
};

/// @brief Typed result returned when reading command progress, independent of the original API.
using CommandValue = std::variant<std::monostate, SessionCreated, RunAccepted, InputQueued,
    RunSteered, InteractionConsumed, InputRequested, ToolCallAccepted, MessageAccepted, GoalUpdated, TaskUpdated, SharedContextRef,
    ChangePlanAccepted, ForkReviewCreated>;

/// @brief Current state of a recorded command.
enum class CommandState {
  /// Accepted and waiting for approval.
  AwaitingApproval,
  /// Accepted for later processing or verification.
  Scheduled,
  /// The command's operation took effect.
  Applied,
  /// The command was denied.
  Rejected,
  /// The command's deadline or approval expired.
  Expired,
  /// The command was withdrawn before application.
  Withdrawn,
  /// Processing ended with a failure.
  Failed
};

/// @brief Current permission-filtered progress for an accepted command.
struct CommandProgress {
  /// Commit stamp for initial acceptance.
  CommitStamp accepted;
  /// Current command state.
  CommandState state;
  /// Commit stamp for the resolution, when recorded.
  std::optional<CommitStamp> resolution;
  /// Typed result now visible to the caller, when available.
  std::optional<CommandValue> result;
  /// Current visible approval, when applicable.
  std::optional<PendingApproval> approval;
  /// Recorded failure or rejection diagnostic, when available.
  std::optional<AgentError> failure;
};

namespace detail { class RuntimeState; }

template<class T>
/// @brief Receives a shared immutable snapshot or an error from a service subscription.
using Observer = std::function<void(AgentResult<std::shared_ptr<const T>>)>;

/// @brief Move-only lifetime handle returned by an ObserveAsync() method.
/// Keep the handle while observing. Reset() or destruction deactivates further callbacks.
class Subscription {
public:
  /// @brief Moves the active subscription from another handle.
  /// @param other Source handle, which becomes inactive.
  Subscription(Subscription&& other) noexcept;

  /// @brief Releases the current subscription and takes ownership from another handle.
  /// @param other Source handle, which becomes inactive unless this is self-assignment.
  /// @return This handle.
  Subscription& operator=(Subscription&& other) noexcept;

  /// @brief Deactivates the associated subscription.
  ~Subscription();

  /// @brief Explicitly deactivates further observation through this handle.
  void Reset() noexcept;

private:
  explicit Subscription(std::shared_ptr<bool> active);
  std::shared_ptr<bool> active_;
  friend class detail::RuntimeState;
};

/// @brief Finds progress by original command ID or by its operation approval.
using CommandLookup = std::variant<CommandId, PendingApproval>;

/// @brief Queries receipts and follows command progress under the caller's current authorization.
/// Use AgentRuntime::Commands() for user operations or RunContext::Commands() for a bound Run.
class CommandService {
public:
  /// @brief Reads progress for the identified command or operation approval.
  /// @param lookup Original command ID or its pending operation approval.
  /// @return Progress and any currently visible receipt.
  Async<CommandProgress> ReadAsync(CommandLookup lookup);

  /// @brief Observes progress changes and returns the subscription lifetime handle.
  /// @param lookup Original command ID or its pending operation approval.
  /// @param observer Callback receiving immutable progress snapshots or an error.
  /// @return A subscription that keeps this observation registered.
  Async<Subscription> ObserveAsync(CommandLookup lookup, Observer<CommandProgress> observer);

private:
  explicit CommandService(std::shared_ptr<detail::RuntimeState> state, std::optional<RunId> actor = {});
  std::shared_ptr<detail::RuntimeState> state_;
  std::optional<RunId> actor_;
  friend class AgentRuntime;
  friend class RunContext;
};

/// @brief Creates a fresh idempotency key for a new logical command.
/// @return A new command ID. Save it when retrying submission of the same logical action.
CommandId NewCommandId();

} // namespace huxerui::agentkit
