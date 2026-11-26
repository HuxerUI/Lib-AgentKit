#pragma once

/// @file
/// @brief Runtime registration, Run-scoped orchestration, and host integration contracts.
///
/// Create one AgentRuntime under an application task scope. Register models, tools, and agent definitions
/// before StartAsync(). Open a Conversation through Sessions(), or supply a RunHandler for custom orchestration.
/// Register RunRestore to resume host workflows from durable checkpoints and outstanding waits.

#include <huxerui/agentkit/store.h>
#include <huxerui/agentkit/artifact.h>
#include <huxerui/agentkit/fork_review.h>
#include <huxerui/agentkit/collaboration.h>
#include <huxerui/agentkit/project.h>
#include <huxerui/agentkit/goals.h>
#include <huxerui/agentkit/skill.h>

namespace huxerui::agentkit {

/// @brief Versioned configuration of one agent and the operations it can request.
/// Host-supplied access, receive and resource policies also apply to every Run using this definition.
struct AgentDefinition {
  /// Stable agent ID and positive definition version.
  AgentRef ref;
  /// Label for session lists and other host UI.
  std::string display_name;
  /// UTF-8 host instructions for model calls.
  std::string instructions;
  /// Default registered model ID.
  ModelId model;
  /// Static tools exposed to the agent by ID.
  std::vector<ToolId> tools;
  /// Dynamic catalog IDs exposed to the agent.
  std::vector<ToolCatalogId> tool_catalogs;
  /// Skill source IDs to search in declaration order.
  std::vector<SkillSourceId> skill_sources;
  /// Versioned custom message types the agent may publish.
  std::vector<ContentTypeRef> content_types;
  /// Context provider IDs in resolution order.
  std::vector<ContextProviderId> context;
  /// Agents eligible to run as direct children in the same session.
  std::vector<AgentId> delegates;
  /// Agent IDs eligible for cross-session collaboration.
  std::vector<AgentId> collaboration_targets;
  /// Agent-level capabilities subject to current host policy.
  std::vector<Capability> capabilities;
  /// Minimum model capabilities checked against the effective registration.
  ModelCapabilities requires_model;
  /// Default execution limits, further bounded by parent, goal or branch policy where applicable.
  Budget budget;
};

/// @brief Accepted workflow input and its origin, passed to RunHandler and RunRestore.
struct RunInput {
  /// Run whose work the host is executing.
  RunId run;
  /// Reason this Run was started.
  RunTrigger trigger;
  /// Original validated input for the Run.
  SendInput input;
  /// Original collaboration message when this Run answers a work request.
  std::optional<CollaborationMessageId> work_request;
  /// Project task attempt associated with this Run, when present.
  std::optional<TaskExecutionRef> task;
  /// Goal definition version bound to this Run, when present.
  std::optional<GoalRef> goal;
  /// Authorized side-chat conclusions resolved from the accepted input.
  std::vector<SideChatConclusion> conclusions;
  /// Versioned artifact action that triggered the Run, when present.
  std::optional<ArtifactAction> artifact_action;
};

/// @brief Application-defined phase state written before a durable wait or continuation.
struct RunCheckpoint {
  /// Stable workflow stage name understood by RunRestore.
  std::string stage;
  /// Host-defined version of the stage payload.
  Revision schema_version;
  /// Owned JSON payload for that stage.
  JsonValue state;
  /// Last applied steering message to acknowledge in the checkpoint, when applicable.
  std::optional<MessageId> steer_through;
};

/// @brief Durable state available when a host workflow is reopened.
struct RunResume {
  /// Latest committed host checkpoint, when one was saved.
  std::optional<RunCheckpoint> checkpoint;
  /// Registered waits to reconnect using their original subject identities.
  std::vector<RunWait> waits;
};

/// @brief One requested child Run under the current Run's session and authority.
struct DelegateRequest {
  /// Direct delegate declared on the parent agent.
  AgentId agent;
  /// Task text, explicit sources and selected versions of the parent's input attachments.
  SendInput input;
  /// Optional per-child limits bounded by the child and all ancestor constraints.
  Budget budget;
  /// Schema for the child's structured result, when required.
  std::optional<JsonSchema> expected_result;
  /// Optional exact model registration authorized for the child. Defaults to the delegate's registered model.
  std::optional<ModelProviderRef> model;
  /// Compares the stored fields for equality.
  bool operator==(const DelegateRequest&) const = default;
};

/// @brief Terminal result for one child, returned in request order.
struct DelegateOutcome {
  /// Child Run ID.
  RunId run;
  /// Terminal state of the child.
  RunState state;
  /// Completion payload from the child, when available.
  std::optional<RunCompletion> result;
  /// Convenience copy of RunCompletion::structured from result, when available.
  std::optional<JsonValue> structured;
  /// Child failure or cancellation diagnostic, if recorded.
  std::optional<AgentError> failure;
};

/// @brief Services bound to a specific authorized Run and its cancellation lifetime.
///
/// Use a stable CommandId with RequestInputAsync(), StartToolAsync() or DelegateAsync() when a workflow
/// needs to reconnect after restart. Save enough state in a RunCheckpoint for RunRestore to repeat the
/// lookup or wait against the original objects.
class RunContext {
public:
  /// @brief Returns the current Run ID.
  /// @return The stable ID of the Run bound to this context.
  RunId Id() const;

  /// @brief Returns the exact model registration bound to this Run.
  /// @return The model and revision pinned when this Run was accepted.
  ModelProviderRef Model() const;

  /// @brief Returns artifact operations bound to the Run's actor.
  ArtifactChanges Artifacts() const;

  /// @brief Returns the cancellation signal for the current Run.
  /// @return A token for cooperative cancellation of asynchronous work owned by the Run.
  std::stop_token StopToken() const;

  /// @brief Discovers skill summaries authorized for the current Run.
  /// @return The discoverable skill versions for this Run.
  Async<std::vector<SkillSummary>> ListSkillsAsync();

  /// @brief Returns the skill versions already activated by this Run.
  /// @return A copy of the activations already recorded for this Run.
  std::vector<SkillActivation> ActiveSkills() const;

  /// @brief Authorizes a skill version for this Run and returns its resource manifest.
  /// @param skill Exact skill version from ListSkillsAsync().
  /// @return The authorized resource manifest for that version.
  Async<std::vector<SkillResource>> ActivateSkillAsync(SkillRef skill);

  /// @brief Opens a stream for an authorized resource in an active skill.
  /// @param skill Exact skill version already activated for this Run.
  /// @param relative_path Normalized package-relative path from its manifest.
  /// @return The host stream for that resource.
  Async<huxerui::AsyncInputStream> OpenSkillResourceAsync(SkillRef skill, std::string relative_path);

  /// @brief Reads an active skill's textual resource under the effective model's authorization and byte budget.
  /// @param skill Exact skill version already activated for this Run.
  /// @param relative_path Normalized path to a textual manifest resource.
  /// @return UTF-8 text after authorization and size validation.
  Async<std::string> ReadSkillTextForModelAsync(SkillRef skill, std::string relative_path);

  /// @brief Runs a bounded hosted model completion with the context and permitted tools from ModelInput.
  /// @param input Per-turn instructions, sources, output schema and tool restrictions.
  /// @return The committed reply, including any visible artifacts published by this completion.
  /// @note The effective model and budget come from the Run binding. The supplied tools and sources are checked before use.
  /// @see ModelInput::allowed_tools, ModelInput::include_catalog_tools
  Async<ModelReply> CompleteAsync(ModelInput input);

  /// @brief Records a form request with a stable command ID for later waiting or recovery.
  /// @param command Stable ID for this logical request. Reuse it with the same form after recovery.
  /// @param request Form schema, display content and optional expiration.
  /// @return The receipt containing the interaction ID used by WaitForInputAsync().
  Async<Receipt<InputRequested>> RequestInputAsync(CommandId command, FormRequest request);

  /// @brief Reads a settled form outcome or suspends this Run until its interaction resolves.
  /// @param interaction Original interaction ID returned by RequestInputAsync().
  /// @return The settled outcome, including submission, dismissal or expiration.
  Async<InteractionOutcome> WaitForInputAsync(InteractionId interaction);

  /// @brief Requests a form and waits for its outcome as one operation.
  /// @param request Form schema, display content and optional expiration.
  /// @return The settled interaction outcome.
  /// @note Use RequestInputAsync() and WaitForInputAsync() if a workflow needs to reconnect to the original interaction.
  Async<InteractionOutcome> AskAsync(FormRequest request);

  /// @brief Accepts a tool call with a stable command ID and routes it through preparation and approval.
  /// @param command Stable ID for the logical call. Reuse it with the same request during recovery.
  /// @param request Tool ID and arguments. The runtime binds the registered tool revision at acceptance.
  /// @return The accepted call ID used by WaitForToolAsync().
  Async<Receipt<ToolCallAccepted>> StartToolAsync(CommandId command, ToolRequest request);

  /// @brief Waits for an accepted tool call's committed result.
  ///
  /// Rechecks read access to each artifact version on every read, including after recovery.
  /// @param call Original call ID returned by StartToolAsync().
  /// @return The committed result, with visible references for the current actor.
  Async<ToolResult> WaitForToolAsync(ToolCallId call);

  /// @brief Starts a tool and waits for its result as one operation.
  /// @param request Tool ID and arguments validated against the bound tool's schema.
  /// @return The committed result for this call.
  /// @note For external writes in resumable workflows, use StartToolAsync() with a stable CommandId followed by WaitForToolAsync().
  /// @see WaitForToolAsync() for result visibility.
  Async<ToolResult> CallToolAsync(ToolRequest request);

  /// @brief Runs delegated requests in the current session, awaiting results in input order.
  /// Recovery can reconnect an already recorded batch. Prefer the overload with a stable CommandId in a resumable host phase.
  /// @param requests Child workflows and their requested budgets and inputs.
  /// @return Terminal outcomes in request order.
  /// @note Child Runs are subject to the parent tree's cancellation, permissions and remaining budget.
  Async<std::vector<DelegateOutcome>> DelegateAsync(std::vector<DelegateRequest> requests);

  /// @brief Starts or reconnects a child batch with a stable command ID.
  /// @param command Original ID of this logical batch. Reuse it with identical requests during recovery.
  /// @param requests Children to schedule in the order used for result association.
  /// @return One outcome per child when the batch settles.
  /// @note Recovery joins the recorded batch. Child scheduling is bounded by the runtime's tree concurrency and depth limits.
  Async<std::vector<DelegateOutcome>> DelegateAsync(CommandId command, std::vector<DelegateRequest> requests);

  /// @brief Reads accepted steering input not yet acknowledged by a host checkpoint.
  Async<std::vector<RunSteerInput>> ReadPendingSteersAsync();

  /// @brief Commits stage state and acknowledges pending steering through steer_through, if supplied.
  /// @param checkpoint Serializable workflow stage and state, with an optional steering high-water mark.
  /// @return Success once the new checkpoint is committed.
  Async<void> SaveCheckpointAsync(RunCheckpoint checkpoint);

  /// @brief Returns a SessionService operating under this Run's identity.
  SessionService Sessions();

  /// @brief Returns a SessionCollaboration service operating under this Run's identity.
  SessionCollaboration Collaboration();

  /// @brief Returns a ProjectService operating under this Run's identity.
  ProjectService Projects();

  /// @brief Returns a ForkReviewService operating under this Run's identity.
  ForkReviewService ForkReviews();

  /// @brief Returns goal operations bound to this Run and its session.
  SessionGoals Goals();

  /// @brief Returns command lookup and observation under this Run's identity.
  CommandService Commands();

private:
  RunContext(std::shared_ptr<detail::RuntimeState> runtime, RunId run);
  std::shared_ptr<detail::RuntimeState> runtime_;
  RunId run_;
  friend class detail::RuntimeState;
};

/// @brief Host coroutine implementing an agent workflow with an accepted RunInput.
using RunHandler = std::function<Async<RunCompletion>(RunContext, RunInput)>;
/// @brief Host coroutine that resumes a recorded workflow using the original RunInput and its RunResume.
using RunRestore = std::function<Async<RunCompletion>(RunContext, RunInput, RunResume)>;

/// @brief Versioned agent definition and optional host execution entry points.
struct AgentRegistration {
  /// Agent definition validated against the runtime registry.
  AgentDefinition definition;
  /// Optional host workflow. Empty uses the default model-driven Run.
  RunHandler run;
  /// Optional restart entry point for a host workflow with recoverable state.
  RunRestore restore;
};

/// @brief Synchronous callback resolving the current user principal for runtime actions.
using PrincipalResolver = std::function<AgentResult<PrincipalId>()>;

/// @brief Context supplied for optimizing a title derived from the first input.
struct SessionTitleRequest {
  /// Session being titled.
  SessionId session;
  /// Agent definition bound to that session.
  AgentRef agent;
  /// First input text eligible for generating a better title.
  std::string first_input_text;
  /// Optional cancellation signal for this title request. The current runtime supplies a default token.
  std::stop_token stop;
};

/// @brief Optional asynchronous title optimizer. An empty result leaves the current title in place.
using SessionTitleGenerator =
    std::function<Async<std::optional<std::string>>(SessionTitleRequest)>;

/// @brief Versioned session or comparison operation presented to the host workspace resolver.
struct WorkspaceRequest {
  /// Caller authorized for the requested workspace binding.
  ActionParty actor;
  /// Workspace operation identity for idempotent allocation.
  CommandId command;
  /// Session ID reserved for the resulting binding.
  SessionId new_session;
  /// Explicit existing or copied workspace selection, when supplied.
  std::optional<WorkspaceChoice> choice;
  /// Project receiving the workspace binding, when present.
  std::optional<ProjectId> project;
  /// Source history boundary for a fork, when applicable.
  std::optional<HistoryBoundary> fork_from;
  /// Parent comparison command for candidate allocations, when applicable.
  std::optional<CommandId> comparison_command;
};

/// @brief Resolves or allocates a workspace for a session operation. Success with an empty value leaves it unbound.
using WorkspaceResolver = std::function<Async<std::optional<WorkspaceId>>(WorkspaceRequest)>;

/// @brief Authorized context for preparing a registered message-component action.
struct ContentActionContext {
  /// User invoking the component action.
  ActionParty actor;
  /// Stable command ID for this action.
  CommandId command;
  /// Exact message version and block receiving the action.
  CustomBlockRef target;
  /// Shared immutable content of that block.
  std::shared_ptr<const CustomBlock> block;
};

/// @brief Prepared effect of a component action: new input, steering, reply, or artifact action.
using ContentActionPlan = std::variant<SendInput, SteerRequest, InteractionReply, ArtifactAction>;
/// @brief Validates and prepares a component action for later runtime submission.
using ContentActionHandler = std::function<Async<ContentActionPlan>(ContentActionContext, JsonValue)>;

/// @brief One named action supported by a ContentComponentRegistration.
struct ContentActionRegistration {
  /// Action name referenced by CustomBlockAction::action.
  std::string id;
  /// Schema applied to the action payload.
  JsonSchema payload_schema;
  /// Handler that constructs the effect without directly committing it.
  ContentActionHandler prepare;
};

/// @brief Schema and actions for one version of a custom message type.
struct ContentComponentRegistration {
  /// Registered ContentTypeRef for custom blocks.
  ContentTypeRef type;
  /// Schema for values of this block type.
  JsonSchema data_schema;
  /// Named actions validated on dispatch.
  std::vector<ContentActionRegistration> actions;
};

/// @brief Authorized input passed to a registered artifact action handler.
struct ArtifactActionContext {
  /// User requesting the action.
  ActionParty actor;
  /// Stable command ID.
  CommandId command;
  /// Session that will receive the prepared input.
  SessionId session;
  /// Exact artifact version targeted by the action.
  ArtifactRef artifact;
};

/// @brief Prepares Run input from a validated action on an exact artifact version.
using ArtifactActionHandler = std::function<Async<SendInput>(ArtifactActionContext, JsonValue)>;

/// @brief Binds an artifact type and action name to a host preparation callback.
struct ArtifactActionRegistration {
  /// Artifact metadata type covered by this registration.
  std::string artifact_type;
  /// Action name used in ArtifactAction.
  std::string action;
  /// Schema for the supplied action payload.
  JsonSchema payload_schema;
  /// Handler that prepares input for the resulting Run.
  ArtifactActionHandler prepare;
};

/// @brief Application-owned execution, policy and persistence adapters passed to AgentRuntime::Create().
struct RuntimeOptions {
  /// Application-level task scope that outlives transient views.
  huxerui::TaskScope tasks;
  /// Resolves the current user identity for user-bound service calls.
  PrincipalResolver principal;
  /// Host access policy evaluated for current identities and targets.
  AccessPolicy access;
  /// Optional receiving-side collaboration policy.
  ReceivePolicy receive;
  /// Placeholder used before the first meaningful input or a manual title.
  std::string untitled_session_title = "New conversation";
  /// Optional title optimizer run after the initial title is chosen.
  SessionTitleGenerator generate_session_title;
  /// Optional automatic reviewer for policy decisions that allow tool review.
  std::optional<ToolReviewer> tool_reviewer;
  /// Optional resolver returning resource versions and handles for tool phases.
  ToolResourceResolver tool_resources;
  /// Positive scheduling cap for tool calls in a model turn. Tools remain subject to their own conflict rules.
  std::size_t max_parallel_tools_per_turn = 1;
  /// Optional host workspace binding or allocation service.
  WorkspaceResolver workspace;
  /// Optional resolver for authorized project and workspace bindings.
  ProjectResolver project;
  /// Optional host verifier for goal and project task evidence.
  GoalVerifier verify;
  /// Callbacks resolving artifact previews, bytes and canonical selections.
  ArtifactStore artifact_store;
  /// Optional rework, revert and adoption preparation and execution callbacks.
  ChangeAdapters changes;
  /// Optional transactional store. Empty uses in-memory runtime state.
  std::shared_ptr<SessionStore> store;
  /// Optional clock for deadlines and timestamps. Empty uses the system clock.
  std::function<TimePoint()> now;
  /// Optional host callback for asynchronous errors reported by the runtime.
  std::function<void(std::exception_ptr)> on_error;
  /// Maximum child distance from the root Run.
  std::size_t max_delegation_depth = 3;
  /// Maximum concurrently scheduled delegated Runs in a tree.
  std::size_t max_parallel_delegates = 4;
};

/// @brief Application-scoped engine that owns registrations, session state and execution tasks.
/// Register the initial agents and their dependencies before StartAsync(). Observe state through a service handle or Conversation.
/// Call CloseAsync() during application shutdown to settle or persist outstanding work.
/// @note Configure and use the runtime on the application task scope. Own the runtime through application shutdown.
/// @code{.cpp}
/// auto created = AgentRuntime::Create(std::move(options));
/// if (!created.Succeeded()) co_return AgentResult<void>::Failure(created.Error());
/// auto runtime = std::move(created).Value();
/// auto model_status = runtime->RegisterModel(std::move(model));
/// if (!model_status.Succeeded()) co_return model_status;
/// auto agent_status = runtime->RegisterAgent(std::move(agent));
/// if (!agent_status.Succeeded()) co_return agent_status;
/// co_return co_await runtime->StartAsync();
/// @endcode
class AgentRuntime {
public:
  /// @brief Constructs the runtime from host callbacks and an application task scope.
  /// @param options Application task scope, identity resolver, access policy and optional host adapters.
  /// @return An unstarted runtime or an error for an invalid configuration.
  static AgentResult<std::shared_ptr<AgentRuntime>> Create(RuntimeOptions options);

  /// @brief Registers a versioned model configuration and provider.
  /// Add a model ID or publish a higher revision on the application thread, including after StartAsync().
  /// Default selections use the highest revision. Explicit selections, queued inputs and started Runs retain their exact revisions.
  /// Register the original revisions at startup to restore Runs that still use them.
  /// @param model Exact model identity, descriptor and provider.
  AgentResult<void> RegisterModel(ModelRegistration model);

  /// @brief Registers a static tool with argument schema and effect policy.
  /// @param tool Versioned schema, effects, execution and recovery callbacks.
  AgentResult<void> RegisterTool(ToolDefinition tool);

  /// @brief Declares a versioned dynamic tool catalog by ID.
  /// @param catalog Host-assigned identifier referenced by AgentDefinition.
  AgentResult<void> RegisterToolCatalog(ToolCatalogId catalog);

  /// @brief Publishes a catalog snapshot and checks outstanding references against its tool revisions.
  /// @param catalog Previously registered catalog ID.
  /// @param snapshot New version and the complete set of tools it publishes.
  /// @return Success after the new catalog revision is installed.
  Async<void> ReplaceToolCatalogAsync(ToolCatalogId catalog, ToolCatalogSnapshot snapshot);

  /// @brief Adds a versioned skill source under its host-assigned ID.
  /// @param id Host-assigned identifier referenced by agent definitions.
  /// @param source Adapter listing versioned manifests and loading their resources.
  AgentResult<void> RegisterSkillSource(SkillSourceId id, std::shared_ptr<SkillSource> source);

  /// @brief Adds a resolver for authorized resource selections under its host-assigned ID.
  /// @param id Host-assigned identifier listed in AgentDefinition::context.
  /// @param provider Callback serving selections from an authorized resource version.
  AgentResult<void> RegisterContextProvider(ContextProviderId id, ContextProvider provider);

  /// @brief Registers an agent definition and optional workflow entry points.
  /// @param agent Agent dependencies and workflow entry points, including an optional restore callback.
  AgentResult<void> RegisterAgent(AgentRegistration agent);

  /// @brief Registers schemas, rendering and actions for a custom transcript type and version.
  /// @param component Versioned block schema, renderer and action handlers.
  AgentResult<void> RegisterContentComponent(ContentComponentRegistration component);

  /// @brief Registers a version-aware action for an artifact type.
  /// @param action Artifact type, action name, payload schema and input preparation callback.
  AgentResult<void> RegisterArtifactAction(ArtifactActionRegistration action);

  /// @brief Checks agent references, required model capabilities and registry consistency.
  /// @return Success for a consistent registry or the first encountered validation error.
  AgentResult<void> ValidateRegistration() const;

  /// @brief Validates the registry, loads the configured store and resumes recoverable work.
  /// @return Success after initialization prepares recoverable work to resume.
  Async<void> StartAsync();

  /// @brief Closes the runtime and settles outstanding runtime-owned work.
  /// @return Success after shutdown settles local work.
  Async<void> CloseAsync();

  /// @brief Reevaluates visible projections and work affected by changes to current authorization.
  /// @return Success after the cached access decisions are invalidated and projections are scheduled for refresh.
  /// @note An existing Conversation::Snapshot() can be empty until authorization is checked again. Resume an expired
  /// event subscription from an authorized snapshot and its cursor.
  Async<void> RefreshAccessAsync();

  /// @brief Lists authorized skill summaries for the supplied agent or session scope.
  /// @param query Agent and project scope, optionally restricted to an existing session.
  /// @return Skills visible to the current user in that scope.
  Async<std::vector<SkillSummary>> ListSkillsAsync(SkillCatalogQuery query);

  /// @brief Returns the user-bound command lookup service.
  CommandService Commands();

  /// @brief Returns the user-bound session directory and lifecycle service.
  SessionService Sessions();

  /// @brief Returns the user-bound collaboration service.
  SessionCollaboration Collaboration();

  /// @brief Returns the user-bound project task and shared-context service.
  ProjectService Projects();

  /// @brief Returns the user-bound comparison and adoption service.
  ForkReviewService ForkReviews();

  /// @brief Returns the user-bound directory of pending forms and approvals.
  InteractionService Interactions();

  /// @brief Returns the user-bound artifact and change service.
  ArtifactChanges Artifacts();

  /// @brief Records verified external effects under the original unknown tool call.
  /// Use the same CommandId when resolving an uncertain reconciliation submission.
  /// @param command Stable ID of this reconciliation submission.
  /// @param session Session that owns the original call.
  /// @param call Unknown call whose external outcome has been checked.
  /// @param result Verified outcome and external effects to record.
  /// @return Receipt for the resolved call.
  Async<CommandReceipt> ReconcileToolAsync(CommandId command, SessionId session, ToolCallId call, ToolExecutionResult result);

private:
  explicit AgentRuntime(std::shared_ptr<detail::RuntimeState> state);
  std::shared_ptr<detail::RuntimeState> state_;
};

} // namespace huxerui::agentkit
