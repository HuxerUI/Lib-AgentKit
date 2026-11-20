#pragma once

/// @file
/// @brief Project tasks, current attempts, shared context and host verification.
/// Use ProjectService for project-level work. A task's attempt controls who may start and settle it.
/// Use SessionGoals for the session-level objective and configure GoalVerifier to check versioned evidence.

#include <huxerui/agentkit/collaboration.h>

namespace huxerui::agentkit {

/// @brief Host-defined evidence values with their exact source selections.
struct Evidence {
  /// Structured evidence to validate or pass into a verifier.
  JsonValue values;
  /// Versioned references that support these values.
  std::vector<SourceSlice> sources;
  /// Compares the stored fields for equality.
  bool operator==(const Evidence&) const = default;
};

/// @brief Result category from checking project or goal evidence.
enum class VerificationStatus {
  /// Required conditions were verified.
  Passed,
  /// Additional or replacement evidence is required.
  MissingEvidence,
  /// Evidence failed a completion condition.
  Failed
};

/// @brief Host's assessment of submitted evidence.
struct VerificationResult {
  /// Result of checking the conditions.
  VerificationStatus status;
  /// Explanation to present for the result, when supplied.
  std::string reason;
  /// Exact source selections confirmed by the verifier.
  std::vector<SourceSlice> verified;
};

/// @brief Versioned evidence check passed to the host verifier.
struct VerificationRequest {
  /// Associated project, when applicable.
  std::optional<ProjectId> project;
  /// Associated session, when applicable.
  std::optional<SessionId> session;
  /// Exact goal definition or task attempt being verified.
  std::variant<GoalRef, TaskExecutionRef> subject;
  /// Revision of the subject used to reject stale verification results.
  Revision expected;
  /// Structured criteria defined by the goal or task.
  JsonValue conditions;
  /// Evidence being verified after runtime authorization.
  Evidence evidence;
  /// Cancellation signal for this check.
  std::stop_token stop;
};

/// @brief Host callback registered with RuntimeOptions::verify for both goals and task attempts.
using GoalVerifier = std::function<Async<VerificationResult>(VerificationRequest)>;

/// @brief Lifecycle of a project work item and its current attempt.
enum class ProjectTaskState {
  /// Registered task awaiting further scheduling state.
  Pending,
  /// A dependency or condition prevents execution.
  Blocked,
  /// Dependencies allow the task to be claimed.
  Ready,
  /// Task has a current execution claimant or assignment awaiting start.
  Assigned,
  /// The current attempt has started.
  Running,
  /// The current attempt completed successfully.
  Succeeded,
  /// The current attempt completed with a failure.
  Failed,
  /// The task or its current attempt has been cancelled.
  Cancelled
};

/// @brief Declared dependency on the result of another project task.
struct TaskDependency {
  /// Predecessor task in the same project.
  ProjectTaskId upstream;
  /// Artifact types the upstream result must provide.
  std::vector<std::string> required_artifact_types;
  /// Allows usable partial results from a failed predecessor.
  bool accept_partial = false;
  /// Compares the stored fields for equality.
  bool operator==(const TaskDependency&) const = default;
};

/// @brief Host-defined work, prerequisite graph and completion conditions.
struct ProjectTaskSpec {
  /// Human-readable task title.
  std::string title;
  /// Input for the task's execution.
  SendInput input;
  /// Associated session goal ID, when applicable.
  std::optional<GoalId> goal;
  /// Upstream tasks that constrain readiness.
  std::vector<TaskDependency> dependencies;
  /// Structured criteria for validating completion evidence.
  JsonValue completion_conditions;
  /// Compares the stored fields for equality.
  bool operator==(const ProjectTaskSpec&) const = default;
};

/// @brief Session or agent eligible to receive a project task.
using TaskAssignee = std::variant<SessionId, AgentId>;

/// @brief Terminal result bound to one exact attempt.
struct TaskOutcome {
  /// Project, task and attempt identity for the result.
  TaskExecutionRef execution;
  /// Goal definition version bound at execution, when applicable.
  std::optional<GoalRef> goal;
  /// Terminal attempt state.
  ProjectTaskState state;
  /// Authorized artifacts delivered by the attempt.
  std::vector<ArtifactRef> artifacts;
  /// Evidence attached to completion, when present.
  std::optional<Evidence> evidence;
  /// Failure, cancellation or other terminal diagnostic, when present.
  std::optional<AgentError> reason;
};

/// @brief Current visible task definition, assignment and result.
struct ProjectTaskSnapshot {
  /// Stable task ID.
  ProjectTaskId id;
  /// Task revision for ChangeTask::expected.
  Revision revision;
  /// Current definition of the work.
  ProjectTaskSpec spec;
  /// Current task lifecycle state.
  ProjectTaskState state;
  /// Requested destination session or agent, when assigned.
  std::optional<TaskAssignee> assignee;
  /// Current attempt ID, when claimed.
  std::optional<AttemptId> attempt;
  /// Goal definition version bound to this attempt, when applicable.
  std::optional<GoalRef> attempt_goal;
  /// Session that owns the current claim, when present.
  std::optional<SessionId> claimant;
  /// Run executing the current attempt, when bound.
  std::optional<RunId> run;
  /// Sources selected from upstream work for the current attempt.
  std::vector<SourceSlice> resolved_inputs;
  /// Current visible attempt result, when available.
  std::optional<TaskOutcome> outcome;
  /// Reason a task is blocked, when provided.
  std::optional<std::string> blocked_reason;
};

/// @brief Creates a new task against the current project revision.
struct CreateTask {
  /// Expected project revision for modifying the task graph.
  Revision expected_project;
  /// Initial task specification.
  ProjectTaskSpec spec;
  /// Compares the stored fields for equality.
  bool operator==(const CreateTask&) const = default;
};

/// @brief Updates an existing task specification.
struct EditTask {
  /// Replacement task specification.
  ProjectTaskSpec spec;
  /// Expected project revision for editing dependencies.
  Revision expected_project;
  /// Compares the stored fields for equality.
  bool operator==(const EditTask&) const = default;
};

/// @brief Selects the destination allowed to claim a task.
struct AssignTask {
  /// Permitted session or agent.
  TaskAssignee assignee;
  /// Compares the stored fields for equality.
  bool operator==(const AssignTask&) const = default;
};
/// @brief Claims an eligible task for one session and obtains an attempt ID.
struct ClaimTask {
  /// Session requesting the execution claim.
  SessionId session;
  /// Compares the stored fields for equality.
  bool operator==(const ClaimTask&) const = default;
};
/// @brief Releases a matching, unstarted attempt.
struct ReleaseTask {
  /// Attempt to release.
  AttemptId attempt;
  /// Compares the stored fields for equality.
  bool operator==(const ReleaseTask&) const = default;
};
/// @brief Starts a claimed attempt in its claimant session.
struct StartTask {
  /// Attempt authorized to start.
  AttemptId attempt;
  /// Compares the stored fields for equality.
  bool operator==(const StartTask&) const = default;
};
/// @brief Finishes a task attempt with evidence and artifacts.
struct CompleteTask {
  /// Attempt being settled.
  AttemptId attempt;
  /// Evidence to check against the task's completion conditions.
  Evidence evidence;
  /// Exact artifact versions delivered by the attempt.
  std::vector<ArtifactRef> artifacts;
  /// Compares the stored fields for equality.
  bool operator==(const CompleteTask&) const = default;
};
/// @brief Records an unsuccessful attempt and available partial artifacts.
struct FailTask {
  /// Attempt being settled.
  AttemptId attempt;
  /// Structured failure diagnostic.
  AgentError reason;
  /// Partial artifacts available from the attempt.
  std::vector<ArtifactRef> artifacts;
  /// Compares the stored fields for equality.
  bool operator==(const FailTask&) const = default;
};
/// @brief Cancels a project task through the state change API.
struct CancelTask {
  /// Compares the stored fields for equality.
  bool operator==(const CancelTask&) const = default;
};
/// @brief Makes a failed or cancelled task eligible for another attempt, subject to its prerequisites.
struct RetryTask {
  /// Compares the stored fields for equality.
  bool operator==(const RetryTask&) const = default;
};

/// @brief Supported operations on an existing project task.
using TaskChange = std::variant<EditTask, AssignTask, ClaimTask, ReleaseTask, StartTask,
    CompleteTask, FailTask, CancelTask, RetryTask>;

/// @brief Optimistic command to edit or advance a task.
struct ChangeTask {
  /// Task to change.
  ProjectTaskId task;
  /// Expected current task revision.
  Revision expected;
  /// State change or replacement specification.
  TaskChange change;
  /// Compares the stored fields for equality.
  bool operator==(const ChangeTask&) const = default;
};

/// @brief Project-level task creation or change.
using TaskCommand = std::variant<CreateTask, ChangeTask>;

/// @brief Publication lifecycle of a project shared-context item.
enum class SharedContextState {
  /// Item is being prepared.
  Draft,
  /// Item was submitted for review.
  PendingReview,
  /// Version is available for authorized project references.
  Published,
  /// Version is withdrawn from future resolution.
  Withdrawn
};

/// @brief Proposed shared context and its versioned sources.
struct SharedContextDraft {
  /// Human-readable title used in the project.
  std::string title;
  /// Structured material made available through a published version.
  JsonValue content;
  /// Underlying source slices retained for provenance and validation.
  std::vector<SourceSlice> sources;
  /// Host policy label attached to the material.
  std::string access_profile;
  /// Compares the stored fields for equality.
  bool operator==(const SharedContextDraft&) const = default;
};

/// @brief Versioned project entry returned by ReadContextAsync().
struct SharedContextItem {
  /// Project, item and exact version.
  SharedContextRef ref;
  /// Principal credited with publishing or authoring the version.
  PrincipalId publisher;
  /// Publication status of the requested version.
  SharedContextState state;
  /// Content, sources and host access label.
  SharedContextDraft data;
};

/// @brief Creates a versioned draft in a project.
struct ProposeSharedContext {
  /// Expected project revision at creation.
  Revision expected_project;
  /// Draft to register.
  SharedContextDraft draft;
  /// Compares the stored fields for equality.
  bool operator==(const ProposeSharedContext&) const = default;
};
/// @brief Writes a new draft version of an existing item.
struct EditSharedContext {
  /// Replacement title, content, sources and access label.
  SharedContextDraft draft;
  /// Compares the stored fields for equality.
  bool operator==(const EditSharedContext&) const = default;
};
/// @brief Transitions an eligible draft to review.
struct RequestContextReview {
  /// Compares the stored fields for equality.
  bool operator==(const RequestContextReview&) const = default;
};
/// @brief Publishes an eligible version for authorized project readers.
struct PublishContext {
  /// Compares the stored fields for equality.
  bool operator==(const PublishContext&) const = default;
};
/// @brief Withdraws the specified shared-context version.
struct WithdrawContext {
  /// Compares the stored fields for equality.
  bool operator==(const WithdrawContext&) const = default;
};

/// @brief Supported operations on an existing shared item.
using ContextChange = std::variant<EditSharedContext, RequestContextReview, PublishContext, WithdrawContext>;

/// @brief Version-checked update to a shared context item.
struct ChangeSharedContext {
  /// Exact item and version that must still match.
  SharedContextRef expected;
  /// Publication or content operation to perform.
  ContextChange change;
  /// Compares the stored fields for equality.
  bool operator==(const ChangeSharedContext&) const = default;
};

/// @brief Proposes a shared item or changes an existing version.
using SharedContextCommand = std::variant<ProposeSharedContext, ChangeSharedContext>;

/// @brief Permission-filtered project view returned by the project service.
struct ProjectSnapshot {
  /// Project ID.
  ProjectId project;
  /// Project revision for project-level optimistic writes.
  Revision revision;
  /// Visible task state and current attempts.
  std::vector<ProjectTaskSnapshot> tasks;
  /// Visible shared-context references.
  std::vector<SharedContextRef> shared_context;
  /// Visible sessions bound to the project.
  std::vector<SessionSummary> sessions;
};

/// @brief Host-provided project definition and registered workspaces.
struct ProjectBinding {
  /// Resolved project identity.
  ProjectId project;
  /// Workspace IDs recognized for this project.
  std::vector<WorkspaceId> workspaces;
};

/// @brief Host callback resolving project and workspace membership from a ProjectId.
using ProjectResolver = std::function<Async<ProjectBinding>(ProjectId)>;

/// @brief User- or Run-bound access to project tasks and shared material.
class ProjectService {
public:
  /// @brief Reads the current authorized projection of a project.
  /// @param project Project ID to resolve under this service's actor.
  /// @return The authorized task and shared-context projection.
  Async<ProjectSnapshot> ReadAsync(ProjectId project);

  /// @brief Observes project changes under the caller's current permissions.
  /// @param project Project to observe.
  /// @param observer Callback receiving immutable projections or an error.
  /// @return A subscription that keeps the project observation registered.
  Async<Subscription> ObserveAsync(ProjectId project, Observer<ProjectSnapshot> observer);

  /// @brief Applies a task command after checking revisions, ownership and the dependency graph.
  /// @param project Project that owns the task.
  /// @param command Stable ID for this logical mutation.
  /// @param request Task operation and its revision preconditions.
  /// @return A receipt identifying the updated task and revision.
  Async<Receipt<TaskUpdated>> UpdateTaskAsync(ProjectId project, CommandId command, TaskCommand request);

  /// @brief Waits for the exact task attempt to settle. Run-bound waits participate in dependency-cycle checks.
  /// @param execution Exact task execution being awaited.
  /// @param options Optional absolute deadline for the wait.
  /// @return The outcome of that execution.
  /// @note The wait deadline does not cancel the task.
  Async<TaskOutcome> WaitForTaskAsync(TaskExecutionRef execution, WaitOptions options = {});

  /// @brief Commits a shared-context proposal, edit or publication transition.
  /// @param project Project that owns the shared item.
  /// @param command Stable ID for this logical mutation.
  /// @param request Context operation and its revision preconditions.
  /// @return A receipt identifying the resulting item version.
  Async<Receipt<SharedContextRef>> UpdateContextAsync(ProjectId project, CommandId command, SharedContextCommand request);

  /// @brief Resolves the exact shared item version when authorized.
  /// @param item Exact shared-context version.
  /// @return The content and metadata visible to this actor.
  Async<SharedContextItem> ReadContextAsync(SharedContextRef item);

private:
  explicit ProjectService(std::shared_ptr<detail::RuntimeState> runtime, std::optional<RunId> actor = {});
  std::shared_ptr<detail::RuntimeState> runtime_;
  std::optional<RunId> actor_;
  friend class AgentRuntime;
  friend class RunContext;
};

} // namespace huxerui::agentkit
