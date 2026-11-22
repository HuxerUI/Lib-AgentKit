#pragma once

#include <huxerui/agentkit/runtime.h>

#include <algorithm>
#include <deque>
#include <set>
#include <span>

namespace huxerui::agentkit::detail {

AgentError Error(ErrorCode code, std::string message);

template<class T>
AgentResult<T> Fail(ErrorCode code, std::string message) {
  return AgentResult<T>::Failure(Error(code, std::move(message)));
}

bool Terminal(RunState state);
void SetObservedRunState(RunSnapshot& run, RunState state, TimePoint now);

std::optional<AgentError> RestartToolError(std::span<const ToolCallSnapshot> tools, const std::set<RunId>& runs,
                                          bool regenerate = false);

bool ShowsArtifact(const SessionSnapshot& snapshot, const ArtifactRef& ref);

bool ValidUtf8(std::string_view text);

bool ValidSkillRef(const SkillRef& ref);
inline constexpr std::string_view ActivateSkillTool = "agentkit_activate_skill";
inline constexpr std::string_view ReadSkillTool = "agentkit_read_skill";
bool SkillTool(std::string_view name);

bool ArtifactChangeTool(const ToolId& id);

std::optional<AgentError> CheckSkillOffer(const JsonValue& arguments, const std::vector<SkillSummary>& catalog);

std::vector<ModelToolSpec> SkillTools(const ModelTurnRequest& request);

std::string Title(std::string_view text);

std::string MakeId();

bool Supports(ModelCapabilities actual, ModelCapabilities required);

ToolCallState StateForOutcome(ToolOutcome outcome);

AgentResult<JsonValue> ParseJson(std::string_view text);

AgentResult<void> ValidateJson(const JsonValue& value, const JsonSchema& schema);

std::vector<SideChatConclusion> MessageConclusions(const Message& message);

enum class Operation {
  Create, Send, Queue, Steer, Withdraw, Submit, Cancel, Retry, Regenerate, Metadata, Transfer,
  SendMessage, Reply, StartRequest, WithdrawMessage, Fork, Goal, RenderedAction, ActivateSkill, Rework, Revert, AdoptCandidate,
  CreateComparison, ReconcileTool
};

struct ToolReconciliation {
  ToolCallId call;
  ToolOutcome outcome;
  std::vector<ArtifactRef> artifacts;
  std::vector<ResourceEffect> effects;
  std::optional<AgentError> reason;
  bool operator==(const ToolReconciliation&) const = default;
};

using RenderedActionOrigin = std::variant<CustomBlockAction, ArtifactAction>;

struct PreparedRenderedAction {
  std::variant<SendInput, SteerRequest, InteractionReply> request;
  std::optional<ArtifactAction> artifact;
  std::optional<ContentTypeRef> content_type;
};

struct CommandInput {
  Operation operation;
  SessionId session;
  RunId run;
  QueueItemId queue_item;
  std::optional<SessionOptions> options;
  std::optional<SendInput> input;
  UserRunOptions start;
  std::optional<InteractionReply> reply;
  std::optional<SessionMetadataChange> metadata;
  std::optional<TransferRequest> transfer;
  Revision expected = 0;
  std::optional<CollaborationMessageId> message;
  std::optional<SessionMessageRequest> delivery;
  std::optional<SessionReply> final_reply;
  std::optional<WorkRequest> work;
  std::optional<HistoryBoundary> fork;
  std::optional<GoalCommand> goal;
  std::optional<WorkspaceChoice> workspace_choice;
  std::optional<SideChatSeed> side_chat;
  std::optional<RenderedActionOrigin> rendered_action;
  std::optional<SkillActivation> skill;
  std::optional<ReworkRequest> rework;
  std::optional<RevertRequest> revert;
  std::optional<AdoptCandidateRequest> adoption;
  std::optional<ForkReviewRequest> comparison;
  std::optional<ToolReconciliation> reconciliation;
  bool operator==(const CommandInput&) const = default;
};

struct CandidateApprovalBinding {
  SessionId session;
  AgentRef agent;
  RunStartBinding start;
  std::vector<Capability> confirmation;
};

struct ComparisonApprovalBinding {
  ForkReviewId review;
  std::vector<CandidateApprovalBinding> candidates;
};

struct OperationPlan {
  std::vector<Capability> confirmation;
  std::optional<Revision> target_binding;
  std::optional<AgentRef> new_agent;
  std::optional<RunStartBinding> run_start;
  std::optional<GoalEditImpact> goal_edit;
  std::optional<PreparedRenderedAction> rendered_action;
  std::optional<ComparisonApprovalBinding> comparison;
};

struct RenderedExecution {
  CommandInput key;
  PreparedRenderedAction prepared;
  std::optional<OperationPlan> approved;
};

struct ArtifactChangePlan {
  RunId run;
  ToolCallId call;
  PreparedChange change;
  std::optional<Revision> source_revision;
};

struct RetiredComparisonWorkspace {
  WorkspaceCopyPlanRef copy;
  WorkspaceId workspace;
};

struct CommandRecord {
  CommandInput input;
  CommandProgress progress;
  CommandValue accepted;
  std::optional<OperationPlan> plan;
  std::optional<PendingApproval> original_approval;
  std::optional<WorkspaceId> retired_workspace;
  std::optional<ArtifactChangePlan> artifact_change;
  std::vector<RetiredComparisonWorkspace> retired_comparison_workspaces;
  std::optional<SendInput> rendered_steer_input;
};

struct PendingModelCall {
  ModelToolCall call;
  std::optional<ToolRef> tool;
  std::optional<AgentError> error;
  bool budget_reserved = false;
  std::optional<ToolCallId> local_call;
  std::optional<ModelToolResult> result;
  std::optional<AgentError> terminal_error;
  std::optional<CommandId> delegation;
};

struct ModelSkillResourceUse {
  SkillRef skill;
  std::string path;
  std::uint64_t bytes;
};

struct FormRecord {
  FormRequest request;
  CommitStamp stamp;
  InputRequested result;
};

struct HostToolRecord {
  ToolRequest request;
  ToolRef ref;
  CommitStamp stamp;
  ToolCallId call;
  std::optional<ToolResult> result;
  std::optional<AgentError> failure;
};

struct StoredArtifact {
  ArtifactRecord record;
  std::string handle;
};

struct StoredCollaboration {
  CollaborationRecord record;
  bool auto_start = false;
  std::optional<WorkScope> scope;
  std::optional<MessageId> steer_message;
  std::optional<GoalRef> goal;
};

struct HostWorkflow {
  RunHandler run;
  RunRestore restore;
};

struct PendingToolReview {
  std::stop_source stop;
  TaskHandle worker;
  TaskHandle timer;
  std::optional<CallbackCompletion<std::optional<ToolReviewResult>>> completion;

  ~PendingToolReview() {
    stop.request_stop();
    worker.Cancel();
    timer.Cancel();
  }

  void Interrupt() {
    stop.request_stop();
    if (completion) completion->Complete(std::nullopt);
  }
};

struct ToolPlanState {
  std::optional<PreparedToolCall> prepared;
  std::optional<InteractionId> approval;
  std::optional<Revision> source_revision;
};

struct ToolAttempt {
  ToolPlanState plan;
  std::optional<ToolEffect> executing;
  bool issued = false;
  bool parallel = false;
  std::weak_ptr<PendingToolReview> review;

  void InterruptReview() {
    if (auto pending = review.lock()) pending->Interrupt();
  }
};

struct ToolTurnSchedule;

struct HostedModelStep {
  std::optional<std::size_t> input_index;
  std::vector<ToolId> allowed_tools;
  std::map<ToolCallId, std::vector<ArtifactRef>> published;
};

inline const ToolId DelegationToolId{"agentkit_delegate"};

struct DelegatedRun {
  Budget budget;
  std::optional<JsonSchema> expected_result;
  bool started = false;
};

struct RunExecution {
  RunSnapshot view;
  PrincipalId principal;
  SendInput input;
  std::optional<ArtifactAction> artifact_action;
  std::vector<ModelItem> items;
  bool separate_inherited_history = true;
  std::vector<std::pair<MessageId, SendInput>> pending_steers;
  std::optional<ModelContinuation> continuation;
  std::deque<PendingModelCall> pending_calls;
  bool model_boundary = false;
  std::optional<TimePoint> deadline;
  std::stop_source stop;
  std::shared_ptr<ModelTurnStream> stream;
  std::optional<ModelStopOutcome> model_stop;
  std::optional<InteractionId> waiting;
  std::set<InteractionId> waiting_for_tool_approvals;
  std::optional<SkillActivation> activating_skill;
  std::vector<CallbackCompletion<AgentResult<std::vector<SkillResource>>>> skill_waiters;
  std::vector<ModelSkillResourceUse> model_skill_resources;
  std::map<ToolCallId, ToolAttempt> tool_attempts;
  std::shared_ptr<ToolTurnSchedule> tool_turn;
  std::optional<RunCheckpoint> checkpoint;
  std::map<CommandId, FormRecord> form_requests;
  std::map<CommandId, HostToolRecord> tool_requests;
  std::optional<ToolCallId> dispatched_host_tool;
  std::optional<CallbackCompletion<AgentResult<ToolResult>>> host_tool_waiter;
  TaskHandle host_tool_task;
  std::vector<MessageId> observed_steers;
  bool restoring = false;
  bool suspended = false;
  bool host_model_busy = false;
  std::optional<HostedModelStep> host_model_step;
  std::optional<MessageId> streaming;
  TaskHandle task;
  TaskHandle timer;
  std::optional<DelegatedRun> delegate;
  std::map<CommandId, std::vector<RunId>> delegations;
  std::optional<CommandId> waiting_for_delegates;
  std::optional<CallbackCompletion<bool>> delegate_waiter;
  bool delegate_dispatched = false;
  bool finishing = false;

  bool UsingHostedModelTools() const {
    return host_model_busy && host_model_step && std::ranges::any_of(host_model_step->allowed_tools,
        [](const ToolId& id) { return !SkillTool(id.value); });
  }

  bool ExecutingTool() const {
    return std::ranges::any_of(tool_attempts, [](const auto& entry) { return entry.second.executing.has_value(); });
  }

  void InterruptToolReviews() {
    for (auto& [call, attempt] : tool_attempts) attempt.InterruptReview();
  }
};

struct GoalVerification {
  CommandId command;
  ActionParty actor;
  Revision revision;
  std::map<ProjectTaskId, Revision> dependencies;
};

struct StoredGoal {
  GoalSnapshot view;
  PrincipalId owner;
  std::optional<TimePoint> enabled_at;
  std::map<Revision, GoalDefinition> definitions;
  std::map<Revision, GoalSnapshot> retired;
  std::optional<GoalVerification> verification;
  std::stop_source verification_stop;
};

struct PreparedGoalEdit {
  SessionId session;
  ActionParty actor;
  Revision expected;
  GoalDefinition definition;
  GoalEditImpact impact;
  TimePoint expires;
};

struct ForkBranchBinding {
  ForkReviewId review;
  CandidateId candidate;
  RunId run;
  Budget budget;
  TimePoint started;
  bool isolated = false;
};

struct SessionData {
  SessionSummary summary;
  SessionRetention retention;
  SessionOrigin origin;
  Sequence sequence = 0;
  Sequence event_sequence = 0;
  std::vector<Message> messages;
  std::vector<RunId> runs;
  std::vector<ToolCallSnapshot> tools;
  std::vector<StoredArtifact> artifacts;
  std::map<CollaborationMessageId, StoredCollaboration> collaboration;
  std::vector<InteractionSnapshot> interactions;
  std::vector<QueuedInput> queue;
  std::map<QueueItemId, PrincipalId> queue_owners;
  std::map<InteractionId, std::optional<JsonValue>> replies;
  std::map<InteractionId, CallbackCompletion<InteractionOutcome>> waiters;
  std::map<InteractionId, TaskHandle> interaction_timers;
  std::optional<GoalId> current_goal;
  std::map<GoalId, StoredGoal> goals;
  std::optional<ForkBranchBinding> fork_branch;
  bool consuming = false;
};

struct PreparedForkCandidate {
  CommandId comparison;
  bool inspect = false;
  std::optional<SessionId> reserved_session;
  std::vector<Capability> confirmation;
  SessionData session;
  RunStartBinding start;
};

struct StoredRun {
  RunSnapshot view;
  PrincipalId principal;
  SendInput input;
  std::optional<TimePoint> deadline;
  std::optional<PreparedToolCall> prepared_tool;
  std::optional<InteractionId> tool_approval;
  std::map<ToolCallId, ToolPlanState> tool_plans;
  std::vector<ModelItem> items;
  bool separate_inherited_history = false;
  std::vector<std::pair<MessageId, SendInput>> pending_steers;
  std::optional<ModelContinuation> continuation;
  bool continuation_missing = false;
  std::deque<PendingModelCall> pending_calls;
  bool model_boundary = false;
  std::optional<RunCheckpoint> checkpoint;
  std::map<CommandId, FormRecord> form_requests;
  std::map<CommandId, HostToolRecord> tool_requests;
  std::vector<ModelSkillResourceUse> model_skill_resources;
  std::optional<std::size_t> host_skill_input;
  std::optional<HostedModelStep> host_model_step;
  std::optional<ArtifactAction> artifact_action;
  std::optional<DelegatedRun> delegate;
  std::map<CommandId, std::vector<RunId>> delegations;
};

struct StoredCommand {
  PrincipalId principal;
  CommandRecord record;
};

struct StoredSession {
  SessionData data;
  std::vector<StoredRun> runs;
  std::vector<StoredCommand> commands;
};

struct SaveTicket {
  std::optional<AgentResult<void>> result;
  std::vector<CallbackCompletion<AgentResult<void>>> waiters;
};

struct SavedSession {
  Revision revision;
  StoredSession image;
};

using ArtifactVersions = std::set<std::pair<ArtifactId, Revision>>;
void CollectArtifactRefs(ArtifactVersions& refs, const ContentBlock& block);

void FilterArtifactRefs(std::vector<ArtifactRef>& refs, const ArtifactVersions& allowed);

bool VisibleArtifactBlock(const ContentBlock& block, const ArtifactVersions& allowed);

bool InheritedArtifact(const SessionData& session, const ArtifactRef& artifact);

bool FilterHistoryArtifacts(Message& message, const ArtifactVersions& permitted, bool inherited);

struct CloseForkReview {
  ForkReviewId review;
  Revision expected;
  bool operator==(const CloseForkReview&) const = default;
};

using ProjectCommand = std::variant<TaskCommand, SharedContextCommand, SourceChange, ForkReviewRequest, CloseForkReview>;

struct ProjectCommandRecord {
  PrincipalId principal;
  std::optional<RunId> actor;
  ProjectCommand input;
  CommandProgress progress;
  std::optional<SourceChange> source_change;

  const SourceChange* SourceChangeResult() const {
    return source_change ? &*source_change : std::get_if<SourceChange>(&input);
  }
};

struct PendingSourceChange {
  CommandId command;
  PrincipalId principal;
  RunId run;
  SourceChange change;
};

struct StoredProjectTask {
  ProjectTaskSnapshot snapshot;
  std::map<AttemptId, TaskOutcome> attempts;
};

struct ForkAdoptionBinding {
  SessionId session;
  PrincipalId principal;
  CommandId command;
};

struct ProjectData {
  ProjectId id;
  Revision revision = 0;
  std::map<ProjectTaskId, StoredProjectTask> tasks;
  std::map<SharedContextId, std::vector<SharedContextItem>> context;
  std::vector<ProjectCommandRecord> commands;
  std::map<ForkReviewId, ForkReviewSnapshot> fork_reviews;
  std::map<ForkReviewId, ForkAdoptionBinding> fork_adoptions;
};

struct SavedProject {
  Revision revision;
  ProjectData image;
};

struct StoredSessionEvent {
  SessionEvent event;
  std::optional<PrincipalId> command_owner;
  std::vector<PrincipalId> pending_recipients;
};

struct SaveChange {
  CommitId id;
  std::vector<StoredSession> images;
  bool order;
  std::shared_ptr<SaveTicket> ticket;
  std::vector<ProjectData> projects;
  std::vector<StoredSessionEvent> events;
};

struct ActiveObserver {
  std::weak_ptr<bool> active;
  std::function<Task<void>()> deliver;
  std::function<void()> close;
};

struct RegisteredTool {
  ToolRef ref;
  std::shared_ptr<const ToolDefinition> definition;
};

struct RegisteredToolCatalog {
  Revision revision = 0;
  std::map<ToolId, std::shared_ptr<const ToolDefinition>> tools;
  std::map<ToolId, Revision> versions;
};

class RuntimeState : public std::enable_shared_from_this<RuntimeState> {
public:
  explicit RuntimeState(RuntimeOptions options);

  RuntimeOptions options;
  enum class Stage { Registering, Starting, Started, Closed } stage = Stage::Registering;
  std::map<ModelId, std::map<Revision, ModelRegistration>> models;
  const ModelRegistration* ResolveModel(const ModelProviderRef& ref) const;

  const ModelRegistration* LatestModel(const ModelId& id) const;

  std::map<ToolId, std::shared_ptr<const ToolDefinition>> tools;
  std::map<ToolCatalogId, RegisteredToolCatalog> tool_catalogs;
  bool ReservedToolId(const ToolId& id) const;

  std::shared_ptr<const ToolDefinition> ResolveTool(const ToolRef& ref) const;

  std::optional<RegisteredTool> FindAgentTool(const AgentDefinition& agent, const ToolId& id) const;

  std::vector<RegisteredTool> CaptureAgentTools(const AgentDefinition& agent) const;

  Async<void> ReplaceToolCatalog(ToolCatalogId catalog, ToolCatalogSnapshot snapshot);
  std::map<SkillSourceId, std::shared_ptr<SkillSource>> skill_sources;
  std::map<AgentId, AgentDefinition> agents;
  std::map<std::pair<std::string, Revision>, ContentComponentRegistration> content_components;
  std::map<std::pair<std::string, std::string>, ArtifactActionRegistration> artifact_actions;
  Async<void> ValidateToolBlocks(const SessionData& session, const RunExecution& run, std::vector<ContentBlock>& blocks);
  std::map<AgentId, HostWorkflow> workflows;
  std::map<ContextProviderId, ContextProvider> context;
  std::map<SessionId, SessionData> sessions;
  std::map<RunId, RunExecution> runs;
  std::map<std::pair<PrincipalId, CommandId>, CommandRecord> commands;
  std::set<SessionId> allocating_workspaces;
  std::set<std::pair<PrincipalId, CommandId>> executing_operations;
  std::map<std::pair<PrincipalId, CommandId>, CommandInput> preparing_actions;
  std::map<std::pair<PrincipalId, CommandId>, ForkReviewRequest> preparing_fork_reviews;
  std::map<std::string, PreparedGoalEdit> goal_edits;
  std::set<std::pair<PrincipalId, SessionId>> visible_sessions;
  std::map<std::pair<PrincipalId, SessionId>, ArtifactVersions> visible_history_artifacts;
  std::map<std::pair<PrincipalId, SessionId>, Sequence> checked_session_artifacts;
  std::set<std::pair<PrincipalId, SessionId>> checking_session_artifacts;
  std::map<std::pair<PrincipalId, SessionId>, ArtifactVersions> visible_approval_artifacts;
  std::map<std::pair<PrincipalId, SessionId>, std::vector<SourceSlice>> visible_selections;
  Revision access_revision = 0;
  Revision revision = 0;
  Revision order_revision = 0;
  std::vector<ActiveObserver> observers;
  bool notifying = false;
  bool dirty = false;
  Revision store_revision = 0;
  std::map<SessionId, SavedSession> saved_sessions;
  std::map<SessionId, StoredSession> ephemeral_sessions;
  std::map<SessionId, std::vector<StoredSessionEvent>> session_events;
  std::map<ProjectId, ProjectData> projects;
  std::map<SessionId, std::vector<PendingSourceChange>> pending_source_changes;
  std::map<ProjectId, SavedProject> saved_projects;
  std::deque<SaveChange> saves;
  std::optional<AgentError> storage_error;
  bool saving = false;

  TimePoint Now() const;

  AgentResult<PrincipalId> User() const;

  static PolicyRequest Request(const PrincipalId& user, Capability capability,
                               const SessionData* session = nullptr, const RunExecution* run = nullptr);

  Async<PolicyDecision> Decide(PolicyRequest request);

  Async<void> Require(PolicyRequest request);

  void ReportException(std::exception_ptr error) const noexcept;

  void Changed(bool order = false);

  void ScheduleObservers();

  Task<void> Notify();

  Async<void> LoadStored();

  StoredSession Capture(const SessionData& session) const;

  bool CanRestore(const StoredRun& run, const SessionData& session) const;

  std::shared_ptr<SaveTicket> Persist(const SessionData& session, bool order = false, CommitId id = {},
      const SessionData* other = nullptr, const ProjectData* project = nullptr,
      std::span<const SessionData* const> additional = {});

  std::shared_ptr<SaveTicket> PersistProject(const ProjectData& project, CommitId id, const SessionData* session = nullptr);

  std::shared_ptr<SaveTicket> EnqueueSave(SaveChange change);

  static Async<void> WhenSaved(std::shared_ptr<SaveTicket> ticket);

  Task<void> WritePending();

  void PublishSessions(SaveChange& change);

  const SessionData* Published(const SessionData& session) const;

  AgentResult<const CommandRecord*> Find(const PrincipalId& user, const CommandId& command,
                                         const CommandInput& input) const;

  void Accept(const PrincipalId& user, const CommandId& command, CommandInput input,
              const SessionData& session, CommandValue value, CommandState status = CommandState::Applied);

  template<class T>
  AgentResult<Receipt<T>> ReceiptFor(const PrincipalId& user, const CommandId& command) const {
    const auto& record = commands.at({user, command});
    if (record.original_approval) {
      return AgentResult<Receipt<T>>::Success({record.progress.accepted, *record.original_approval});
    }
    if (record.progress.approval && std::holds_alternative<std::monostate>(record.accepted)) {
      return AgentResult<Receipt<T>>::Success({record.progress.accepted, *record.progress.approval});
    }
    return AgentResult<Receipt<T>>::Success({record.progress.accepted, std::get<T>(record.accepted)});
  }

  template<class T>
  Async<Receipt<T>> SaveReceipt(const SessionData& session, PrincipalId user, CommandId command, bool order = false) {
    auto receipt = ReceiptFor<T>(user, command);
    auto committed = co_await WhenSaved(Persist(session, order, receipt.Value().stamp.commit));
    if (!committed.Succeeded()) co_return AgentResult<Receipt<T>>::Failure(committed.Error());
    co_return receipt;
  }

  template<class T>
  Async<Receipt<T>> ReplayReceipt(AgentResult<Receipt<T>> receipt) {
    if (!receipt.Succeeded()) co_return receipt;
    const auto& stamp = receipt.Value().stamp;
    if (const auto pending = std::ranges::find(saves, stamp.commit, &SaveChange::id); pending != saves.end()) {
      auto committed = co_await WhenSaved(pending->ticket);
      if (!committed.Succeeded()) co_return AgentResult<Receipt<T>>::Failure(committed.Error());
    } else if (storage_error && stamp.project && !stamp.session) {
      auto principal = User();
      if (!principal.Succeeded()) co_return AgentResult<Receipt<T>>::Failure(principal.Error());
      if (!FindProjectCommand(principal.Value(), stamp.command, true)) co_return AgentResult<Receipt<T>>::Failure(*storage_error);
    } else if (storage_error && stamp.session && sessions.at(*stamp.session).retention == SessionRetention::Stored) {
      const auto published = saved_sessions.find(*stamp.session);
      const bool accepted = published != saved_sessions.end() &&
          (std::ranges::any_of(published->second.image.commands, [&](const auto& command) {
            return command.record.progress.accepted.commit == stamp.commit;
          }) || std::ranges::any_of(published->second.image.runs, [&](const auto& run) {
            return std::ranges::any_of(run.form_requests, [&](const auto& form) {
              return form.second.stamp.commit == stamp.commit;
            }) || std::ranges::any_of(run.tool_requests, [&](const auto& tool) {
              return tool.second.stamp.commit == stamp.commit;
            });
          }));
      if (!accepted) {
        co_return AgentResult<Receipt<T>>::Failure(*storage_error);
      }
    }
    co_return receipt;
  }

  void Resolve(CommandRecord& record, SessionData& session, CommandState status,
               std::optional<AgentError> failure = {});

  Async<void> CheckOperation(PolicyRequest request, std::vector<Capability>& confirmation,
                             const std::optional<OperationPlan>& approved);

  Async<void> StageOperation(PrincipalId principal, CommandId command, CommandInput input, SessionId home,
                             OperationPlan plan, ApprovalPreview preview, std::optional<TimePoint> expires);

  void DecideOperation(SessionData& session, const InteractionSnapshot& interaction, InteractionState decision);

  void ScheduleOperation(PrincipalId principal, CommandId command, std::optional<CallbackCompletion<void>> completed = {});

  Task<void> ExecuteOperation(PrincipalId principal, CommandId command);

  void ScheduleOperationExpiry(SessionId session, InteractionId interaction, TimePoint deadline);

  Task<void> ExpireOperation(SessionId session, InteractionId interaction, TimePoint deadline);

  void ResumeOperations();

  Async<CommandReceipt> DispatchRenderedAction(SessionId session, PrincipalId principal, CommandId command,
                                               RenderedActionOrigin action);

  Async<CommandReceipt> ApplyRenderedAction(PrincipalId principal, CommandId command,
                                            std::shared_ptr<const RenderedExecution> execution);

  Async<bool> CheckRenderedCommit(const RenderedExecution& execution, const PrincipalId& principal,
                                  const CommandId& command, PolicyRequest request,
                                  std::optional<RunStartBinding> start = {});

  Async<std::shared_ptr<const CustomBlock>> ResolveRenderedBlock(SessionId session, PrincipalId user,
                                                                 CustomBlockRef target);

  Async<StoredArtifact> ReadActionArtifact(SessionId session, PrincipalId user, ArtifactRef artifact);

  static Async<std::string> ReadConversationArtifactName(std::shared_ptr<Conversation> conversation, ArtifactRef artifact);

  Async<StoredArtifact> ReadApprovalArtifact(SessionId session, PrincipalId user, ArtifactRef artifact);

  Async<ActionParty> GoalActor(SessionId id, PrincipalId principal, std::optional<RunId> actor,
                               Capability capability, std::optional<GoalId> goal = {});

  Async<std::optional<GoalSnapshot>> ReadGoal(SessionId id, PrincipalId principal, std::optional<RunId> actor);

  Async<GoalDefinition> ReadGoalDefinition(SessionId id, PrincipalId principal, std::optional<RunId> actor, GoalRef ref);

  Async<GoalEditPreview> PreviewGoalEdit(SessionId id, PrincipalId principal, std::optional<RunId> actor,
                                        GoalId goal, Revision expected, GoalDefinition definition);

  Async<Receipt<GoalUpdated>> UpdateGoal(SessionId id, PrincipalId principal, std::optional<RunId> actor,
      CommandId command, GoalCommand input, std::optional<OperationPlan> approved = {});

  void ClearGoalVerification(SessionData& session, StoredGoal& goal);

  void UpdateGoalReview(SessionData& session, const InteractionSnapshot& interaction, InteractionState decision);

  Async<Receipt<InteractionConsumed>> ApproveGoal(SessionId id, GoalId goal, PrincipalId user, CommandId command,
                                                   InteractionReply reply);

  Async<std::optional<Receipt<InteractionConsumed>>> CheckGoalCompletion(SessionId id, GoalId goal,
      Revision verification, bool approved, std::optional<CommandId> decision = {}, std::optional<InteractionReply> reply = {});

  Task<void> VerifyGoal(SessionId id, GoalId goal, Revision verification, bool approved = false);

  void ScheduleGoalVerification(SessionId id, const StoredGoal& goal);

  Task<void> StartGoalRun(SessionId id);

  const SessionData* GoalHome(const GoalId& goal) const;

  std::optional<GoalRef> ActorGoal(const ActionParty& actor) const;

  AgentResult<void> CheckGoalDispatch(const SessionData& target, const std::optional<GoalRef>& goal) const;

  Async<void> AuthorizeGoalDispatch(const SessionData& target, ActionParty actor, std::optional<GoalRef> goal);

  SessionData* AttachGoalRun(RunExecution& run, std::optional<GoalRef> goal);

  SessionData* FinishGoalRun(const RunSnapshot& run);

  ProjectData* ReleaseGoalClaims(const SessionData& home, const GoalRef& ref);

  std::vector<const SessionData*> WithdrawGoalWork(const GoalRef& ref);

  bool RetireWorkWithGoal(const RunExecution& run) const;

  Budget RunBudget(const RunExecution& run) const;

  void ConstrainDelegationBudget(const RunExecution& run, Budget& budget) const;

  Async<std::optional<ModelContent>> GoalContext(const RunExecution& run);

  template<class T>
  Async<Receipt<T>> FinishOperation(SessionData& target, const PrincipalId& principal, const CommandId& command,
      const CommandInput& input, T result, bool order, bool approved, const ProjectData* project = nullptr,
      std::vector<const SessionData*> additional = {}, CommandState status = CommandState::Applied,
      std::optional<SendInput> rendered_steer_input = {}) {
    if (!approved) {
      Accept(principal, command, input, target, result, status);
      if (rendered_steer_input) commands.at({principal, command}).rendered_steer_input = std::move(rendered_steer_input);
      if (!project && additional.empty()) co_return co_await SaveReceipt<T>(target, principal, command, order);
      auto& stamp = commands.at({principal, command}).progress.accepted;
      if (project) {
        stamp.project = project->id;
        stamp.project_revision = project->revision;
      }
      auto receipt = ReceiptFor<T>(principal, command);
      auto saved = co_await WhenSaved(Persist(target, order, receipt.Value().stamp.commit, nullptr, project, additional));
      if (!saved.Succeeded()) co_return AgentResult<Receipt<T>>::Failure(saved.Error());
      co_return receipt;
    }
    auto& record = commands.at({principal, command});
    if (rendered_steer_input) record.rendered_steer_input = std::move(rendered_steer_input);
    auto& home = sessions.at(*record.progress.accepted.session);
    ++home.sequence;
    record.progress.result = result;
    if (status == CommandState::Scheduled) {
      record.original_approval = record.progress.approval;
      record.accepted = result;
    }
    Resolve(record, home, status);
    if (project) {
      record.progress.resolution->project = project->id;
      record.progress.resolution->project_revision = project->revision;
    }
    auto stamp = *record.progress.resolution;
    auto saved = co_await WhenSaved(Persist(home, order, stamp.commit, &target, project, additional));
    if (!saved.Succeeded()) co_return AgentResult<Receipt<T>>::Failure(saved.Error());
    co_return AgentResult<Receipt<T>>::Success({std::move(stamp), std::move(result)});
  }

  std::shared_ptr<const SessionSnapshot> Snapshot(const SessionData& session, const PrincipalId& user,
      std::optional<SessionHistoryWindow> history = {}) const;

  Async<std::shared_ptr<const SessionSnapshot>> ReadSession(SessionId id, PrincipalId bound,
      std::optional<SessionHistoryWindow> history = {});

  void ScheduleSessionArtifactCheck(const SessionId& id, const PrincipalId& user);

  Async<std::optional<SessionEventBatch>> ReadSessionEvents(SessionId id, PrincipalId user, Sequence after,
      std::optional<Revision> previous_access);

  Async<std::set<SessionId>> VisibleHistorySources(const SessionData& image, ActionParty actor);

  Async<ArtifactVersions> VisibleHistoryArtifacts(const SessionData& image, ActionParty actor,
      const std::set<SessionId>& history_sources, ArtifactVersions related = {});

  Async<std::vector<ModelContent>> AssembleInheritedHistory(const RunExecution& run);

  Async<std::string> ResolveSelection(SessionId session, ActionParty actor, SourceSlice slice,
                                     std::optional<ModelId> model = {});

  Async<std::vector<SourceSlice>> VisibleSelections(SessionId session, PrincipalId user);

  Async<void> PrepareSelectedContext(const SessionData& session, const RunExecution& run, ModelTurnRequest& request);

  Async<void> ValidateUserSources(const SessionData& session, PrincipalId user, std::vector<SourceSlice> sources, ModelId model);

  AgentResult<const QueuedInput*> RejectedInput(const SessionId& session, const PrincipalId& user, const QueueItemId& item) const;

  Async<QueuedDraft> ReadQueuedDraft(SessionId session, PrincipalId user, QueueItemId item);

  AgentResult<std::pair<const SendInput*, const Message*>> UnappliedSteer(
      const SessionId& session, const PrincipalId& user, const CommandId& command) const;

  Async<RecoveredDraft> ReadUnappliedSteerDraft(SessionId session, PrincipalId user, CommandId command);

  Async<RecoveredDraft> RecoverDraft(SessionId session, PrincipalId user, SendInput input,
      std::vector<SideChatConclusion> conclusions, std::variant<QueueItemId, CommandId> origin);

  Async<std::vector<SideChatConclusion>> ResolveConclusions(SessionId session, PrincipalId user,
      const SendInput& input, ModelId model);

  Async<void> CheckConclusions(SessionId session, ActionParty actor, std::vector<SideChatConclusion> conclusions,
                              std::optional<ModelId> model);

  Async<SessionPage> List(SessionFilter filter, std::optional<PageCursor> cursor, std::optional<RunId> actor = {});

  Async<PendingInteractionPage> ListPending(InteractionFilter filter, std::optional<PageCursor> cursor);

  Async<CommandProgress> ReadCommand(CommandLookup lookup, std::optional<RunId> actor = {});

  Async<HistoryPage> ReadHistory(SessionId id, HistoryQuery query, std::optional<RunId> actor = {});

  Async<std::shared_ptr<Conversation>> Open(SessionId id);

  template<class T, class Reader>
  static Async<Subscription> Observe(std::shared_ptr<RuntimeState> self, Observer<T> observer, Reader reader,
                                      bool retry_expired = true) {
    if (!observer) co_return Fail<Subscription>(ErrorCode::InvalidArgument, "An observer is required");
    auto principal = self->User();
    if (!principal.Succeeded()) co_return AgentResult<Subscription>::Failure(principal.Error());
    auto active = std::make_shared<bool>(true);
    auto weak = std::weak_ptr<bool>(active);
    auto reader_for_observer = [weak, reader = std::move(reader), observer, self = std::weak_ptr<RuntimeState>(self),
                                principal = principal.Value(), retry_expired]() mutable -> Task<void> {
      auto flag = weak.lock();
      auto state = self.lock();
      if (!flag || !*flag || !state) co_return;
      try {
        auto user = state->User();
        auto result = user.Succeeded() && user.Value() == principal
            ? co_await reader(*state)
            : Fail<T>(ErrorCode::PermissionDenied, "The observing principal changed");
        if (!*flag) co_return;
        if (result.Succeeded()) {
          observer(AgentResult<std::shared_ptr<const T>>::Success(
              std::make_shared<const T>(std::move(result).Value())));
        } else if (retry_expired && result.Error().code == ErrorCode::CursorExpired) {
          state->ScheduleObservers();
        } else {
          *flag = false;
          observer(AgentResult<std::shared_ptr<const T>>::Failure(result.Error()));
        }
      } catch (...) {
        *flag = false;
        state->ReportException(std::current_exception());
      }
    };
    self->observers.push_back({
        weak, std::move(reader_for_observer),
        [weak, observer = std::move(observer)] {
          if (auto flag = weak.lock(); flag && std::exchange(*flag, false)) {
            observer(Fail<std::shared_ptr<const T>>(ErrorCode::RuntimeClosed, "Agent runtime closed"));
          }
        }});
    self->ScheduleObservers();
    co_return AgentResult<Subscription>::Success(Subscription(std::move(active)));
  }

  static Async<Receipt<SessionCreated>> Create(std::shared_ptr<RuntimeState> self, CommandId command,
                                               CreateSessionRequest request, std::optional<RunId> actor = {},
                                               std::optional<HistoryBoundary> fork = {},
                                               std::optional<OperationPlan> approved = {},
                                               std::optional<WorkspaceChoice> copy = {},
                                               PreparedForkCandidate* candidate = nullptr);

  Async<void> AbandonWorkspaceFork(CommandId command, ForkSessionRequest request, WorkspaceId allocated);

  bool WorkspaceRetired(const WorkspaceId& workspace) const;

  bool WorkspaceCopyRetired(const WorkspaceCopyPlanRef& copy) const;

  static Async<Receipt<RunAccepted>> Send(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
      CommandId command, SendInput input, UserRunOptions start, std::shared_ptr<const RenderedExecution> rendered = {});

  static Async<Receipt<InputQueued>> Queue(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
                                           CommandId command, SendInput input, UserRunOptions start);

  static Async<Receipt<RunSteered>> Steer(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
      CommandId command, RunId expected, SendInput input, std::shared_ptr<const RenderedExecution> rendered = {});

  static Async<Receipt<InteractionConsumed>> Submit(std::shared_ptr<RuntimeState> self, SessionId id,
      PrincipalId bound, CommandId command, InteractionReply reply, std::shared_ptr<const RenderedExecution> rendered = {});

  static Async<CommandReceipt> Cancel(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
                                      CommandId command, RunId run);

  static Async<CommandReceipt> Withdraw(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
                                        CommandId command, QueueItemId item);

  static Async<Receipt<RunAccepted>> Restart(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
      CommandId command, RunId source, bool regenerate);

  static Async<CommandReceipt> UpdateMetadata(std::shared_ptr<RuntimeState> self, CommandId command,
      SessionId id, Revision expected, SessionMetadataChange change);

  static Async<CommandReceipt> Transfer(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId bound,
      CommandId command, TransferRequest request);

  Async<void> WithSession(SessionId id, PrincipalId bound, Capability capability);

  Async<RunStartBinding> ResolveStart(const SessionData& session, const PrincipalId& user, UserRunOptions start);

  AgentResult<void> CheckInput(const SendInput& input, bool user_selection = false, bool initial_skills = false) const;

  Async<std::vector<StoredArtifact>> ResolveModelAttachments(const SessionData& session, ActionParty actor,
      std::vector<ArtifactRef> refs, ModelProviderRef model);

  Async<void> CheckModelAttachments(const SessionData& session, PrincipalId user,
      std::vector<ArtifactRef> refs, ModelProviderRef model);

  Async<std::vector<StoredArtifact>> PrepareInputAttachments(const SessionData& session, PrincipalId user,
      const SendInput& input, ModelProviderRef model);

  AgentResult<void> CommitInputAttachments(SessionData& session, SendInput& input, std::vector<StoredArtifact> prepared);

  Async<void> PrepareModelAttachments(const SessionData& session, const RunExecution& run,
      ModelTurnRequest& request, std::uint64_t byte_limit);

  RunExecution& RecordRun(SessionData& session, PrincipalId user, SendInput input, RunStartBinding start,
                          RunTrigger trigger, std::optional<RunId> source = {},
                          std::vector<SideChatConclusion> conclusions = {}, std::optional<RunId> reserved = {});

  void AppendInput(SessionData& session, const RunId& run, const SendInput& input,
                   std::optional<MessageId> message = {}, MessageAuthor author = MessageAuthor::User,
                   std::vector<SideChatConclusion> conclusions = {});

  void FirstTitle(SessionData& session, const SendInput& input);

  Task<void> OptimizeTitle(SessionId id, std::string text, Revision expected);

  void Launch(RunExecution& run);

  void UpdateRunState(SessionData& session, RunExecution& run, RunState state);

  bool HasActiveDelegates(const RunExecution& run) const;

  void StopDelegates(const RunExecution& run);

  void ScheduleDelegates(SessionData& session);

  Async<void> CheckDelegation(const RunExecution& parent, const DelegateRequest& request);

  Async<std::vector<DelegateOutcome>> Delegate(RunId parent, CommandId command, std::vector<DelegateRequest> requests);

  bool AllowsRunTool(const RunExecution& run, const ToolRef& tool) const;

  Async<std::vector<ResourceAccess>> ResolveRunToolResources(const RunExecution& run, ToolInvocation invocation);

  bool AllowsAgentOperation(const RunExecution& run, Capability capability) const;

  std::optional<ModelToolSpec> DelegationTool(const RunExecution& run) const;

  Async<JsonValue> ExecuteDelegationTool(RunId run, CommandId command, const JsonValue& arguments);

  Task<void> ConsumeQueue(SessionId id);

  Task<void> Drive(RunId run);

  Async<ModelReply> RunHostModel(RunId run, ModelInput input);

  Async<Receipt<ToolCallAccepted>> RequestTool(RunId run, CommandId command, ToolRequest request);

  Async<ToolResult> WaitForHostedTool(RunId run, ToolCallId call);

  Async<ToolResult> ProjectToolResult(RunId run, ToolResult result);

  void ScheduleHostedTool(RunExecution& run, ToolCallId call);

  Task<void> ExecuteHostedTool(RunId run, ToolCallId call);

  Async<StoredArtifact> ReadArtifact(ArtifactRef artifact);

  Async<StoredArtifact> ReadArtifact(ArtifactRef artifact, ActionParty actor);

  Async<StoredArtifact> ReadHistoryArtifact(const SessionData& target, ActionParty actor, ArtifactRef artifact);

  Async<ArtifactPreview> PreviewArtifact(ArtifactRef artifact, std::optional<SessionId> target, PrincipalId principal);

  Async<ArtifactVersions> VisibleApprovalArtifacts(SessionId session, PrincipalId user);

  Async<ArtifactImpact> InspectArtifactImpact(ArtifactImpactQuery query, std::optional<RunId> actor = {});

  Async<Receipt<ChangePlanAccepted>> PrepareArtifactChange(CommandId command, CommandInput input);

  Task<void> StartQueuedArtifactChange(SessionId session, QueueItemId item, CommandId command);

  void StartArtifactChangeTool(RunExecution& run, const CommandId& command, const CommandRecord& record);

  Task<void> DriveArtifactChange(RunId id);

  Async<PolicyDecision> ArtifactChangeAccess(CommandInput request, std::optional<PlanId> plan = {});

  AgentResult<void> BindArtifactChangeInvocation(ToolInvocation& invocation) const;

  Async<PolicyDecision> DecideToolExecution(PolicyRequest request);

  void SettleArtifactChange(CommandRecord& record, SessionData& session, const HostToolRecord& tool);

  Async<CommandReceipt> ReconcileTool(CommandId command, SessionId session, ToolCallId call, ToolExecutionResult result);

  void InstallArtifactChangeTools();

  void RecordRevertEffects(const SessionData& session, ToolCallSnapshot& call, const ActionParty& actor);

  Async<std::vector<ArtifactRef>> PublishArtifacts(RunId run, ToolCallId call, std::vector<ArtifactDraft> drafts,
                                                  std::optional<ActionParty> origin = {}, bool reverting = false);

  Async<void> DriveToolTurn(RunId run);

  Async<void> DriveParallelTools(RunId run);

  void RecordModelTool(SessionData& session, const RunId& run, PendingModelCall& pending);

  Task<void> DriveModelTool(RunId run, PendingModelCall& pending);

  void AbortToolTurn(RunExecution& run, const AgentError& error);

  void CancelToolTurnTasks(RunExecution& run);

  Async<std::vector<SkillSummary>> DiscoverSkills(PolicyRequest context, std::stop_token stop);

  Async<std::vector<SkillResource>> ActivateSkill(RunId run, SkillActivation activation);

  Async<std::vector<SkillResource>> LoadAndActivateSkill(RunExecution& run, SkillActivation activation);

  Async<SkillPackage> ReadSkillPackage(const RunExecution& run, SkillActivation activation);

  Async<void> CheckSkillAccess(const RunExecution& run, SkillActivation activation, bool confirmed);

  Async<void> ActivateInputSkills(RunExecution& run);

  Async<void> PrepareSkills(const RunExecution& run, ModelTurnRequest& request);

  Async<void> VerifyActiveSkills(const RunExecution& run, bool model = false);

  Async<void> VerifyModelSkillResources(const RunExecution& run);

  Async<JsonValue> ExecuteSkillTool(RunId run, std::string name, JsonValue arguments);

  Async<huxerui::AsyncInputStream> OpenSkillResource(RunId run, SkillRef ref, std::string path, bool model = false);

  Async<std::string> ReadSkillTextForModel(RunId run, SkillRef ref, std::string path);

  void ApplySteers(SessionData& session, RunExecution& run, std::size_t count, bool apply);

  MessageId RecordSteer(SessionData& session, RunExecution& run, SendInput input, MessageAuthor author,
                        std::vector<SideChatConclusion> conclusions = {});

  void SettleCollaborationSteer(SessionData& session, RunSteerSnapshot& steer, bool applied);

  Task<void> RunDeadline(RunId run, Duration duration);

  void Finish(RunExecution& run, RunState state, RunEndReason reason, std::optional<AgentError> error = {},
              std::optional<RunCompletion> result = {});

  void Stop(RunExecution& run, RunEndReason reason);

  static Task<void> StopStream(std::shared_ptr<RuntimeState> self, RunId run,
                               std::shared_ptr<ModelTurnStream> stream, RunEndReason reason);

  void CloseInteraction(SessionData& session, InteractionSnapshot& interaction,
                        InteractionState state, std::optional<JsonValue> value = {});

  Async<InteractionOutcome> WaitForInteraction(RunId run, InteractionId id, bool tool_approval = false);

  Async<InteractionOutcome> Ask(RunId run, FormRequest request, bool alongside_approvals = false);

  Async<Receipt<InputRequested>> RequestForm(RunId run, CommandId command, FormRequest request);

  void ScheduleInteractionExpiry(RunId run, InteractionId id, TimePoint deadline);

  Task<void> ExpireInteraction(RunId run, InteractionId id, Duration delay);

  AgentResult<void> ReportProgress(const RunId& run, const ToolCallId& call, std::string text);

  Async<ToolResult> ExecuteTool(RunId run, ToolRef tool, ToolRequest request, ToolCallId call);

  Task<std::optional<ToolReviewResult>> ReviewTool(ToolAttempt& attempt, ToolReviewRequest request);

  void SettleToolException(RunExecution& run, const ToolCallId& call);

  AgentResult<ActionParty> Actor(const std::optional<RunId>& run, Capability capability) const;

  PolicyRequest CollaborationAccess(const ActionParty& actor, Capability capability, const SessionData& target) const;

  Async<ReceiveDecision> Receive(ActionParty actor, const SessionData& target, CollaborationMessageId id,
      std::variant<SessionMessageRequest, SessionReply> payload, std::optional<GoalRef> goal = {});

  AgentResult<void> CheckWork(const WorkRequest& work) const;

  void EnqueueMessage(SessionData& target, CollaborationRecord record, ReceiveDecision decision, std::optional<GoalRef> goal = {});

  RunExecution& RecordWork(SessionData& target, CollaborationMessageId request, PrincipalId user, RunStartBinding binding,
                           std::vector<SourceSlice> task_inputs = {});

  void FinishWork(SessionData& session, RunSnapshot& run);

  bool PendingStart(CollaborationMessageId id) const;

  void ClosePendingStart(SessionData& target, CollaborationMessageId request, DeliveryState delivery, AgentError reason);

  std::optional<std::pair<SessionId, CollaborationRecord>> FindMessage(CollaborationMessageId id) const;

  Async<CollaborationRecord> ReadMessage(CollaborationMessageId id, std::optional<RunId> actor,
                                        Capability capability, std::vector<Capability>* confirmation = nullptr,
                                        std::optional<OperationPlan> approved = {});

  Async<Receipt<MessageAccepted>> SendMessage(CommandId command, SessionMessageRequest request, std::optional<RunId> actor,
                                             std::optional<OperationPlan> approved = {});

  Async<Receipt<MessageAccepted>> ReplyMessage(CommandId command, CollaborationMessageId request,
                                              SessionReply reply, std::optional<RunId> actor,
                                              std::optional<OperationPlan> approved = {});

  bool ReceivedArtifact(const ActionParty& actor, const ArtifactRecord& artifact) const;

  Async<Receipt<MessageAccepted>> StartMessage(CommandId command, CollaborationMessageId request, bool automatic = false,
                                               std::optional<OperationPlan> approved = {});

  Async<CommandReceipt> WithdrawMessage(CommandId command, CollaborationMessageId message, std::optional<RunId> actor);

  Async<ReplyOutcome> WaitMessage(CollaborationMessageId id, WaitOptions options, std::optional<RunId> actor);

  bool WaitCycle(RunId waiter, CollaborationMessageId request) const;

  bool WaitCycle(RunId waiter, const RunWait& wait) const;

  std::optional<RunId> DependencyRun(const RunWait& wait) const;

  const WorkScope* Scope(const RunExecution& run) const;

  Async<void> CheckInheritedHistory(const RunExecution& run);

  Async<ActionParty> ProjectActor(ProjectId project, std::optional<RunId> actor, Capability capability);

  Async<ActionParty> ProjectAccess(ProjectId project, std::optional<RunId> actor, Capability capability);

  PolicyRequest ProjectPolicy(const ActionParty& actor, ProjectId project, Capability capability) const;

  const ProjectData* PublishedProject(const ProjectId& project) const;

  const ProjectCommandRecord* FindProjectCommand(const PrincipalId& user, const CommandId& command, bool committed) const;

  template<class T>
  AgentResult<std::optional<Receipt<T>>> ReplayProject(const ActionParty& actor, const ProjectId& project,
                                                     const CommandId& command, const ProjectCommand& input) {
    using Result = AgentResult<std::optional<Receipt<T>>>;
    if (command.value.empty()) return Fail<std::optional<Receipt<T>>>(ErrorCode::InvalidArgument, "A command ID is required");
    if (commands.contains({actor.principal, command})) {
      return Fail<std::optional<Receipt<T>>>(ErrorCode::Conflict, "The command ID is bound to a session operation");
    }
    auto existing = FindProjectCommand(actor.principal, command, false);
    if (!existing) return Result::Success(std::nullopt);
    if (existing->progress.accepted.project != project || existing->actor != actor.run || existing->input != input) {
      return Fail<std::optional<Receipt<T>>>(ErrorCode::Conflict, "The command ID is bound to other project input");
    }
    return Result::Success(Receipt<T>{existing->progress.accepted, std::get<T>(*existing->progress.result)});
  }

  template<class T>
  Async<Receipt<T>> CommitProject(ProjectData& project, const ActionParty& actor, CommandId command, ProjectCommand input,
                                  T result, const SessionData* session = nullptr, const SessionData* goal_home = nullptr,
                                  std::optional<SourceChange> source_change = {},
                                  std::span<const SessionData* const> additional = {}) {
    CommitStamp stamp{.command = std::move(command), .commit = {MakeId()},
        .project = project.id, .project_revision = project.revision};
    project.commands.push_back({actor.principal, actor.run, std::move(input),
        {.accepted = stamp, .state = CommandState::Applied, .result = CommandValue{result}}, std::move(source_change)});
    auto saved = co_await WhenSaved(session ? Persist(*session, true, stamp.commit, goal_home, &project, additional)
                                           : PersistProject(project, stamp.commit));
    if (!saved.Succeeded()) co_return AgentResult<Receipt<T>>::Failure(saved.Error());
    co_return AgentResult<Receipt<T>>::Success({std::move(stamp), std::move(result)});
  }

  Async<ProjectSnapshot> ReadProject(ProjectId project, std::optional<RunId> actor);

  std::optional<ProjectId> FindForkReviewProject(const ForkReviewId& review) const;

  Async<Receipt<ForkReviewCreated>> CreateForkReview(CommandId command, ForkReviewRequest request, std::optional<RunId> actor,
                                                   std::optional<OperationPlan> approved = {});

  Async<void> AbandonComparisonWorkspace(CommandId comparison, WorkspaceCopyPlanRef copy, WorkspaceId allocated);

  Async<void> FilterComparisonResult(ForkReviewCreated& result, std::optional<RunId> actor);

  Async<Receipt<ForkReviewCreated>> ReadComparisonCommand(const CommandId& command, std::optional<RunId> actor);

  Async<std::vector<ForkReviewSummary>> ListForkReviews(SessionId source, std::optional<RunId> actor);

  Async<ForkReviewSnapshot> ReadForkReview(ForkReviewId review, std::optional<RunId> actor);

  Async<ArtifactRecord> ReadAdoptionCandidate(const AdoptCandidateRequest& request, const ActionParty& actor);

  AgentResult<void> CheckAdoptionSlot(const SessionData& session, const AdoptCandidateRequest& request) const;

  bool AdoptionBound(const SessionData& session, const AdoptCandidateRequest& request, const ToolCallId& call) const;

  ProjectData* RefreshForkAdoptions(const SessionData& session);

  Async<CommandReceipt> CloseForkReviewAsync(CommandId command, CloseForkReview request, std::optional<RunId> actor);

  ProjectData* FinishForkCandidate(const RunSnapshot& run);

  Async<SharedContextItem> ReadSharedContext(SharedContextRef item, std::optional<RunId> actor, bool published_only = false);

  Async<Receipt<SharedContextRef>> UpdateSharedContext(ProjectId project, CommandId command, SharedContextCommand input,
                                                     std::optional<RunId> actor);

  Async<CommandReceipt> RecordSourceChange(ProjectId project, CommandId command, SourceChange change,
                                            std::optional<RunId> actor);

  std::optional<Revision> ProjectSourceRevision(const SessionData& session) const;

  bool SourcesChanged(const SessionData& session, Revision revision, const std::vector<SourceSlice>& inputs) const;

  bool SourcePlanStale(const SessionData& session, const ToolPlanState& plan) const;

  void InvalidateSourcePlans(const ProjectId& project);

  Async<Receipt<TaskUpdated>> UpdateProjectTask(ProjectId project, CommandId command, TaskCommand input, std::optional<RunId> actor);

  Async<TaskOutcome> WaitProjectTask(TaskExecutionRef task, WaitOptions wait, std::optional<RunId> actor);

  Async<void> ValidateProjectSources(ProjectId project, ActionParty actor, const std::vector<SourceSlice>& sources);

  Async<void> AuthorizeWithdrawnContextImpact(ProjectId project, ActionParty actor, SharedContextRef source);

  Async<void> ValidateInputSources(const SessionData& session, ActionParty actor, const std::vector<SourceSlice>& sources);

  Async<std::vector<ModelContent>> AssembleProjectInput(const RunExecution& run, std::vector<SourceSlice> extra = {});

  ProjectData* FinishProjectRun(const RunSnapshot& run);

  Async<ProjectTaskSnapshot> ResolveWorkTask(const SessionData& target, WorkRequest work, const ActionParty& actor);

  bool CurrentWorkClaim(const ProjectTaskSnapshot& task, const TaskExecutionRef& execution) const;

  ProjectData& BindWorkTask(const TaskExecutionRef& execution, RunExecution& run);

  void RefreshTaskReadiness(ProjectData& project);
};

} // namespace huxerui::agentkit::detail
