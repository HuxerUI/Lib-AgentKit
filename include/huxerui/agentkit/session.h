#pragma once

/// @file
/// @brief Session lifecycle, versioned history, Run snapshots, commands and interactions.
///
/// Use SessionService to create, fork and open a session. Keep its Conversation while observing the current
/// snapshot or submitting actions. Use CommandService for pending approvals and uncertain commits,
/// and SessionGoals for the persistent objective. Keep versioned references from the observed view when
/// submitting an approval, opening an artifact or resolving a selection.

#include <huxerui/agentkit/types.h>

namespace huxerui::agentkit {

/// @brief Versioned agent definition bound to a session or Run.
struct AgentRef {
  /// Stable agent identity.
  AgentId id;
  /// Positive registered definition revision.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const AgentRef&) const = default;
};

/// @brief Exact registration of a model adapter, including its configuration revision.
struct ModelProviderRef {
  /// Model configuration identity in the runtime registry.
  ModelId id;
  /// Registration revision to pin for execution or recovery.
  Revision version;
  /// Orders values lexicographically by their declared fields.
  auto operator<=>(const ModelProviderRef&) const = default;
};

/// @brief Versioned host reference used to bind execution permissions.
struct ExecutionProfileRef {
  /// Host execution-profile identity.
  ExecutionProfileId id;
  /// Exact profile revision presented to access policy.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const ExecutionProfileRef&) const = default;
};

/// @brief Optional model and execution-profile choices for a new user-initiated Run.
struct UserRunOptions {
  /// Explicit model registration. Empty selects the agent's default.
  std::optional<ModelProviderRef> model;
  /// Optional host execution-profile revision to authorize.
  std::optional<ExecutionProfileRef> execution_profile;
  /// Compares the stored fields for equality.
  bool operator==(const UserRunOptions&) const = default;
};

/// @brief Resolved model and profile recorded at acceptance for a new or queued Run.
struct RunStartBinding {
  /// Exact model registration selected at acceptance.
  ModelProviderRef model;
  /// Selected host profile, when present.
  std::optional<ExecutionProfileRef> execution_profile;
  /// Compares the stored fields for equality.
  bool operator==(const RunStartBinding&) const = default;
};

/// @brief Exact version of a tool, including its catalog when dynamically registered.
struct ToolRef {
  /// Logical registered tool ID.
  ToolId id;
  /// Tool definition revision.
  Revision version;
  /// Owning dynamic catalog. Empty identifies a static or built-in tool.
  std::optional<ToolCatalogId> catalog;
  /// Compares the stored fields for equality.
  bool operator==(const ToolRef&) const = default;
};

/// @brief Exact version of a skill inside a registered source.
struct SkillRef {
  /// Owning SkillSource ID.
  SkillSourceId source;
  /// Identity within the source.
  SkillId id;
  /// Exact package version to activate or recover.
  Revision version;
  /// Orders values lexicographically by their declared fields.
  auto operator<=>(const SkillRef&) const = default;
};

/// @brief Discoverable metadata for a skill before reading its instructions.
struct SkillSummary {
  /// Exact version offered for activation.
  SkillRef ref;
  /// Display name.
  std::string name;
  /// Short discovery text for the user or model.
  std::string description;
};

/// @brief Exact published artifact identity and version for reads and actions.
struct ArtifactRef {
  /// Stable artifact ID across its versions.
  ArtifactId id;
  /// Published version to resolve.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const ArtifactRef&) const = default;
};

/// @brief Exact visible version of one message in its originating session.
struct MessageRef {
  /// Origin session, also for a message appearing in inherited history.
  SessionId session;
  /// Stable message ID within the origin.
  MessageId message;
  /// Version of the message content and block set.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const MessageRef&) const = default;
};

/// @brief Registered type and schema version of a host-defined message component.
struct ContentTypeRef {
  /// Host type name used for renderer lookup.
  std::string id;
  /// Schema revision for payload and action validation.
  Revision schema_version;
  /// Compares the stored fields for equality.
  bool operator==(const ContentTypeRef&) const = default;
};

/// @brief Exact version of a host resource for context or tool baseline checks.
struct ResourceRef {
  /// Logical host resource identity.
  ResourceId id;
  /// Opaque host-supplied baseline or version token.
  std::string version;
  /// Compares the stored fields for equality.
  bool operator==(const ResourceRef&) const = default;
};

/// @brief One versioned resource, message, artifact or project shared-context reference.
using SourceRef = std::variant<ResourceRef, MessageRef, ArtifactRef, SharedContextRef>;

/// @brief Half-open UTF-8 byte interval in one canonical text leaf.
/// Offsets address original message text or text resolved by the host. Use complete UTF-8 boundaries with begin_utf8 < end_utf8.
struct SourceTextRange {
  /// Message block or selectable artifact/resource part containing the text.
  std::variant<BlockId, PartId> leaf;
  /// Inclusive byte offset into the canonical text.
  std::uint64_t begin_utf8;
  /// Exclusive byte offset into the canonical text.
  std::uint64_t end_utf8;
  /// Compares the stored fields for equality.
  bool operator==(const SourceTextRange&) const = default;
};

/// @brief Versioned source with optional part or text-range narrowing.
/// @code{.cpp}
/// SourceSlice selected{message, {}, SourceTextRange{block, begin_utf8, end_utf8}};
/// auto excerpt = co_await conversation->ReadSelectionAsync(selected);
/// @endcode
struct SourceSlice {
  /// Exact source version.
  SourceRef source;
  /// Selected structural parts. Leave empty when using text_range.
  std::vector<PartId> parts;
  /// Optional text interval in one leaf. Empty with no parts references the whole source.
  std::optional<SourceTextRange> text_range;
  /// Compares the stored fields for equality.
  bool operator==(const SourceSlice&) const = default;
};

/// @brief Text brought back from a side chat with the selections that authorized its derivation.
struct SideChatConclusion {
  /// Visible versioned conclusion text.
  std::string text;
  /// Underlying authorized source slices in reading order.
  std::vector<SourceSlice> sources;
};

/// @brief Reference to a conclusion saved with a rejected queued input.
struct QueuedConclusionRef {
  /// Rejected queue item that holds the conclusion.
  QueueItemId item;
  /// Zero-based conclusion index within that item.
  std::uint64_t index;
  /// Compares the stored fields for equality.
  bool operator==(const QueuedConclusionRef&) const = default;
};

/// @brief Reference to a conclusion saved with an unapplied steering command.
struct UnappliedSteerConclusionRef {
  /// Accepted steering command that holds the conclusion.
  CommandId command;
  /// Zero-based conclusion index within that command.
  std::uint64_t index;
  /// Compares the stored fields for equality.
  bool operator==(const UnappliedSteerConclusionRef&) const = default;
};

/// @brief User or host task input and explicit business context.
/// Use the same value with CreateAsync(), SendAsync(), QueueAsync() or a supported work request.
/// References are validated again when delivered to the effective model.
struct SendInput {
  /// UTF-8 task or message body.
  std::string text;
  /// Ordered, versioned selections to resolve as model context.
  std::vector<SourceSlice> context;
  /// Exact published file or image versions to provide as attachments.
  std::vector<ArtifactRef> attachments;
  /// Exact skill versions the caller requests to activate.
  std::vector<SkillRef> skills;
  /// Exact slices of side-chat replies selected as conclusions.
  std::vector<SourceSlice> side_chat_conclusions;
  /// Conclusion references recovered from rejected queue items.
  std::vector<QueuedConclusionRef> recovered_conclusions;
  /// Conclusion references recovered from unapplied steering.
  std::vector<UnappliedSteerConclusionRef> recovered_steer_conclusions;
  /// Host-staged uploads to resolve and register at acceptance.
  std::vector<ArtifactId> staged_attachments;
  /// Compares the stored fields for equality.
  bool operator==(const SendInput&) const = default;
};

/// @brief Whether a session is retained across runtime restarts.
enum class SessionRetention {
  /// Eligible for normal persistence using the configured store.
  Stored,
  /// Owned by the current runtime, as for temporary side chats.
  Ephemeral
};
/// @brief Directory and lifecycle state of a session.
enum class SessionLifecycle {
  /// Active session visible to ordinary authorized directory queries.
  Open,
  /// Archived session visible to queries that opt in.
  Archived,
  /// Session is being removed and is no longer generally available.
  Deleting
};
/// @brief Origin used to prioritize updates to a visible title.
enum class SessionTitleSource {
  /// Default title while no better one is available.
  Placeholder,
  /// Title derived from the first meaningful input.
  FirstInput,
  /// Title improved by the host title generator.
  Optimized,
  /// Explicit title chosen by the user.
  Manual
};

/// @brief Initial identity, ownership and display settings for a new session.
struct SessionOptions {
  /// Agent ID to bind to the session.
  AgentId agent;
  /// Owning project, when applicable.
  std::optional<ProjectId> project;
  /// Workspace to associate with the session, when present.
  std::optional<WorkspaceId> workspace;
  /// Explicit starting title. Empty allows runtime title selection.
  std::string title;
  /// Persistence policy for the session.
  SessionRetention retention = SessionRetention::Stored;
  /// Compares the stored fields for equality.
  bool operator==(const SessionOptions&) const = default;
};

/// @brief Committed point in another session's timeline used for forking or comparing.
struct HistoryBoundary {
  /// Source session.
  SessionId session;
  /// Visible message or Run at the boundary.
  std::variant<MessageId, RunId> anchor;
  /// Committed sequence through which history may be inherited.
  Sequence through_sequence;
  /// Compares the stored fields for equality.
  bool operator==(const HistoryBoundary&) const = default;
};

/// @brief Versioned host allocation plan for a worktree or other copied workspace.
struct WorkspaceCopyPlanRef {
  /// Stable prepared copy plan ID.
  WorkspaceCopyPlanId id;
  /// Exact plan revision to execute or release.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const WorkspaceCopyPlanRef&) const = default;
};

/// @brief Existing or preplanned copied workspace for a session fork.
struct WorkspaceChoice {
  /// @brief How a workspace is selected for the new session.
  enum class Kind {
    /// Bind to an existing workspace.
    Existing,
    /// Ask the host to allocate a copy from the source workspace.
    CopyFromSource
  };
  /// Binding or allocation operation.
  Kind kind;
  /// Workspace identity referenced by the choice.
  WorkspaceId workspace;
  /// Exact host copy plan for an explicit copy, when supplied.
  std::optional<WorkspaceCopyPlanRef> copy_plan;
  /// Compares the stored fields for equality.
  bool operator==(const WorkspaceChoice&) const = default;
};

/// @brief Requirements for a final reply to an accepted WorkRequest.
struct ReplyExpectation {
  /// Optional schema for the reply value.
  std::optional<JsonSchema> schema;
  /// Artifact types required from a successful reply.
  std::vector<std::string> artifact_types;
  /// Absolute deadline for a valid final reply.
  std::optional<TimePoint> reply_by;
  /// Compares the stored fields for equality.
  bool operator==(const ReplyExpectation&) const = default;
};

/// @brief Exact project task attempt used for claim, execution and result tracking.
struct TaskExecutionRef {
  /// Project owning the task.
  ProjectId project;
  /// Task being executed.
  ProjectTaskId task;
  /// Claim and attempt identity.
  AttemptId attempt;
  /// Compares the stored fields for equality.
  bool operator==(const TaskExecutionRef&) const = default;
};

/// @brief Explicit upper bound on input, tools and writes for collaboration work.
struct WorkScope {
  /// Versioned sources allowed for the requested work.
  std::vector<SourceSlice> inputs;
  /// Logical resources eligible for approved writes.
  std::vector<ResourceId> writable_resources;
  /// Tool IDs eligible for the work.
  std::vector<ToolId> tools;
  /// Compares the stored fields for equality.
  bool operator==(const WorkScope&) const = default;
};

/// @brief Initial or delivered work that can start its own Run in a receiving session.
struct WorkRequest {
  /// Task content and explicit references for the receiving Run.
  SendInput input;
  /// Requested upper bound intersected with receiving-side authorization.
  std::optional<WorkScope> scope;
  /// Whether the sender requests automatic start when accepted.
  bool request_start;
  /// Final reply requirements, when a reply is expected.
  std::optional<ReplyExpectation> reply;
  /// Project task attempt to bind to the receiving Run, when applicable.
  std::optional<TaskExecutionRef> task;
  /// Opts into retiring linked work after a relevant goal definition edit.
  bool cancel_on_goal_revision = false;
  /// Compares the stored fields for equality.
  bool operator==(const WorkRequest&) const = default;
};

/// @brief Origin and precise selections used to create an ephemeral side chat.
struct SideChatSeed {
  /// Conversation from which the user selected content.
  SessionId source;
  /// Authorized source slices in reading order.
  std::vector<SourceSlice> selected;
  /// Compares the stored fields for equality.
  bool operator==(const SideChatSeed&) const = default;
};

/// @brief Creates a session and optionally accepts its initial input atomically.
struct CreateSessionRequest {
  /// Configuration of the new session.
  SessionOptions options;
  /// No initial Run, direct input, or a collaboration work request.
  /// A side chat can include new user-staged attachments in its first input. Its source references come from SideChatSeed.
  std::variant<std::monostate, SendInput, WorkRequest> initial;
  /// Origin and selections for an ephemeral side chat, when applicable.
  std::optional<SideChatSeed> side_chat;
  /// Explicit starting configuration for direct user input.
  UserRunOptions user_start{};
};

/// @brief Creates a session with authorized inherited history from a committed point.
struct ForkSessionRequest {
  /// Source history boundary.
  HistoryBoundary from;
  /// New session configuration.
  SessionOptions options;
  /// Optional workspace binding or copy plan for the fork.
  std::optional<WorkspaceChoice> workspace;
  /// Optional task to accept along with the fork.
  std::optional<WorkRequest> initial;
};

/// @brief Execution lifecycle observed in RunSnapshot.
enum class RunState {
  /// Run can continue executing.
  Running,
  /// Run is waiting for a form or approval.
  WaitingForInput,
  /// Run is waiting for collaboration reply.
  WaitingForReply,
  /// Run is waiting for a prerequisite or child work.
  WaitingForDependency,
  /// New work has stopped and issued work is settling.
  Cancelling,
  /// Run completed successfully.
  Succeeded,
  /// Run ended in failure.
  Failed,
  /// Run ended after cancellation.
  Cancelled
};

/// @brief Compact permission-filtered session entry for directory and UI state.
struct SessionSummary {
  /// Stable session ID for opening the conversation.
  SessionId id;
  /// Current human-readable name.
  std::string title;
  /// Origin and precedence of the current title.
  SessionTitleSource title_source;
  /// Current versioned agent binding.
  AgentRef agent;
  /// Optimistic revision used when transferring the agent.
  Revision binding_revision;
  /// Optimistic revision used when updating title or lifecycle.
  Revision metadata_revision;
  /// Owning project, when present and visible.
  std::optional<ProjectId> project;
  /// Bound workspace, when present and visible.
  std::optional<WorkspaceId> workspace;
  /// Directory and lifecycle state.
  SessionLifecycle lifecycle;
  /// Timestamp used for activity reporting and ordering.
  TimePoint last_activity;
  /// Current active root Run, if visible.
  std::optional<RunId> active_run;
  /// State of the active or latest Run, when available.
  std::optional<RunState> run_state;
  /// Count of pending visible forms and approvals.
  std::size_t pending_interactions;
  /// Count of queued inputs waiting to be consumed.
  std::size_t queued_inputs;
  /// Compares the stored fields for equality.
  bool operator==(const SessionSummary&) const = default;
};

/// @brief Opaque continuation for a particular paged query.
struct PageCursor {
  /// Pass back unchanged with the same query to load the next page.
  std::string value;
};

/// @brief Restricts a SessionService directory query.
struct SessionFilter {
  /// Only sessions in this project, when set.
  std::optional<ProjectId> project;
  /// Only sessions currently bound to this agent, when set.
  std::optional<AgentId> agent;
  /// Includes archived sessions in the authorized directory.
  bool include_archived = false;
  /// Includes temporary sessions visible in the active runtime.
  bool include_ephemeral = false;
  /// Requested maximum items in one page.
  std::size_t page_size = 50;
};

/// @brief Directory page projected under the current caller's authorization.
struct SessionPage {
  /// Current visible catalog revision.
  Revision revision;
  /// Revision of catalog ordering used for pagination.
  Revision order_revision;
  /// Visible sessions in directory order.
  std::vector<SessionSummary> sessions;
  /// Cursor for the next page, when one exists.
  std::optional<PageCursor> next;
};

/// @brief Transcript text suitable for plain or Markdown presentation.
struct TextBlock {
  /// Canonical UTF-8 text used when resolving source ranges.
  std::string text;
  /// Allows Markdown display while retaining text as the source.
  bool markdown;
};

/// @brief System content recording a committed change of agent binding.
struct AgentTransferred {
  /// Previous agent definition, when visible.
  std::optional<AgentRef> from;
  /// New agent definition, when visible.
  std::optional<AgentRef> to;
  /// Session binding revision after transfer.
  Revision binding_revision;
  /// Authorized handoff text, when provided.
  std::optional<std::string> handoff_summary;
};
/// @brief Inline transcript reference to an existing published artifact version.
struct ArtifactBlock {
  /// Exact version to preview.
  ArtifactRef artifact;
  /// Human-readable name displayed in the transcript.
  std::string name;
};
/// @brief Transcript reference to a form or approval shown as an interaction.
/// Retained in the storage representation. The tool output validator does not accept this block type.
/// AgentChat presents forms and approvals from SessionSnapshot::interactions.
struct InteractionBlock {
  /// Identity of the session interaction.
  InteractionId interaction;
};
/// @brief Transcript reference to a recorded tool invocation.
/// Retained in the storage representation. The tool output validator does not accept this block type.
/// AgentChat reads call state from SessionSnapshot::tools.
struct ToolBlock {
  /// Logical call visible through the session's tool snapshots.
  ToolCallId call;
};
/// @brief Typed host-defined content and its allowed visible associations.
struct CustomBlock {
  /// Registered rendering type and payload schema version.
  ContentTypeRef type;
  /// Validated structured payload for the component.
  JsonValue data;
  /// Plain text for views without the registered renderer.
  std::string fallback;
  /// Interactions bound to this component.
  std::vector<InteractionId> interactions;
  /// Versioned artifacts bound to this component.
  std::vector<ArtifactRef> artifacts;
};

/// @brief Exact transcript location for a registered component action.
struct CustomBlockRef {
  /// Origin message and version.
  MessageRef message;
  /// Component block ID within that message version.
  BlockId block;
  /// Compares the stored fields for equality.
  bool operator==(const CustomBlockRef&) const = default;
};

/// @brief One ordered segment in a transcript message.
struct ContentBlock {
  /// Stable block ID for actions and source selection.
  BlockId id;
  /// Content or reference stored for this segment.
  std::variant<TextBlock, ArtifactBlock, InteractionBlock, ToolBlock,
      CustomBlock, AgentTransferred, SideChatConclusion> body;
};

/// @brief Origin category of a transcript entry.
enum class MessageAuthor {
  /// Submitted by the user.
  User,
  /// Published by an agent.
  Agent,
  /// Produced by a tool operation.
  Tool,
  /// Delivered by another session.
  Collaboration,
  /// Produced by the runtime for lifecycle or binding information.
  System
};
/// @brief Publication state of a transcript message.
enum class MessageState {
  /// Content can continue changing during the active turn.
  Streaming,
  /// Version is sealed and eligible for authorized source selection.
  Complete,
  /// Partial content was retained after interruption.
  Interrupted
};

/// @brief Versioned transcript entry with ordered content blocks.
struct Message {
  /// Exact origin and version of this entry.
  MessageRef ref;
  /// Position in the session history projection.
  Sequence sequence;
  /// Run associated with the entry, when applicable and visible.
  std::optional<RunId> run;
  /// Origin category of the content.
  MessageAuthor author;
  /// Whether the content is streaming, complete or interrupted.
  MessageState state;
  /// Ordered visible content blocks.
  std::vector<ContentBlock> blocks;
};

/// @brief Selects a bounded window of committed session history.
struct HistoryQuery {
  /// Maximum messages in one page.
  std::size_t page_size = 50;
  /// Optional fixed upper history boundary for the query.
  std::optional<Sequence> through_sequence;
  /// Cursor for an earlier page from the same authorized query.
  std::optional<PageCursor> before;
};

/// @brief One page of committed, visible history.
struct HistoryPage {
  /// Upper sequence bound used for this page and following cursors.
  Sequence through_sequence;
  /// Visible messages in reading order.
  std::vector<Message> messages;
  /// Cursor for an earlier page, when available.
  std::optional<PageCursor> older;
};

/// @brief Controls how much preceding history to include in a Conversation observation.
struct SessionHistoryWindow {
  /// Sequence anchor for the retained view.
  Sequence first;
  /// Number of older messages requested before first.
  std::size_t preceding = 0;
  /// Compares the stored fields for equality.
  bool operator==(const SessionHistoryWindow&) const = default;
};

/// @brief Updates the visible title and records a manual title source.
struct RenameSession {
  /// Replacement human-readable title.
  std::string title;
  /// Compares the stored fields for equality.
  bool operator==(const RenameSession&) const = default;
};
/// @brief Moves the session to the archived directory.
struct ArchiveSession {
  /// Compares the stored fields for equality.
  bool operator==(const ArchiveSession&) const = default;
};
/// @brief Returns an archived session to the ordinary directory.
struct UnarchiveSession {
  /// Compares the stored fields for equality.
  bool operator==(const UnarchiveSession&) const = default;
};
/// @brief Removes the session under the applicable runtime and storage lifecycle.
struct DeleteSession {
  /// Compares the stored fields for equality.
  bool operator==(const DeleteSession&) const = default;
};
/// @brief Supported metadata and lifecycle changes for an existing session.
using SessionMetadataChange = std::variant<RenameSession, ArchiveSession, UnarchiveSession, DeleteSession>;

class Conversation;
struct ArtifactPreview;
class SessionGoals;

/// @brief User- or Run-bound creation, discovery and history operations.
class SessionService {
public:
  /// @brief Accepts a session and its initial input or work request under one command ID.
  /// @param command Stable ID of this creation request.
  /// @param request Session options and optional initial input or project work.
  /// @return The created session ID and its initial Run, when one was accepted.
  Async<Receipt<SessionCreated>> CreateAsync(CommandId command, CreateSessionRequest request);

  /// @brief Creates a session from an authorized committed history boundary.
  /// @param command Stable ID used to recover a submission or its workspace allocation.
  /// @param request Parent session, committed history boundary and destination options.
  /// @return The receipt identifying the forked session.
  Async<Receipt<SessionCreated>> ForkAsync(CommandId command, ForkSessionRequest request);

  /// @brief Releases an unused copied workspace after checking the original fork and its allocation.
  /// @param command Original fork command ID.
  /// @param request Original fork request with its copy plan.
  /// @param allocated Workspace allocated for that request.
  /// @note A bound or still-needed allocation cannot be abandoned through this method.
  Async<void> AbandonWorkspaceForkAsync(CommandId command, ForkSessionRequest request, WorkspaceId allocated);

  /// @brief Opens a Conversation handle under the current principal and authorization.
  /// @param session Session ID from a visible listing or creation receipt.
  /// @return An authorized conversation handle or a read-access or lifecycle error.
  Async<std::shared_ptr<Conversation>> OpenAsync(SessionId session);

  /// @brief Reads a page of the authorized directory using the supplied filter and cursor.
  /// @param filter Visibility, lifecycle and scope constraints for the directory.
  /// @param cursor Continuation returned by the previous page for that filter. Empty starts at the first page.
  /// @return A page and its next cursor, when more results are available.
  Async<SessionPage> ListAsync(SessionFilter filter, std::optional<PageCursor> cursor);

  /// @brief Observes the first page of the authorized session directory.
  /// @param filter Scope and size for the directory's first page.
  /// @param observer Callback receiving immutable pages or an observation error.
  /// @return A subscription that keeps this observation registered.
  Async<Subscription> ObserveListAsync(SessionFilter filter, Observer<SessionPage> observer);

  /// @brief Reads a page of authorized committed history, including inherited history when available.
  /// @param session Session whose history is being read.
  /// @param query History window and page size.
  /// @return A page of versioned messages and continuation information.
  Async<HistoryPage> ReadHistoryAsync(SessionId session, HistoryQuery query);

  /// @brief Applies a version-checked title or lifecycle change.
  /// @param command Stable ID of this metadata change.
  /// @param session Target session ID.
  /// @param expected Current metadata revision from the session summary.
  /// @param change Title or lifecycle transition to apply.
  /// @return The committed receipt, or a conflict if the revision changed.
  Async<CommandReceipt> UpdateMetadataAsync(CommandId command, SessionId session,
      Revision expected, SessionMetadataChange change);

private:
  explicit SessionService(std::shared_ptr<detail::RuntimeState> state, std::optional<RunId> actor = {});
  std::shared_ptr<detail::RuntimeState> state_;
  std::optional<RunId> actor_;
  friend class AgentRuntime;
  friend class RunContext;
};

/// @brief Origin of a specific execution attempt.
enum class RunTrigger {
  /// Direct user input.
  UserInput,
  /// Re-execution of eligible prior input with retry safety checks.
  Retry,
  /// New execution of eligible prior input with the earlier history prefix.
  Regenerate,
  /// Accepted cross-session work.
  Collaboration,
  /// Execution of a claimed project task.
  ProjectTask,
  /// Automatic work associated with a session goal.
  Goal,
  /// Child Run requested by another Run in the same session.
  Delegation,
  /// Versioned artifact operation such as rework, revert or a registered action.
  ArtifactChange,
  /// Initial work for a comparison candidate.
  ForkComparison
};
/// @brief Reason a Run reached its terminal state.
enum class RunEndReason {
  /// Run completed normally.
  Completed,
  /// Run failed for the recorded reason.
  Failed,
  /// User cancellation ended the Run.
  UserCancelled,
  /// The parent Run's cancellation ended this child.
  ParentCancelled,
  /// The execution deadline expired.
  Deadline,
  /// A configured execution budget was exhausted.
  Budget,
  /// Execution was interrupted by an incomplete stream or unavailable recovery state.
  Interrupted,
  /// Reserved Run end reason for a missing collaboration reply. Current waits report ReplyEnd::MissingReply.
  ReplyMissing,
  /// An issued tool's effects could not be determined.
  ToolOutcomeUnknown,
  /// Run belongs to a goal definition retired by an edit.
  GoalRevised
};
/// @brief Kind of outstanding work recorded for a suspended Run.
enum class WaitKind {
  /// Form or interaction response.
  Input,
  /// Reply to a collaboration request.
  Reply,
  /// Result of a project task attempt.
  Dependency,
  /// Results of a delegated child batch.
  Delegation
};

/// @brief Identity and deadline of a wait visible in RunSnapshot or RunResume.
struct RunWait {
  /// Operation this Run is waiting to resolve.
  WaitKind kind;
  /// Interaction ID, collaboration message ID, task attempt or delegation command, according to kind.
  std::variant<InteractionId, CollaborationMessageId, TaskExecutionRef, CommandId> subject;
  /// Applicable absolute deadline, when recorded.
  std::optional<TimePoint> deadline;
};

/// @brief Accumulated execution usage at the scope of the containing record.
struct Usage {
  /// Input tokens reported by provider turns.
  std::uint64_t input_tokens;
  /// Output tokens reported by provider turns.
  std::uint64_t output_tokens;
  /// Count of model turns accounted for.
  std::uint64_t model_turns;
  /// Count of logical tool calls accounted for.
  std::uint64_t tool_calls;
};

/// @brief Limits placed on a Run, project goal or group of branches.
/// Empty fields leave that dimension to the enclosing scope and runtime defaults.
struct Budget {
  /// Maximum reported input and output tokens combined.
  std::optional<std::uint64_t> tokens;
  /// Maximum model turns available.
  std::optional<std::uint64_t> model_turns;
  /// Maximum logical tool calls available.
  std::optional<std::uint64_t> tool_calls;
  /// Maximum context payload size measured in bytes.
  std::optional<std::uint64_t> context_bytes;
  /// Elapsed time allowance from the applicable starting point.
  std::optional<Duration> deadline;
  /// Compares the stored fields for equality.
  bool operator==(const Budget&) const = default;
};

/// @brief Business result of a successful Run or host workflow.
struct RunCompletion {
  /// Human-readable completion summary.
  std::string summary;
  /// Structured result, when calculated.
  std::optional<JsonValue> structured;
  /// Exact output versions associated with completion.
  std::vector<ArtifactRef> artifacts;
};

/// @brief Origin of a skill activation in the Run.
enum class SkillActivationTrigger {
  /// Explicitly selected in the accepted user input.
  User,
  /// Requested by the model through the activation tool.
  Model,
  /// Requested by the host workflow.
  Host
};

/// @brief Version and origin of an activated skill.
struct SkillActivation {
  /// Exact active skill version.
  SkillRef skill;
  /// Origin of the activation request.
  SkillActivationTrigger trigger;
  /// Compares the stored fields for equality.
  bool operator==(const SkillActivation&) const = default;
};

/// @brief Application state of supplemental input accepted for a Run.
enum class SteerState {
  /// Accepted and awaiting a safe application boundary.
  Pending,
  /// Incorporated into the addressed Run.
  Applied,
  /// The Run ended or the supplement otherwise could not be applied.
  NotApplied
};

/// @brief Visible state of a recorded steering message.
struct RunSteerSnapshot {
  /// Transcript message that owns the supplement.
  MessageId message;
  /// Current application status.
  SteerState state;
  /// Reason the supplement was not applied, when available.
  std::optional<AgentError> reason;
  /// Original command used to recover a usable draft, when available.
  std::optional<CommandId> recovery;
};

/// @brief Pending supplemental input offered to a host workflow.
struct RunSteerInput {
  /// Message ID to acknowledge with RunCheckpoint::steer_through after application.
  MessageId message;
  /// Accepted supplemental user input.
  SendInput input;
  /// Authorized side-chat conclusions derived from that input.
  std::vector<SideChatConclusion> conclusions;
};

/// @brief Durable wall-clock timing for a Run and its user-input waits.
struct RunTiming {
  /// When execution began. A queued delegate has no start time until dispatched.
  TimePoint started_at;
  /// When the Run reached its terminal state, if it has ended.
  std::optional<TimePoint> ended_at;
  /// Sum of closed intervals spent waiting for a form or approval.
  Duration waiting_for_input{};
  /// Start of the current form or approval wait, if one is open.
  std::optional<TimePoint> input_wait_started_at;
};

/// @brief Estimated input occupancy of a model turn with a known context budget.
struct ModelContextUsage {
  /// Input tokens estimated for the latest assembled request, including model instructions and tools.
  std::uint64_t estimated_input_tokens = 0;
  /// Input tokens available after reserving space for this turn's work.
  std::uint64_t input_budget_tokens = 0;
  /// Initial estimate for this request if compaction reduced it.
  std::optional<std::uint64_t> before_compaction_tokens;
  /// Total input and output window of the bound model revision, when known.
  std::optional<std::uint64_t> context_window_tokens;
};

/// @brief Authorized, durable state of one Run.
struct RunSnapshot {
  /// Execution attempt ID.
  RunId id;
  /// Session owning the Run.
  SessionId session;
  /// Exact agent definition used for this Run.
  AgentRef agent;
  /// Exact model registration bound to this Run.
  ModelProviderRef model;
  /// Reason this Run started.
  RunTrigger trigger;
  /// Parent Run ID for same-session delegation, when visible.
  std::optional<RunId> parent;
  /// Earlier attempt used as the source of a retry or regeneration.
  std::optional<RunId> retry_of;
  /// Incoming collaboration request, when applicable.
  std::optional<CollaborationMessageId> work_request;
  /// Bound task attempt, when applicable.
  std::optional<TaskExecutionRef> task;
  /// Goal definition version bound to this Run, when applicable.
  std::optional<GoalRef> goal;
  /// Observable execution state.
  RunState state;
  /// Current registered waits.
  std::vector<RunWait> waits;
  /// Application state of accepted supplemental input.
  std::vector<RunSteerSnapshot> steering;
  /// Versioned skills currently active in this Run.
  std::vector<SkillActivation> active_skills;
  /// Accumulated usage for this Run.
  Usage usage;
  /// Reason a terminal Run ended.
  std::optional<RunEndReason> end_reason;
  /// Completion result of a successful Run, when available.
  std::optional<RunCompletion> result;
  /// Error for a failed or interrupted Run, when available.
  std::optional<AgentError> failure;
  /// Exact host execution profile resolved at acceptance, when present.
  std::optional<ExecutionProfileRef> execution_profile;
  /// Timing recorded by the runtime. Older stored Runs may have no timing.
  std::optional<RunTiming> timing;
  /// Provider-supplied public reasoning summary for this Run, when offered.
  /// Adapter continuation state is private and excluded.
  std::string reasoning_summary;
  /// Whether the runtime is requesting or consuming a model turn.
  bool model_active = false;
  /// Occupancy of the latest model request with a known input budget, when available.
  std::optional<ModelContextUsage> context_usage;
};

/// @brief Versioned interaction payload requesting user data.
struct FormRequest {
  /// Human-readable request title.
  std::string title;
  /// Supported JSON schema subset used to validate the submission.
  JsonSchema schema;
  /// Host-defined presentation hints and fields for rendering the form.
  JsonValue presentation;
  /// Absolute time after which the form cannot be submitted, when set.
  std::optional<TimePoint> expires_at;
};

/// @brief Declared external effect category of a tool.
enum class ToolEffect {
  /// Reads input without issuing a write.
  Read,
  /// Stages output for runtime publication.
  StageArtifact,
  /// Can modify a host resource.
  WriteResource,
  /// Can submit work or effects to an external system.
  ExternalSubmit
};

/// @brief One resource and the version expected at execution.
struct TargetBaseline {
  /// Logical target resource identity.
  ResourceId target;
  /// Expected resource reference, when an existing version is required.
  std::optional<ResourceRef> expected;
  /// Compares the stored fields for equality.
  bool operator==(const TargetBaseline&) const = default;
};

/// @brief Reviewable consequences of replacing an active goal definition.
struct GoalEditImpact {
  /// Exact definition that would be replaced.
  GoalRef previous;
  /// Primary Run that would be retired, when applicable.
  std::optional<RunId> retiring_run;
  /// Issued calls that must settle or be reconciled.
  std::vector<ToolCallId> in_flight_calls;
  /// Linked Runs allowed to continue under their current bindings.
  std::vector<RunId> continuing_runs;
  /// Linked project attempts that remain active.
  std::vector<TaskExecutionRef> continuing_attempts;
  /// Collaboration requests that would be withdrawn.
  std::vector<CollaborationMessageId> withdrawn_requests;
  /// Task claims that would be released.
  std::vector<TaskExecutionRef> released_attempts;
  /// Interactions that would no longer be valid.
  std::vector<InteractionId> invalidated_interactions;
  /// Whether the existing completion review would be cleared.
  bool resets_completion_review;
  /// Other linked Runs that would be retired.
  std::vector<RunId> retiring_linked_runs;
  /// Compares the stored fields for equality.
  bool operator==(const GoalEditImpact&) const = default;
};

/// @brief Computed impact and token for confirming a particular goal edit.
struct GoalEditPreview {
  /// Visible impacts computed against the current state.
  GoalEditImpact impact;
  /// Opaque token to use in EditGoal::preview_token.
  std::string token;
};

/// @brief Visible context for reviewing a tool or an operation.
struct ApprovalPreview {
  /// Human-readable title.
  std::string title;
  /// Description of the proposed change.
  std::string summary;
  /// Exact preview artifact versions available to the reviewer.
  std::vector<ArtifactRef> artifacts;
  /// Resource baselines bound to the plan.
  std::vector<TargetBaseline> targets;
  /// Goal definition change impact, when this approval covers an edit.
  std::optional<GoalEditImpact> goal_edit;
};

/// @brief Approval interaction bound to an original tool call and plan.
struct ToolApproval {
  /// Logical tool call that will receive the decision.
  ToolCallId call;
  /// Exact immutable plan being approved.
  PlanId plan;
  /// Visible review information for this plan.
  ApprovalPreview preview;
};

/// @brief User response to an approval interaction.
enum class ApprovalDecision {
  /// Accepts the exact current approval.
  Approve,
  /// Refuses the proposed action.
  Deny
};
/// @brief Lifecycle of a form or approval.
enum class InteractionState {
  /// Interaction is open for its authorized recipient.
  Pending,
  /// A valid form reply was accepted.
  Submitted,
  /// An approval decision was accepted.
  Approved,
  /// The proposal was refused.
  Denied,
  /// Its deadline elapsed before a valid submission.
  Expired,
  /// The interaction was closed by cancellation.
  Cancelled,
  /// The interaction's underlying request, policy or plan no longer matches.
  Invalidated
};

/// @brief Approval gating a command other than a single prepared tool call.
struct OperationApproval {
  /// Original command whose progress is affected by this decision.
  CommandId pending_command;
  /// Visible scope of the proposed operation.
  ApprovalPreview preview;
};

/// @brief Form, tool approval or operation approval carried by an interaction.
using InteractionBody = std::variant<FormRequest, ToolApproval, OperationApproval>;

/// @brief Authorized form or approval projection including the version required for submission.
struct InteractionSnapshot {
  /// Stable identity of the interaction.
  InteractionId id;
  /// Current version to submit in InteractionReply.
  Revision version;
  /// Session owning the interaction.
  SessionId session;
  /// Owning Run, when applicable.
  std::optional<RunId> run;
  /// Principal allowed to handle this interaction.
  PrincipalId recipient;
  /// Current lifecycle state.
  InteractionState state;
  /// Typed form or approval payload.
  InteractionBody body;
  /// Effective absolute deadline when one is configured.
  std::optional<TimePoint> expires_at;
};


/// @brief Invocation of an action registered for a particular transcript block version.
struct CustomBlockAction {
  /// Exact version and block on which the action is invoked.
  CustomBlockRef target;
  /// Registered action name for this content type.
  std::string action;
  /// JSON validated against the registered action schema.
  JsonValue payload;
  /// Compares the stored fields for equality.
  bool operator==(const CustomBlockAction&) const = default;
};

/// @brief Invocation of an action on a particular artifact version.
struct ArtifactAction {
  /// Exact target version.
  ArtifactRef artifact;
  /// Name registered for the artifact type.
  std::string action;
  /// JSON validated against the registered action schema.
  JsonValue payload;
  /// Compares the stored fields for equality.
  bool operator==(const ArtifactAction&) const = default;
};

/// @brief Closes a form without providing a value.
struct DismissForm {
  /// Compares the stored fields for equality.
  bool operator==(const DismissForm&) const = default;
};

/// @brief Conditional form submission, dismissal or approval decision.
struct InteractionReply {
  /// Interaction being answered.
  InteractionId id;
  /// Visible interaction version that must still match.
  Revision expected_version;
  /// Submitted JSON, form dismissal or approval decision.
  std::variant<JsonValue, DismissForm, ApprovalDecision> response;
  /// Compares the stored fields for equality.
  bool operator==(const InteractionReply&) const = default;
};

/// @brief Current or terminal result of an interaction wait.
struct InteractionOutcome {
  /// State with which the wait resolved.
  InteractionState state;
  /// Accepted form data, when the outcome includes it.
  std::optional<JsonValue> value;
};

/// @brief Current state of input accepted for later execution.
enum class QueueItemState {
  /// Waiting to start.
  Queued,
  /// Consumed to start a Run.
  Started,
  /// Removed before starting.
  Withdrawn,
  /// Failed to start after revalidation.
  Rejected
};

/// @brief Recorded input and binding for one session queue entry.
struct QueuedInput {
  /// Stable queue item identity.
  QueueItemId id;
  /// User input, collaboration request ID or stored operation command.
  std::variant<SendInput, CollaborationMessageId, CommandId> input;
  /// Lifecycle state of the entry.
  QueueItemState state;
  /// Run that consumed this entry, when started and visible.
  std::optional<RunId> run;
  /// Exact user model and profile selection captured at queue acceptance, when applicable.
  std::optional<RunStartBinding> user_start;
  /// Resolved side-chat conclusions carried by this queue item.
  std::vector<SideChatConclusion> conclusions;
  /// Failure, rejection or withdrawal diagnostic, when available.
  std::optional<AgentError> reason;
};

/// @brief Display text and optional usable source for a recovered draft item.
struct QueuedDraftReference {
  /// Authorized reattachment reference. Empty marks an unavailable source.
  std::optional<std::variant<SourceSlice, QueuedConclusionRef, UnappliedSteerConclusionRef, ArtifactRef>> reference;
  /// Text for presenting the recovered reference.
  std::string text;
  /// Compares the stored fields for equality.
  bool operator==(const QueuedDraftReference&) const = default;
};

/// @brief Recoverable user input, references and skills from scheduled or unapplied work.
struct QueuedDraft {
  /// Original input text.
  std::string text;
  /// References to restore with the input, including unavailable markers.
  std::vector<QueuedDraftReference> references;
  /// Skill versions that can be restored into the draft.
  std::vector<SkillRef> skills;
};

/// @brief Draft shape also used for an unapplied steering command.
using RecoveredDraft = QueuedDraft;

/// @brief Lifecycle of a logical tool call across preparation, approval and effects.
enum class ToolCallState {
  /// Runtime is resolving and preparing the execution plan.
  Preparing,
  /// An automatic reviewer is evaluating the plan.
  Reviewing,
  /// Plan is waiting for the authorized user.
  AwaitingApproval,
  /// Plan can be scheduled when its grants and conflicts allow execution.
  Ready,
  /// Call has been issued with its approved plan.
  Executing,
  /// Call completed successfully.
  Succeeded,
  /// Policy or approval denied the call.
  Denied,
  /// Call ended with a known failure.
  Failed,
  /// The tool revision, approval or expected target no longer matches.
  Conflict,
  /// Some effects occurred and are known.
  PartiallyApplied,
  /// Call closed with a known cancellation.
  Cancelled,
  /// Issued work requires reconciliation before the effects can be relied upon.
  ResultUnknown
};

/// @brief Whether a particular resource was changed by an issued call.
enum class ResourceEffectState {
  /// A change to this resource was confirmed.
  Applied,
  /// The resource was confirmed unchanged by the call.
  Unchanged,
  /// Effect on this resource could not be confirmed.
  Unknown
};
/// @brief Compensation mechanism declared by a plan or an actual resource effect.
enum class UndoSupport {
  /// No undo mechanism is available.
  None,
  /// The host can restore a previously recorded version.
  RestoreVersion,
  /// The host can prepare a compensating action for the effect.
  Compensate
};

/// @brief Host-owned key for the data needed to prepare a compensating operation.
struct UndoRef {
  /// Opaque handle retained for the host adapter.
  std::string handle;
  /// Compensation mode associated with this handle.
  UndoSupport support;
  /// Compares the stored fields for equality.
  bool operator==(const UndoRef&) const = default;
};

/// @brief Confirmed or uncertain effect of a logical tool call on one resource.
struct ResourceEffect {
  /// Logical resource affected by execution.
  ResourceId resource;
  /// Whether the resource changed, stayed unchanged, or needs reconciliation.
  ResourceEffectState state;
  /// Version observed before the effect, when known.
  std::optional<ResourceRef> before;
  /// Version observed after the effect, when known.
  std::optional<ResourceRef> after;
  /// Host data reference for preparing a revert, when supported.
  std::optional<UndoRef> undo;
  /// Resource-specific failure or uncertainty detail.
  std::optional<AgentError> error;
  /// Compares the stored fields for equality.
  bool operator==(const ResourceEffect&) const = default;
};

/// @brief Result requested by the host automatic reviewer.
enum class ToolReviewDecision {
  /// Approve the specific immutable plan.
  Approve,
  /// Reject the specific plan.
  Deny,
  /// Require the authorized user to decide.
  DeferToUser
};
/// @brief Recorded result of the review lifecycle.
enum class ToolReviewOutcome {
  /// Automatic approval was accepted.
  Approved,
  /// Automatic denial was accepted.
  Denied,
  /// The review was routed to human approval.
  Escalated,
  /// The plan, policy or review binding no longer matches.
  Invalidated,
  /// Review was recorded but has no final decision yet.
  Pending
};

/// @brief Durable record of review of a particular prepared plan.
struct ToolReviewRecord {
  /// Plan that was reviewed.
  PlanId plan;
  /// Registered reviewer identifier.
  std::string reviewer;
  /// Reviewer registration version for this decision.
  Revision reviewer_version;
  /// Recorded review state.
  ToolReviewOutcome outcome;
  /// Human-readable review diagnostic or rationale.
  std::string reason;
};

/// @brief Authorized projection of an original logical tool call.
struct ToolCallSnapshot {
  /// Stable call ID to use in waits or reconciliation.
  ToolCallId id;
  /// Run that owns this call.
  RunId run;
  /// Exact tool definition bound to the call.
  ToolRef tool;
  /// Prepared plan ID, when available.
  std::optional<PlanId> plan;
  /// Current lifecycle and execution outcome.
  ToolCallState state;
  /// Applicable automatic-review record, when present.
  std::optional<ToolReviewRecord> review;
  /// Latest reported progress text.
  std::string progress;
  /// Visible resource effects recorded for reconciliation or rework.
  std::vector<ResourceEffect> effects;
  /// Associated visible artifact versions.
  std::vector<ArtifactRef> artifacts;
  /// Failure, conflict or cancellation diagnostic, when available.
  std::optional<AgentError> reason;
  /// Original tool call targeted by this compensation, when applicable.
  std::optional<ToolCallId> reverts;
  /// Commands that recorded source changes caused by this call.
  std::vector<CommandId> source_changes;
  /// Targets recorded when external execution was issued, when available.
  std::optional<std::vector<TargetBaseline>> issued_targets;
  /// Command that recorded verified effects for a previously unknown call, when present.
  std::optional<CommandId> reconciliation;
  /// Session sequence at which this call was registered. Older calls may have no recorded position.
  std::optional<Sequence> registered_sequence;
  /// Exact tool-authored message containing this call's published output, when visible to the reader.
  std::optional<MessageRef> output_message;
};

/// @brief Status of a collaboration request's reply expectation.
enum class RequestState {
  /// Waiting for an accepted final reply.
  AwaitingReply,
  /// A final reply has been recorded.
  Replied,
  /// The request could not obtain the required reply.
  Failed,
  /// The reply expectation was cancelled.
  Cancelled
};

/// @brief Authenticated principal and optional agent, Run and tool identity for policy checks.
struct ActionParty {
  /// @brief Role under which the principal is acting.
  enum class Kind {
    /// A user-bound operation.
    User,
    /// An operation bound to an agent Run.
    Agent,
    /// A host-side runtime operation.
    Host
  };
  /// Current actor role.
  Kind kind;
  /// Authenticated principal behind this operation.
  PrincipalId principal;
  /// Actor's session, when bound.
  std::optional<SessionId> session;
  /// Actor's Run, when bound.
  std::optional<RunId> run;
  /// Actor's logical tool call, when bound.
  std::optional<ToolCallId> call;
  /// Exact agent definition acting on behalf of the principal, when bound.
  std::optional<AgentRef> agent;
};

/// @brief How the session was created.
enum class SessionOriginKind {
  /// Ordinary new session.
  Created,
  /// Session with inherited history at a committed boundary.
  Forked,
  /// Ephemeral session scoped to selected sources.
  SideChat
};

/// @brief Visible provenance of a session, filtered by current permissions.
struct SessionOrigin {
  /// How this session originated.
  SessionOriginKind kind;
  /// Originating actor, when visible.
  std::optional<ActionParty> creator;
  /// Originating session for a fork or side chat, when visible.
  std::optional<SessionId> source_session;
  /// Original committed boundary for inherited history, when visible.
  std::optional<HistoryBoundary> fork_point;
  /// Exact visible source selections used to originate a side chat.
  std::vector<SourceSlice> side_chat_selection;
};

/// @brief Compact delivery and reply status shown in the receiving session.
struct CollaborationSummary {
  /// Stable collaboration message ID.
  CollaborationMessageId id;
  /// Actor that sent the message.
  ActionParty from;
  /// Receiving session.
  SessionId to;
  /// Original request ID if this message is a reply.
  std::optional<CollaborationMessageId> in_reply_to;
  /// Current delivery state.
  DeliveryState delivery;
  /// Reply expectation state, when the message expects a reply.
  std::optional<RequestState> request;
  /// Run started by the message, when known and visible.
  std::optional<RunId> target_run;
};

/// @brief Immutable, permission-filtered view of a conversation at a committed sequence.
/// Snapshot windows can be widened to include older history. Access changes can remove previously visible material.
struct SessionSnapshot {
  /// Current session identity, metadata and activity.
  SessionSummary summary;
  /// Visible provenance, when available.
  std::optional<SessionOrigin> origin;
  /// Latest committed sequence included in the snapshot.
  Sequence through_sequence;
  /// Visible transcript window in reading order.
  std::vector<Message> messages;
  /// Visible execution attempts.
  std::vector<RunSnapshot> runs;
  /// Visible logical tool calls and current effects.
  std::vector<ToolCallSnapshot> tools;
  /// Visible forms and approvals.
  std::vector<InteractionSnapshot> interactions;
  /// Visible scheduled inputs and their dispositions.
  std::vector<QueuedInput> queue;
  /// Visible incoming collaboration messages.
  std::vector<CollaborationSummary> inbox;
  /// Published artifact versions visible under the observer's current read permissions.
  std::vector<ArtifactRef> artifacts;
  /// Optional cursor for older messages.
  std::optional<PageCursor> older_history;
  /// Revision of current visibility and access, used to discard outdated async results.
  Revision access_revision = 0;
  /// Committed event cursor to pass to ObserveEventsAsync() after applying this snapshot.
  Sequence event_sequence = 0;
};

/// @brief Appends UTF-8 text to a previously published streaming message and block.
/// The completed or interrupted version is published as a full Message.
struct MessageDelta {
  /// Versioned message receiving the increment.
  MessageRef message;
  /// Target text block.
  BlockId block;
  /// New UTF-8 text bytes.
  std::string utf8;
};

/// @brief Records the publication of an exact artifact version.
struct ArtifactRegistered {
  /// Newly registered version visible to the observer.
  ArtifactRef artifact;
};

/// @brief Changes delivered by Conversation::ObserveEventsAsync().
using SessionEventBody = std::variant<SessionSummary, Message, MessageDelta,
    RunSnapshot, InteractionSnapshot, ToolCallSnapshot, QueuedInput,
    CollaborationSummary, ArtifactRegistered, SessionOrigin>;

/// @brief Committed change in a session's event stream.
/// A cross-session transaction uses the same commit ID in each affected session's stream.
struct SessionEvent {
  /// Session that owns the event.
  SessionId session;
  /// Sequence of the event in that session.
  Sequence sequence;
  /// Commit that recorded the event.
  CommitId commit;
  /// Associated command, when unambiguous and visible to the observer.
  std::optional<CommandId> command;
  /// Typed change payload.
  SessionEventBody body;
};

/// @brief Ordered events after the caller's cursor, batched for replay and live delivery.
struct SessionEventBatch {
  /// Last journal sequence covered. Permission filtering can leave gaps in events.
  Sequence through_sequence;
  /// Visible events in increasing sequence order.
  std::vector<SessionEvent> events;
};

/// @brief Version-checked transfer of an idle session to another agent.
struct TransferRequest {
  /// New agent to bind to the session.
  AgentId next;
  /// Expected SessionSummary::binding_revision.
  Revision expected_binding;
  /// Optional handoff text shown in the system transfer message.
  std::optional<std::string> handoff_summary;
  /// Compares the stored fields for equality.
  bool operator==(const TransferRequest&) const = default;
};

/// @brief One visible pending form or approval with its owning session.
struct PendingInteraction {
  /// Current session summary.
  SessionSummary session;
  /// Exact pending interaction version.
  InteractionSnapshot interaction;
};

/// @brief Page of visible pending interactions across sessions.
struct PendingInteractionPage {
  /// Revision of the pending interaction projection.
  Revision revision;
  /// Visible interactions in this page.
  std::vector<PendingInteraction> items;
  /// Cursor for the next page, when available.
  std::optional<PageCursor> next;
};

/// @brief Restricts the user-bound pending interaction directory.
struct InteractionFilter {
  /// Only sessions in this project, when set.
  std::optional<ProjectId> project;
  /// Only this session, when set.
  std::optional<SessionId> session;
  /// Maximum items to request per page.
  std::size_t page_size = 50;
};

/// @brief Lists and observes the authorized user's pending interactions.
class InteractionService {
public:
  /// @brief Returns a page of pending visible forms and approvals.
  /// @param filter Scope and page size for pending interactions.
  /// @param cursor Continuation from a previous page with the same filter. Empty starts at the first page.
  /// @return A page of visible interactions and its next cursor, if present.
  Async<PendingInteractionPage> ListPendingAsync(InteractionFilter filter, std::optional<PageCursor> cursor);

  /// @brief Observes the first page of pending visible forms and approvals.
  /// @param filter Scope and size for the first page.
  /// @param observer Callback receiving immutable pages or an observation error.
  /// @return A subscription that keeps this observation registered.
  Async<Subscription> ObservePendingAsync(InteractionFilter filter, Observer<PendingInteractionPage> observer);

private:
  explicit InteractionService(std::shared_ptr<detail::RuntimeState> state);
  std::shared_ptr<detail::RuntimeState> state_;
  friend class AgentRuntime;
};

/// @brief Open session handle for observing state and submitting versioned actions.
///
/// Open through SessionService::OpenAsync(). Keep returned subscriptions alive while observing.
/// Supply a new CommandId for a new action and reuse that ID only to recover the same action.
class Conversation {
public:
  /// @brief Returns the user-bound goal service for this session.
  SessionGoals Goals();

  /// @brief Returns the latest immutable projection for this handle, when available.
  ///
  /// Reads committed state and cached authorization synchronously. Newly published artifact versions
  /// appear after their read permissions have been checked.
  /// @return The latest accessible snapshot, or an empty pointer when unavailable.
  std::shared_ptr<const SessionSnapshot> Snapshot() const;

  /// @brief Validates a block version and dispatches its named registered action.
  /// @param command Stable ID for this action submission.
  /// @param action Block reference, action name and input payload.
  /// @return The action receipt.
  Async<CommandReceipt> DispatchContentActionAsync(CommandId command, CustomBlockAction action);

  /// @brief Submits a registered action on an exact artifact version and returns its typed receipt.
  /// @param command Stable ID for this action submission.
  /// @param action Exact artifact version, registered action name and payload.
  /// @return The receipt for the resulting Run.
  Async<Receipt<RunAccepted>> DispatchArtifactActionAsync(CommandId command, ArtifactAction action);

  /// @brief Resolves visible display data for an artifact version reachable from this conversation.
  /// @param artifact Reachable artifact ID and exact version in this conversation.
  /// @return Authorized preview content for the referenced version.
  Async<ArtifactPreview> PreviewArtifactAsync(ArtifactRef artifact);

  /// @brief Returns canonical text for one exact, authorized source selection.
  /// @param selection Exact source version and UTF-8 byte range or part to resolve.
  /// @return The canonical text for the source selection.
  Async<std::string> ReadSelectionAsync(SourceSlice selection);

  /// @brief Returns canonical texts in supplied order after checking every slice.
  /// @param selection One to 64 slices, with a combined limit of 1 MiB.
  /// @return All resolved text in input order, or an error without a partial result.
  Async<std::vector<std::string>> ReadSelectionsAsync(std::vector<SourceSlice> selection);

  /// @brief Subscribes to authorized snapshots with the default transcript window.
  /// @param observer Callback receiving immutable snapshots or an observation error.
  /// @return A subscription that keeps this observation registered.
  Async<Subscription> ObserveAsync(Observer<SessionSnapshot> observer);

  /// @brief Subscribes to authorized snapshots with an optional history window.
  /// @param observer Callback receiving immutable snapshots or an observation error.
  /// @param history Oldest requested message sequence. Empty uses the default window.
  /// @return A subscription that keeps this observation registered.
  Async<Subscription> ObserveAsync(Observer<SessionSnapshot> observer, std::optional<SessionHistoryWindow> history);

  /// @brief Replays committed events after a cursor and continues with live events.
  ///
  /// Batches reflect current read permissions. If there is a sequence gap, rebuild from an authorized
  /// snapshot and continue from its event cursor.
  /// @param after Last applied event sequence, typically SessionSnapshot::event_sequence.
  /// Pass zero to replay recorded events from the beginning.
  /// @param observer Receives ordered batches on the runtime task scope. An access revision change ends
  /// the subscription with CursorExpired. Loss of session read access ends observation with an access error.
  /// @return A subscription that keeps this event observation registered.
  Async<Subscription> ObserveEventsAsync(Sequence after, Observer<SessionEventBatch> observer);

  /// @brief Offers new input for an idle session and returns its acceptance receipt.
  /// @param command Stable ID of this logical submission.
  /// @param input User message and its skill, source and attachment references.
  /// @param start User overrides for the new Run. Empty values use the registered defaults.
  /// @return The accepted Run's receipt.
  Async<Receipt<RunAccepted>> SendAsync(CommandId command, SendInput input, UserRunOptions start = {});

  /// @brief Captures input and resolved start options for later execution.
  /// @param command Stable ID of this logical submission.
  /// @param input User message and references to retain in the queue.
  /// @param start User overrides resolved for this queue entry at acceptance.
  /// @return The receipt identifying the queued input.
  Async<Receipt<InputQueued>> QueueAsync(CommandId command, SendInput input, UserRunOptions start = {});

  /// @brief Reads editable user input from an eligible rejected queue entry.
  /// @param item Rejected queue item from this conversation.
  /// @return Editable recovered input and the currently accessible references.
  Async<QueuedDraft> ReadQueuedDraftAsync(QueueItemId item);

  /// @brief Reads editable user input from a steering command already recorded as not applied.
  /// @param command Original steering command whose input was recorded as unapplied.
  /// @return Editable text and references for the unapplied steer.
  Async<RecoveredDraft> ReadUnappliedSteerDraftAsync(CommandId command);

  /// @brief Submits supplemental input addressed to the exact active Run.
  /// @param command Stable ID of this logical submission.
  /// @param expected Active Run observed when editing or sending the supplement.
  /// @param input Text and references to offer to that Run.
  /// @return An acceptance receipt. Later application is reported separately in the Run state.
  Async<Receipt<RunSteered>> SteerAsync(CommandId command, RunId expected, SendInput input);

  /// @brief Withdraws a queue item before it starts.
  /// @param command Stable ID for withdrawal.
  /// @param item Queue item to withdraw.
  /// @return A receipt, or an error if the input can no longer be withdrawn.
  Async<CommandReceipt> WithdrawQueuedInputAsync(CommandId command, QueueItemId item);

  /// @brief Submits a form reply, dismissal or approval for the expected interaction version.
  /// @param command Stable ID of this interaction submission.
  /// @param reply Reply for the exact version observed by the caller.
  /// @return The receipt for the consumed interaction.
  Async<Receipt<InteractionConsumed>> SubmitAsync(CommandId command, InteractionReply reply);

  /// @brief Requests cancellation of the addressed Run.
  /// @param command Stable ID for this cancellation request.
  /// @param run ID of the Run to cancel in this session.
  /// @return Acceptance of the cancellation request. Observe the Run for its terminal state.
  Async<CommandReceipt> CancelAsync(CommandId command, RunId run);

  /// @brief Starts a new attempt from an eligible terminal root Run under current authorization.
  /// The retry uses the original model registration and requires that revision to remain available.
  /// Calls that may have written require reconciliation. A known applied write prevents replay as a retry.
  /// @param command Stable ID for the new attempt.
  /// @param source Eligible terminal root Run in this conversation.
  /// @return The accepted retry attempt's receipt.
  /// @note Reconcile an unknown effect before trying again.
  Async<Receipt<RunAccepted>> RetryAsync(CommandId command, RunId source);

  /// @brief Reuses eligible prior input with the history prefix before that input's original message.
  /// Verify any known changes before regenerating. Unknown issued effects still require reconciliation.
  /// @param command Stable ID for the new attempt.
  /// @param source Eligible root Run whose input and prior history are reused.
  /// @return The accepted regeneration attempt's receipt.
  Async<Receipt<RunAccepted>> RegenerateAsync(CommandId command, RunId source);

  /// @brief Rebinds an idle session to a different agent after checking the expected binding version.
  /// @param command Stable ID for this transfer request.
  /// @param request New agent and expected session binding version.
  /// @return The receipt for the updated binding.
  Async<CommandReceipt> TransferAsync(CommandId command, TransferRequest request);

private:
  Conversation(std::shared_ptr<detail::RuntimeState> state, SessionId id, PrincipalId principal);
  std::shared_ptr<detail::RuntimeState> state_;
  SessionId id_;
  PrincipalId principal_;
  friend class detail::RuntimeState;
};

} // namespace huxerui::agentkit
