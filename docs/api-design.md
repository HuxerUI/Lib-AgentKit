# Lib-AgentKit API 设计

本稿定义 [Lib-AgentKit 完整版](design.md) 的 C++20 公共契约。应用可单独接入运行时，也可选用默认对话组件。本文的代码块是拟定的头文件声明，按依赖顺序阅读。运行时在 HuxerUI 宿主应用内执行，模型和宿主工具可以调用外部服务。

相对于总体设计的示意签名，`AgentRuntime::Sessions()` 统一承担会话创建、发现、观察和历史读取。`Collaboration()` 承担会话间消息及回复。`Conversation` 绑定已经打开的会话。Preview 的默认通用助手是应用配置，创建请求始终携带明确的 `AgentId`。

当前实现的调用路径、字段使用和命名评估记录于 [公开接口审计](api-audit.md)。

## 库与参考应用的边界

`HuxerUI::AgentKit` 是通用 Agent App 开发套件。它交付执行引擎、会话与事件契约、协议适配器和可选的默认对话组件。自绘界面的宿主可包含 `<huxerui/agentkit/runtime.h>` 并链接 `HuxerUI::AgentKitRuntime`。通过 `add_subdirectory` 接入时，可以设置 `HUXERUI_AGENTKIT_BUILD_VIEWS=OFF`，此时不构建对话组件，也不获取 Lib-Markdown。运行时仍使用 HuxerUI SDK 的应用级任务作用域。需要默认对话组件时，包含 `<huxerui/agentkit.h>` 并链接 `HuxerUI::AgentKit`。库的公开 API、构建和默认行为独立于 Preview 的助手、配置及资源。

`examples/preview` 是消费公开 API 的参考应用。应用负责工作台及其主题、导航和设置，注册代码、PPT、设计与通用助手，选择模型与凭据存储、授权范围及本地存储方案。补丁、Git、CTest、PPTX、设计画板和发布协作由 Preview 的工作流、工具及视图实现。其他宿主按自己的业务注册同类扩展。

Run 状态、耗时、工具记录和提供方公开摘要属于会话事件。使用默认对话的宿主可通过 `AgentChatLayoutStyle` 选择布局和活动区，通过 `AgentChatOptions::tool_label` 提供工具显示名，也可分别挂载 `AgentChatMessages` 和 `AgentChatComposer`。Preview 在应用层组合这些视图，并自行布局旁聊面板。自定义输入控件可以使用 `AgentChatComposerAnchor` 附着浮层。自绘聊天界面的宿主可直接订阅 `Conversation::ObserveEventsAsync`，在无聊天页的业务流程中也可使用相同的 Run 和权限语义。

逐文件差异的视图与具体补丁格式由宿主提供。库通过版本化产物和审批接口提供访问及执行约束。第三方宿主验收检查独立接入，Preview 验收检查参考应用自己的设置、渲染及跨领域流程。

核心测试与原生 Preview 共用 `support/` 中的 SQLite `SessionStore` 实现。会话事务、版本和回执使用相同的表与编码。Preview 单独管理导航和输入偏好。配置 `HUXERUI_AGENTKIT_BUILD_TESTS=ON` 和 `HUXERUI_AGENTKIT_BUILD_PREVIEW_TESTS=OFF` 可独立构建模型协议、委派、SQLite 及运行时回归。

## 阅读顺序

| 接入任务 | 对应章节 |
| --- | --- |
| 接入 Preview、宿主 UI 或定义新 Agent | [会话创建、列表和历史](#会话创建列表和历史)、[Run、事件和交互](#run事件和交互)、[Runtime 注册与宿主接入](#runtime-注册与宿主接入) |
| 实现 Preview 桌面交互 | [导航与工作区](#preview-导航与工作区)、[复制会话与 worktree](#复制会话与-worktree)、[来源选区与旁聊](#来源选区与旁聊)、[模型与权限选择](#preview-的模型与权限选择) |
| 注册对话组件和动作 | [自定义对话组件与动作](#自定义对话组件与动作) |
| 实现模型、上下文和工具 | [模型、上下文、工具和授权](#模型上下文工具和授权)、[协议与能力](#提供商协议与能力) |
| 注册和使用 Skill | [Skill 的来源、激活和资源](#skill-的来源激活和资源) |
| 编排多个会话与持续任务 | [跨会话协作与子 Run](#跨会话协作与子-run)、[项目任务、持续目标和共享上下文](#项目任务持续目标和共享上下文) |
| 实现产物管理和恢复 | [产物、分叉审阅、返工和回撤](#产物分叉审阅返工和回撤)、[持久化和恢复](#持久化和恢复) |

## 公共约定

```cpp
template<class Tag>
struct Id {
  std::string value;
  auto operator<=>(const Id&) const = default;
};

using AgentId = Id<struct AgentTag>;
using ModelId = Id<struct ModelTag>;
using ToolId = Id<struct ToolTag>;
using ToolCatalogId = Id<struct ToolCatalogTag>;
using SkillSourceId = Id<struct SkillSourceTag>;
using SkillId = Id<struct SkillTag>;
using ContextProviderId = Id<struct ContextProviderTag>;
using SessionId = Id<struct SessionTag>;
using RunId = Id<struct RunTag>;
using TurnId = Id<struct TurnTag>;
using MessageId = Id<struct MessageTag>;
using BlockId = Id<struct BlockTag>;
using QueueItemId = Id<struct QueueItemTag>;
using InteractionId = Id<struct InteractionTag>;
using ToolCallId = Id<struct ToolCallTag>;
using PlanId = Id<struct PlanTag>;
using CommandId = Id<struct CommandTag>;
using CommitId = Id<struct CommitTag>;
using PrincipalId = Id<struct PrincipalTag>;
using ProjectId = Id<struct ProjectTag>;
using WorkspaceId = Id<struct WorkspaceTag>;
using WorkspaceCopyPlanId = Id<struct WorkspaceCopyPlanTag>;
using ExecutionProfileId = Id<struct ExecutionProfileTag>;
using ResourceId = Id<struct ResourceTag>;
using ArtifactId = Id<struct ArtifactTag>;
using PartId = Id<struct PartTag>;
using CollaborationMessageId = Id<struct CollaborationMessageTag>;
using GoalId = Id<struct GoalTag>;
using ProjectTaskId = Id<struct ProjectTaskTag>;
using AttemptId = Id<struct AttemptTag>;
using SharedContextId = Id<struct SharedContextTag>;
using ForkReviewId = Id<struct ForkReviewTag>;
using CandidateId = Id<struct CandidateTag>;

using Revision = std::uint64_t;
using Sequence = std::uint64_t;
using Duration = std::chrono::milliseconds;
using TimePoint = std::chrono::sys_time<Duration>;

struct GoalRef {
  GoalId id;
  Revision version;
  auto operator<=>(const GoalRef&) const = default;
};

struct JsonValue {
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue, std::less<>>;
  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> value;
};

struct JsonSchema { JsonValue document; };

enum class ErrorCode {
  InvalidArgument, InvalidState, NotFound, PermissionDenied, Unsupported,
  Conflict, Busy, CursorExpired, AlreadyClosed, DeadlineExceeded,
  RateLimited, BudgetExceeded, WaitCycle, ProviderFailure, ProtocolError,
  PersistenceFailure, CommitUncertain, ResultUnknown, ScopeExpansionRequired,
  RuntimeClosed
};

struct ScopeExpansion {
  std::vector<PartId> affected_parts;
  bool whole_artifact;
  std::string summary;
};

struct FieldError {
  std::string path;
  std::string code;
  std::string message;
};

struct AgentError {
  ErrorCode code;
  std::string message;
  std::vector<FieldError> fields;
  std::optional<Revision> current_revision;
  std::optional<Duration> retry_after;
  std::optional<std::string> provider_code;
  std::optional<ScopeExpansion> proposed_scope;
};

template<class T> using AgentResult = huxerui::Result<T, AgentError>;
template<class T> using Async = huxerui::Task<AgentResult<T>>;

struct CommitStamp {
  CommandId command;
  CommitId commit;
  std::optional<SessionId> session;
  std::optional<Sequence> session_sequence;
  std::optional<ProjectId> project;
  std::optional<Revision> project_revision;
};

struct PendingApproval {
  SessionId session;
  InteractionId interaction;
  Revision version;
};

template<class T>
struct Receipt {
  CommitStamp stamp;
  std::variant<T, PendingApproval> outcome;
};

using CommandReceipt = Receipt<std::monostate>;

struct SessionCreated {
  SessionId session;
  std::optional<RunId> initial_run;
  std::optional<CollaborationMessageId> initial_request;
};

enum class DeliveryState { AwaitingApproval, Queued, Delivered, Rejected, Expired, Withdrawn };

struct RunAccepted { RunId run; };
struct InputQueued { QueueItemId item; };
struct RunSteered { RunId run; MessageId message; };
struct InteractionConsumed { InteractionId id; Revision version; };
struct InputRequested { InteractionId id; Revision version; };
struct ToolCallAccepted { ToolCallId call; };

struct MessageAccepted {
  CollaborationMessageId message;
  DeliveryState delivery;
  std::optional<RunId> target_run;
  std::optional<InteractionId> approval;
};

struct GoalUpdated { GoalRef goal; Revision revision; };

struct TaskUpdated { ProjectTaskId task; Revision revision; std::optional<AttemptId> attempt; };

struct SharedContextRef {
  ProjectId project;
  SharedContextId item;
  Revision version;
};

struct ChangePlanAccepted {
  std::optional<RunId> run;
  std::optional<ToolCallId> call;
  std::optional<QueueItemId> queue_item;
  std::optional<InteractionId> approval;
};

struct ForkReviewCreated {
  ForkReviewId review;
  std::vector<SessionId> branches;
};

using CommandValue = std::variant<std::monostate, SessionCreated, RunAccepted, InputQueued,
    RunSteered, InteractionConsumed, InputRequested, ToolCallAccepted, MessageAccepted, GoalUpdated, TaskUpdated, SharedContextRef,
    ChangePlanAccepted, ForkReviewCreated>;

enum class CommandState { AwaitingApproval, Scheduled, Applied, Rejected, Expired, Withdrawn, Failed };

struct CommandProgress {
  CommitStamp accepted;
  CommandState state;
  std::optional<CommitStamp> resolution;
  std::optional<CommandValue> result;
  std::optional<PendingApproval> approval;
  std::optional<AgentError> failure;
};

template<class T>
using Observer = std::function<void(AgentResult<std::shared_ptr<const T>>)>;

class Subscription {
public:
  Subscription(Subscription&&) noexcept;
  Subscription& operator=(Subscription&&) noexcept;
  ~Subscription();
  void Reset() noexcept;
};

using CommandLookup = std::variant<CommandId, PendingApproval>;

class CommandService {
public:
  Async<CommandProgress> ReadAsync(CommandLookup lookup);
  Async<Subscription> ObserveAsync(CommandLookup lookup, Observer<CommandProgress> observer);
};

CommandId NewCommandId();
```

- 每个请求的必填 ID 和版本须有效。空 ID、零版本以及超出 schema 或宿主大小限制的载荷返回 `InvalidArgument`。命令去重以解析后的字段和值比较，对象键顺序不影响判定。JSON 保留数组顺序，拒绝重复对象键及非有限数值。
- ID 比较使用完整值。工具、Agent 和提供器的注册 ID 由宿主命名；会话等运行记录的 ID 由运行时分配。修订用于条件修改，序号只在同一会话内排序。时间字段使用 UTC，调度用宿主时钟，持续时间和测试时钟可独立注入。
- 异步操作使用拥有生命周期的值和句柄。会话、项目、命令及权限状态在 HuxerUI 应用线程串行提交，调用和观察回调也在该线程。模型适配器和工具可将 I/O 或计算交给后台，状态仍由执行器提交。
- 应用级 `TaskScope` 持有已接受的运行。取消视图 Task 或销毁订阅只结束该调用方的等待或观察。命令提交后应用继续执行，停止工作须发送业务取消命令。回调收到的不可变快照可保留，切换会话后按 `SessionId` 校验迟到的 UI 更新。
- 状态命令的去重键由存储域、可信的调用主体和 `CommandId` 组成。用户调用以登录身份为范围，Agent 调用再绑定来源 `RunId`。比较同一键时包含操作、目标和规范化请求。相同请求返回原回执，载荷不同返回 `Conflict`。在接受前确定失败的请求不创建目标对象或调度工作。执行器仍可记录拒绝原因或结清已经到期的交互。回执和状态在同一事务提交。提交结果不明返回 `CommitUncertain`，调用方用原命令 ID 确认。回执仍受当前可见性限制。
- 回执表示已受理。初始结果可能是已登记的对象、队列项，或 `PendingApproval`。同一键重试仍返回原受理回执，经当前权限投影。`Commands()` 通过 `CommandProgress` 跟踪命令。推进到 `Applied` 时保存与该操作的 `Receipt<T>` 同类型的结果 `T`，含审批后分叉、项目任务领取、产物准备等复合回执。排队或待批准时，`result` 仅返回已登记的同类型结果，未知部分保持空。Run、调用或消息的最终业务结果由各自状态表示。批准后读取的结果不会替换原受理回执。失败、撤回或拒绝在命令进度中提供终态与可见原因。
- `CommandLookup` 使用发起方作用域下的 `CommandId`，或使用 `PendingApproval` 查询操作审批的命令。后一种查询按同一会话、交互版本和当前审阅权限解析，适用于审阅他人命令。必须按对象与字段的当前权限投影结果。无权访问时返回 `PermissionDenied` 或按策略隐藏为 `NotFound`。
- 观察操作在一次提交边界登记并读取初始快照，回调在订阅建立后交付初始值及后续最新快照。订阅可合并中间快照。失败或访问撤销产生一次错误并结束订阅，UI 清除对应可见投影。已交付给宿主的副本由宿主管理。观察者抛出的异常由宿主错误处理入口接收，仅结束该订阅。

`JsonSchema` 的运行时校验支持 `minLength`、`maxLength`、`minItems`、`maxItems`、`minimum` 与 `maximum`。字符串长度按 UTF-8 解码后的 Unicode 码点计算，数组按元素计数，数值边界包含端点。超出范围返回 `InvalidArgument`，字段错误附 JSON Pointer 路径及约束名称。表单校验失败保留原交互及版本，可更正后提交。

## 会话创建、列表和历史

```cpp
struct AgentRef {
  AgentId id;
  Revision version;
};

struct ModelProviderRef {
  ModelId id;
  Revision version;
  auto operator<=>(const ModelProviderRef&) const = default;
};

struct ExecutionProfileRef {
  ExecutionProfileId id;
  Revision version;
};

struct UserRunOptions {
  std::optional<ModelProviderRef> model;
  std::optional<ExecutionProfileRef> execution_profile;
};

struct RunStartBinding {
  ModelProviderRef model;
  std::optional<ExecutionProfileRef> execution_profile;
};

struct ToolRef {
  ToolId id;
  Revision version;
  std::optional<ToolCatalogId> catalog;
};

struct SkillRef {
  SkillSourceId source;
  SkillId id;
  Revision version;
  auto operator<=>(const SkillRef&) const = default;
};

struct SkillSummary {
  SkillRef ref;
  std::string name;
  std::string description;
};

struct ArtifactRef {
  ArtifactId id;
  Revision version;
};

struct MessageRef {
  SessionId session;
  MessageId message;
  Revision version;
};

struct ContentTypeRef {
  std::string id;
  Revision schema_version;
};

struct ResourceRef {
  ResourceId id;
  std::string version;
};

using SourceRef = std::variant<ResourceRef, MessageRef, ArtifactRef, SharedContextRef>;

struct SourceTextRange {
  std::variant<BlockId, PartId> leaf;
  std::uint64_t begin_utf8;
  std::uint64_t end_utf8;
};

struct SourceSlice {
  SourceRef source;
  std::vector<PartId> parts;
  std::optional<SourceTextRange> text_range;
};

struct SideChatConclusion {
  std::string text;
  std::vector<SourceSlice> sources;
};

struct QueuedConclusionRef {
  QueueItemId item;
  std::uint64_t index;
};

struct UnappliedSteerConclusionRef {
  CommandId command;
  std::uint64_t index;
};

struct SendInput {
  std::string text;
  std::vector<SourceSlice> context;
  std::vector<ArtifactRef> attachments;
  std::vector<SkillRef> skills;
  std::vector<SourceSlice> side_chat_conclusions;
  std::vector<QueuedConclusionRef> recovered_conclusions;
  std::vector<UnappliedSteerConclusionRef> recovered_steer_conclusions;
  std::vector<ArtifactId> staged_attachments;
};

enum class SessionRetention { Stored, Ephemeral };
enum class SessionLifecycle { Open, Archived, Deleting };
enum class SessionTitleSource { Placeholder, FirstInput, Optimized, Manual };

struct SessionOptions {
  AgentId agent;
  std::optional<ProjectId> project;
  std::optional<WorkspaceId> workspace;
  std::string title;
  SessionRetention retention;
};

struct HistoryBoundary {
  SessionId session;
  std::variant<MessageId, RunId> anchor;
  Sequence through_sequence;
};

struct WorkspaceCopyPlanRef {
  WorkspaceCopyPlanId id;
  Revision version;
};

struct WorkspaceChoice {
  enum class Kind { Existing, CopyFromSource };
  Kind kind;
  WorkspaceId workspace;
  std::optional<WorkspaceCopyPlanRef> copy_plan;
};

struct ReplyExpectation {
  std::optional<JsonSchema> schema;
  std::vector<std::string> artifact_types;
  std::optional<TimePoint> reply_by;
};

struct TaskExecutionRef {
  ProjectId project;
  ProjectTaskId task;
  AttemptId attempt;
};

struct WorkScope {
  std::vector<SourceSlice> inputs;
  std::vector<ResourceId> writable_resources;
  std::vector<ToolId> tools;
};

struct WorkRequest {
  SendInput input;
  std::optional<WorkScope> scope;
  bool request_start;
  std::optional<ReplyExpectation> reply;
  std::optional<TaskExecutionRef> task;
  bool cancel_on_goal_revision = false;
};

struct SideChatSeed {
  SessionId source;
  std::vector<SourceSlice> selected;
};

struct CreateSessionRequest {
  SessionOptions options;
  std::variant<std::monostate, SendInput, WorkRequest> initial;
  std::optional<SideChatSeed> side_chat;
  UserRunOptions user_start{};
};

struct ForkSessionRequest {
  HistoryBoundary from;
  SessionOptions options;
  std::optional<WorkspaceChoice> workspace;
  std::optional<WorkRequest> initial;
};

enum class RunState {
  Running, WaitingForInput, WaitingForReply, WaitingForDependency,
  Cancelling, Succeeded, Failed, Cancelled
};

struct SessionSummary {
  SessionId id;
  std::string title;
  SessionTitleSource title_source;
  AgentRef agent;
  Revision binding_revision;
  Revision metadata_revision;
  std::optional<ProjectId> project;
  std::optional<WorkspaceId> workspace;
  SessionLifecycle lifecycle;
  TimePoint last_activity;
  std::optional<RunId> active_run;
  std::optional<RunState> run_state;
  std::size_t pending_interactions;
  std::size_t queued_inputs;
};

struct PageCursor { std::string value; };

struct SessionFilter {
  std::optional<ProjectId> project;
  std::optional<AgentId> agent;
  bool include_archived;
  bool include_ephemeral;
  std::size_t page_size;
};

struct SessionPage {
  Revision revision;
  Revision order_revision;
  std::vector<SessionSummary> sessions;
  std::optional<PageCursor> next;
};

struct TextBlock { std::string text; bool markdown; };

struct AgentTransferred {
  std::optional<AgentRef> from;
  std::optional<AgentRef> to;
  Revision binding_revision;
  std::optional<std::string> handoff_summary;
};
struct ArtifactBlock {
  ArtifactRef artifact;
  std::string name;
};
struct InteractionBlock { InteractionId interaction; };
struct ToolBlock { ToolCallId call; };
struct CustomBlock {
  ContentTypeRef type;
  JsonValue data;
  std::string fallback;
  std::vector<InteractionId> interactions;
  std::vector<ArtifactRef> artifacts;
};

struct CustomBlockRef {
  MessageRef message;
  BlockId block;
};

struct ContentBlock {
  BlockId id;
  std::variant<TextBlock, ArtifactBlock, InteractionBlock, ToolBlock,
      CustomBlock, AgentTransferred, SideChatConclusion> body;
};

enum class MessageAuthor { User, Agent, Tool, Collaboration, System };
enum class MessageState { Streaming, Complete, Interrupted };

struct Message {
  MessageRef ref;
  Sequence sequence;
  std::optional<RunId> run;
  MessageAuthor author;
  MessageState state;
  std::vector<ContentBlock> blocks;
};

struct HistoryQuery {
  std::size_t page_size;
  std::optional<Sequence> through_sequence;
  std::optional<PageCursor> before;
};

struct HistoryPage {
  Sequence through_sequence;
  std::vector<Message> messages;
  std::optional<PageCursor> older;
};

struct SessionHistoryWindow {
  Sequence first;
  std::size_t preceding = 0;
};

struct RenameSession { std::string title; };
struct ArchiveSession {};
struct UnarchiveSession {};
struct DeleteSession {};
using SessionMetadataChange = std::variant<RenameSession, ArchiveSession, UnarchiveSession, DeleteSession>;

class Conversation;

class SessionService {
public:
  Async<Receipt<SessionCreated>> CreateAsync(CommandId command, CreateSessionRequest request);
  Async<Receipt<SessionCreated>> ForkAsync(CommandId command, ForkSessionRequest request);
  Async<void> AbandonWorkspaceForkAsync(CommandId command, ForkSessionRequest request, WorkspaceId allocated);
  Async<std::shared_ptr<Conversation>> OpenAsync(SessionId session);
  Async<SessionPage> ListAsync(SessionFilter filter, std::optional<PageCursor> cursor);
  Async<Subscription> ObserveListAsync(SessionFilter filter, Observer<SessionPage> observer);
  Async<HistoryPage> ReadHistoryAsync(SessionId session, HistoryQuery query);
  Async<CommandReceipt> UpdateMetadataAsync(CommandId command, SessionId session,
      Revision expected, SessionMetadataChange change);
};
```

### 创建和首条消息

| 请求 | 提交边界 | 结果 |
| --- | --- | --- |
| 空会话 | 登记会话、助手绑定和目录摘要 | 返回会话 ID，后续通过 `Conversation::SendAsync` 启动 Run |
| 用户首条消息 | 一起登记新会话、用户消息和活动根 Run，提交后调度 | 返回会话 ID 和 Run ID |
| 初始协作任务 | 一起登记新会话和一条可关联回复的工作请求，按接收策略审批、排队或启动 | 返回会话 ID 和请求 ID。已启动时附 Run ID |
| 分叉 | 核对来源可见性、已提交的历史边界和目标工作区授权 | 继承边界内获准的历史和版本引用，后续消息和执行归新会话 |
| 旁聊 | 核对源会话、所选片段及其版本和授权 | 新建临时会话，带入获准的选区 |

Preview 在发出首条消息前保存界面草稿，宿主默认选通用助手，用户可改选代码、PPT 或设计助手。点击发送后以选中的 `AgentId`、工作区、完整输入和 `user_start` 调用 `Sessions().CreateAsync`。收到 `SessionCreated` 后，宿主提交可重试的归类关联，用户仍在提交时的草稿视图则用回执中的 `session` 调用 `OpenAsync`。结果为 `PendingApproval` 时，通过已有的授权入口展示交互，用原命令 ID 读取或订阅 `Commands()`，在 `Applied` 后从 `CommandProgress::result` 取得 `SessionCreated` 并按相同规则处理。拒绝、过期、撤回或失败时保留草稿及各项选择。宿主可为用户直接发起的新建预先授予创建权限。

Preview 在窗口内按已授权的 `WorkspaceId` 保留未发送的新建草稿。「无工作区」有自己的草稿。侧栏打开工作区或在新建页改选工作区时，恢复目标草稿的输入、助手、归类、模型、权限、Skill、报告和附件。创建请求固定提交时的工作区和输入。成功只消费对应草稿内已提交的内容。等待回执期间已切换视图时，新会话仍出现在列表，当前视图继续保留。

直接创建和审批后创建采用同一个 `SessionCreated` 契约。创建事务保存新会话 ID、首个 Run ID 及初始请求 ID 中已登记的部分。异步启动的请求用其请求 ID 查询后续 Run。创建权限的审批由调用方可访问的已有会话承载，无此入口时返回 `PermissionDenied`。协作请求的接收确认属于已创建的目标会话。重复点击或结果不明时沿用命令 ID，进度仍能查询原创建结果。打开失败可单独重试。读取回执时按当前权限过滤关联 ID，会话不可见时按权限规则隐藏结果。工作区由宿主解析，必要时按命令 ID 幂等准备，宿主清理未提交的资源。

用户首条消息由用户或代表该用户的可信宿主提交。运行中的 Agent 用初始协作任务，使来源、接收确认及回复可追溯。各 Run 固定启动时的 `AgentRef`。用户通过显式转交变更以后 Run 的助手。分叉点须为已提交且已完成的消息或 Run，对应的序号由运行时核验。新会话取得获准的历史和引用，原 Run、审批及收件箱继续归原会话。

### 会话标题

`SessionOptions::title` 非空时使用提供的标题并标为 `Manual`。空会话默认使用宿主配置的 `RuntimeOptions::untitled_session_title`，标为 `Placeholder`。首条用户输入或可向目标会话显示的初始协作输入提交时，执行器从获准用于该会话标题的正文提取单行短文本并标为 `FirstInput`。标题长度按宿主限制裁剪并保留完整 UTF-8 字符，目录和对话使用同一已提交标题。新会话的标题、首条输入与关联 Run 或工作请求在同一事务登记。先建空会话时，首次写入该输入的事务也更新标题。纯附件、只有空白或正文不可用于标题时沿用占位。分叉引用的历史不计为新会话的首条输入。

首次提交后，运行时可在应用级任务作用域调用宿主配置的 `generate_session_title`。请求只携带首条获准的文本和必要标识。自定义生成器使用云端模型时，宿主先核对实际接收方的授权。该工作独立于会话 Run。回写只在会话仍可更新、首次输入相同、标题来源仍为 `FirstInput` 且 `metadata_revision` 匹配时提交，并标为 `Optimized`。超时、空结果、失败、关闭或重启时已有标题可继续使用，无需恢复未结的优化。标题变更更新会话快照和目录摘要，沿用原活动时间与列表顺序。

手动改名通过 `Sessions().UpdateMetadataAsync(command, session, expected, RenameSession{title})` 提交并标为 `Manual`，任何迟到的优化均丢弃。改名与标题优化均递增 `metadata_revision`。如果用户编辑期间只有自动标题抢先提交，Preview 在核对最新修订及标题来源后可用新命令重提一次，保留用户输入。其他并发改名或元数据变更按 `Conflict` 处理，保留改名草稿供用户复核。

### 目录和历史观察

- Preview 按同一查询观察所有普通会话，使用 `SessionId` 作为列表键。列表按最近活动和 ID 排序。摘要在创建、归档、转交、Run 状态、待审批和排队变化时更新，包括当前未打开的会话。最近活动在提交新消息或启动 Run 时更新，正文流式增量与工具进度只更新当前会话投影。
- `ObserveListAsync` 交付该查询首页和目录修订。游标绑定查询、可见身份和排序修订。成员或排序变化后，旧游标返回 `CursorExpired`，界面从新首页重新拼接。摘要内容更新时保留排序游标，已加载的后续页可按同一查询重新读取。打开或关闭一条会话只改变观察关系和界面草稿。归档与删除先检查当前 Run，活动会话需由调用方先停止或等待收尾。归档暂停自动调度，未消费的输入与来信仍受各自期限约束，恢复会话后才可继续。删除结清待处理交互和请求，并按宿主保留规则处理已有分叉与产物引用。
- 历史分页以调用方指定的已提交位置为上界，省略时在读第一页时固定上界，向早期消息分页，各页保持消息顺序。新事件走会话快照；使用稳定消息 ID 合并历史和当前投影。临时会话、未授权会话和撤销访问后的会话根据当前查询与权限呈现。
- 普通会话快照和默认订阅投影最近 50 条可见消息。`Conversation::ObserveAsync` 可接收 `SessionHistoryWindow`，在同一个提交边界增加从 `first` 开始的消息及之前最多 `preceding` 条可见消息。`AgentChat` 固定当前窗口起点，用最新快照中首条消息的位置加载更早一页，并保留原页边界的阅读位置。每次投影均重新检查来源和产物权限。流式输出扩展当前窗口，阅读早期消息时保持位置，处于末尾时跟随新增内容。

### Preview 导航与工作区

- 宿主把系统目录选择器返回的目录解析为已授权的 `WorkspaceId`。用户取消时保留原视图和草稿。目录不可用或授权失败时提示重新选择。打开文件夹只在新建草稿中预选工作区；实际选定的 ID 随 `CreateSessionRequest::options.workspace` 提交。祖先目录、链接解析后的目标及文件、进程、网络能力由宿主按实际操作授权。既有会话仍使用已绑定的工作区。
- Web Preview 使用浏览器返回的目录句柄识别工作区，以目录名称显示来源。同一目录重新选择时沿用原 `WorkspaceId`，同名目录分别登记。句柄由宿主保存在浏览器本地，恢复会话后通过用户操作申请仍需的权限，浏览和读取时重新核对访问状态。选择目录只登记工作区。用户从所属目录显式选择报告后，宿主才读取文件并按项目权限发布相应的共享资料。
- 归类是按本地用户保存的宿主导航数据。宿主按 `SessionId` 合并 `ObserveListAsync` 的可见目录与归类、折叠、排序和草稿状态。移动到归类、移回根列表或移除归类只更新该投影。归类保存失败时保留可重试的 UI 修改，已创建的会话可从根列表打开，沿原会话 ID 补写关联。删除后清理已删除会话的关联，分页未加载或暂不可见的会话不参与清理。
- 窗口重新激活时刷新已保存的归类。归类写入按最新已提交数据应用单项修改，不同归类或不同会话上的操作可合并。归类重命名和移动会话核对操作开始时的名称或归属。目标状态已经提交时视为成功，其他竞争修改返回 `Conflict` 并刷新可见归类。目标归类已删除时返回 `NotFound`。写入失败后保留该修改和后续修改，重试从失败项开始。放弃时仅移除失败项，重新读取后继续尝试后续修改。
- 右键、行尾入口和键盘使用相同的会话操作。重命名和删除分别经 `UpdateMetadataAsync` 提交 `RenameSession` 或 `DeleteSession`，使用当前 `metadata_revision`。删除活动会话返回 `Busy`，由用户停止或等待。提交后刷新目录与归类。文件、已导出产物及工作区目录由宿主的独立操作管理。
- 统一待办通过 `Interactions().ObservePendingAsync` 观察当前身份可处理的请求。侧栏显示同一投影中的待办数量，仍有下一页时使用 `50+` 等总数下界。打开待办页可继续分页，订阅失败时可在该页重试。宿主用返回的 `SessionId` 打开会话，将 `InteractionId` 和本次打开的序号作为 `AgentChatOptions::interaction_request`（`AgentChatInteractionRequest`）传入。`AgentChat` 从该会话快照定位请求，使用最新版本渲染表单或审批。序号变化允许再次打开同一请求。请求已处理时展示关闭状态和仍可见的审批预览。点击返回后保留对话草稿。
- 短报告在消息中展开。长差异、幻灯片和设计稿由同窗口审阅页呈现。宿主用会话 ID 和原消息位置保存返回锚点，用 `ArtifactRef` 及文件、页或节点 ID 保存审阅位置。独立窗口采用同一只读预览引用和会话订阅。审批从该会话的最新 `InteractionSnapshot` 取得 ID 与版本，经 `SubmitAsync` 提交。另一个窗口已处理请求或计划换版后刷新状态，保留获准查看的原预览。
- 从审批附件进入审阅时，`AgentChatReviewRequest` 同时带 `ArtifactRef` 和 `AgentChatApprovalRef`（交互 ID 与版本）。界面依据最新快照判断该绑定是否仍可提交，提交时沿用已审阅的版本。宿主可通过 `AgentChatOptions::page` 获得当前审阅及其版本绑定，并用同一会话和请求构建独立审阅页。设置 `review_only` 可显示单独的审阅内容。宿主负责打开和关闭窗口。从继承历史打开附件时可携带 `history_first`，使独立窗口按原消息位置取得获授权的投影。PPT 页码或设计节点随打开请求传入，各窗口各自保存后续选中位置。各入口的引用和旁聊结论进入同一会话草稿。预览授权撤回后停止显示对应内容。

### 复制会话与 worktree

Preview 的「复制对话」和「复制到 worktree」都调用 `Sessions().ForkAsync`。宿主从用户可见的已提交历史取得 `HistoryBoundary`，确认时固定该边界。源会话正在运行时可以选最近的已完成消息或 Run；找不到合格边界时不提交复制。普通复制使用 `WorkspaceChoice::Kind::Existing`，其中 `workspace` 是目标工作区。没有绑定工作区的源会话可以原样分叉，此时 `ForkSessionRequest::workspace` 为空。由分叉请求中的工作区选择确定实际绑定。`ForkSessionRequest::options.workspace` 非空或 `Existing` 携带 `copy_plan` 时返回 `InvalidArgument`。

代码会话关联可用 Git 仓库时，宿主以源仓库、用户选择的已提交 commit、目标目录和新分支准备不可变预览。预览由宿主持有，关联当前调用主体、源工作区与 `HistoryBoundary`，并具有效期。Preview 展示完整基线及目标，由用户确认后提交 `WorkspaceChoice{WorkspaceChoice::Kind::CopyFromSource, source_workspace, copy_plan}`。普通复制以及使用已有工作区的分叉不带 `copy_plan`。宿主具备为特定产物复制资源的能力时也可沿用 `CopyFromSource`。只打开预览时不创建或写入目录。

| 对象 | 分叉结果 |
| --- | --- |
| 对话历史 | 共享截至固定边界的获准消息和产物版本引用。在目标工作区按当前授权重新装配，历史引用仍指向原产物版本。 |
| Git 内容 | 从已确认 commit 分配干净 worktree 及新分支。源工作区的未提交修改、暂存、未跟踪文件和未应用补丁继续留在源侧；若用户要复用其中的内容，先独立准备、审阅并应用到目标工作区。 |
| 新会话配置 | Preview 预选原助手和仍适用的模型，使用目标工作区的授权及默认 `Ask` 偏好。归类可沿用源会话，也可由用户改选。工作区 Grant、执行预设授权、模型续轮句柄和凭据由各自原绑定管理。 |
| 执行与协作 | 复制命令只创建会话。源 Run、队列、审批、草稿、等待、收件箱、Goal 和项目任务领取按原归属管理。当前项目获准时，新会话可以独立访问共享项。 |

继承的产物块保留原消息与 `ArtifactRef` 版本。普通历史读取和订阅按来源会话、产物权限及副本的上下文授权生成投影。`Conversation::PreviewArtifactAsync` 在当前会话内核对引用归属，并在宿主返回预览后复核权限。发往模型的继承引用默认使用获准的名称、类型和版本；显式把引用放进 `SendInput::context` 时，同一版本的结构化预览还须通过接收模型和工作范围的授权。授权收紧后，后续轮次按可见历史重新组装。

运行时确认源历史可见、`ForkSession` 及目标授权后，将 `command`、预分配的会话 ID 和 `fork_from` 交给 `WorkspaceResolver`。宿主依据命令核对预览及基线，在创建临界区检查仓库身份、commit 可用性、分支和规范化目标路径。固定 commit 仍可用时，源 HEAD 后续移动不改变已确认基线。计划换版、路径或分支被占用返回 `Conflict`，过期返回 `DeadlineExceeded`，工作区不可用返回 `NotFound` 或 `Unsupported`，撤权返回 `PermissionDenied`。同一预览只允许由一个命令消费。同一命令重放须取回同一个已分配工作区。

宿主先持久登记命令与准备目录的归属，再创建 worktree。请求指定 `CopyFromSource` 时，解析成功须返回按计划分配的目标 `WorkspaceId`；`nullopt` 只用于允许无工作区的请求。运行时确认目标绑定及访问范围后提交会话和来源；Preview 收到 `SessionCreated` 才打开副本。已明确失败时，宿主只回收该命令独占、尚未被使用的准备资源。遇到 `CommitUncertain` 时按原命令查询，确认记录后再重放或清理。内含用户修改、已被其他会话使用或归属未知时保留目录并展示待核对状态。源会话从这些步骤独立读写。

回收前，宿主用用户侧 `AbandonWorkspaceForkAsync` 提交原命令、原请求和已分配的工作区。Runtime 核对源会话修改权限和全量会话记录，在同一个持久存储事务封存原命令、计划和目标工作区 ID。其他会话已绑定目标、原命令已提交或仍在处理时，分别给出冲突或忙碌原因。提交结果仍未知时保留 checkout。结清成功后，Preview 复查仓库、规范路径、分支、commit、Git 归属标记和工作区登记。宿主记录回收进度，将干净 checkout 移到同一父目录内由计划 ID 标识的隔离路径，再检查已跟踪、未跟踪和被忽略的文件。隔离路径被占用或发现额外内容时，保留目录并报告冲突。宿主用普通 Git 移除命令回收干净 checkout，只移除对应的工作区登记，保留 Git 分支。重启时按原命令及持久归属记录续清。

### 来源选区与旁聊

- `SourceSlice::source` 固定到一个已提交的版本。结构化选区用 `parts` 指定可复核的 `PartId`，例如 diff 文件及分段、PPT 的页或元素、设计节点。文本选区用 `text_range` 和空的 `parts`，消息以 `BlockId` 指定正文块，产物等其他来源以 `PartId` 指定单个可提取的文本片段。区间是规范 UTF-8 内容中的半开字节区间，端点须在完整字符边界，起点小于终点，且终点不得超过片段长度。消息文本以对应版本的 `TextBlock::text` 为准，宿主产物适配器为该版本提供稳定的文本与定位映射。跨片段选择按阅读顺序拆成多个切片。两字段皆空表示整个可引用单元。
- 渲染器将屏幕选择映射为上述切片，并回显片段及版本。流式正文须等到该版本封存后再提交引用，过早提交返回 `InvalidState`。版本失效返回 `Conflict`，坏区间或叶子类型返回带字段路径的 `InvalidArgument`，适配器无法精确映射返回 `Unsupported`。如能安全提供更粗的有效单元，界面让用户先看清范围再以新命令提交。提供器和模型装配保留切片边界。
- `TextBlock::markdown` 表示正文可以排版显示。Preview 可在已完成的排版回复中直接选取文本、引用和旁聊。草稿和旁聊回显排版后的选段，来源绑定同一 `MessageRef`、`BlockId` 和原始 `TextBlock::text` 的 UTF-8 半开字节区间。选段跨越链接语法等未显示的字节时拆成多个切片，解码后的实体指向原始实体。宿主若提供不含来源映射的 Markdown 渲染器，仍可切到「选择原文」。
- `ArtifactStore::selection` 用已经授权的版本和宿主存储句柄读取规范文本。宿主按 `parts` 的顺序返回指定片段，空列表表示请求整个产物。文本区间只请求所属片段，运行时按返回的 UTF-8 文本校验边界并切片。宿主遵守 `byte_limit`，不具备准确映射时返回 `Unsupported`。运行时在读取返回后再次核对权限及版本。
- `Conversation::ReadSelectionAsync` 供审阅界面解析当前会话内可见的消息或产物选区，返回该版本的准确文本。`ReadSelectionsAsync` 按请求顺序返回一组文本，最多 64 个切片、合计 1 MiB。任一切片失效或读权限变化会使整次读取失败。运行时按绑定的用户身份检查会话、来源和选区权限，读取返回后复核。直接读取旁聊助手的回复时也复核原始选区，来源读权限撤销后返回 `PermissionDenied`。实际发送或创建旁聊时仍按目标会话及模型核对投递授权。
- 同一会话内选中文字可使用「扩展选区」记住一端，再从已加载的历史里选取另一端。两个端点之间的已完成正文按阅读顺序拆为切片，首尾保留所选的 UTF-8 边界。可从任一端开始。Markdown 端点和中间正文沿用同一来源映射，链接地址与排版语法不进入切片。旁聊弹窗按正文块展示排版文本和对应版本。引用把同一组切片放入主草稿，发送时依次提交 `source` 和 `additional_sources`。关闭旁聊恢复原范围及端点动作的焦点。
- 默认对话组件将引用和旁聊按钮显示在所选正文块下方，收起选区时隐藏。关闭旁聊后按原切片恢复选区和按钮焦点。「引用到输入框」只在原会话草稿添加 `SendInput::context` 引用。「旁聊」先保留原视图及选区。用户提交首个追问时，宿主使用 `Sessions().CreateAsync`、`SessionRetention::Ephemeral`、`SideChatSeed{source_session, selected}` 和首条 `SendInput`。旁聊要求非空选区及临时保留。运行时验证选区在源会话的可见范围内，并按实际旁聊模型复核 `ReadContext` 与 `SendToModel`。Preview 使用宿主配置的受限助手、空工作区、独立 Run 与预算，`user_start.execution_profile` 留空。业务上下文限于这组选区及旁聊自身历史。切片各自带引用标签，以 `ModelContent::Role::Context` 按阅读顺序放在最近一条用户问题之前。标签用来区分所选资料，正文采用源版本的准确选段。只读工具只解析这组选区。主对话的批准、正在运行的任务和权限预设均由主对话持有。
- `SessionOrigin::side_chat_selection` 保存内存中的定位关系。运行时按权限逐项投影；源会话不可见时省略 ID 和位置。消息和产物旁聊弹窗均订阅源会话。访问修订变化或源会话不可读时，弹窗收起已显示的选段、回复及操作，提示关闭并重新选取。关闭弹窗回到原会话、页码或文件及焦点。默认对话组件在同一 Runtime 内按源会话、来源版本和选区保留旁聊与未发送输入。重开同一选区恢复该旁聊并聚焦问题或追问输入框。普通会话列表默认过滤临时会话。
- 「带回结论」把已完成的旁聊回复或其中选段放进源会话草稿，同时保存引用和选区预览。Preview 优先带回当前选中的回复片段，未选时带回最近一条已完成的助手回复。此时只更新草稿。用户明确发送时，由 `SendAsync`、`SteerAsync` 或 `QueueAsync` 携带 `SendInput::side_chat_conclusions`。每个切片指向当前 Runtime 内已提交的旁聊 Agent 回复的纯文本部分，且该旁聊的源会话须为目标会话。该字段只接受用户或代表该用户的可信宿主在这三个入口提交，其他输入须留空。
- 运行时在受理时校验回复版本、来源选区及分享权限，从已验证的切片生成 `SideChatConclusion`，包括实际带回的文本与原旁聊的来源切片。在保存消息、队列或需批准的待处理命令时同事务登记快照，从归一化的 `SendInput` 移除临时回复引用，保留原请求的命令去重摘要。主消息经 `ContentBlock` 投影结论，队列、宿主编排和补充检查点经各自的 `conclusions` 获取内容。批准后执行、排队消费和发往模型前按当前主体及接收方复核关联来源。结论按引用数据装配到最近一条用户问题之前，并在模型请求中标注为旁聊结论。持久化记录保存原结论及其来源。权限过滤同时作用于结论正文及其来源。已提交或入队的结论可随普通会话恢复；尚在草稿的临时引用在 Runtime 重启后显示失效，可让用户明确移除并编辑草稿。

## Run、事件和交互

```cpp
enum class RunTrigger { UserInput, Retry, Regenerate, Collaboration, ProjectTask, Goal, Delegation, ArtifactChange };
enum class RunEndReason {
  Completed, Failed, UserCancelled, ParentCancelled, Deadline, Budget,
  Interrupted, ReplyMissing, ToolOutcomeUnknown, GoalRevised
};
enum class WaitKind { Input, Reply, Dependency, Delegation };

struct RunWait {
  WaitKind kind;
  std::variant<InteractionId, CollaborationMessageId, TaskExecutionRef, CommandId> subject;
  std::optional<TimePoint> deadline;
};

struct Usage {
  std::uint64_t input_tokens;
  std::uint64_t output_tokens;
  std::uint64_t model_turns;
  std::uint64_t tool_calls;
};

struct Budget {
  std::optional<std::uint64_t> tokens;
  std::optional<std::uint64_t> model_turns;
  std::optional<std::uint64_t> tool_calls;
  std::optional<std::uint64_t> context_bytes;
  std::optional<Duration> deadline;
};

struct RunCompletion {
  std::string summary;
  std::optional<JsonValue> structured;
  std::vector<ArtifactRef> artifacts;
};

enum class SkillActivationTrigger { User, Model, Host };

struct SkillActivation {
  SkillRef skill;
  SkillActivationTrigger trigger;
};

enum class SteerState { Pending, Applied, NotApplied };

struct RunSteerSnapshot {
  MessageId message;
  SteerState state;
  std::optional<AgentError> reason;
  std::optional<CommandId> recovery;
};

struct RunSteerInput {
  MessageId message;
  SendInput input;
  std::vector<SideChatConclusion> conclusions;
};

struct RunSnapshot {
  RunId id;
  SessionId session;
  AgentRef agent;
  ModelProviderRef model;
  RunTrigger trigger;
  std::optional<RunId> parent;
  std::optional<RunId> retry_of;
  std::optional<CollaborationMessageId> work_request;
  std::optional<TaskExecutionRef> task;
  std::optional<GoalRef> goal;
  RunState state;
  std::vector<RunWait> waits;
  std::vector<RunSteerSnapshot> steering;
  std::vector<SkillActivation> active_skills;
  Usage usage;
  std::optional<RunEndReason> end_reason;
  std::optional<RunCompletion> result;
  std::optional<AgentError> failure;
  std::optional<ExecutionProfileRef> execution_profile;
};

struct FormRequest {
  std::string title;
  JsonSchema schema;
  JsonValue presentation;
  std::optional<TimePoint> expires_at;
};

enum class ToolEffect { Read, StageArtifact, WriteResource, ExternalSubmit };

struct TargetBaseline {
  ResourceId target;
  std::optional<ResourceRef> expected;
};

struct GoalEditImpact {
  GoalRef previous;
  std::optional<RunId> retiring_run;
  std::vector<ToolCallId> in_flight_calls;
  std::vector<RunId> continuing_runs;
  std::vector<TaskExecutionRef> continuing_attempts;
  std::vector<CollaborationMessageId> withdrawn_requests;
  std::vector<TaskExecutionRef> released_attempts;
  std::vector<InteractionId> invalidated_interactions;
  bool resets_completion_review;
  std::vector<RunId> retiring_linked_runs;
};

struct GoalEditPreview {
  GoalEditImpact impact;
  std::string token;
};

struct ApprovalPreview {
  std::string title;
  std::string summary;
  std::vector<ArtifactRef> artifacts;
  std::vector<TargetBaseline> targets;
  std::optional<GoalEditImpact> goal_edit;
};

struct ToolApproval {
  ToolCallId call;
  PlanId plan;
  ApprovalPreview preview;
};

enum class ApprovalDecision { Approve, Deny };
enum class InteractionState { Pending, Submitted, Approved, Denied, Expired, Cancelled, Invalidated };

struct OperationApproval {
  CommandId pending_command;
  ApprovalPreview preview;
};

using InteractionBody = std::variant<FormRequest, ToolApproval, OperationApproval>;

struct InteractionSnapshot {
  InteractionId id;
  Revision version;
  SessionId session;
  std::optional<RunId> run;
  PrincipalId recipient;
  InteractionState state;
  InteractionBody body;
  std::optional<TimePoint> expires_at;
};

struct ArtifactAction;

struct CustomBlockAction {
  CustomBlockRef target;
  std::string action;
  JsonValue payload;
};

struct DismissForm {};

struct InteractionReply {
  InteractionId id;
  Revision expected_version;
  std::variant<JsonValue, DismissForm, ApprovalDecision> response;
};

struct InteractionOutcome {
  InteractionState state;
  std::optional<JsonValue> value;
};

enum class QueueItemState { Queued, Started, Withdrawn, Rejected };

struct QueuedInput {
  QueueItemId id;
  std::variant<SendInput, CollaborationMessageId, CommandId> input;
  QueueItemState state;
  std::optional<RunId> run;
  std::optional<RunStartBinding> user_start;
  std::vector<SideChatConclusion> conclusions;
  std::optional<AgentError> reason;
};

struct QueuedDraftReference {
  std::optional<std::variant<SourceSlice, QueuedConclusionRef, UnappliedSteerConclusionRef, ArtifactRef>> reference;
  std::string text;
};

struct QueuedDraft {
  std::string text;
  std::vector<QueuedDraftReference> references;
  std::vector<SkillRef> skills;
};

using RecoveredDraft = QueuedDraft;

enum class ToolCallState {
  Preparing, Reviewing, AwaitingApproval, Ready, Executing, Succeeded, Denied,
  Failed, Conflict, PartiallyApplied, Cancelled, ResultUnknown
};

enum class ResourceEffectState { Applied, Unchanged, Unknown };
enum class UndoSupport { None, RestoreVersion, Compensate };

struct UndoRef {
  std::string handle;
  UndoSupport support;
};

struct ResourceEffect {
  ResourceId resource;
  ResourceEffectState state;
  std::optional<ResourceRef> before;
  std::optional<ResourceRef> after;
  std::optional<UndoRef> undo;
  std::optional<AgentError> error;
};

enum class ToolReviewDecision { Approve, Deny, DeferToUser };
enum class ToolReviewOutcome { Approved, Denied, Escalated, Invalidated, Pending };

struct ToolReviewRecord {
  PlanId plan;
  std::string reviewer;
  Revision reviewer_version;
  ToolReviewOutcome outcome;
  std::string reason;
};

struct ToolCallSnapshot {
  ToolCallId id;
  RunId run;
  ToolRef tool;
  std::optional<PlanId> plan;
  ToolCallState state;
  std::optional<ToolReviewRecord> review;
  std::string progress;
  std::vector<ResourceEffect> effects;
  std::vector<ArtifactRef> artifacts;
  std::optional<AgentError> reason;
  std::optional<ToolCallId> reverts;
  std::vector<CommandId> source_changes;
  std::optional<std::vector<TargetBaseline>> issued_targets;
  std::optional<CommandId> reconciliation;
};

enum class RequestState { AwaitingReply, Replied, Failed, Cancelled };

struct ActionParty {
  enum class Kind { User, Agent, Host };
  Kind kind;
  PrincipalId principal;
  std::optional<SessionId> session;
  std::optional<RunId> run;
  std::optional<ToolCallId> call;
  std::optional<AgentRef> agent;
};

enum class SessionOriginKind { Created, Forked, SideChat };

struct SessionOrigin {
  SessionOriginKind kind;
  std::optional<ActionParty> creator;
  std::optional<SessionId> source_session;
  std::optional<HistoryBoundary> fork_point;
  std::vector<SourceSlice> side_chat_selection;
};

struct CollaborationSummary {
  CollaborationMessageId id;
  ActionParty from;
  SessionId to;
  std::optional<CollaborationMessageId> in_reply_to;
  DeliveryState delivery;
  std::optional<RequestState> request;
  std::optional<RunId> target_run;
};

struct SessionSnapshot {
  SessionSummary summary;
  std::optional<SessionOrigin> origin;
  Sequence through_sequence;
  std::vector<Message> messages;
  std::vector<RunSnapshot> runs;
  std::vector<ToolCallSnapshot> tools;
  std::vector<InteractionSnapshot> interactions;
  std::vector<QueuedInput> queue;
  std::vector<CollaborationSummary> inbox;
  std::vector<ArtifactRef> artifacts;
  std::optional<PageCursor> older_history;
  Revision access_revision = 0;
  Sequence event_sequence = 0;
};

struct MessageDelta { MessageRef message; BlockId block; std::string utf8; };
struct ArtifactRegistered { ArtifactRef artifact; };

using SessionEventBody = std::variant<SessionSummary, Message, MessageDelta,
    RunSnapshot, InteractionSnapshot, ToolCallSnapshot, QueuedInput,
    CollaborationSummary, ArtifactRegistered, SessionOrigin>;

struct SessionEvent {
  SessionId session;
  Sequence sequence;
  CommitId commit;
  std::optional<CommandId> command;
  SessionEventBody body;
};

struct SessionEventBatch {
  Sequence through_sequence;
  std::vector<SessionEvent> events;
};

struct TransferRequest {
  AgentId next;
  Revision expected_binding;
  std::optional<std::string> handoff_summary;
};

class SessionGoals;

struct PendingInteraction {
  SessionSummary session;
  InteractionSnapshot interaction;
};

struct PendingInteractionPage {
  Revision revision;
  std::vector<PendingInteraction> items;
  std::optional<PageCursor> next;
};

struct InteractionFilter {
  std::optional<ProjectId> project;
  std::optional<SessionId> session;
  std::size_t page_size;
};

class InteractionService {
public:
  Async<PendingInteractionPage> ListPendingAsync(InteractionFilter filter, std::optional<PageCursor> cursor);
  Async<Subscription> ObservePendingAsync(InteractionFilter filter, Observer<PendingInteractionPage> observer);
};

class Conversation {
public:
  std::shared_ptr<const SessionSnapshot> Snapshot() const;
  Async<ArtifactPreview> PreviewArtifactAsync(ArtifactRef artifact);
  Async<std::string> ReadSelectionAsync(SourceSlice selection);
  Async<std::vector<std::string>> ReadSelectionsAsync(std::vector<SourceSlice> selection);
  Async<Subscription> ObserveAsync(Observer<SessionSnapshot> observer);
  Async<Subscription> ObserveAsync(Observer<SessionSnapshot> observer, std::optional<SessionHistoryWindow> history);
  Async<Subscription> ObserveEventsAsync(Sequence after, Observer<SessionEventBatch> observer);
  Async<Receipt<RunAccepted>> SendAsync(CommandId command, SendInput input, UserRunOptions start = {});
  Async<Receipt<InputQueued>> QueueAsync(CommandId command, SendInput input, UserRunOptions start = {});
  Async<QueuedDraft> ReadQueuedDraftAsync(QueueItemId item);
  Async<RecoveredDraft> ReadUnappliedSteerDraftAsync(CommandId command);
  Async<Receipt<RunSteered>> SteerAsync(CommandId command, RunId expected, SendInput input);
  Async<CommandReceipt> WithdrawQueuedInputAsync(CommandId command, QueueItemId item);
  Async<Receipt<InteractionConsumed>> SubmitAsync(CommandId command, InteractionReply reply);
  Async<CommandReceipt> DispatchContentActionAsync(CommandId command, CustomBlockAction action);
  Async<Receipt<RunAccepted>> DispatchArtifactActionAsync(CommandId command, ArtifactAction action);
  Async<CommandReceipt> CancelAsync(CommandId command, RunId run);
  Async<Receipt<RunAccepted>> RetryAsync(CommandId command, RunId source);
  Async<Receipt<RunAccepted>> RegenerateAsync(CommandId command, RunId source);
  Async<CommandReceipt> TransferAsync(CommandId command, TransferRequest request);
  SessionGoals Goals();
};

struct AgentChatCitation {
  SourceSlice source;
  std::string text;
  std::vector<SourceSlice> additional_sources;
};

struct AgentChatDraftSources {
  std::optional<AgentChatCitation> quote;
  std::optional<AgentChatCitation> conclusion;
  bool send_as_new_task = false;
};

struct AgentChatRunControls {
  huxerui::State<UserRunOptions> next;
  std::function<huxerui::View(const SessionSummary&, std::optional<RunStartBinding>)> render;
  std::function<std::string(const RunStartBinding&)> describe;
};

struct AgentChatApprovalRef {
  InteractionId id;
  Revision version;
};

struct AgentChatReviewRequest {
  ArtifactRef artifact;
  std::uint64_t ticket;
  std::optional<AgentChatApprovalRef> approval;
  std::optional<Sequence> history_first;
};

struct AgentChatInteractionRequest {
  InteractionId interaction;
  std::uint64_t ticket;
};

using AgentChatPage = std::variant<std::monostate, InteractionId, AgentChatReviewRequest>;

struct AgentChatOptions {
  std::function<std::optional<huxerui::View>(const InteractionSnapshot&, std::shared_ptr<Conversation>)> render_form;
  std::optional<huxerui::State<AgentChatDraftSources>> draft_sources;
  std::function<std::optional<huxerui::View>(const ArtifactPreview&, std::shared_ptr<Conversation>,
      huxerui::State<AgentChatDraftSources>)> render_artifact;
  std::optional<huxerui::State<huxerui::TextEditingValue>> draft;
  std::optional<AgentChatRunControls> run_controls;
  std::optional<huxerui::State<std::vector<QueuedDraftReference>>> recovered_references;
  std::optional<huxerui::State<std::vector<SkillRef>>> draft_skills;
  std::optional<AgentChatReviewRequest> review_request;
  std::function<std::optional<huxerui::View>(const ArtifactPreview&, std::shared_ptr<Conversation>)> render_artifact_summary;
  std::optional<AgentChatInteractionRequest> interaction_request;
  bool review_only = false;
  std::optional<huxerui::State<AgentChatPage>> page;
  std::function<std::string(const AgentRef&)> agent_label;
  std::function<std::string(const ApprovalPreview&, const ArtifactRef&)> review_attachment_label;
  std::function<bool(const ArtifactPreview&, std::optional<huxerui::ViewHandle>)> open_artifact_review;
  std::optional<huxerui::ViewHandle> composer_focus;
  std::function<std::optional<huxerui::View>(const Message&, const ContentBlock&, std::shared_ptr<Conversation>)> render_block;
  std::function<std::string(const ToolRef&)> tool_label;
  std::optional<AgentChatTextSelection::Binding> selection;
};

huxerui::View AgentChat(std::shared_ptr<Conversation> conversation, AgentChatOptions options = {});
huxerui::View AgentChatSourceActions(std::shared_ptr<Conversation> conversation,
    std::optional<SourceSlice> source, huxerui::State<AgentChatDraftSources> draft,
    std::function<void()> on_citation_added = {});
huxerui::View AgentChatArtifactText(std::shared_ptr<Conversation> conversation, ArtifactRef artifact, PartId part,
    std::string text, huxerui::State<AgentChatDraftSources> draft);

struct AgentChatLink {
  MessageRef message;
  BlockId block;
  std::string destination;
};

struct AgentChatLinkHandler {
  std::function<bool(const AgentChatLink&)> allows;
  std::function<AgentResult<void>(const AgentChatLink&)> open;
};
```

`AgentChat` 提供默认组合。需要自定义页面的宿主可分别挂载 `AgentChatMessages(conversation, selection)` 和 `AgentChatComposer(conversation, draft)`。消息视图观察会话和历史，输入框持有发送、排队及停止入口。宿主通过同一个选区绑定取得挂载锚点和光标位置，在点击菜单操作后调用 `ReadAsync` 重新校验权限，取回原消息版本及有序的 UTF-8 切片。Preview 自行渲染选区菜单、切换旁聊面板，并把选中的结论写回主会话草稿。

`AgentChatOptions::render_form` 接收可见的待处理表单快照和当前 `Conversation`。宿主可通过 `FormRequest::presentation` 选择大纲编辑器等业务组件，组件用交互 ID 和版本提交 `SubmitAsync`。表单数据校验、权限和状态流转由运行时处理。

`render_artifact` 接收当前会话已授权的产物预览和主输入框的引用状态。返回视图时在同窗口显示审阅页。普通产物也可由 `render_artifact_summary` 提供概要视图，可回退到通用预览。概要展开在发起操作的消息卡片下方，位置绑定 `MessageId`、`BlockId` 和确切的 `ArtifactRef`。收起概要后恢复原卡片焦点，主输入草稿继续保留。通过宿主直接打开概要时，定位到当前历史投影中带该版本的卡片。没有对应卡片时可在会话中独立显示概要。审批和独立窗口使用完整审阅入口。宿主通过会话导航打开目标会话，以 `review_request` 请求读取确切的产物版本。挂载中的会话视图对同一请求执行一次，重复导航使用新票据。界面经 `PreviewArtifactAsync` 复核目标。`AgentChatSourceActions` 查询选区并提供旁聊和引用动作，`AgentChatArtifactText` 从单个稳定的产物片段提取选区。引用和旁聊结论写入宿主的 `draft_sources`。

从消息引用或带回结论后，焦点进入源会话草稿。从同窗口审阅页执行这些动作时先返回对话，保留原审阅版本和位置。独立审阅窗口保持在原处，源会话草稿可观察到已添加的内容。自定义审阅视图可传入 `on_citation_added`，它在引用加入草稿后调用。旁聊带回结论时先关闭弹窗，再调用回调。取消或普通关闭旁聊恢复原选区及动作焦点。

`render_block` 接收已授权投影中的消息、块和所属会话。宿主可为 `TextBlock` 或业务 `CustomBlock` 提供同步渲染，返回空值时沿用默认渲染。自定义视图按原消息块 ID 挂载，动作通过会话接口按源版本提交。

在对话内打开普通产物时，`open_artifact_review` 可以接管审阅导航。`AgentChat` 校验会话、产物版本、访问修订和可见性后传入预览及可选的触发控件，返回 `true` 由宿主显示自己的审阅页，返回 `false` 继续使用 `render_artifact` 和通用审阅。宿主按访问修订停止展示失权的预览，返回时恢复原对话与触发控件焦点。审批附件及独立审阅窗口沿用已有的版本绑定流程。Preview 的设计助手通过该回调打开可选择节点的设计审阅。

宿主可用 `AgentChatOptions::draft` 持有文本草稿，用 `recovered_references` 保存取回的引用，用 `draft_skills` 保存选定的 Skill。`AgentChatDraftSources::send_as_new_task` 随同一会话的草稿保留，使取回的补充按新任务提交。`AgentChatRunControls::next` 是下一次启动偏好，输入框发送和排队时读取该状态。`render` 得到当前会话概要和活动 Run 的实际绑定，供宿主显示差异。`describe` 用于显示队列项已绑定的配置。直接补充带所见的 `RunId` 提交。

`agent_label` 可提供助手的本地化显示名称。会话标题下方用当前会话绑定，消息署名用该消息所属 Run 的助手版本。宿主未提供名称，或继承的消息没有可见的 Run 时，使用默认的 `Assistant` 署名。

标题栏和宿主操作由应用绘制。Preview 根据 `AgentChatPage` 在对话、待处理请求和独立审阅之间切换标题栏与页面动作，工作区文件等入口跟随应用布局。

`composer_focus` 允许宿主把本会话输入框作为对话框的初始焦点或键盘操作的焦点目标。默认旁聊使用同一个目标从首个问题切换到追问编辑器。窗口高度受限时，待审批对话优先为输入框和审批操作分配空间，计划详情可滚动。进入带审批的审阅页后，审阅内容和决定操作分别保留空间。

宿主可用 `ApplicationContext::Provide` 注册 `AgentChatLinkHandler`。`AgentChat` 只为已完成的消息启用链接。`allows` 在渲染和用户激活时校验同一个消息版本、块 ID 及原始 Markdown 解析得到的地址，回调应只做判定。缺少服务、任一回调或策略拒绝时，链接标签仍可阅读。`open` 在 UI 线程的用户激活事件中同步调用，返回失败时在对应消息内显示原因。此操作只委托宿主导航，消息、Run 和队列状态由原会话保留。Preview 的 Windows、Linux 和 Web 宿主允许有明确目标主机的 HTTP(S) 地址，交由系统浏览器或新标签页打开。平台不支持导航时按纯文本展示链接标签。

宿主可注册 `AgentChatImageLoader`，按完成消息的版本、块 ID 和原始图片地址异步提供 `ImageAsset`。未加载或失败时显示 Markdown 替代文字。独立图片取代原段落，混排图片显示在所属段落下方，段落内的替代文字仍可选择。图片加载前后使用同一组选区坐标，引用或旁聊跨过图片时，来源是替代文字的原文切片。Preview 加载绝对 HTTP(S) 地址的 PNG 和 JPEG，最多读取 8 MiB，验证像素内容及单边 4096 像素上限。

同一审批的多个可见附件可以在审阅页内切换。`review_attachment_label` 允许宿主按审批计划及精确产物版本给审批卡片和页签命名。空名称时，审批卡片使用单目标的路径或通用审阅标签，页签使用附件序号。审批计划保留完整的目标路径供核对。切换附件重新调用 `PreviewArtifactAsync`，沿用原交互 ID 和版本。在加载期间暂停提交审批。加载失败时显示原因并保留先前仍可见的附件。独立窗口打开用户当时查看的附件。

工具审批引用会话历史之外的其他会话产物时，普通项目附件需要与审批会话同项目，并通过审批会话的 `ReadContext`、产物所属会话的 `ReadSession` 及确切产物版本的 `ReadArtifact`。会话投影和待办目录只显示校验通过的外部附件，预览在读取宿主内容前后都复核授权和审批绑定。候选采纳沿用候选版本及审批状态的校验。上述授权范围是审阅读取，产物归属和动作入口仍按原契约处理。

UI 可继续通过 `ObserveAsync` 读取不可变快照。需要逐条消费变更时，可以先应用快照，再用 `ObserveEventsAsync(snapshot.event_sequence, observer)` 订阅。游标指最后应用的事件，登记后先交付该序号之后的已提交事件，再继续交付新提交。`SessionSnapshot::through_sequence` 仍用于历史分页和分叉。

一次提交可包含多条连续事件，两条会话间用相同的 `CommitId` 关联。流式正文先以 `Message` 发布版本和块，后续 `MessageDelta` 追加到该版本和块。结束时发布新版本的完整 `Message`，半截流结清为 `Interrupted`。已提交批次和事件序号随持久会话一并恢复，重放无需重新执行原命令。旧记录从已有快照开始记录后续事件。

事件按当前授权投影，批次的 `through_sequence` 标明已检查的最后序号，过滤可能使批次为空或不可连续应用。消费者忽略已应用的序号，遇到缺口或 `CursorExpired` 时用最新授权快照重建，再用该快照的 `event_sequence` 订阅。失去会话读取权时观察以错误结束。临时会话的事件仅在当前 Runtime 中可重放。

`AgentRuntime::Interactions()` 提供统一待办的分页和订阅。按当前主体、接收人、项目和会话权限过滤所有可处理的待办，包括未打开会话。首页观察与会话目录使用相同的原子登记和游标失效规则。按最早截止时间、会话 ID、交互 ID 排序，无期限的待办排在后面。列表变化与会话事件来自同一提交。选中后用 `Sessions().OpenAsync` 打开所属会话，经 `SubmitAsync` 提交原交互版本。操作审批可用交互引用查询待处理命令。

已批准的会话或协作操作可先处于 `Scheduled`。执行前按原命令重新核对目标绑定、权限和期限，重启时继续未结清的操作。跨会话生效时，命令进度与受影响的持久会话在同一存储事务中提交。调用方通过 `Commands()` 观察 `Applied` 或失败终态，并沿用最初的审批回执重试查询。

创建事务写入 `SessionOrigin`。普通创建取发起 Run 所在会话作为来源（若有），分叉和旁聊记录选取历史的会话。创建者的 Run 记录在 `creator`，可与历史来源处于不同会话。分叉点固定到已提交的 `HistoryBoundary`。打开会话时按当前权限过滤来源、创建者和边界。来源会话不可见时同时省略其 ID、分叉点和旁聊选区，必要时省略整个来源。目录只显示会话摘要。

| 操作或事件 | 行为 |
| --- | --- |
| 发送、重试、队列消费 | 每条会话同时只有一个非终态根 Run。`SendAsync` 或重试遇到活动根 Run 返回 `Busy`。取消和等待状态仍占会话的根 Run 位置。显式输入先于自动续轮，队列按既定顺序消费。待批准的命令在生效时重新检查会话状态，竞争失败时提交明确失败结果。 |
| 等待表单、回复或依赖 | 所有可继续的工作受阻时发布对应等待状态。按人工输入、协作回复、依赖的顺序选摘要状态，细项保留在 `waits`。等待时释放执行名额和写锁。接到有效结果后按原 Run ID 继续。 |
| 校验、批准、拒绝、到期 | 表单错误附字段路径并保留待处理状态。审批请求只接受批准或拒绝；表单接受符合 schema 的值或明确放弃。旧版本返回 `Conflict`，已关闭返回 `AlreadyClosed`。仍待处理但已到期的请求按执行器时钟先结清，返回 `DeadlineExceeded`。 |
| 调整当前 Run | `SteerAsync` 将补充登记到指定 Run，并通过消息与快照展示受理及应用状态。生效边界、等待和结束竞态见下文。 |
| 取消和超时 | 先在同一提交进入 `Cancelling`、关闭该 Run 的交互和等待，停止新调度。通知模型、工具及子 Run 后，等待收尾或收尾期限。用户或父级取消以 `Cancelled` 结束，超时或预算耗尽以 `Failed` 结束并记录原因。 |
| 终态与晚到结果 | 完成和取消由执行器提交顺序裁决。先完成则取消返回 `AlreadyClosed`；先取消则晚到结果只用于实际效果核对。模型和进度回调还须匹配操作实例。成功、失败、取消各只写入一次。 |
| 转交 | `TransferAsync` 校验 `TransferRequest` 中的绑定版本、目标 Agent 和当前授权。版本过旧返回 `Conflict`，活动根 Run 存在时返回 `Busy`。成功时在同一提交更新绑定和目录摘要，并追加带 `AgentTransferred` 块的系统消息。可选的交接摘要按文本处理，历史分页时按当前权限投影。后续 Run 按新 Agent 计算权限，已排队的工作在开始前按新绑定复核。已有 Run 记录保留各自的 Agent 版本。 |
| 重试与重新生成 | 校验原 Run 已终止且会话空闲，创建有来源的新 Run。按原启动输入重新检查当前绑定、资源和权限。尚有待核对写入时返回 `ResultUnknown`。重新生成使用所选用户输入结束时的上下文边界，重试从原任务的已确认输入开始，历史均保留。项目任务和持续目标通过各自调度入口绑定新的尝试。 |

调用示例按「create，open，observe，submit，cancel」串联。先提交会话和首条消息，从直接回执或原审批命令的完成状态取得 `SessionCreated`，随后打开并读取快照。如果执行在打开前已经发布审批，初始快照仍包含该请求。界面以该请求的 ID 和版本提交。命令回执可晚于观察到的状态，UI 按命令 ID 对应两者，并以最新快照判断是否继续等待。

### 运行中发送与补充

| 用户意图 | API 与结果 |
| --- | --- |
| 发送给空闲会话 | `SendAsync(command, input, start)` 创建根 Run。若提交时已有活动根 Run，返回 `Busy`。 |
| 补充到当前 Run | `SteerAsync(command, expected, input)` 固定用户发送时看到的活动 `RunId`，成功受理时返回 `RunSteered`，消息在对话中可见。原命令为 `Scheduled`，Run 内记录为 `Pending`。 |
| 当前任务结束后处理 | `QueueAsync(command, input, start)` 登记输入与已解析的启动配置。按既定顺序消费，开始前可经 `WithdrawQueuedInputAsync` 撤回。 |
| 回答已展示的表单或审批 | `SubmitAsync` 按交互 ID 和版本提交。普通补充和被回复的交互各自保留状态。 |

输入栏在首条消息和已有会话中都保留完整的 `TextEditingValue`，包括编辑选区和 IME 组合状态。模型增量和 Run 状态变化时继续使用同一输入会话。输入法正在组字时暂不可发送或排队。组字提交后才允许将文字交给会话接口，异步回执也会保留这期间新开始的组字。

用户输入入队后，原命令为 `Scheduled`，队列项为 `Queued`。启动前通过 [模型与权限选择](#preview-的模型与权限选择) 的复核后，在同一事务创建根 Run、设置 `QueuedInput::run`、将队列项置为 `Started`，并将原命令推进到 `Applied`，结果仍为 `InputQueued`。启动复核失败时，队列项置为 `Rejected`，命令置为 `Failed`，两处记录同一原因。撤回成功后，队列项和原命令均进入 `Withdrawn`。宿主通过原队列 ID 或命令记录确认状态。已启动任务的业务结果由对应 Run 提供。

`ReadQueuedDraftAsync` 按当前会话和提交者读取 `Rejected` 队列项。原命令和队列记录继续保留。结果含原文、授权版本的引用及预览。失效或撤权的引用仅返回提示，用户可逐项移除。另一用户或另一会话请求该队列 ID 返回 `NotFound`，当前用户请求未进入拒绝状态的队列项返回 `InvalidState`。

Preview 的「编辑排队输入」将原文和引用放入空草稿。可用的 `SourceSlice` 随新输入的 `context` 提交。已存旁聊结论用原队列 ID 和固定下标构成 `QueuedConclusionRef`，可在临时旁聊消失后复用已提交的正文和来源切片。发送、补充或再次排队时，运行时检查当前会话、队列提交者和实际接收模型的输入权限。复核通过才生成本次提交的 `SideChatConclusion`。其他输入来源须留空 `recovered_conclusions`。编辑只更新界面草稿，由用户用新命令显式提交。

Preview 只在有活动 Run 时显示「直接发送 / 排队」，默认直接发送。输入栏发送时固定已观察到的目标，界面以原命令进度和消息状态显示是否生效。补充沿用该 Run 的启动配置，按受理顺序应用。在模型运行中，下一次模型请求携带已提交的补充。工具执行许可和补充登记按提交顺序裁决。先受理补充时，执行器在工具开始前应用补充并复核计划。写入先取得开始许可时按原调用收尾。若目标 Run 正等未开始的写入审批，运行时结清受影响的旧计划和批准，唤醒编排以重规划。无法在执行前证明计划与补充无关时，按受影响处理。后续写入以新计划和审批为准。对已开始的写入，在该步骤收尾后的安全边界应用补充，前序实际效果仍归原调用。等表单、协作回复或依赖时，可以登记补充。有效的交互和依赖仍要在各自入口结清。

应用时把消息的补充状态和原命令一起推进到 `Applied`。未应用的补充仍可在原对话看到，界面显示待应用或未生效。对可见目标先查归属和状态。目标已结束时返回 `AlreadyClosed`，目标仍活动但不匹配当前根 Run 时返回 `Conflict`，正在取消等无法接收的状态返回 `InvalidState`。若成功受理后先遇到结束或取消，仍待应用的消息结清为 `NotApplied`，命令记录失败原因。宿主可保留原文并让用户明确另发新任务或排队；运行时不重新绑定消息。未应用的旧补充不作为后续 Run 的指令。重试使用原命令 ID，避免重复登记。

`RunSteerSnapshot::recovery` 在原会话向提交者提供已结清为 `NotApplied` 的原命令 ID。`ReadUnappliedSteerDraftAsync` 以该 ID 读取已提交的原文、Skill 版本和当前仍可读取的引用。正在等待或已应用的原补充返回 `InvalidState`，其他会话或其他提交者的命令返回 `NotFound`。读取期间权限变化返回 `Conflict`。引用失效时返回可移除的占位提示，原文与其余获准引用仍可编辑。旧记录缺少组件动作生成的输入时，界面可恢复已保存的正文与结论，并提示原引用和 Skill 无法查证。

旁聊结论通过 `UnappliedSteerConclusionRef` 指向原命令及在消息中保存的结论下标。临时旁聊关闭后仍可按该引用恢复。再次提交时把该引用放进 `SendInput::recovered_steer_conclusions`。运行时重新校验会话归属、原提交者、来源版本及实际接收模型的授权，再生成新消息的结论。Preview 的「取回草稿」只填充空草稿，保留原消息和原命令。空闲时用户明确发送为新任务，另有活动 Run 时明确排队。后续任务使用当时的模型和权限偏好，历史补充仍归原 Run。

默认模型循环在边界把补充作为用户输入送入下一轮。宿主编排可在其阶段边界通过 `RunContext::ReadPendingSteersAsync` 读取当前 Run 的待应用输入。宿主采用其中按受理顺序连续的一段后，把最后一条消息 ID 写入 `RunCheckpoint::steer_through`，用 `SaveCheckpointAsync` 一起提交阶段状态和应用进度。最后一条消息须属于最近读到的批次，且起始位置与已应用边界一致。边界变化或跨 Run 提交返回 `Conflict`。模型循环已经应用的记录不会再次返回。尚未处理的补充会阻止被影响的旧计划开始写入。宿主完成且没有采用的补充进入 `NotApplied`。持久会话重启后使用原消息 ID、受理顺序和应用记录。恢复不了原 Run 时，待应用补充按未生效结清。

## 模型、上下文、工具和授权

### 模型与上下文

```cpp
enum class ModelCancellation { StopReading, ProviderCancel };

struct ModelCapabilities {
  bool text_stream;
  bool tool_calls;
  bool structured_output;
  bool image_input;
  bool usage;
  bool file_input = false;
  bool multi_tool_calls = false;
  ModelCancellation cancellation = ModelCancellation::StopReading;
};

struct ModelEndpoint {
  std::string provider;
  std::string model;
  std::string protocol;
  std::optional<std::string> base_url;
};

class ModelProvider;

struct ModelRegistration {
  ModelProviderRef ref;
  ModelEndpoint endpoint;
  std::shared_ptr<ModelProvider> provider;
  bool allow_server_state = false;
  bool allow_persist_resume_state = false;
};

struct ModelResumeState {
  ModelProviderRef provider;
  Revision format_version;
  std::vector<std::byte> opaque;
};

struct ModelContinuation {
  ModelResumeState state;
  std::size_t prefix_items;
};

struct ModelInput {
  std::string instruction;
  std::vector<SourceSlice> context;
  std::vector<ToolId> allowed_tools;
  std::optional<JsonSchema> output_schema;
};

struct ModelContent {
  enum class Role { User, Assistant, Context, Tool };
  Role role;
  std::variant<std::string, JsonValue, ArtifactRef> body;
};

struct ModelToolResult {
  std::string provider_call_id;
  std::optional<ToolCallId> call;
  std::optional<JsonValue> value;
  std::optional<AgentError> error;
};

struct ModelToolCall {
  std::string provider_call_id;
  std::string name;
  JsonValue arguments;
};

using ModelItem = std::variant<ModelContent, ModelToolCall, ModelToolResult>;

struct ModelAttachment {
  ArtifactRef artifact;
  std::string media_type;
  std::function<Async<huxerui::AsyncInputStream>()> open;
};

struct ModelToolSpec {
  ToolRef tool;
  std::string name;
  std::string description;
  JsonSchema parameters;
};

struct ModelSkillInstruction {
  SkillRef skill;
  std::string markdown;
};

struct ModelTurnRequest {
  TurnId turn;
  RunId run;
  ModelProviderRef provider;
  std::optional<ModelContinuation> continuation;
  bool allow_server_state;
  std::string instructions;
  std::vector<ModelItem> items;
  std::vector<ModelAttachment> attachments;
  std::vector<ModelToolSpec> tools;
  std::vector<SkillSummary> available_skills;
  std::vector<ModelSkillInstruction> skill_instructions;
  std::optional<JsonSchema> output_schema;
  std::optional<TimePoint> deadline;
  std::stop_token stop;
};

struct ModelTextDelta {
  std::string utf8;
  bool show_in_transcript = true;
};
struct ModelToolStarted { std::string provider_call_id; std::string name; };
struct ModelToolArguments { std::string provider_call_id; std::string bytes; };
struct ModelToolFinished { std::string provider_call_id; };
struct ModelStructuredOutput { JsonValue value; };
struct ModelUsage { Usage usage; };
enum class ModelFinishReason { Completed, ToolCalls, Length, Filtered, Refused };
struct ModelFinished {
  ModelFinishReason reason;
  std::optional<ModelResumeState> resume;
};

using ModelEvent = std::variant<ModelTextDelta, ModelToolStarted, ModelToolArguments,
    ModelToolFinished, ModelStructuredOutput, ModelUsage, ModelFinished>;

struct ModelReply {
  std::string text;
  std::optional<JsonValue> structured;
  Usage usage;
  std::vector<ArtifactRef> artifacts;
};

enum class ModelStopOutcome { LocalStopped, ProviderConfirmed, ProviderUnknown };

class ModelTurnStream {
public:
  virtual ~ModelTurnStream() = default;
  virtual Async<std::optional<ModelEvent>> ReadAsync() = 0;
  virtual Async<ModelStopOutcome> StopAsync() = 0;
};

class ModelProvider {
public:
  virtual ~ModelProvider() = default;
  virtual ModelCapabilities Capabilities() const = 0;
  virtual Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) = 0;
};

struct ContextRequest {
  ActionParty actor;
  ModelId recipient;
  std::vector<SourceSlice> selected;
  std::uint64_t byte_limit;
  std::stop_token stop;
};

struct ContextFragment {
  SourceSlice source;
  ModelContent content;
};

struct ContextSnapshot {
  std::vector<ContextFragment> fragments;
};

using ContextProvider = std::function<Async<ContextSnapshot>(ContextRequest)>;
```

- 执行器检查模型能力和预算，向上下文提供器获取已授权的版本化片段，再以当前模型接收方校验并装配历史与工具声明。`ModelTurnRequest` 拥有数据，适配器持有鉴权，续轮状态通过下述契约传递。工具名映射由本轮声明确定。
- `SendInput::context` 和 `ModelInput::context` 中的 `ResourceRef` 按 `AgentDefinition::context` 的登记顺序交给宿主提供器。每次调用只传递尚未解析的已授权切片，以及 Run、模型接收方、剩余额度和取消令牌。提供器可跳过不负责的资源。返回片段须保留请求的资源 ID、版本与位置，正文可用 UTF-8 文本或 JSON，角色为 `Context`。宿主负责核对实际版本和 UTF-8 选区边界。运行时按输入顺序组装片段，发送前再核对授权。缺少提供器时返回 `Unsupported`，所有提供器均未能解析时返回 `NotFound`，返回额外切片、重复切片或替换版本时返回 `ProtocolError`。读取中撤权使该轮停止，超出额度时返回 `BudgetExceeded`。
- 当输入引用完整的产物版本且宿主提供结构化预览时，执行器可将该预览作为模型上下文。装配前复核来源和 `SendToModel`，装配后核对解析结果的产物 ID 和版本并再次检查权限。准备工具时，`ToolPrepareContext::sources` 提供获准的输入引用。工具将实际使用的来源登记到 `PreparedChange::inputs`，由执行器在写入前复核。
- 每条流只允许一个读者。适配器解码供应商格式，文本增量以有效 UTF-8 交付，工具参数可以按字节分段。正常流含一个结束事件。EOF 之前缺少结束、顺序错误、调用 ID 重复或超长数据返回 `ProtocolError`。传输错误返回 `ProviderFailure`，截断保留已显示内容并关闭该轮。
- `ModelTextDelta::show_in_transcript` 决定增量是否进入会话消息、订阅投影与持久化历史，默认显示。适配器将用于组装结构化结果的协议文本标为 `false`。运行时仍累计这些增量并校验 UTF-8，宿主通过 `ModelReply` 取得轮次文本与结构化结果。Preview 在该轮完成后用对话回复、表单或产物视图呈现业务结果。Responses 的独立拒绝文本沿用可见增量。普通文本轮次继续增量提交，用户可直接阅读请求生成的文本，包括 JSON。
- 工具参数按提供商调用 ID 拼接，结束原因为 `ToolCalls` 且结构完整后校验整个调用集合。调度使用本轮记录的工具身份和版本，按 [同轮工具调度](#同轮工具调度) 执行。结构化结果按请求 schema 验证。单个参数非法、权限拒绝或工具换版产生对应的工具结果。轮次不完整时丢弃整组待调度调用。下一轮携带原调用及结果，按声明顺序组装并用原提供商调用 ID 对应。恢复仅在已提交的模型轮次或宿主阶段边界继续。用量事件为本轮累计值，按末次已知值结算一次。图像或文件交给模型时使用有权限的附件句柄，读取前再做权限检查。
- `CompleteAsync` 执行限定工具范围的模型循环。默认驱动和宿主 `RunHandler` 都从该机制获得文本、结构化结果和工具交付。是否完成跨轮目标仍由后述目标验证器决定。

### 提供商协议与能力

`RegisterModel` 为一个提供商、实际模型、协议和端点注册适配器。每个 `ModelId` 选择一个配置，Run 启动时固定注册修订，写入 `RunSnapshot::model`，恢复时按该修订核对。宿主编排恢复需要原配置，或显式从可重建阶段以新 Run 继续。应用可登记同品牌的多个配置。本地适配器可省略端点 URL。协议使用稳定字符串，兼容端点可共用编解码器，并各自处理字段、认证和结束语义。`ModelProvider::Capabilities()` 对应当前注册的组合。注册检查 Agent 的必需能力，并在每轮开始前核对媒体格式、schema 子集等实际请求。无法满足的必需能力返回 `Unsupported`。宿主可显式选择 schema 工具等替代路径。

| 提供商 | 适配范围 | 协议内处理 |
| --- | --- | --- |
| OpenAI GPT | Chat Completions、Responses | 归一化 Chat 的增量与索引，以及 Responses 的类型化事件及 `call_id`。将 Responses 的续轮引用或必需的输出项交给同一适配器。 [OpenAI Docs](https://developers.openai.com/api/docs/guides/migrate-to-responses) |
| Anthropic Claude | Messages | 组装内容块、工具输入的 JSON 分片以及对应的 `tool_result`。保留按协议需要原样传回的状态项。 [Claude 流式文档](https://platform.claude.com/docs/en/build-with-claude/streaming) |
| Google Gemini | Interactions、`generateContent`、OpenAI Chat Completions 兼容端点 | 区分 Interactions 的步骤和续轮引用、原生内容分段与推理签名，并按兼容端点实际支持项声明能力。 [Interactions](https://ai.google.dev/gemini-api/docs/interactions-overview) · [GenerateContent](https://ai.google.dev/api/generate-content) · [OpenAI 兼容端点](https://ai.google.dev/gemini-api/docs/openai) |
| DeepSeek | Chat Completions、Responses、Messages 兼容端点 | 分别处理流式结束、工具回传与推理状态。按所选协议和模型校验能力。 [DeepSeek Chat Completions](https://api-docs.deepseek.com/guides/build_a_chatbot/) · [DeepSeek Responses](https://api-docs.deepseek.com/guides/responses_api/) · [DeepSeek Messages](https://api-docs.deepseek.com/guides/anthropic_api/) |
| 智谱 GLM | Chat Completions、Responses、Claude API 兼容端点 | 按智谱文档适配存储默认值、流终止和取消等差异。 [智谱 Chat](https://docs.bigmodel.cn/cn/guide/develop/openai/introduction) · [智谱 Responses](https://docs.bigmodel.cn/cn/guide/develop/responses/introduction) · [智谱 Claude 兼容](https://docs.bigmodel.cn/cn/guide/develop/claude/introduction) |

各适配器用 `ModelCapabilities` 声明文本流、工具、结构化输出、媒体、用量、同轮多调用和取消能力。模型可以发出多项工具调用时，实际执行仍由 [同轮工具调度](#同轮工具调度) 决定。协议的成功结束归一化为一个 `ModelFinished`，业务截断、拒绝或内容过滤保留明确原因。适配器根据已确认的协议终止标志归一化流，连接中断仍按不完整处理。`stop` 停止本地读取、接收结果和工具调度。运行时在收尾时限内调用幂等的 `StopAsync`，它可以与尚未完成的 `ReadAsync` 重叠。声明 `ProviderCancel` 时尝试提供商取消。`ProviderConfirmed` 需要明确的提供商确认，`LocalStopped` 仅表示本地流已停止，等待不到确认时记录 `ProviderUnknown`。取消结果进入轮次审计。

#### Chat Completions 适配器接入

`<huxerui/agentkit/providers.h>` 提供 `MakeChatCompletionsModel(ChatCompletionsOptions, std::shared_ptr<huxerui::HttpClient> = {})`。在应用线程创建，成功时取得可交给 `RegisterModel` 的 `ModelRegistration`。配置包含模型 ID 和修订、服务基础 URL、模型名、协议变体和可选的异步凭据回调。适配器从基础 URL 构造 `/chat/completions` 路径，使用 HuxerUI 流式 HTTP 读取。端点的工具、同轮多工具、用量和结构化输出能力由宿主按实际模型声明。取消关闭本地读取，结果为 `LocalStopped`。

工具名称按端点命名限制映射，运行时继续使用注册名称。工具回传固定使用端点发出的调用 ID。Chat 的 `finish_reason`、尾部用量和 `[DONE]` 一起决定轮次结束。DeepSeek 思考模式的续轮数据从私有状态回传，写入本地会话存储须显式授权。 [Chat API](https://developers.openai.com/api/reference/resources/chat) · [DeepSeek 续轮](https://api-docs.deepseek.com/guides/thinking_mode/)

兼容端点使用 `json_object` 模式时，适配器把宿主的输出 schema 加到模型指令中。使用 `json_schema` 模式时，适配器在请求参数中提交 schema 供端点约束。两种模式的最终输出均由 Runtime 按同一 schema 校验。

#### Responses 适配器接入

`<huxerui/agentkit/providers.h>` 提供 `MakeResponsesModel(ResponsesOptions, std::shared_ptr<huxerui::HttpClient> = {})`。应用线程上的工厂返回可注册的模型。配置包含模型引用、模型名、服务基础 URL、`ResponsesDialect`、Bearer 凭据回调、超时、工具能力和 `ResponsesOutputFormat`。适配器向 `/responses` 提交流式请求，使用 `text.format` 约束结构化输出。`JsonObject` 模式将 schema 纳入提示，并在请求的 `input` 中加入说明 JSON 输出格式的系统消息。该消息只在请求装配时生成。`JsonSchema` 模式在 `text.format` 中提交 schema。两种模式的最终结果均由 Runtime 校验。当前适配器声明文本、工具、结构化输出、用量及本地停止。 [Responses JSON 模式](https://developers.openai.com/api/docs/guides/structured-outputs?api-mode=responses#json-mode)

适配器用 `output_index` 和 `item_id` 归属工具参数分片，以 `call_id` 关联本地工具结果。正常完成且完整参数和调用身份均通过校验后，工具才可调度。服务报告截断、内容过滤、拒绝或连接中断时，保留已显示的内容，按相应结束原因结清该轮。

三种协议变体均按原 `call_id` 回传 `function_call_output`，默认从获准本地历史重建输入。各变体按以下约定处理推理项、存储和结构化输出。

| `ResponsesDialect` | 默认输出模式 | 续轮 |
| --- | --- | --- |
| `OpenAI` | `JsonSchema` | 提交 `store: false`。启用 `encrypted_reasoning` 时，在原位置回放协议所需的加密推理项。 [流式事件](https://developers.openai.com/api/reference/resources/responses/streaming-events) · [工具调用与回传](https://developers.openai.com/api/docs/guides/function-calling) |
| `DeepSeek` | `JsonSchema` | 端点无状态。请求用显式的 `message` 项装配历史，按原位置回放数组形式的 `reasoning_text`，省略不适用的服务端状态、加密推理和并行工具控制字段。 [DeepSeek Responses API](https://api-docs.deepseek.com/api/create-response/) |
| `GlmCompatible` | `JsonObject` | 提交 `store: false`，使用显式 `message` 项和对象形式的 `reasoning_text`。仅宿主注册和当前轮次均允许服务端状态时，使用 `store: true`。续轮提交原 `previous_response_id` 及新增输入，继续提交当前指令和工具。 [智谱 Responses API](https://docs.bigmodel.cn/cn/guide/develop/responses/introduction) |

DeepSeek 和 GLM 的私有推理项只在同一 Run、模型注册修订和授权前缀内回放。GLM 的服务端引用同样受这些约束，过期引用在流建立前返回 HTTP 404 或 410 时，以当前获准历史回退一次。推理项保留在私有续轮数据中，界面显示可见文本和结构化结果。`allow_persist_resume_state` 默认为关闭，宿主可按策略启用私有续轮数据落盘。`GlmCompatible` 显式要求 `JsonSchema` 时返回 `Unsupported`。直接配置服务端状态只适用于 `GlmCompatible`。 [DeepSeek 协议差异](https://api-docs.deepseek.com/guides/responses_api/) · [智谱状态与结束语义](https://docs.bigmodel.cn/cn/guide/develop/responses/introduction)

#### Claude Messages 适配器接入

`MakeMessagesModel(MessagesOptions, std::shared_ptr<huxerui::HttpClient> = {})` 在应用线程创建模型注册，支持 Claude 原生、DeepSeek 兼容和 GLM 兼容端点。配置指定 API 根路径、模型引用、版本、最大输出 token 和异步密钥回调。请求发送到 `/messages`，使用 `x-api-key` 和 `anthropic-version`。宿主按所选模型的能力启用思考模式、工具和同轮多调用。 [Claude API 版本](https://platform.claude.com/docs/en/api/versioning) · [DeepSeek Messages](https://api-docs.deepseek.com/guides/anthropic_api/) · [GLM 兼容接口](https://docs.bigmodel.cn/cn/guide/develop/claude/introduction)

适配器按内容块索引组装文本和参数分片。工具结果在紧接的用户消息中以 `tool_result` 按原 `tool_use_id` 回传，后续用户文本位于结果之后。累计用量按轮次结算。仅收到 `message_stop` 且所有工具块完整时，正常工具轮次才交给调度器。缺少终止事件、参数对象不完整、服务报告拒绝或超长时，保留已显示文本并按对应失败原因结束。 [Claude 流式消息](https://platform.claude.com/docs/en/build-with-claude/streaming) · [工具结果回传](https://platform.claude.com/docs/en/agents-and-tools/tool-use/handle-tool-calls)

Claude 原生配置可用 `JsonSchema` 发送 `output_config.format`。兼容端点可配置 `PromptedJson`，由适配器在指令中提供 schema、解析 JSON，再由运行时校验结果。已封存的思考块、签名和屏蔽块按模型引用及授权前缀留在私有续轮状态，和原对应的文本、调用块按原顺序拼装。宿主显式启用 `allow_persist_resume_state` 后允许落盘。重启无法取得所需私有状态时，原 Run 按中断结清，待审批工具关闭。 [Claude 结构化输出](https://platform.claude.com/docs/en/build-with-claude/structured-outputs) · [Claude 思考块](https://platform.claude.com/docs/en/build-with-claude/thinking)

#### Gemini GenerateContent 适配器接入

`MakeGeminiGenerateContentModel(GeminiGenerateContentOptions, std::shared_ptr<huxerui::HttpClient> = {})` 在应用线程创建模型注册。以 `/v1beta` 等版本根路径为基础，调用 `/models/{model}:streamGenerateContent?alt=sse` 并以 `x-goog-api-key` 鉴权。宿主按具体模型启用工具、结构化输出和媒体能力。结构化输出采用 `generationConfig.responseJsonSchema`。只有支持同轮组合的配置才可同时提交工具和结构化输出。 [生成接口](https://ai.google.dev/api/generate-content) · [工具与结构化输出](https://ai.google.dev/gemini-api/docs/generate-content/function-calling)

工具结果与原 `functionCall.id` 关联。端点省略 ID 时，适配器为本地运行时分配标识，并在回传请求体中省略协议的 ID 字段。工具结果按原调用顺序放进后续用户消息。思考签名绑定原 `Part`，包括空文本块，只有绑定相同模型和授权前缀的续轮状态可用于重放。候选结束后继续读取尾部用量，完整流结束时交付最终结果。缺少结束标记或流在片段中截断时保留已显示文本，未完成的工具轮次按协议错误结清。 [思考签名](https://ai.google.dev/gemini-api/docs/generate-content/thought-signatures)

用户在 `SendInput::attachments` 中指定已登记的确切版本。首条消息、直接发送、排队和补充使用相同的附件校验。目标会话须有权引用该产物，实际模型须获准读取该版本。宿主为可内联产物使用媒体类型作为 `ArtifactMetadata::type`，并通过 `ArtifactStore::open` 提供该版本的原始字节。图片需要 `image_input`，其他文件需要 `file_input`。旁聊继续使用原选区。

用户从宿主选择新文件时，宿主冻结该文件的内容并为草稿分配唯一 ID。用户提交时传入 `SendInput::staged_attachments`。AgentKit 按提交者、目标会话和实际绑定的模型检查拟登记的 v1，随后调用 `ArtifactStore::resolve_staged_attachment`。宿主返回带原 ID、媒体类型、显示名、精确字节数和稳定存储句柄的 `ArtifactDraft`，并负责持久保存其字节。新版本和消息、排队项或补充输入在同一会话事务提交。提交完成后，输入保存普通版本引用。用同一个命令 ID 重放时沿用原结果。未提交的草稿 ID 在会话和项目产物目录中不可见。若宿主需要在重启后继续批准外部草稿，应在其选择流程中保存可重新解析的快照。纯附件首条消息保留占位标题。

发送前，运行时在上下文预算内读取并复核字节，再由 `ModelContent` 按消息顺序引用 `ArtifactRef`，以 `ModelAttachment` 为每个不同版本提供只读内容。相同版本重复引用时按出现次数计算预算。权限变更、大小不一致、读取失败或取消会阻止该请求。附件版本随用户消息及 Run 输入保存。普通后续任务可显式重新选择同一版本。重试复核原 Run 已应用输入中的附件，重新生成复核原启动输入中的附件。排队消费时重查原定模型和授权。取回被拒的排队任务或未生效补充时，能读取的附件保留为 `QueuedDraftReference` 中的 `ArtifactRef`，其余显示不可用。

Gemini GenerateContent 支持 PNG、JPEG、WebP、HEIC、HEIF、PDF、纯文本、CSV 和 JSON。适配器核对各引用的 ID、版本和媒体类型，再以 `inlineData` 发送。适配器的原始字节上限默认为 8 MiB，序列化请求最多 20 MB。 [媒体输入](https://ai.google.dev/gemini-api/docs/generate-content/image-understanding) · [文件输入](https://ai.google.dev/gemini-api/docs/file-input-methods?generate-content=true)

#### Gemini Interactions 适配器接入

`MakeGeminiInteractionsModel(GeminiInteractionsOptions, std::shared_ptr<huxerui::HttpClient> = {})` 在应用线程创建模型注册。适配器通过配置的 API 根路径调用 `/interactions`，使用 `x-goog-api-key` 鉴权。宿主声明具体模型的文本、工具、结构化输出与媒体能力。结构化输出使用 `response_format.schema`。图片和 PDF 输入复用 GenerateContent 的版本校验和字节限制，使用 Interactions 的图片与文档内容类型。 [Interactions API](https://ai.google.dev/gemini-api/docs/interactions-overview) · [结构化输出](https://ai.google.dev/gemini-api/docs/structured-output)

适配器按步骤索引归属文本和参数增量。终止事件报告 `requires_action` 且全部步骤已关闭、参数是完整 JSON 对象时，才可调度宿主工具。后续请求以 `function_result` 关联原工具名和 `call_id`。流缺少结束事件、步骤或参数不完整时保留已显示的文本，并关闭本轮待调度调用。报告 `incomplete` 时按截断处理。前台流的取消停止本地读取，结果为 `LocalStopped`。 [Interactions 流式事件](https://ai.google.dev/gemini-api/docs/streaming) · [Interactions API v1 参考](https://aistudio.google.com/api-reference/interactions-api?api-version=v1)

默认用 `store: false` 从获准本地历史重放同一 Run 的步骤及协议所需的思考签名。宿主注册与本轮请求都允许服务器状态时，适配器提交 `store: true`。续轮在同一个 Run、模型注册版本和输入前缀边界内使用 `previous_interaction_id`，仅发送新增输入。每轮重新提交当前指令、工具声明和输出约束。服务器对续轮引用返回 HTTP 404 或 410 且流尚未建立时，适配器用获准本地历史尝试一次无状态请求。续轮私有数据只有启用 `allow_persist_resume_state` 才能写入本地存储。 [服务器状态与续轮](https://ai.google.dev/gemini-api/docs/interactions-overview) · [思考签名](https://ai.google.dev/gemini-api/docs/thought-signatures)

#### Preview 模型与工作区配置

Preview 使用 `HUXERUI_AGENTKIT_BASE_URL` 和 `HUXERUI_AGENTKIT_MODEL`。基础 URL 填写所选协议的 API 根路径，比如 Claude 的 `/v1`、DeepSeek Messages 的 `/anthropic/v1` 或 Gemini 的 `/v1beta`。`HUXERUI_AGENTKIT_PROTOCOL` 支持 `chat-completions`、`responses`、`messages`、`generate-content` 和 `interactions`，缺省使用 Chat Completions。

`HUXERUI_AGENTKIT_PROVIDER` 在 Chat Completions 下可选 `openai`、`deepseek`、`gemini`、`glm` 或 `compatible`，Responses 可选 `openai`、`deepseek` 或 `glm`，Messages 可选 `anthropic`、`deepseek` 或 `glm`，GenerateContent 和 Interactions 接收 `gemini`。Messages 缺省为 `anthropic`，两个 Gemini 原生协议缺省为 `gemini`，其余协议缺省为 `openai`。Messages 可通过 `HUXERUI_AGENTKIT_ANTHROPIC_VERSION` 指定 API 版本，缺省为 `2023-06-01`。需要鉴权的端点使用 `HUXERUI_AGENTKIT_API_KEY`。凭据由宿主回调持有，模型注册标识包含协议和适用的 API 版本，实际模型与修订随 Run 固定。

Interactions 和 GLM Responses 默认关闭服务器续轮状态。宿主可用 `HUXERUI_AGENTKIT_ALLOW_SERVER_STATE=1` 启用，运行时仍按实际 Run 的上下文授权决定是否使用服务器引用。Interactions 和各 Responses 变体可用 `HUXERUI_AGENTKIT_PERSIST_RESUME_STATE=1` 启用私有数据落盘。

Web Preview 启动时从入口页面同目录读取公开的 `agentkit-models.json`。按 `profiles` 顺序登记可选配置，首项作为默认模型。`id` 在目录内唯一，由 1 到 32 个大写英文字母、数字或下划线组成。`base_url` 可以是相对页面的路径或绝对 HTTP(S) API 根地址。协议、提供商及默认值与进程配置一致。可选字段 `anthropic_version`、`allow_server_state`、`persist_resume_state` 分别接收版本字符串和布尔开关。独立模型审查使用可选的 `reviewer` 对象，字段与模型配置相同，省略 `id`。

Gemini GenerateContent 和 Interactions 配置可显式启用 `image_input` 和 `file_input`。原生 Preview 对应 `HUXERUI_AGENTKIT_IMAGE_INPUT=1` 和 `HUXERUI_AGENTKIT_FILE_INPUT=1`。启用后，Preview 可从系统选择器选取声明支持的 PNG、JPEG、WebP、HEIC、HEIF 和 PDF，每个文件最多 4 MiB。直接补充使用活动 Run 的模型，排队使用排队项保存的模型。模型不支持、文件读取失败或权限失效时，界面显示错误并保留可用草稿。

```json
{
  "profiles": [
    {
      "id": "DEFAULT",
      "model": "host-model",
      "base_url": "./models/v1",
      "protocol": "chat-completions",
      "provider": "compatible"
    }
  ]
}
```

部署服务提供鉴权所需的模型代理并持有提供商凭据。`agentkit-models.json` 随浏览器请求提供，接受公开的模型配置字段。端点 URL 禁止用户信息、查询串和片段。跨源端点须允许应用的源及所选协议的请求头，并提供可增量读取的 SSE 响应。同源端点可使用宿主服务的会话鉴权。配置文件返回 404 时使用本地演示模型。配置读取失败或聊天模型条目无效时显示配置错误，用户仍可打开本地会话。受理的队列和 Run 保留原模型绑定，按缺失注册修订的规则处理。无效的独立审查配置沿用人工回退规则。

代码助手使用支持结构化输出的模型。Preview 桌面宿主可用 `HUXERUI_AGENTKIT_CTEST` 配置 CTest 可执行文件的绝对路径。宿主在工作区补丁写入后提交独立的测试审批，预览实际执行位置。批准后由已配置的 CTest 执行工作区根目录中的测试，并在执行会话登记退出状态和诊断。对话常显退出结果，可在原工具消息展开及复制诊断。Preview 去掉 CTest 的输出分段标记及终端控制序列，已保存的旧消息在渲染时使用相同规则。会话仅随工具结果登记不超过 64 KiB 的日志对应的诊断。原始 CTest 日志按工具调用 ID 保存在本地 `test-logs`。

跨会话发布流程的测试审批同时预览截图采集。CTest 进程收到 `HUXERUI_AGENTKIT_VERIFICATION_PNG`，值为该次调用专有的本地输出路径。工作区测试应将实际运行界面的 PNG 写到此路径，图片最多 8 MiB，单边不超过 4096 像素。独立代码测试无需截图。发布验证核对测试前后的实现文件版本，分别登记报告和 PNG，保留各自产物的版本与实现来源。测试失败、文件版本变化或截图不可用时，验证不能解锁演示任务。已准备的旧计划如果没有绑定截图输出，需要重新准备并审批。

PPT 宿主可用获准的截图版本生成演示。在 Preview 中，截图出现在末页，与验证结论一同审阅并导出。来自截图的依赖只登记到实际使用它的页。在 Goal 完成验证中复核报告和截图来自同一次测试调用，并核对导出的演示中确实包含这个版本的图片。

### Preview 的模型与权限选择

宿主从已注册配置为当前用户、助手与工作区生成模型列表。每项关联具体 `ModelProviderRef`，显示可辨认的模型、提供商与必要的运行位置。模型选择由宿主保存为当前会话的后续任务偏好，审批预设按当前用户和工作区管理。Preview 运行中从 `RunSnapshot` 显示「本次」配置，并标出相异的「下次」选择。两项偏好只影响之后提交的新建、空闲发送和排队。运行中的用户补充使用 `SteerAsync`，继续按原 Run 的配置处理。

Preview 的新建和现有会话输入框随内容区宽度展开。模型、权限和 Skill 控件按可用宽度换行。模型按钮显示模型名及已登记的配置名称，选择菜单保留提供商和协议。

`UserRunOptions` 由用户或代表该用户的可信宿主提交，仅用于带首条用户消息的创建、空闲发送及用户输入入队。省略模型时，在受理时从当时的 `AgentDefinition::model` 解析具体注册修订。空的权限预设引用表示使用宿主基础授权策略。Preview 会显式提交用户选中的预设版本，默认提交 `Ask` 的版本。受理后得到 `RunStartBinding`。需要操作确认时，在登记待批准命令时固定同一绑定，并在批准后复核。恢复、重试和再生成使用各自入口的规则，不能隐式继承 UI 的待选项。

| 时点 | 配置及生效规则 |
| --- | --- |
| 新建首条消息或空闲发送 | 以提交的 `UserRunOptions` 或当时解析的默认值核对模型修订、助手能力和执行预设。会话、消息与 Run 按原事务边界受理。成功启动时，`RunSnapshot` 固定使用的模型和预设引用。 |
| 直接发送到活动 Run | 指定所见 `RunId`。所有补充沿用该 Run 的实际模型、预设和授权上限。宿主在模型调用及工具执行前仍核对最新可访问范围。 |
| 排队受理 | 把输入及解析后的 `RunStartBinding` 一起存入 `QueuedInput::user_start`。以后改变输入框偏好、宿主默认值或当前 Run 的设置时，已入队项继续显示原选择。用户可撤回，再用新命令排队。 |
| 真正消费队列 | 核对原提交者、当前会话绑定和工作区、精确的模型修订、能力、预设和有效授权。按队列状态契约提交结果。复核失败的项不创建 Run 或调用模型，允许后续有效队列项继续。界面按原队列 ID 找回输入并提示改选。 |

在当前配置目录找不到已提交的注册修订，或可见的预设换版或作用范围变化，返回 `Conflict`。预设过期返回 `DeadlineExceeded`，不满足助手或输入所需能力返回 `Unsupported`。权限失效返回 `PermissionDenied`，策略可以将不可发现的对象隐藏为 `NotFound`。模型存在但原配置的传输或认证失败时按提供商错误报告。宿主和运行时不得为失败请求自行替换模型、执行预设或接收方。当前任务因配置不可用而中止时，从已提交历史由用户选择新任务。相同命令重试取回原绑定或结果，重试内容发生变化时使用新命令。

### 审批预设的宿主绑定

`ExecutionProfileRef` 引用由宿主签发的版本化预设。宿主登记适用主体、工作区、可执行能力、有效期和授权来源，按管理策略确定可选项。引用本身不授予权限。`AccessPolicy` 在用户启动配置受理、批准后生效和队列消费时收到 `UseRunConfiguration`，同时收到实际主体、目标、工作区与 `PolicyRequest::run_configuration`。该检查只有 `Allow` 可继续，宿主可返回明确的 `AgentError` 说明校验失败；提升授权需先由宿主取得用户确认。涉及该 Run 的模型输入、工具准备和执行权限检查也携带绑定的 `run_configuration`。Agent 消息、委派和项目任务沿用各自的宿主授权，不能注入用户的启动覆盖项或借用该 Run 的一次性批准。

| Preview 预设 | `AccessPolicy` 和审批 |
| --- | --- |
| `Ask` | 按项目授权允许低风险读取和暂存。需要确认时返回 `RequireConfirmation`，向用户显示实际计划。 |
| `Auto Review` | 对允许委托审核的准备完成工具返回 `AllowReview`。独立审查器按原计划批准、拒绝或转人工；超时、失败或不可用回到常规人工交互。 |
| `Full Access` | 用户确认工作区、允许的操作与有效期后，宿主可对范围内可豁免的操作返回 `Allow`。组织禁用或范围不可确认时不可选。强制人审、资源限制、跨会话权限及模型接收规则继续适用。 |

UI 改选范围只影响后续提交。管理员撤权、授权过期和强制审批规则收紧在实际调用前立即参与复核。未执行计划和自动批准按当前授权结清或转人工，已开始的外部写入按原调用核对实际效果。启用方式、预设引用和准入结果按宿主的审计策略记录。 [宿主自定义工具审批](#宿主自定义工具审批)


桌面 Preview 从进程配置注册模型。`HUXERUI_AGENTKIT_MODEL` 等环境变量配置默认端点。`HUXERUI_AGENTKIT_MODEL_PROFILES` 可登记以逗号分隔的配置名，每个配置读取 `HUXERUI_AGENTKIT_<名称>_MODEL`、`_BASE_URL`、`_PROTOCOL`、`_PROVIDER`、`_API_KEY` 等同名选项。Web Preview 读取部署目录中的模型配置。输入区显示符合助手能力的具体注册。宿主以本地主体和 `SessionId` 为键保存后续偏好，重开应用读回已保存的模型修订及预设引用。已受理队列项及 Run 继续使用提交时各自持久化的绑定。授权由宿主另行核对，重开后的 Full Access 需要重新确认。

Preview 只把本地导出目录内新建单个饼图 CSV 的准备计划委托审查。默认由本地规则审查。设定 `HUXERUI_AGENTKIT_REVIEW_MODEL` 和 `HUXERUI_AGENTKIT_REVIEW_BASE_URL` 后使用独立模型。可通过 `HUXERUI_AGENTKIT_REVIEW_PROTOCOL`、`HUXERUI_AGENTKIT_REVIEW_PROVIDER`、`HUXERUI_AGENTKIT_REVIEW_API_KEY` 等配置该端点。独立配置本身不加入对话模型选项。审查请求只包含经宿主检查的操作、目标范围、目标数和是否覆盖等事实。请求不携带对话历史、报告内容、文件名或本地路径，不声明工具，也不请求服务端续轮状态。

独立模型须提交结构化的 `approve`、`deny` 或 `defer` 并正常结束流。配置不完整、能力不符、模型失败、异常响应或在审查截止前未得出有效决定时，原计划转人工。审查使用最多 15 秒，受 Run 剩余期限约束。持久记录绑定审查器标识及配置修订。切换规则和模型审查、切换模型端点或模型配置修订时，待恢复的决定按当前配置复核。

Full Access 授权需确认工作区路径，适用期为当前应用进程内的 30 分钟。授权覆盖代码助手在已分享文件内的补丁。测试、其他工具审批和跨会话操作继续使用各自的策略。应用重启或授权过期后，执行前会检查该授权是否仍有效。

### 续轮、分叉与恢复

本地历史和已提交的工具结果是会话恢复依据。每次提交完整模型轮次时，可将协议必需的签名、往返字段或提供商续轮引用包装成 `ModelResumeState`。带服务器状态的引用仅在宿主启用 `allow_server_state` 后使用。该状态按 Run、对应轮次和已提交的输入边界保存，随 `continuation` 传给同一 Run 的下一轮。`ModelTurnRequest::items` 描述本轮选定的授权历史。`prefix_items` 指明续轮状态已覆盖的前缀长度，适配器按协议使用新增工具结果或其他后缀。不同 Run 各自维护状态，多个窗口观察同一个 Run 时共享该 Run 的状态。运行时校验 `ModelProviderRef` 和状态格式，凭据由适配器持有。续轮包装格式不匹配时留给同一配置尝试从授权历史重建，端点或模型的修订变化关闭已有续轮状态。`allow_persist_resume_state` 控制这类数据是否可落盘。正文历史、产物和其他模型的上下文各自使用授权投影。

向提供商发送请求前，按实际模型及接收方复核输入可见性。检查续轮覆盖的历史、附件和指令仍符合当前授权与选取范围。压缩、修改或撤销改变覆盖范围时，舍弃原引用，从获准历史重建。历史分叉、新 Run 或另选模型配置从各自授权的本地历史组装请求。原协议要求的工具关联项、签名或状态缺失且无法重建时返回 `Unsupported` 或 `ProviderFailure`，宿主按错误原因重新发起工作。已提交的工具调用及实际效果继续使用原 ID，恢复过程避免重复执行已经发出的工具。

### 工具定义与准备

```cpp
struct ToolRequest {
  ToolId tool;
  JsonValue arguments;
};

struct ArtifactMetadata {
  ArtifactRef ref;
  std::string type;
  std::string name;
  std::optional<std::uint64_t> size_bytes;
};

struct ArtifactPart {
  PartId id;
  std::string label;
};

struct ArtifactDependency {
  std::optional<PartId> output_part;
  SourceSlice input;
};

struct ArtifactDraft {
  std::optional<ArtifactId> id;
  std::optional<Revision> expected_latest;
  std::string type;
  std::string name;
  std::string storage_handle;
  std::optional<std::uint64_t> size_bytes;
  std::vector<ArtifactPart> parts;
  std::vector<ArtifactDependency> dependencies;
  std::vector<PartId> changed_parts;
  std::optional<ResourceEffect> published_effect;
};

enum class ResourceAccessMode { Read, Write };

struct ToolResourceClaim {
  ResourceId key;
  ResourceAccessMode mode;
};

struct PreparedChange {
  ApprovalPreview preview;
  std::vector<ArtifactDraft> preview_artifacts;
  std::vector<TargetBaseline> targets;
  std::vector<SourceSlice> inputs;
  std::optional<std::vector<ToolResourceClaim>> conflict_keys;
  JsonValue execution_data;
  UndoSupport undo_support;
};

struct PreparedToolCall {
  ToolCallId call;
  PlanId plan;
  ToolRef tool;
  ToolRequest request;
  PreparedChange change;
};

enum class ToolOutcome { Succeeded, Failed, PartiallyApplied, Cancelled, ResultUnknown };

struct ToolExecutionResult {
  ToolOutcome outcome;
  std::optional<JsonValue> model_value;
  std::vector<ContentBlock> blocks;
  std::vector<ArtifactRef> existing_artifacts;
  std::vector<ArtifactDraft> new_artifacts;
  std::vector<ResourceEffect> effects;
  std::optional<AgentError> reason;
};

struct ToolResult {
  ToolCallId call;
  ToolOutcome outcome;
  std::optional<JsonValue> model_value;
  std::vector<ContentBlock> blocks;
  std::vector<ArtifactRef> existing_artifacts;
  std::vector<ArtifactRef> published_artifacts;
  std::vector<ResourceEffect> effects;
  std::optional<AgentError> reason;
};

struct ResourceAccess {
  ResourceId resource;
  std::optional<ResourceRef> current;
  ResourceAccessMode mode;
  std::optional<huxerui::FileReference> file;
};

enum class ToolAccessPhase { Prepare, Execute };

struct ToolInvocation {
  ActionParty actor;
  ToolAccessPhase phase;
  SessionId session;
  RunId run;
  ToolCallId call;
  std::optional<ProjectId> project;
  std::optional<WorkspaceId> workspace;
  std::optional<GoalRef> goal;
  ToolRef tool;
  ToolEffect effect;
  JsonValue arguments;
  std::vector<TargetBaseline> prepared_targets;
  std::optional<ToolCallId> reverts;
  std::optional<ArtifactRef> candidate_artifact;
};

using ToolResourceResolver =
    std::function<Async<std::vector<ResourceAccess>>(ToolInvocation)>;

struct ToolPrepareContext {
  ActionParty actor;
  ToolCallId call;
  std::vector<ResourceAccess> readable;
  std::stop_token stop;
  std::vector<SourceSlice> sources;
  std::optional<ArtifactAction> artifact_action;
};

class ToolContext {
public:
  ToolCallId CallId() const;
  std::stop_token StopToken() const;
  std::vector<ResourceAccess> Resources() const;
  AgentResult<void> ReportProgress(std::string text);
  Async<InteractionOutcome> AskAsync(FormRequest request);
};

using ToolPrepare = std::function<Async<PreparedChange>(ToolPrepareContext, ToolRequest)>;
using ToolHandler = std::function<Async<ToolExecutionResult>(ToolContext, PreparedToolCall)>;

enum class ToolConcurrency { Serial, ReadOnly, DisjointResources };

struct ToolDefinition {
  ToolId id;
  Revision version;
  std::string description;
  JsonSchema input_schema;
  std::optional<JsonSchema> output_schema;
  ToolEffect effect;
  ToolConcurrency concurrency = ToolConcurrency::Serial;
  bool idempotent_by_call_id;
  ToolPrepare prepare;
  ToolHandler execute;
  std::function<Async<std::optional<ToolExecutionResult>>(ToolCallId)> lookup_result;
  std::function<std::optional<ResourceEffect>(const ArtifactRecord& before, const ArtifactRecord& after)>
      recover_published_effect;
};
```

`prepare` 只读取已有授权的数据、解析真实目标并准备暂存内容。准备前由宿主解析获授权的只读资源，批准后凭已登记的目标取得执行所需句柄。目标须明确给出期望版本；空的 `expected` 表示目标应不存在。执行器生成调用 ID、计划 ID，登记不可变的计划并绑定参数、目标、来源版本及预览。在准备回调返回后发布预览产物的版本，把引用固化到计划，再发布审批交互。`PreparedChange::targets` 是执行约束，`ApprovalPreview::targets` 是经宿主策略过滤的展示。只读工具没有自定义准备时，执行器从校验后的请求及宿主解析的授权范围构造计划。

执行成功或已发生副作用的业务失败由宿主通过 `ToolExecutionResult` 逐项报告，包含每个目标的前后版本、恢复引用和待核对状态。启动前或无法生成结果的基础设施失败使用 `AgentError`，运行时根据已知边界区分未执行、失败或结果未知。输出不符合 schema 时保留已发生的效果记录，将协议错误传给调用方。

### 工具结果与产物交付

宿主的 `ToolHandler` 返回 `ToolExecutionResult`，其中 `existing_artifacts` 引用已登记的版本，`new_artifacts` 包含本次产出的草稿。运行时校验草稿、来源和预期版本，并为通过校验的整批草稿分配实际的 `ArtifactRef`。宿主在回报前确保内容可按存储句柄读取。持久模式下其保存期限需覆盖已登记记录的保留周期。一批新产物的元数据、版本、依赖、产物登记事件和本次工具结果在同一事务提交。完成提交后再唤醒等待方和通知观察者。

`WaitForToolAsync` 和 `CallToolAsync` 返回该次调用的 `ToolResult`。其中 `published_artifacts` 按 `new_artifacts` 的相对顺序提供本次成功登记的版本，`existing_artifacts` 保留本次引用的原版本。执行回报可把已登记的预览放进 `existing_artifacts`。两组引用按读取权限投影，工具完成快照的 `artifacts` 展示同一批可见版本。宿主可直接用返回的引用读取或准备下一个工具调用。例如宿主按注册工具约定的输出顺序，或通过 `Artifacts().ReadAsync` 返回的产物元数据取得 PPTX 版本，用它准备导出。测试报告的版本可以直接用于协作请求。

每次交付工具结果都复核调用者对确切产物版本的当前读取权限，包括恢复后的等待和重复读取。会话快照、历史消息和事件重放按观察者的当前权限过滤产物目录、工具、Run 结果、审批、队列及消息块中的产物引用。结果含有被过滤的引用时，省略可能包含相同引用的宿主结构化载荷。`RefreshAccessAsync` 清除同步快照使用的授权缓存。使用旧授权的事件订阅以 `CursorExpired` 结束。调用方可重新取得快照，或用原事件序号重新订阅并获取当前可见的事件。原提交中的工具结果和事件记录供恢复与核对使用。

`CompleteAsync` 在非结构化轮次使用 `ModelInput::allowed_tools` 显式选用 Agent 声明且该 Run 当前有权发现的已注册工具，以及适用的内置 Skill 工具。普通工具进入现有的权限、预算与审批路径。一次宿主模型循环内，`ModelReply::artifacts` 按模型调用及各工具的发布顺序返回 `published_artifacts` 中已提交的版本，核对原会话、Run、调用 ID 及用户和 Agent 的当前读取权限。恢复时用原指令和工具集合继续，沿用已保存的调用及产物版本。普通模型工具循环与同一 Run 的独立宿主工具调用互斥。模型实际可接收的数据继续按 `SendToModel` 过滤。

工具部分完成时，宿主可以同时报告已确认的资源效果和仍有用的完整产物。任何新草稿的版本不符或引用无效时，拒绝登记整批新草稿。在原工具结果中记录具体失败原因及已经发生的资源效果。本地提交失败时将原调用保留在待结清状态。提交结果不明时先核对前一次提交，再使用已知的执行回报或按调用 ID 核对实际效果。重启、恢复等待和重复核对使用同一 `ToolCallId`。已有提交时读取原来登记的产物版本。原提交确定未生效且已取得可信执行回报时，可以在原调用下补登记。已登记的版本按原记录返回，写入结果仍未知时沿用核对流程。宿主保留未结调用所需的草稿内容，并清理确定未被登记的暂存内容。

### 同轮工具调度

模型输出的多个调用默认按声明顺序串行。宿主通过 `ToolDefinition::concurrency` 声明可并行的工具，并将 `RuntimeOptions::max_parallel_tools_per_turn` 设为大于一。并发上限的最小值为一。预算继续按每个调用计数，并在调度前逐项预留。可并行的工具需要在准备阶段声明覆盖执行阶段全部资源访问的 `conflict_keys`，使用宿主归一化的 `ResourceId` 和读写模式。同一真实资源或重叠范围在各工具中使用共同的键。键缺省表示范围未知，降为串行。空集合表示无需访问可冲突的资源。需要写入的计划仍须覆盖所有写入目标并提供基线。准备结果与工具效果或写入目标矛盾时，在审批前返回 `InvalidArgument`。

| 声明 | 调度约束 |
| --- | --- |
| `Serial` | 等本轮之前的调用收尾后单独执行，后续调用等其收尾。 |
| `ReadOnly` | 仅用于 `ToolEffect::Read`，只声明读键，且与已在执行的调用在相同键上无读写冲突。多个调用可同时读同一个键。 |
| `DisjointResources` | 存在共享键且任一调用要写时串行。读写集合互不冲突时可一起执行。 |

准备读取授权的版本化快照，审批沿用原流程。就绪后按声明顺序取得执行名额和所需键，遇到未就绪或冲突的前序调用时等待。等待审批时该调用不占资源锁或执行名额。需要在执行中通过 `ToolContext::AskAsync` 等待用户的工具使用串行；并行执行中的调用请求表单时返回 `InvalidState`。执行前再查审批、权限及输入版本。取消停止新调用并按已启动调用的实际效果收尾。同轮调度用冲突键协调并行，跨 Run 的冲突和实际写入边界由宿主协调。结果按原调用 ID 归集，完成顺序可以与提交给模型的顺序不同。

执行阶段通过同一资源 ID 将实际授权与冲突键核对，授权范围超出已声明读写集合时返回 `InvalidArgument`。持久会话按调用保存原计划、审批、预算预留和已记录的结果，新记录使用会话 schema 2。读取 schema 1 时将单调用计划和审批迁移到原调用 ID。恢复同轮等待时重新核对授权和基线，已记录的结果按原调用顺序回传。执行已开始而结果仍未知时，按原调用核对实际效果。

### 动态工具目录

```cpp
struct ToolCatalogSnapshot {
  Revision revision;
  std::vector<ToolDefinition> tools;
};
```

宿主在启动前通过 `RegisterToolCatalog` 预留目录 ID。目录项使用 `<目录 ID>/<工具名>` 作为 `ToolId`，目录前缀专用于该目录。Agent 用 `AgentDefinition::tool_catalogs` 声明可用目录，与 `tools` 中的固定注册项一起按当前授权过滤。适配器用宿主的 `AgentRuntime::ReplaceToolCatalogAsync` 发布完整快照，首次发布可以在启动前完成。未发布的目录视为空。目录发布仅更新该目录；固定工具和 Agent 定义沿用启动前的注册流程。MCP 适配器按协议获取当前授权范围的完整 `tools/list`，收到列表变更通知时重新获取。适配器维护身份、获取和重新连接，按授权域隔离目录，工具效果和并行资格由宿主确定。 [MCP 工具列表与变更通知](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)

- 发布在应用线程原子替换目录。同一 Runtime 内目录修订严格递增，过期发布返回 `Conflict`，非法定义或命名冲突返回错误并保留之前的快照。目录中的 `ToolId` 固定对应同一能力，描述、参数、效果、处理器或授权绑定有变化时由适配器递增工具版本。工具移除后重新加入使用新版本。工具版本与身份跨重启可核对，目录修订用于当前 Runtime 的更新排序。适配器无法确认工具仍是原定义时，分配新版本。
- 模型每轮开始时装配当前获准的目录项，记录工具名到 `ToolRef` 的映射。运行中发布的变更进入下一轮。调用在准备前、审批生效时及执行前核对绑定的目录、工具版本和当前授权。已移除或变更的工具返回 `Conflict`，相关审批结清；授权撤销按权限失败处理。当前版本相同的工具可继续。需要新版工具时由新轮次或宿主发起新调用，并走所需审批。
- 宿主编排直接调用工具时，在调用受理时解析版本。连接或凭据的有效性由适配器在实际请求前核对，获准范围失效时发布移除对应条目的快照。已发出的调用按原版本记录结果或核对实际效果。需要恢复目录工具时，宿主在启动前发布可用快照。恢复对照已登记的 `ToolRef`，版本缺失或权限失效时按相同规则结清待调度调用。

`<huxerui/agentkit/mcp.h>` 的 `McpToolCatalog::Register` 绑定一个 Runtime、目录和宿主传输。每个服务器及授权域使用独立目录。宿主通过 `McpToolBinder` 将获准的 `McpToolDescription` 绑定到明确的版本、效果、并行资格和准备处理器。返回空绑定可隐藏工具。服务端描述和提示不授予权限；工具绑定变化时由宿主增加版本，启动恢复时沿用可核对的原版本。

启动前调用 `RefreshAsync` 发布初始目录。适配器获取并校验完整的分页清单后再发布。传输收到列表变更通知时调用 `HandleNotificationAsync`，刷新遇到 `Busy` 时保留变更信号并重试。`WithdrawAsync` 移除授权域的目录项，失效发生在刷新期间时，旧刷新不能重新发布该清单。分页或响应格式错误保留上一个已发布的快照。刷新期间及刷新失败后，尚未发出的调用在传输边界返回 `Conflict`，直至新清单验证并发布成功。传输报告授权撤回时清空目录，执行请求仍须以当前授权发送。

`McpToolExchange` 接收带协议元数据的 JSON-RPC 请求、对应工具定义（调用工具时）及取消信号，返回同一请求的最终响应。连接配置、认证、传输帧、订阅和连接重建由该传输管理。调用通过原审批计划发送，传输不自动重发工具调用。结构化输出校验后才交给模型，读取或执行结果按 AgentKit 的工具状态记录。此协议层只声明已支持的能力，未声明的多轮输入请求返回明确错误。 [MCP 工具协议](https://modelcontextprotocol.io/specification/2026-07-28/server/tools) · [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)

### 权限策略

```cpp
enum class Capability {
  DiscoverSession, ReadSession, CreateSession, ForkSession, TransferAgent,
  SubmitInput, CancelRun, SubmitInteraction, UseRunConfiguration,
  SendMessage, StartSessionRun, SteerRun, Reply, WithdrawMessage, WaitForResult,
  UpdateSession, ReadContext, SendToModel, UseContentAction, UseArtifactAction,
  DiscoverTool, PrepareTool, ExecuteTool, DiscoverSkill, ActivateSkill, ReadSkillResource,
  ReadArtifact, WriteArtifact, RevertChange,
  CreateComparison, AdoptCandidate, ReworkArtifact,
  ReadGoal, ManageGoal, EditGoalDefinition, CompleteGoal, ManageTask, PublishSharedContext, RecordSourceChange, ReconcileTool,
  ReadProject, DelegateRun
};

enum class AccessDecision { Allow, Deny, RequireConfirmation, AllowReview };

struct OperationTarget {
  std::optional<SessionId> session;
  std::optional<RunId> run;
  std::optional<CollaborationMessageId> message;
  std::optional<GoalId> goal;
  std::optional<ProjectTaskId> task;
  std::optional<ForkReviewId> review;
  std::vector<ResourceId> resources;
  std::optional<InteractionId> interaction;
};

struct PolicyRequest {
  ActionParty actor;
  Capability capability;
  std::optional<AgentRef> agent;
  std::optional<ProjectId> project;
  std::optional<WorkspaceId> workspace;
  OperationTarget target;
  std::optional<AgentId> new_agent;
  std::optional<ToolRef> tool;
  std::optional<SkillRef> skill;
  std::optional<std::string> skill_resource;
  std::optional<ModelId> recipient_model;
  std::optional<GoalRef> goal;
  std::optional<CustomBlockAction> content_action;
  JsonValue operation;
  std::optional<ToolInvocation> invocation;
  std::optional<PlanId> plan;
  std::vector<SourceSlice> sources;
  std::optional<RunStartBinding> run_configuration;
  std::optional<ArtifactAction> artifact_action;
};

struct PolicyDecision {
  AccessDecision decision;
  std::string reason;
};

using AccessPolicy = std::function<Async<PolicyDecision>(PolicyRequest)>;
```

| 边界 | 检查 |
| --- | --- |
| 工具可见和准备 | 调用范围取当前主体授权、Agent 定义、调用阶段和资源范围的交集。模型轮次通过 `DiscoverTool` 按 `ToolRef` 和模型接收方检查目录可见性，仅展示 `Allow` 的条目。准备前核对参数及可读取的资源，再解析实际目标。工具处理器使用当前操作授予的资源句柄，执行批准只开放已登记的目标。 |
| 执行或协作副作用 | 策略返回允许、拒绝、要求用户确认，或允许工具计划进入受委托的审查。待确认交互绑定主体、会话、Run、调用、计划及目标。接收侧的消息控制独立判断。 |
| 实际调度与写入 | 执行器重查权限、有效期与版本，批准只消费一次。宿主在写入临界区复核资源版本和授权，或使用条件写入。工作区被编辑、来源失效、权限撤回或计划变化时结清旧批准，返回冲突或拒绝。 |
| 取消与核对 | 停止信号由运行时传递。已开始的工具逐资源返回结果；未知结果通过可信宿主回报或查询。运行时避免重复调度，同一批准绑定一个逻辑调用，外部操作的可重试性由工具的幂等或核对能力决定。 |

`RequireConfirmation` 对能在会话中审阅的操作创建 `InteractionSnapshot`。工具使用工具审批，其他操作使用操作审批，操作审批记录待处理命令和经过过滤的预览。无可见确认入口的调用返回 `PermissionDenied`。读取路径若需要新授权，应先通过相应用户操作取得。宿主凭据和准备数据只在执行端使用，用户、模型和审计获得各自允许的数据投影。

### 宿主自定义工具审批

```cpp
struct ToolReviewRequest {
  ToolInvocation invocation;
  PlanId plan;
  ApprovalPreview preview;
  std::vector<SourceSlice> authorized_intent;
  TimePoint deadline;
  std::stop_token stop;
};

struct ToolReviewResult {
  ToolReviewDecision decision;
  std::string reason;
};

struct ToolReviewer {
  std::string id;
  Revision version;
  Duration timeout;
  std::function<Async<ToolReviewResult>(ToolReviewRequest)> review;
};
```

宿主可通过 `RuntimeOptions::tool_reviewer` 提供规则或独立审查模型。`AllowReview` 仅对准备完成后的 `ExecuteTool` 有效，其他检查收到此决定时按策略配置错误结束。审查输入来自不可变计划，包括授权投影下的已校验参数、实际目标、基线和可审阅的差异或产物版本。可带入获准用于审查的用户意图片段引用。宿主解析获准引用，发给云端模型前按接收方权限筛选。审查器获得决策数据和取消信号，执行能力仍由原工具流程持有。由模型审查时仅接受受约束的结构化决定。用户表单、操作审批、强制人审工具及要求人审的目标完成沿用各自入口。

| 审查结果 | 调度 |
| --- | --- |
| `Approve` | 执行器核对审查仍对应同一计划、工具版本、目标基线、权限和有效期。核对通过后一次提交自动批准并进入 `Ready`。 |
| `Deny` | 记录拒绝原因，调用进入 `Denied`。 |
| `DeferToUser`、无审查器、超时或出错 | 在原计划上创建常规 `ToolApproval`，进入 `AwaitingApproval`。没有可见的人审入口时返回 `PermissionDenied`。 |
| 计划换版、授权撤销或取消 | 停止审查，旧决定失效并按原因结清。晚到响应仅在匹配当前审查实例时生效。 |

进入 `Reviewing` 前保存准备计划和 `Pending` 审查记录，记录绑定调用、计划及审查器修订。等待期间释放工具执行名额及资源锁，未批准的前序调用继续约束同轮调度。审查决定记录来源、审查器配置修订、计划及可公开的理由。自动决定只可消费一次。执行前复核宿主策略、批准、目标和工具版本。策略更新为强制人审时，自动批准失效并转交用户。已满足当前策略的批准才可在调度时消费。重启时复核已登记的计划与审查器修订，已提交且仍有效的决定按原记录恢复。没有已提交决定时走人审入口。审查截止时间取审查器超时与 Run 剩余期限中较早者。审查器超时转人审，Run 到期走 Run 的超时收尾。自动审查期间以工具的 `Reviewing` 投影展示，转人工后才产生用户输入等待。

### 宿主通过 SSH 提交任务

宿主将已配置的远端任务注册为 `ToolEffect::ExternalSubmit`。工具入参只声明宿主开放的任务标识，目标和命令由宿主配置。准备阶段通过 `ToolPrepareContext::readable` 核对所属项目和版本，审批预览给出 SSH 主机和任务。执行阶段按 `ToolContext::Resources()` 复核审批所绑定的授权和版本，再调用宿主配置的 OpenSSH 客户端。SSH 密钥、可信主机及网络可达性由宿主负责。示例使用批处理认证和严格的主机密钥检查。

取得可确认的远端退出码后，宿主将退出码及成功或失败状态交给原工具调用。SSH 已启动但断线、超时或本地取消时，若无远端退出状态，发起调用保持 `ResultUnknown`。宿主可根据实际远端证据通过 `ReconcileToolAsync` 结清。审批重放返回原回执。会话、Run 与审批状态保存在本地 AgentKit Runtime。

独立 SDK 宿主示例位于 [ssh_task_host.cpp](../tests/third_party_host/ssh_task_host.cpp)。配置第三方宿主时开启 `HUXERUI_AGENTKIT_BUILD_SSH_TASK_HOST`，宿主通过 Lib-Terminal 调用桌面 SSH 客户端。启用 Python 和 Paramiko 的验证环境可运行 `ssh_task_host` CTest，用隔离回环服务检查批准、拒绝、撤权、重放、已知非零退出和传输断开。

## Skill 的来源、激活和资源

Skill 是当前 Run 可采用的流程指令与配套资源。宿主通过 `RegisterSkillSource` 接入打包资源、获准目录或自己管理的远端目录，并在 `AgentDefinition::skill_sources` 声明可用来源。来源按 Agent Skills 规范提供 `SKILL.md` 的名称、描述、正文和可选文件。 [Agent Skills 规范](https://agentskills.io/specification) · [Agent Skills 接入指南](https://agentskills.io/client-implementation/adding-skills-support)

```cpp
struct SkillCatalogQuery {
  std::variant<AgentId, SessionId> target;
  std::optional<ProjectId> project;
  std::optional<WorkspaceId> workspace;
};

struct SkillSourceRequest {
  ActionParty actor;
  AgentRef agent;
  std::optional<ProjectId> project;
  std::optional<WorkspaceId> workspace;
  std::stop_token stop;
};

struct SkillResource {
  std::string relative_path;
  std::string media_type;
  std::optional<std::uint64_t> size_bytes;
};

struct SkillPackage {
  SkillRef ref;
  std::string instructions;
  std::vector<SkillResource> resources;
};

class SkillSource {
public:
  virtual ~SkillSource() = default;
  virtual Async<std::vector<SkillSummary>> ListAsync(SkillSourceRequest request) = 0;
  virtual Async<SkillPackage> LoadAsync(SkillSourceRequest request, SkillRef skill) = 0;
  virtual Async<huxerui::AsyncInputStream> OpenResourceAsync(
      SkillSourceRequest request, SkillRef skill, std::string relative_path) = 0;
};
```

### 发现、选用和权限

宿主为草稿按目标 Agent 和拟使用的项目、工作区调用 `AgentRuntime::ListSkillsAsync`。已有会话按 `SessionId` 查询，运行时先检查会话读取权限，再取当前绑定及所在范围。此时请求中的项目、工作区字段须为空。宿主编排使用 `RunContext::ListSkillsAsync` 查询当前 Run，默认模型循环在每轮的 `available_skills` 提供名称、描述与可回传的引用。发现时只读取元数据，按 Agent、主体、工作区与 `DiscoverSkill` 过滤。发给模型的目录还须满足对应接收方的 `SendToModel`。目录读取失败时保留其他可用来源，并把失败交给宿主诊断。不同来源的同名 Skill 用来源区分，选择和激活一律使用完整 `SkillRef`。每个来源在同一次发现中为每个 ID 提供一个当前版本。

用户在发送或调整输入时通过 `SendInput::skills` 指定版本，首条消息也使用该字段。此字段用于用户或其可信宿主创建会话带入的首条消息，以及发送、排队和调整输入。协作消息、委派和项目任务由执行侧按自己的授权选择 Skill，其 `SendInput::skills` 须为空。模型经内置工具激活，宿主编排经 `RunContext::ActivateSkillAsync` 激活，均在当前 Run 内核对 `ActivateSkill`、来源与版本。目录只展示允许发现的条目，需确认的激活走会话内的 `OperationApproval`，绑定当前 Run、Skill 版本、操作主体和发起方式。持久模式下随待办一起保存内部命令和绑定，恢复沿用原命令及截止时间，等待遵循 Run 的状态规则。内置工具和宿主编排的调用在批准或结清后返回。装载正文前及提交激活前重查权限。同一 Run 中，已经激活的版本经权限复核后返回已有的资源列表，正在审批的相同请求共用待办。对同一来源与 ID 请求另一版本返回 `Conflict`。请求的版本不可取得时返回 `Conflict`，来源暂时不可读时返回`ProviderFailure` 或 `ProtocolError`。显式选择的 Skill 未能激活时，相关输入的下一次模型轮次推迟到激活完成，失败时保留具体原因并结束该次执行。模型主动激活失败时收到对应工具结果。

### 模型上下文、资源与恢复

激活通过后，把获准的 `SKILL.md` 正文登记为 `ModelTurnRequest::skill_instructions` 的独立区块。适配器用协议支持的角色或显式区块标识这些流程指令，保留宿主指令的优先级，随该 Run 的后续模型轮次提交。目录、正文和提交给模型的资源计入上下文预算，模型对激活与读取的工具调用计入工具预算。普通历史压缩保留这一独立区块，超出预算时按预算规则报告。运行时通过 `SendToModel` 确认正文可发给实际模型。Skill 及其资料不得改变宿主指令、实际权限、预算或审批规则。普通资源由内容类型决定读法，引用文本作为上下文数据。

模型用内置读取工具，宿主用 `RunContext::OpenSkillResourceAsync` 获取已激活版本的文件。路径相对该版本的 Skill 根目录，来源必须限制解析结果在其授权范围内，包括链接跳转后的目标。运行时拒绝绝对路径和越界请求，打开前复核 `ReadSkillResource`、登记的相对路径及读取额度。读取接口只接受已有授权，需要新授权时使用宿主的显式操作。模型读取的结果还须通过 `SendToModel`。工具需求字段（包括 `allowed-tools`）可展示给用户，实际调用按原 Tool 策略处理。执行脚本须由宿主经注册的工具处理，并使用正常的准备、审批、执行和取消流程。

`SkillRef` 的版本对应不可变的正文与资源。激活成功后，在下一轮开始前提交 `RunSnapshot::active_skills`，记录版本与 `SkillActivationTrigger`。资料读取记录关联 Run 和 `SkillRef`，快照及审计按权限投影。目录更新供后续发现，已激活的 Run 继续使用登记版本。子 Run 和其他会话按自身授权重新选择。持久模式重启时按原版本恢复并复核权限。旧版本缺失返回 `Conflict`，访问撤销返回 `PermissionDenied` 或按策略隐藏。恢复无法读取或无权使用已激活的正文时，Run 以中断原因结清。撤销后，在再次装配正文或读取资源时返回权限错误。使用该正文的模型轮次尚有待调度工具时，执行器在调度前核对授权。已启动的工具按实际效果记录。

公开定义位于 `<huxerui/agentkit/skill.h>`。默认模型循环向支持工具调用的模型按需提供 `agentkit_activate_skill` 和 `agentkit_read_skill`。前者只接受该轮目录实际提供的引用。两个工具用字符串传递版本，关联原模型调用 ID。读取工具支持 UTF-8 文本及 JSON。其他媒体由宿主通过 `OpenSkillResourceAsync` 按原版本读取。已经送入模型轮次的资源在后续组装时重新核对读取和发送授权。

宿主编排可在无结构化输出约束的 `CompleteAsync` 中用 `ModelInput::allowed_tools` 显式允许 `SkillActivationTool` 或 `SkillResourceTool`。模型调用使用相同的预算、审批及调用 ID。可恢复的编排先保存检查点，恢复时以原阶段的指令和工具范围继续。宿主通过 `ActiveSkills()` 查询已激活的实际版本，通过 `ReadSkillTextForModelAsync` 读取需发给当前模型的 UTF-8 文本或 JSON。此读取复核资源和模型接收授权，并登记正在使用的版本和路径。

Preview 在通用会话的输入区按当前范围查询 Skill。选中的版本随首条消息、后续发送、补充或显式排队提交。发布演示流程可使用该选择，也可由模型提议激活并交给用户确认。大纲生成时按活动版本读取模板和验证报告，与 PPT 助手共用大纲表单、暂存 PPTX 和导出审批。

## 跨会话协作与子 Run

```cpp
struct Notice { SendInput input; };
struct SteerRequest { RunId expected_run; SendInput input; };

struct SessionMessageRequest {
  SessionId to;
  std::variant<Notice, WorkRequest, SteerRequest> body;
  std::optional<CollaborationMessageId> related;
  std::optional<TimePoint> deliver_by;
};

enum class ReplyStatus { Succeeded, Failed, Refused };

struct SessionReply {
  ReplyStatus status;
  std::string summary;
  std::optional<JsonValue> value;
  std::vector<ArtifactRef> artifacts;
};

enum class ReplyEnd {
  Received, Rejected, Expired, Withdrawn, TargetFailed, TargetCancelled,
  MissingReply, WaitTimedOut, WaitCancelled, WaitCycle
};

struct ReplyOutcome {
  CollaborationMessageId request;
  ReplyEnd end;
  std::optional<SessionReply> reply;
  std::optional<RunId> target_run;
};

struct WaitOptions { std::optional<TimePoint> deadline; };

struct CollaborationRecord {
  CollaborationSummary summary;
  std::optional<SessionMessageRequest> request;
  std::optional<SessionReply> reply;
  std::optional<ReplyOutcome> outcome;
  std::optional<AgentError> reason;
};

class SessionCollaboration {
public:
  Async<Receipt<MessageAccepted>> SendMessageAsync(CommandId command, SessionMessageRequest request);
  Async<Receipt<MessageAccepted>> ReplyAsync(
      CommandId command, CollaborationMessageId request, SessionReply reply);
  Async<Receipt<MessageAccepted>> StartRequestAsync(CommandId command, CollaborationMessageId request);
  Async<CommandReceipt> WithdrawMessageAsync(CommandId command, CollaborationMessageId message);
  Async<CollaborationRecord> ReadMessageAsync(CollaborationMessageId message);
  Async<ReplyOutcome> WaitForReplyAsync(CollaborationMessageId request, WaitOptions options);
};

struct DelegateRequest {
  AgentId agent;
  SendInput input;
  Budget budget;
  std::optional<JsonSchema> expected_result;
};

struct DelegateOutcome {
  RunId run;
  RunState state;
  std::optional<RunCompletion> result;
  std::optional<JsonValue> structured;
  std::optional<AgentError> failure;
};
```

发送方身份从运行时的用户或 Run 绑定取得。模型可使用宿主授予的内置会话工具，采用同一 API、命令去重和审批。发现、历史读取、创建和分叉复用 `SessionService`。发送、读取引用和跨项目共享分别授权。接收方的工作权限取自身配置、工作请求范围及当前授权交集。工具和资源限制取请求明确允许的范围。省略范围时，由接收策略给出默认范围。目标 Agent 按收到的来源信息把消息作为业务输入。创建独立会话可受发送侧审批约束，接收规则另行决定初始工作请求是否生效。接收方拒收可保留已创建会话及明确的拒收记录。一次性批准归原工具调用。

| 操作 | 提交和失败语义 |
| --- | --- |
| 通知 | 受接收策略控制，送达即进入收件箱。可附已授权的来源引用。 |
| 工作请求 | 可直接进入待确认或队列。目标空闲且接收规则允许时可自动消费，也可由获授权用户手动启动。接收确认可关联接收侧命令，其结果写回原消息。消费消息、分配根 Run、绑定项目任务尝试以及提交状态一次完成。带任务尝试的请求须匹配有效领取者和同一项目，使用任务的条件启动规则。保留稳定的消息 ID、投递状态和目标 Run。 |
| 指定 Run 的补充消息 | 需要接收侧调整权限，投递只针对 `expected_run`。目标状态及错误遵循 [直接补充](#运行中发送与补充) 的 `AlreadyClosed`、`Conflict` 和 `InvalidState`。受理前确定失败时返回对应错误。受理后、应用前发现目标失效时，将投递记录结清为 `Rejected`，在 `CollaborationRecord::reason` 记录原因。初始受理回执仍可查询。 |
| 撤回 | 可撤回尚未生效的消息，并以相应原因结束待回复请求。已启动的 Run 由接收会话的取消命令管理。 |
| 回复 | 由关联 Run 或获授权的用户提交，校验原请求、应答 schema 和产物。一个请求至多接受一条最终回复。回复可独立待确认，接受后直接更新请求记录并唤醒等待方。期限、回复到达、拒收和取消按提交顺序处理。 |
| 等待 | 原子读取已有结果或登记等待。收到已接受的回复时返回 `ReplyOutcome`，其 `end` 为 `ReplyEnd::Received`。等待超时或取消只结束本次等待，接收 Run 依自身规则继续。目标 Run 成功结束但未提交要求的回复时，返回 `ReplyEnd::MissingReply`。 |
| 循环与限额 | 等待图包含运行中的 Run、排队请求及目标会话锁定、项目任务依赖。新增等待会成环时返回 `ErrorCode::WaitCycle`。状态变化使已登记的等待成环时，结束环中最近登记的一条等待。协作回复等待以 `ReplyEnd::WaitCycle` 结清，任务结果等待以 `ErrorCode::WaitCycle` 结清。按会话、项目和因果链检查消息、唤醒、派生 Run 及预算上限。 |

宿主或 UI 观察结果与 Run 内等待的区分由服务句柄的绑定决定。Run 内等待可保存稳定请求 ID、期限和继续的宿主阶段。状态可恢复时从同一 Run 继续。用户授权的独立会话在发起 Run 结束后存续。

子 Run 使用 `RunContext::DelegateAsync`，与父 Run 共用会话。宿主在 `AgentDefinition::delegates` 声明直接可委派的 Agent，策略通过 `Capability::DelegateRun` 核对目标和输入。子 Run 以自己的 Agent 和模型版本执行，工具及资源授权取其自身与各级父 Run 的交集。模型上下文只装配委派任务的说明和显式来源。默认模型驱动可调用 `agentkit_delegate`，可用来源索引指向本次 Run 的显式输入。

运行时通过 `max_delegation_depth` 限制深度，通过 `max_parallel_delegates` 限制每棵树同时调度的子 Run。等待下一层委派的 Run 让出执行名额。祖先与子 Run 的额度同时约束模型轮次、工具调用、已报告 token 用量、上下文大小和期限。批准仍归具体子 Run 和调用。独立结果按请求顺序返回，父取消同时结清排队和已开始的子 Run。

需要恢复的宿主阶段先保存检查点，再调用带稳定 `CommandId` 的重载。已登记等待以 `WaitKind::Delegation` 和原命令出现在 `RunResume::waits`。重启后使用同一命令与批次参数重接现有子 Run，已结清的子 Run 直接返回保存的结果。复用命令但修改批次参数返回 `Conflict`。在恢复的待处理委派上，无命令重载也可沿用已登记命令。

运行时在调用或订阅建立时取得主体，异步阶段保持同一主体。打开的会话句柄绑定原主体。新的操作及继续交付观察数据前复核身份和权限。`Snapshot()` 返回最后发布的授权投影。运行时获知权限变化后立即清空失效投影，失效句柄读取时返回空指针。宿主授权变化后调用 `AgentRuntime::RefreshAccessAsync`，运行时重新投影活跃订阅和待办，并结清已失去权限的计划。主体变更后旧视图失效，由宿主以新主体重开并订阅。

## 项目任务、持续目标和共享上下文

```cpp
enum class GoalCompletionPolicy { Verified, RequireConfirmation };

struct GoalDefinition {
  std::string objective;
  std::optional<JsonSchema> evidence_schema;
  JsonValue completion_conditions;
  GoalCompletionPolicy completion = GoalCompletionPolicy::Verified;
};

struct GoalSpec {
  GoalDefinition definition;
  Budget budget;
};

enum class GoalState {
  Proposed, Active, Blocked, Paused, BudgetLimited, Verifying,
  AwaitingCompletionApproval, Completed, Cancelled
};
enum class VerificationStatus { Passed, MissingEvidence, Failed };

struct Evidence {
  JsonValue values;
  std::vector<SourceSlice> sources;
};

struct VerificationResult {
  VerificationStatus status;
  std::string reason;
  std::vector<SourceSlice> verified;
};

struct GoalCompletionReview {
  PendingApproval approval;
  GoalRef definition;
  Revision verification_revision;
};

struct GoalSnapshot {
  GoalRef ref;
  SessionId session;
  Revision revision;
  GoalSpec spec;
  GoalState state;
  Usage usage;
  std::vector<ProjectTaskId> tasks;
  std::vector<RunId> runs;
  std::optional<Evidence> evidence;
  std::optional<VerificationResult> verification;
  std::optional<GoalCompletionReview> completion_review;
  std::optional<std::string> reason;
};

struct ProposeGoal { GoalSpec spec; };
struct EditGoal {
  GoalDefinition definition;
  std::optional<std::string> preview_token;
};
struct EnableGoal {};
struct PauseGoal {};
struct ResumeGoal {};
struct CancelGoal {};
struct SetGoalBudget { Budget budget; };
struct SetGoalTasks { std::vector<ProjectTaskId> tasks; };
struct SubmitGoalEvidence {
  GoalRef goal;
  Evidence evidence;
};

using GoalChange = std::variant<EditGoal, EnableGoal, PauseGoal, ResumeGoal, CancelGoal,
    SetGoalBudget, SetGoalTasks, SubmitGoalEvidence>;

struct ChangeGoal { GoalId goal; Revision expected; GoalChange change; };
using GoalCommand = std::variant<ProposeGoal, ChangeGoal>;
class SessionGoals {
public:
  Async<std::optional<GoalSnapshot>> ReadAsync();
  Async<GoalDefinition> ReadDefinitionAsync(GoalRef goal);
  Async<GoalEditPreview> PreviewEditAsync(GoalId goal, Revision expected, GoalDefinition definition);
  Async<Subscription> ObserveAsync(Observer<std::optional<GoalSnapshot>> observer);
  Async<Receipt<GoalUpdated>> UpdateAsync(CommandId command, GoalCommand request);
};

enum class ProjectTaskState { Pending, Blocked, Ready, Assigned, Running, Succeeded, Failed, Cancelled };

struct TaskDependency {
  ProjectTaskId upstream;
  std::vector<std::string> required_artifact_types;
  bool accept_partial;
};

struct ProjectTaskSpec {
  std::string title;
  SendInput input;
  std::optional<GoalId> goal;
  std::vector<TaskDependency> dependencies;
  JsonValue completion_conditions;
};

using TaskAssignee = std::variant<SessionId, AgentId>;

struct TaskOutcome {
  TaskExecutionRef execution;
  std::optional<GoalRef> goal;
  ProjectTaskState state;
  std::vector<ArtifactRef> artifacts;
  std::optional<Evidence> evidence;
  std::optional<AgentError> reason;
};

struct ProjectTaskSnapshot {
  ProjectTaskId id;
  Revision revision;
  ProjectTaskSpec spec;
  ProjectTaskState state;
  std::optional<TaskAssignee> assignee;
  std::optional<AttemptId> attempt;
  std::optional<GoalRef> attempt_goal;
  std::optional<SessionId> claimant;
  std::optional<RunId> run;
  std::vector<SourceSlice> resolved_inputs;
  std::optional<TaskOutcome> outcome;
  std::optional<std::string> blocked_reason;
};

struct CreateTask { Revision expected_project; ProjectTaskSpec spec; };
struct EditTask { ProjectTaskSpec spec; Revision expected_project; };
struct AssignTask { TaskAssignee assignee; };
struct ClaimTask { SessionId session; };
struct ReleaseTask { AttemptId attempt; };
struct StartTask { AttemptId attempt; };
struct CompleteTask { AttemptId attempt; Evidence evidence; std::vector<ArtifactRef> artifacts; };
struct FailTask { AttemptId attempt; AgentError reason; std::vector<ArtifactRef> artifacts; };
struct CancelTask {};
struct RetryTask {};

using TaskChange = std::variant<EditTask, AssignTask, ClaimTask, ReleaseTask, StartTask,
    CompleteTask, FailTask, CancelTask, RetryTask>;

struct ChangeTask { ProjectTaskId task; Revision expected; TaskChange change; };
using TaskCommand = std::variant<CreateTask, ChangeTask>;

enum class SharedContextState { Draft, PendingReview, Published, Withdrawn };

struct SharedContextDraft {
  std::string title;
  JsonValue content;
  std::vector<SourceSlice> sources;
  std::string access_profile;
};

struct SharedContextItem {
  SharedContextRef ref;
  PrincipalId publisher;
  SharedContextState state;
  SharedContextDraft data;
};

struct ProposeSharedContext { Revision expected_project; SharedContextDraft draft; };
struct EditSharedContext { SharedContextDraft draft; };
struct RequestContextReview {};
struct PublishContext {};
struct WithdrawContext {};
using ContextChange = std::variant<EditSharedContext, RequestContextReview, PublishContext, WithdrawContext>;
struct ChangeSharedContext { SharedContextRef expected; ContextChange change; };
using SharedContextCommand = std::variant<ProposeSharedContext, ChangeSharedContext>;

struct ProjectSnapshot {
  ProjectId project;
  Revision revision;
  std::vector<ProjectTaskSnapshot> tasks;
  std::vector<SharedContextRef> shared_context;
  std::vector<SessionSummary> sessions;
};

class ProjectService {
public:
  Async<ProjectSnapshot> ReadAsync(ProjectId project);
  Async<Subscription> ObserveAsync(ProjectId project, Observer<ProjectSnapshot> observer);
  Async<Receipt<TaskUpdated>> UpdateTaskAsync(ProjectId project, CommandId command, TaskCommand request);
  Async<TaskOutcome> WaitForTaskAsync(TaskExecutionRef execution, WaitOptions options);
  Async<Receipt<SharedContextRef>> UpdateContextAsync(
      ProjectId project, CommandId command, SharedContextCommand request);
  Async<SharedContextItem> ReadContextAsync(SharedContextRef item);
};

struct VerificationRequest {
  std::optional<ProjectId> project;
  std::optional<SessionId> session;
  std::variant<GoalRef, TaskExecutionRef> subject;
  Revision expected;
  JsonValue conditions;
  Evidence evidence;
  std::stop_token stop;
};

using GoalVerifier = std::function<Async<VerificationResult>(VerificationRequest)>;
```

项目读取使用 `ReadProject`，任务操作携带目标任务 ID 供宿主策略核验。读取共享版本及其来源使用 `ReadContext`，把同一版本交给模型时再检查 `SendToModel`。带执行尝试的协作工作请求只能发往该任务的领取会话，启动时在同一事务登记投递状态、对应 Run 和任务状态。

- 宿主提供项目、成员、工作区和访问策略。任务图边在项目预期修订下更新；只接受同一项目的前置任务、去掉重复边并检查循环。初始任务从 `Pending` 归约为 `Blocked` 或 `Ready`。默认前置任务成功且必需产物可解析时就绪，接受部分结果由边和宿主验证器共同决定。领取时固定实际输入版本，并在启动前复核。 部署在同一项目的目标与任务可通过 `SetGoalTasks` 关联。运行时在同一事务更新目标和受影响的任务修订。`ProjectTaskSpec::goal` 是这一关系在任务侧的投影。新任务可通过该字段请求关联，既有任务的关联变更由目标入口提交。
- 分配指定可领取的会话或 Agent，领取以任务修订竞争并分配唯一 `AttemptId`。释放未启动的领取后重新计算就绪态。启动检查领取和输入版本，创建或原子绑定一个目标 Run。在 `Running` 中提交证据和产物。完成命令可处于待验证，验证通过且版本、执行尝试和引用仍有效时才提交成功。验证失败返回原因，任务保留实际结果，负责人可补证据或提交失败。Run 失败或取消时关闭对应尝试；Run 成功后，任务可继续等待提交或验证完成证据。失败与取消按原因结清。旧尝试或并发更新返回 `Conflict`，过期结果可供审计。重试创建新的执行尝试，保留旧产物引用。等待绑定明确尝试，避免悄悄换成后来的重试。
- 一条会话同时有一个已启用且未终结的持续目标。提案由获权 Agent 或用户提交，启用、暂停、恢复、撤销和提高预算使用用户或宿主授权。空闲、就绪、用户输入已处理且预算足够时，通过活动目标版本和尝试 ID 启动续轮。目标预算对已分配的本地及获准的跨会话工作做预留、结算和归因。
- 无进展、缺少证据或依赖阻塞时停止自动续轮并记录原因。暂停阻止该目标启动新 Run，活动 Run 可在已授予的预算与权限内继续收尾，也可单独取消。预算耗尽停止相关工作并核对已发出操作。单个 Run 成功只记录为进展。目标取消停止目标调度，独立会话的 Run 由其自身控制。
- 共享项在授权下提出、审核、发布和撤回，编辑产生新版本。历史与任务保留来源引用。读、解析、送入模型和写入共享资料分别授权；已撤回资料在执行前重新校验。普通共享资料作为模型数据，指令来源单独由宿主控制。发布替换版本和撤回已发布版本时，来源变更随原命令及项目修订一并提交。草稿与待审核的版本在发布前不影响已准备的计划。
- `EditSharedContext`、`RequestContextReview` 和 `PublishContext` 的 `expected` 指向最近的未撤回版本。`WithdrawContext` 可针对任意未撤回版本，编辑分配的版本号继续递增。错误版本返回 `Conflict`。Preview 从会话菜单管理该项目已导入的验证报告，保存新草稿时保留原资料的来源与访问范围，送审、发布和撤回分别提交版本化命令。

### Goal 模式的宿主界面

Preview 在会话内经 `Conversation::Goals()` 调用 `ReadAsync` 与 `ObserveAsync` 读取并订阅当前目标，展示目标定义版本、预算、证据、暂停和阻塞原因。创建入口提交 `ProposeGoal`。获授权用户核对目标和预算后，以 `EnableGoal` 启用。修改目标、暂停、恢复、取消及配置预算使用对应的目标命令和当前修订。Goal 面板通过当前会话快照单独显示 Run 的状态。完成或取消后的目标仍显示定义版本和累计用量。面板显示当前用户可读的证据，产物使用名称及确切版本。目标完成审批出现在该会话及统一待办。面板上的「打开完成审批」通过相同的待办导航按 `InteractionId` 定位请求，决定经 `SubmitAsync` 提交。

图表、任务列表等视图以已登记的任务执行尝试、用量和证据为展示来源。用户操作回执与观察到的快照可能异步到达，界面以目标 ID、命令 ID 和修订关联二者。同一会话的普通输入继续使用会话命令，目标调度让出优先级。

### 执行中修改目标

Preview 的目标编辑器将通用目标的多行完成条件保存为文字。工作区发布目标提供专用页数输入，保留项目、工作区和演示绑定。其他工作流持有的结构化条件继续由其宿主维护。完成策略可在编辑器选择验证后直接完成或追加用户确认。草稿绑定打开时的定义版本，提交使用本次影响预览的状态修订。定义已被更新时，原草稿保留并提示重新打开最新版本。能否完成取决于宿主验证器，Preview 内置的发布验证器核对实际产物和测试证据。

`GoalRef` 使用稳定的 `GoalId` 和不可变的定义版本，首次创建时版本为一。目标描述、证据 schema、完成条件和完成策略由 `EditGoal` 更新。相同定义只返回当前版本。预算和项目任务关联分别通过 `SetGoalBudget` 与 `SetGoalTasks` 修改。目标定义的版本只随定义改动递增。目标状态修订 `GoalSnapshot::revision` 用于命令并发控制，在预算、任务关联和状态变化时也可递增。累计用量按 `GoalId` 统计，编辑不重置用量、额度和目标时限。历史定义通过 `ReadDefinitionAsync` 按版本读取。会话入口只读取所属会话的目标，Run 入口也可读取自身绑定的目标版本。两者都需通过 `ReadGoal`。发布和调度的权限独立检查。

获授权的用户或宿主可提交 `ChangeGoal{goal, expected, EditGoal{...}}`。受 Run 约束的 Agent 通过同一入口提议修改。策略检查 `EditGoalDefinition`，获准的 Agent 请求进入用户确认。直接由用户或获权宿主编辑时，Preview 先用 `PreviewEditAsync` 读取结构化影响，提交时传入 `preview_token`。令牌绑定目标、拟提交的定义、预期修订、调用主体、有效期和本次影响记录。宿主需要确认的操作使用 `OperationApproval`，预览展示改动及获准的 `GoalEditImpact`。Agent 使用审批预览作为确认入口。申请后审批挂在目标会话，独立于发起 Run 的等待和收尾。预览本身不提交状态。执行器在生效前核对预期修订与权限。已有预览或待审批时，同时比对其完整影响记录。新增需要收尾的 Run、失效待办、继续执行或需要撤回的派发工作，或待执行调用先进入实际执行边界时，返回 `Conflict` 并要求重新预览。已结束且无需处理的计划可以从影响中移除。投影只展示调用方有权读取的项目。批准和目标完成按提交顺序裁决。旧状态修订或预览到期返回 `Conflict`，新命令编辑已终结目标返回 `AlreadyClosed`。重复请求按原命令去重。修改获准前，原定义继续参与调度。

每个目标关联的 Run 固定启动时的 `GoalRef`。由目标驱动或派发的 Run 使用该版本的定义。默认驱动将该定义加入获授权的模型输入，宿主编排从 `RunInput::goal` 取得绑定并按版本读取。发起任务或协作时，由执行器从可信 Run 或任务记录取得目标绑定。跨会话接收策略可检查 `ReceiveRequest::goal`，目标资料读取仍按接收方权限计算。独立会话按自身权限读取所需定义。

存储会话接收绑定目标的工作请求时，目标定义须可随之恢复。将临时目标派发到存储会话会返回 `InvalidArgument`。跨项目工作请求另按发送、接收与目标读取权限核验。项目任务由同项目的领取会话启动。

| 触发情况 | 生效规则 |
| --- | --- |
| 目标定义获准修改 | 一次提交新定义版本、原命令结果、受影响的本地调度与审批状态。仍处于提案或暂停的目标保留其状态。已启用目标按新条件重新评估预算和依赖。 |
| 主会话中有绑定旧版本的活动 Run | 停止旧版本开始新一轮模型调用、工具执行和任务派发，取消尚未提交的模型工作并保留已显示内容，结束与旧工作的表单、审批和等待。其子 Run 按父取消规则收尾。等待中的旧 Run 可直接进入收尾。已发出的工具按实际效果收尾，效果未知走原核对流程。旧 Run 进入 `Cancelling`，随后以 `Cancelled` 和 `GoalRevised` 结清。无目标绑定的普通 Run 继续按自身规则执行。 |
| 存在旧版本的计划、自动审查与调度 | 主会话旧 Run 及随其收尾的子 Run 使用同一边界。它们尚未发出的调用和审批失效，晚到的模型、验证和自动审查回调无法重新发布。等待启动的旧目标续轮登记失效。主会话空闲且用户输入已处理、预算与新条件允许后，启动绑定新版本的 Run。存在结果未知的旧工具时，影响相同资源的后续工作等待核对。同一版本和尝试最多启动一次。 |
| 已派往独立会话的工作 | 已经开始的独立 Run 与它的原任务尝试按原版本继续，可单独取消。其原工作范围内的工具与审批仍按接收侧权限处理。旧派发尚未启动时，待启动的消息按撤回处理，未启动的任务尝试解除领取并重新计算就绪。后续按当前版本重新核对任务输入、完成条件及范围，再领取或派发。独立 Run 派生的新目标工作也须通过当前版本校验。普通通知与无目标绑定的协作不受影响。目标派发在接收受理时从可信来源固定 `GoalRef`。有关联目标的项目任务在领取时记录 `GoalRef`，启动前复核。已交付的结果继续保留，其产物可按新目标显式引用和复核。 |
| 存在完成证据或待人审 | 将旧验证结果与完成审批归入原版本记录，撤回未结清的证据命令。新版本清空当前完成验证与待办。获授权调用方可显式引用历史证据并按新条件提交验证。旧版本回调返回 `Conflict` 或结清原操作。晚到结果可作为有版本的历史产物供新工作显式采用。 |

已启动的跨会话工作默认按原定义继续。宿主用 `WorkRequest::cancel_on_goal_revision` 显式声明只服务于本次目标定义的工作，例如最终演示导出。该工作在改版影响中列为 `retiring_linked_runs`，尚未执行的审批失效，已经开始的调用按原调用核对。当前 Goal 的 Run 可在旧执行尝试结清后，通过 `EditTask` 用当前计划更新同一任务。旧尝试保留其定义版本和产物。未改变条件的前置任务可继续执行或复用已提交结果。

工具是否在途以运行时提交的执行开始边界为准。修改先提交时，待执行调用无法取得许可。开始先提交时，按原调用记录确认未执行、已产生效果或结果未知。两窗口提交同一修订只接受一次实际改动。重复提交同一命令返回原回执。新目标定义、待失效记录、旧 Run 的收尾标记和当前调度资格在同一事务提交。重启后先恢复该提交并核对在途工具，再恢复新版本调度。已提交的新定义可从 `GoalUpdated::goal` 读取。

Preview 的发布目标可编辑页数条件。只修改页数时复用获批设计、已应用补丁和独立验证结果。新 PPT Run 按当前定义固定页数，确认大纲和验证实际导出时分别检查。完成验收还核对 PPT 产物来自当前定义版本，历史版本的文件和调用记录可继续审计。

### 目标完成验证与人审

宿主可在每个 `GoalDefinition` 选择验证通过即完成，或在验证后加一次用户确认。启用按 `ManageGoal` 检查权限，完成策略的变更走目标定义编辑流程。实际完成时再以 `CompleteGoal` 检查目标、调用主体和证据来源。宿主可以拒绝完成或强制人审。目标编辑权限独立授予，由此取得的确认只批准本次编辑。

`SubmitGoalEvidence` 同时核对当前目标修订、证据指向的 `GoalRef`、schema 与证据引用。Run 侧提交还须匹配自身绑定的目标定义版本。版本不符返回 `Conflict`。校验后登记证据，目标进入 `Verifying`，命令进度为 `Scheduled`。缺少验证器时返回 `Unsupported`。若提交证据本身需要确认，获得这次批准后才进入 `Scheduled`。可接受新证据的状态为 `Active`、`Blocked`、`Verifying` 和 `AwaitingCompletionApproval`，后两种状态接收新证据时先取代旧验证或审核。宿主验证器读取固定版本的证据及依赖结果。验证或等待完成确认时暂停目标自动续轮，已有 Run 和普通用户输入继续按各自规则处理。提交证据的 Run 结束后，已受理的验证和完成审核仍由目标会话持有。

| 检查结果或操作 | 目标与命令状态 |
| --- | --- |
| 缺少证据、验证失败、依赖失效或完成权限被拒绝 | 保留证据和原因，进入 `Blocked`，原证据命令记录失败。 |
| 验证通过且满足自动完成策略 | 复核目标修订、依赖、证据版本及权限，在同一次提交记录验证并进入 `Completed`，证据命令成为 `Applied`。 |
| 验证通过且需要人审 | 同一次提交保存验证结论、固定依赖和证据版本、更新后的目标修订及 `OperationApproval`。交互归属目标会话，通过 `pending_command` 关联证据命令，预览呈现获准的验证结论与证据。目标进入 `AwaitingCompletionApproval`，可通过目标快照或 `Commands()` 找到待办。 |
| 批准 | 核对交互版本、审批主体、目标定义版本和当前验证记录、已验证的依赖和证据版本及当前权限。本次批准满足同一计划的确认要求。复核通过后原子消费审批、结清命令并完成目标。重复决定只接受一次。 |
| 拒绝、到期、无法展示审批 | 记录原因并停止完成。目标进入 `Blocked`，命令依次为 `Rejected`、`Expired` 或 `Failed`。再次请求完成时提交新的证据命令。 |
| 暂停、取消、修改目标或替换证据，或依赖和证据失效 | 使旧验证结果和待办失效，更新后的目标按相应状态继续。旧命令结清并保留审计记录。版本核对失败返回 `Conflict`，已关闭的交互返回 `AlreadyClosed`。 |

`GoalCompletionReview::verification_revision` 标识发布审核的验证提交，`definition` 指明不可变的定义版本。证据与验证结论绑定这两项和已核验的依赖。纯用量更新沿用状态修订。调整预算时保留仍有效的验证和审核，预算耗尽而进入 `BudgetLimited` 或任务关联变化使依赖失效时，结清旧审核。原证据命令从 `Scheduled` 进入 `AwaitingApproval`，批准后进入 `Applied`。直接受理的回执中，`GoalUpdated` 指向登记证据时的修订，最终状态由 `Commands()` 与目标快照提供。持久化保存审核绑定的版本、原命令 ID 和有效期。重启后先复核绑定再恢复审核；处于 `Verifying` 的目标可按相同输入恢复无副作用的验证。

## 产物、分叉审阅、返工和回撤

```cpp
enum class ArtifactAvailability { Available, Missing, Withdrawn };

struct ArtifactRecord {
  ArtifactMetadata metadata;
  SessionId session;
  RunId run;
  std::optional<ToolCallId> call;
  ArtifactAvailability availability;
  std::vector<ArtifactPart> parts;
  std::vector<ArtifactDependency> dependencies;
  std::optional<CommandId> source_change;
};

struct ArtifactPreview {
  ArtifactRef artifact;
  std::optional<JsonValue> structured;
  std::optional<ArtifactRef> thumbnail;
  std::string summary;
  std::string type;
  Revision access_revision = 0;
  std::string name;
};

struct ArtifactResolveRequest {
  ActionParty actor;
  ArtifactRef artifact;
  std::string storage_handle;
};

struct ArtifactSelectionRequest {
  ArtifactResolveRequest resource;
  std::vector<PartId> parts;
  std::uint64_t byte_limit;
};

struct ArtifactStore {
  std::function<Async<ArtifactPreview>(ArtifactResolveRequest)> preview;
  std::function<Async<huxerui::AsyncInputStream>(ArtifactResolveRequest)> open;
  std::function<Async<std::string>(ArtifactSelectionRequest)> selection;
  std::function<Async<ArtifactDraft>(ActionParty, SessionId, ArtifactId)> resolve_staged_attachment;
};

enum class SourceChangeKind { Modified, Withdrawn, Reverted };

struct SourceChange {
  SourceSlice before;
  std::optional<SourceRef> after;
  SourceChangeKind kind;
};

struct ArtifactImpactQuery {
  ProjectId project;
  std::vector<SourceChange> changes;
  std::optional<CommandId> change_command;
};

struct ImpactItem {
  ArtifactRef affected;
  std::vector<PartId> parts;
  std::vector<SourceSlice> path;
  bool approximate;
};

struct ArtifactImpact {
  Revision project_revision;
  std::vector<ImpactItem> affected;
  std::optional<SourceChange> recorded_change;
};

struct SourceReplacement {
  SourceSlice previous;
  SourceSlice replacement;
};

struct ReworkRequest {
  SessionId session;
  ArtifactRef baseline;
  std::vector<PartId> parts;
  std::vector<TargetBaseline> targets;
  std::vector<SourceReplacement> replacements;
  bool accept_whole_artifact_scope;
};

struct RevertRequest {
  SessionId session;
  ToolCallId original_call;
  std::vector<TargetBaseline> expected_current;
};

class ArtifactChanges {
public:
  Async<ArtifactRecord> ReadAsync(ArtifactRef artifact);
  Async<ArtifactPreview> PreviewAsync(ArtifactRef artifact);
  Async<huxerui::AsyncInputStream> OpenAsync(ArtifactRef artifact);
  Async<ArtifactImpact> InspectImpactAsync(ArtifactImpactQuery query);
  Async<CommandReceipt> RecordSourceChangeAsync(ProjectId project, CommandId command, SourceChange change);
  Async<Receipt<ChangePlanAccepted>> PrepareReworkAsync(CommandId command, ReworkRequest request);
  Async<Receipt<ChangePlanAccepted>> PrepareRevertAsync(CommandId command, RevertRequest request);
};

struct CandidateRequest {
  SessionOptions session;
  WorkspaceChoice workspace;
  SendInput task;
};

struct ForkReviewRequest {
  HistoryBoundary from;
  ProjectId project;
  std::vector<SourceSlice> inputs;
  std::vector<CandidateRequest> candidates;
  JsonValue comparison_criteria;
  Budget shared_budget;
};

enum class CandidateState { Queued, Running, Ready, Failed, Cancelled };
enum class ForkReviewState { Running, Ready, Closed };

struct CandidateVerification {
  std::optional<ModelId> model;
  std::optional<WorkspaceId> workspace;
  std::optional<std::string> workspace_revision;
  JsonValue conditions;
  std::optional<Evidence> evidence;
};

struct ForkCandidate {
  CandidateId id;
  SessionId session;
  std::optional<RunId> run;
  CandidateState state;
  std::vector<ArtifactRef> artifacts;
  std::optional<CandidateVerification> validation;
  Usage usage;
  Duration elapsed;
  std::optional<AgentError> reason;
};

struct ForkReviewSnapshot {
  ForkReviewId id;
  Revision revision;
  ForkReviewState state;
  ForkReviewRequest request;
  std::vector<ForkCandidate> candidates;
  std::optional<CandidateId> selected;
  std::optional<ToolCallId> adoption_call;
  std::optional<ToolCallState> adoption_state;
  bool adoption_reconciled = false;
};

struct AdoptCandidateRequest {
  ForkReviewId review;
  Revision expected_review;
  CandidateId candidate;
  ArtifactRef candidate_artifact;
  SessionId target_session;
  std::vector<PartId> parts;
  std::vector<TargetBaseline> targets;
};

struct ForkReviewSummary {
  ForkReviewId id;
  Revision revision;
  ForkReviewState state;
  JsonValue criteria;
};

class ForkReviewService {
public:
  Async<Receipt<ForkReviewCreated>> CreateComparisonAsync(CommandId command, ForkReviewRequest request);
  Async<std::vector<ForkReviewSummary>> ListComparisonsAsync(SessionId source);
  Async<Subscription> ObserveComparisonsAsync(SessionId source, Observer<std::vector<ForkReviewSummary>> observer);
  Async<ForkReviewSnapshot> ReadComparisonAsync(ForkReviewId review);
  Async<Subscription> ObserveComparisonAsync(ForkReviewId review, Observer<ForkReviewSnapshot> observer);
  Async<Receipt<ChangePlanAccepted>> PrepareAdoptionAsync(CommandId command, AdoptCandidateRequest request);
  Async<CommandReceipt> CloseComparisonAsync(CommandId command, ForkReviewId review, Revision expected);
  Async<void> AbandonComparisonWorkspaceAsync(CommandId comparison, WorkspaceCopyPlanRef copy, WorkspaceId allocated);
};

struct ChangePrepareContext {
  ToolPrepareContext access;
  std::optional<ArtifactRecord> artifact;
  std::vector<ResourceEffect> original_effects;
};

struct ChangeAdapters {
  std::function<Async<PreparedChange>(ChangePrepareContext, ReworkRequest)> prepare_rework;
  ToolHandler execute_rework;
  std::function<Async<PreparedChange>(ChangePrepareContext, RevertRequest)> prepare_revert;
  ToolHandler execute_revert;
  std::function<Async<PreparedChange>(ChangePrepareContext, AdoptCandidateRequest)> prepare_adoption;
  ToolHandler execute_adoption;
};
```

### 版本、来源及局部返工

- `ArtifactRef` 固定到一个不可变版本。新产物可由执行器分配 ID，更新时以期望的最新版本进行条件提交。记录内容元数据、资源句柄、所属会话、Run、出产工具和片段关系。宿主生成内容、预览和流式读取。用户打开文件或导出走已授权的宿主入口。
- 片段 ID 由生产工具定义，例如设计节点、代码区域、报告结论或 PPT 页面。空片段集表示整个来源或输出。依赖边指向已提交的源版本。创建产物版本及依赖一起登记，反向索引可以重建并核对项目修订。
- 外部资源变化经可信宿主提交，项目共享资料变更、内部产物升版和成功回撤也产生变化记录。影响查询沿依赖边推导直接和传递影响，返回调用方可读的节点和范围。宿主无法确定片段时返回产物级影响，权限隐藏的原因路径以可见范围内的摘要给出。
- `InspectImpactAsync` 使用 `SourceChange::before` 的确切版本和片段。命中整份来源的依赖时可返回近似范围；隐藏中间节点后只返回获准的路径片段。查询期间项目快照或授权投影发生变化时返回 `CursorExpired`，调用方重新发起查询。
- Preview 从设计节点查询时，每个链接绑定命中的 `ArtifactRef` 与可用的 `PartId`。点击后通过 `Artifacts().ReadAsync`、`Sessions().OpenAsync` 和再次查询影响核对产物权限、所属项目和范围。代码差异按原调用登记的写入前后版本生成，现场文件修改后仍可审阅该历史版本。幻灯片按页 ID 打开，已升版的演示要求重新查询。近似范围明确标记，未定位片段时打开整份预览。过期或不可读的链接报错并留在设计节点，返回时恢复节点、滚动位置和链接焦点。
- 宿主可用 `RecordSourceChangeAsync` 记录已确认的来源变更。执行器核对 `RecordSourceChange` 权限、项目和前后来源的可见性，在同一项目提交保存命令、来源变更及修订。沿用原 ID 重放相同内容取得原回执，复用 ID 改动内容返回 `Conflict`。撤回项省略替换来源。宿主已获授权且发生撤回时，应在旧引用仍可核对的阶段登记撤回。
- 项目中的已存产物升版与对应来源记录同一事务提交。宿主可在 `ArtifactDraft::changed_parts` 传入旧版片段 ID，空列表表示整份。局部返工按批准的片段生成记录。获准查看该操作的调用方可从新版 `ArtifactRecord::source_change` 取得影响查询命令。
- 影响查询可以显式给出变更，也可用 `change_command` 指定一条当前主体的已提交变更。按命令查询时 `changes` 为空。运行时核对该命令归属和当前来源授权，返回原变更与按现行项目修订计算的影响。如果替换来源变得不可读，按对应权限错误结束查询。
- `Projects().UpdateContextAsync` 发布替换版本时以此前最近的已发布版本作为 `before`，撤回已发布版本时以被撤回的版本作为 `before` 并省略 `after`。以该操作的原 `CommandId` 作为 `change_command` 查询。原命令的结果仍是 `SharedContextRef`，重放取得同一受理回执。查询撤回记录仍检查原命令主体、当前 `ReadContext` 和原资料的来源权限，只返回变更标识及可见的影响范围。已撤回的正文在 `ReadContextAsync`、模型输入和显式变更查询中保持不可用。
- 返工以目标基线、替换来源版本和选定片段生成新计划。宿主验证其余片段的内容及稳定 ID，发布新产物版本。若只支持整份重做，返回 `ScopeExpansionRequired` 并展示扩大范围，用户明确选择后提交新命令。已有版本和已导出文件保留旧来源。新写入和导出再经现有审批。
- 基线在准备或写入前改变返回 `Conflict`，不可见或撤回的来源返回相应权限或不可用错误。待调度计划遇到失效引用时先结清旧批准，再按新来源审批。

工具计划持久保存开始准备时所见的项目修订。该修订之后提交的来源变更与计划声明的输入片段重叠时，未发出的调用按 `Conflict` 结清，待处理的人工审批失效，自动审查停止。已登记的撤回也会阻止使用该片段的新计划。运行时在批准、恢复和实际发出调用之前复核。未命中的片段继续按原流程处理，已发出调用按原 ID 核对实际效果。用户可在复核之后新准备计划，历史版本仍按当前授权读取。

`PrepareReworkAsync` 从产物所属会话的最新版本准备计划。替换请求给出原依赖边上的确切 `SourceSlice` 和替代切片。未选中的输出片段保留原依赖。只影响整份产物的边返回建议范围，接受整份返工时以新命令提交空片段列表和 `accept_whole_artifact_scope`。宿主可用已授权的旧内容和新内容生成预览，审批后由原调用按所存计划执行。

会话外发起的命令进入目标会话队列。启动后可通过 `Commands()` 读取关联的 Run 和工具调用。具备权限的宿主编排可在当前 Run 的空闲工具阶段直接受理，并使用工具等待接口获知结果。持久模式保存队列、计划和原调用 ID，重复命令返回原受理回执。选中的源版本、目标版本及权限在写入前复核。

Preview 的发布实现按文件记录批准的设计节点 ID。补丁准备时核对节点存在于任务收到的设计版本，并在审阅中显示关联。成功写入的实现证据将每个文件的片段连接到审批基线、写入后的文件版本和对应设计节点。已发布设计的审阅页可从所选节点发起影响查询。

Preview 的 PowerPoint 审阅页以当前页的稳定 `PartId` 准备报告换源。界面从该页已登记的依赖取得现用报告，在当前会话及所属项目的可见会话中列出可读取的报告版本。用户比较结论并选择替换版本，准备成功后回到原会话核对计划与审批。受理只登记任务，写入仍以工具批准为准。准备失败保留选择，切换来源时分配新命令 ID，提交结果不确定时可用原命令重试。

在新建 PPT 草稿和逐页换源界面，用户可以通过文件选择器导入含 `summary` 字符串的 JSON 报告。宿主通过选择器返回的 `FileReference` 读取最多 64 KiB，校验摘要后以文件名和摘要发布项目共享项。草稿随首条消息提交所选版本的 `SourceSlice`，模型输入继续经过接收方授权。用户在大纲中选择需要引用该摘要的页，生成工具为已确认内容中实际引用摘要的页面登记 `PartId` 和共享版本。换源审批复用同一条返工流程，并按选中页核对旧来源和新来源的权限及版本。

Preview 的验证报告概要按确切报告版本查询影响。界面按当前项目和所属会话过滤后展示仍有效的 PPTX 版本与页级定位，近似或整份影响标注复核范围。打开链接时再查依赖与授权，按 `SessionId`、产物版本和稳定 `PartId` 打开目标。候选版本出现新版时提示刷新，原会话和草稿可继续使用。点击定位只读取并显示产物，后续返工通过原计划和审批入口提交。

### 分叉审阅和合入

同一比较固定一份历史边界、输入版本与判定条件。创建命令登记比较和各分支 ID 后再启动，宿主按命令和分支幂等准备工作区。分支有自己的 Run 和预算并计入比较上限，可在隔离工作区执行或生成暂存候选。失败的分支保留诊断，可用结果仍能比较。审阅显示候选、范围、验证环境和证据。用户选定版本后，`PrepareAdoptionAsync` 向目标会话准备差异，写入使用该目标最新基线的独立批准。比较关闭后保留候选，活动分支由对应会话管理。

宿主可要求对创建比较、分叉候选或创建候选会话进行人工确认。`CreateComparisonAsync` 先完成只读检查。任一候选需要确认时，在源会话登记同一比较的 `PendingApproval`，固定历史边界、候选和工作区计划、Agent 修订、模型和权限预设。待审批时没有可见分支，也不会调用工作区分配器。批准后用原命令重新检查权限及各绑定，出现新增确认要求或绑定变更时返回冲突。拒绝后原命令结清为 `Rejected`。

按原命令重放时，返回最初的确认回执。通过 `Commands().ReadAsync` 查询该命令或审批回执，取得等待、执行、拒绝、失败或完成状态。批准生效时，源会话的命令结果、项目比较及全部分支和首个 Run 一次提交，提交确认后才启动。审批等待及批准后的调度可在同一设备重启恢复。按当前权限投影已创建的分支，直接创建和审批创建的比较都按提交顺序出现在源会话目录。

持久模式的一次存储事务登记比较、原命令回执、各分支会话及各自的首个 `RunId`。结果为 `CommitUncertain` 时先从同一命令核对，恢复使用原分支与 Run ID。任何候选准备失败时，比较及其分支保持未受理。工作区的准备可能先于存储事务，宿主按命令和分支核对已准备资源的归属。

共享预算中的总用量上限按候选数预分，余量按请求次序分配，截止时限由各分支共同遵守。共享工作区只开放读取和暂存工具，实际写入和外部提交在工具准备前返回 `PermissionDenied`。调用方可在 `ObserveComparisonAsync` 中读取成功和失败的分支。会话或输入撤权后按当前权限投影候选与回执，比较读取期间修订发生变化时可返回 `CursorExpired`。关闭比较以比较修订作条件，活动分支继续由其会话管理。

`ListComparisonsAsync` 和 `ObserveComparisonsAsync` 按源会话返回最近创建的比较摘要，包含已关闭的比较。摘要要求可读取源会话、项目及比较的共同输入。候选会话、产物、证据和选中项由读取单项比较时另行按权限投影。源会话被撤权后目录不可读，共同输入失效的比较从目录隐藏。

Preview 在同一窗口进入比较时保留源对话的已加载历史、滚动位置和草稿，通过比较页打开候选会话时也保留这些状态。点击「返回对话」后，键盘焦点回到源会话工具栏的「比较方案」。如果比较页是从候选会话打开的，宿主先用 `SessionService::OpenAsync` 复核源会话访问，再恢复原视图。复核失败时停留在比较页并提示原因。

`PrepareAdoptionAsync` 固定比较修订、已成功的候选、原候选产物版本及目标会话的明确资源基线。目标须为同项目内有写入授权的会话，可选比较的源会话，目标会话与候选会话须不同。持久比较使用持久目标会话。准备适配器从 `ChangePrepareContext::artifact` 读到已核实的候选记录及目标的只读授权。比较的选中项、调用 ID 与目标会话命令一起提交。沿用原 `CommandId` 可重放受理回执，待处理的审批和实际调用随目标会话恢复。

采纳使用独立的工具计划及审批。宿主许可委托审查时可走审查器。执行前重新核对源版本、访问权及目标资源基线。若排队期间较早的操作已发布目标新版本，旧计划可在启动前以 `Conflict` 结清，这时比较也显示 `ToolCallState::Conflict`。目标变更后的旧批准按冲突处理，可刷新比较修订并按新基线准备。待处理的采纳可从目标会话撤回或结束，比较关闭请求在该操作结清前返回 `Busy`。实际效果不明或部分应用需先核对，再决定是否准备新计划。新版本的依赖可回溯到选中的候选版本及范围。

隔离候选使用不同的 `WorkspaceCopyPlanRef`。分配请求的 `command` 是该候选的稳定命令，`comparison_command` 是所属比较的命令。宿主将每个副本计划绑定到比较和历史边界，并以候选命令幂等登记 checkout。Preview 会为两份候选固定同一个 Git 提交，显示分支和路径后再启动。

持久隔离比较通过人工批准或只读检查后，先有可恢复的 `Scheduled` 命令和固定候选计划，随后才调用工作区分配器。重启继续原命令并重新验证权限及基线，宿主以相同候选命令核对或复用已分配 checkout。直接提交隔离比较时，同一命令仍在处理可返回 `Busy`，调用方可查询命令状态。比较和分支以最终事务为准，未确认提交时保留 checkout 与登记。

`AbandonComparisonWorkspaceAsync` 供宿主在明确未提交的终态下封存一个候选副本计划和工作区 ID。Runtime 校验源会话授权、计划归属、处理中操作及实际会话占用，持久登记后才允许宿主回收。处理中返回 `Busy`，已提交或占用返回 `Conflict`，缺少可核对的命令返回 `NotFound`。重试相同封存请求沿用现有记录，其他会话及新比较不能再使用已封存计划或工作区。宿主另行核对路径、Git 注册、锁和未跟踪或忽略的文件，完成受控移除和工作区注销。

`ToolInvocation::candidate_artifact` 在采纳准备与执行时提供选中的确切版本，便于宿主选择资源解析器。Preview 代码适配器从候选产物及产生它的补丁调用读取持久的前后版本，以目标工作区内的相对路径映射实际文件。已配置测试时，执行前后核对候选文件版本，测试报告引用实际应用的实施产物。采纳请求范围与文件目标必须一致。

Preview 在本地暂存区用 Git 对候选修改前、候选修改后和目标当前磁盘内容做三方合并。无重叠冲突时，逐文件 diff 以目标当前内容为起点，保留独立的用户编辑。提交 `PrepareAdoptionAsync` 时只包含仍需写入的文件及对应 `PartId`。全部已包含时提示无需采纳。重叠编辑、未保存的编辑器草稿或目标版本在准备中发生变化时返回 `Conflict`。重叠编辑解决后可重新准备。准备过程只读工作区，执行时按新批准的基线及内容再次核对，复用版本化补丁写入与恢复记录。新实施产物按文件引用选中的候选及片段。

代码采纳在首次替换前登记每个文件的批准基线、计划版本及恢复引用，随后按原调用逐文件记录发出和实际替换结果。Preview 在比较页核对时读取这些记录。只有已登记的替换才计为应用，证据不全的调用继续待核对。设计稿和 PPT 以原调用最终登记的目标产物版本作为证据，私有暂存文件留待宿主维护。核对入口只读取现场和调用记录，结清时提交新的核对命令。已核对的部分应用可按已知效果准备回撤，新的采纳仍须按当前基线另行准备和审批。

Preview 的 PowerPoint 采纳适配器用目标演示的 `ArtifactRef` 构造资源基线，预览附上当前演示和候选的确切版本。仅支持整份演示采纳，选择部分页面时返回 `ScopeExpansionRequired`。批准后宿主把候选 PPTX 的完整字节保存到该调用的私有暂存文件，提交带 `expected_latest` 和 `published_effect` 的 `ArtifactDraft`。只有版本登记成功才产生应用效果。新版本沿用目标的产物 ID，逐页引用候选的对应页面。预览、读取、选区和导出均按已登记版本解析，导出使用独立的目标路径审批。

### 回撤已执行的变更

回撤从原工具调用的实际资源结果和受控的恢复记录生成反向补丁或补偿。只处理宿主明确支持并可核对的资源。效果未知时先查询或人工核对，结果未知返回 `ResultUnknown`。没有恢复能力时返回 `Unsupported`。同一原调用的前次回撤结果未知时，重叠范围的新回撤先结清原结果。可用 `ReconcileToolAsync` 将已核对的资源效果提交到原反向调用，保持原 Run 终态。审批绑定当前基线、反向计划和范围。基线改变时给出冲突，宿主可另行准备保留后续编辑的新计划。回撤是独立操作，原工具记录保留，新版本和反向操作单独审计。多资源的部分完成逐项标记，重放命令取回回执。

`PrepareRevertAsync` 接受原调用所属会话、调用 ID 和明确选中的 `expected_current`。每个目标的当前版本须与原调用记录的已应用版本相符。适配器的 `ChangePrepareContext::original_effects` 以该目标顺序提供可信记录。`ToolInvocation::reverts` 在准备和执行阶段都标明原调用，资源解析根据 `prepared_targets` 分别返回只读或执行授权。已知的反向写入按工具结果和来源变更一并持久化。会话快照的 `ToolCallSnapshot::reverts` 保留原调用链接，获准查询者可用 `source_changes` 中的命令读取影响。Preview 的代码适配器在实际写入前保存恢复数据，并用严格的内容比较和反向 diff 核对准备时的工作区。

以产物版本表示实际写入的宿主，可将 `ResourceEffect` 放在更新产物的 `ArtifactDraft::published_effect`。资源 ID 前后一致，两个版本明确且不同。同一调用内，每项资源只登记一次。预览产物和失败或取消结果不登记这类效果。运行时仅在条件发布成功后把效果记入工具结果。回撤按该产物的确切前后版本登记 `SourceChangeKind::Reverted`，和产物及调用结果一起提交。Preview 将设计版本映射为资源版本；恢复计划固定当前版本及经授权读取的前一版，实际发布仍以 `expected_latest` 防止覆盖较新内容。

宿主可为已注册的本地 `StageArtifact` 工具提供同步的 `recover_published_effect`，用于读取缺少恢复记录的旧会话。运行时先核对工具版本、成功调用、该调用发布的单一产物，以及同一会话内已登记的同类直接前一版本。新产物须明确记录对该前一版的整份依赖。回调只把这两份记录映射为资源版本与恢复凭据。与已保存的效果或宿主工具结果冲突时维持原记录。恢复后的工具快照和已保存的工具结果随会话的下次提交写入。Preview 仅为设计发布工具登记此映射。准备回撤时按权限读取两版内容，执行时复核当前版本及恢复来源，并沿用独立审批。

三个准备接口统一通过工具的准备、审批及执行流程。`ChangePlanAccepted` 表示准备已受理，目标 Run 忙且另行执行时可以排队。当前 Run 只能在自身获准的会话和范围里内联准备。界面根据回执关联的队列项、调用或交互观察结果。第三方宿主用 `ChangeAdapters` 提供实际比较、恢复、局部生成及执行操作。

会话快照仅向提交者投影产物变更的队列项。启动前复核失败时，队列项进入 `Rejected`，对应命令进入 `Failed`，保留同一原因。Preview 在原回撤页显示原因并恢复文件选择和重试入口，返回会话或重启后也可以看到最近一次变更的启动失败。回撤页观察到对应 Run 的审批后才提示返回会话审阅。

## Runtime 注册与宿主接入

```cpp
struct AgentDefinition {
  AgentRef ref;
  std::string display_name;
  std::string instructions;
  ModelId model;
  std::vector<ToolId> tools;
  std::vector<ToolCatalogId> tool_catalogs;
  std::vector<SkillSourceId> skill_sources;
  std::vector<ContentTypeRef> content_types;
  std::vector<ContextProviderId> context;
  std::vector<AgentId> delegates;
  std::vector<AgentId> collaboration_targets;
  std::vector<Capability> capabilities;
  ModelCapabilities requires_model;
  Budget budget;
};

struct RunInput {
  RunId run;
  RunTrigger trigger;
  SendInput input;
  std::optional<CollaborationMessageId> work_request;
  std::optional<TaskExecutionRef> task;
  std::optional<GoalRef> goal;
  std::vector<SideChatConclusion> conclusions;
  std::optional<ArtifactAction> artifact_action;
};

struct RunCheckpoint {
  std::string stage;
  Revision schema_version;
  JsonValue state;
  std::optional<MessageId> steer_through;
};

struct RunResume {
  std::optional<RunCheckpoint> checkpoint;
  std::vector<RunWait> waits;
};

class RunContext {
public:
  RunId Id() const;
  std::stop_token StopToken() const;
  Async<ModelReply> CompleteAsync(ModelInput input);
  Async<Receipt<InputRequested>> RequestInputAsync(CommandId command, FormRequest request);
  Async<InteractionOutcome> WaitForInputAsync(InteractionId interaction);
  Async<InteractionOutcome> AskAsync(FormRequest request);
  Async<Receipt<ToolCallAccepted>> StartToolAsync(CommandId command, ToolRequest request);
  Async<ToolResult> WaitForToolAsync(ToolCallId call);
  Async<ToolResult> CallToolAsync(ToolRequest request);
  Async<std::vector<SkillSummary>> ListSkillsAsync();
  std::vector<SkillActivation> ActiveSkills() const;
  Async<std::vector<SkillResource>> ActivateSkillAsync(SkillRef skill);
  Async<huxerui::AsyncInputStream> OpenSkillResourceAsync(SkillRef skill, std::string relative_path);
  Async<std::string> ReadSkillTextForModelAsync(SkillRef skill, std::string relative_path);
  Async<std::vector<DelegateOutcome>> DelegateAsync(std::vector<DelegateRequest> requests);
  Async<std::vector<DelegateOutcome>> DelegateAsync(CommandId command, std::vector<DelegateRequest> requests);
  Async<std::vector<RunSteerInput>> ReadPendingSteersAsync();
  Async<void> SaveCheckpointAsync(RunCheckpoint checkpoint);
  CommandService Commands();
  SessionService Sessions();
  SessionCollaboration Collaboration();
  ProjectService Projects();
  SessionGoals Goals();
  ArtifactChanges Artifacts();
  ForkReviewService ForkReviews();
};

using RunHandler = std::function<Async<RunCompletion>(RunContext, RunInput)>;
using RunRestore = std::function<Async<RunCompletion>(RunContext, RunInput, RunResume)>;

struct AgentRegistration {
  AgentDefinition definition;
  RunHandler run;
  RunRestore restore;
};

enum class ReceiveAction { Allow, Hold, Deny };

struct ReceiveRequest {
  ActionParty sender;
  SessionSummary target;
  CollaborationMessageId message;
  std::variant<SessionMessageRequest, SessionReply> payload;
  std::optional<GoalRef> goal;
};

struct ReceiveDecision {
  ReceiveAction action;
  bool allow_auto_start;
  std::optional<WorkScope> allowed_work_scope;
  std::string reason;
};

using ReceivePolicy = std::function<Async<ReceiveDecision>(ReceiveRequest)>;
using PrincipalResolver = std::function<AgentResult<PrincipalId>()>;

struct SessionTitleRequest {
  SessionId session;
  AgentRef agent;
  std::string first_input_text;
  std::stop_token stop;
};

using SessionTitleGenerator =
    std::function<Async<std::optional<std::string>>(SessionTitleRequest)>;

struct WorkspaceRequest {
  ActionParty actor;
  CommandId command;
  SessionId new_session;
  std::optional<WorkspaceChoice> choice;
  std::optional<ProjectId> project;
  std::optional<HistoryBoundary> fork_from;
  std::optional<CommandId> comparison_command;
};

using WorkspaceResolver = std::function<Async<std::optional<WorkspaceId>>(WorkspaceRequest)>;

struct ProjectBinding {
  ProjectId project;
  std::vector<WorkspaceId> workspaces;
};

using ProjectResolver = std::function<Async<ProjectBinding>(ProjectId)>;

struct ArtifactAction {
  ArtifactRef artifact;
  std::string action;
  JsonValue payload;
};

struct ArtifactActionContext {
  ActionParty actor;
  CommandId command;
  SessionId session;
  ArtifactRef artifact;
};

using ArtifactActionHandler = std::function<Async<SendInput>(ArtifactActionContext, JsonValue)>;

struct ArtifactActionRegistration {
  std::string artifact_type;
  std::string action;
  JsonSchema payload_schema;
  ArtifactActionHandler prepare;
};
struct ContentActionContext {
  ActionParty actor;
  CommandId command;
  CustomBlockRef target;
  std::shared_ptr<const CustomBlock> block;
};

using ContentActionPlan = std::variant<SendInput, SteerRequest, InteractionReply, ArtifactAction>;
using ContentActionHandler = std::function<Async<ContentActionPlan>(ContentActionContext, JsonValue)>;

struct ContentActionRegistration {
  std::string id;
  JsonSchema payload_schema;
  ContentActionHandler prepare;
};

struct ContentComponentRegistration {
  ContentTypeRef type;
  JsonSchema data_schema;
  std::vector<ContentActionRegistration> actions;
};

class SessionStore;

struct RuntimeOptions {
  huxerui::TaskScope tasks;
  PrincipalResolver principal;
  AccessPolicy access;
  std::string untitled_session_title = "New conversation";
  SessionTitleGenerator generate_session_title;
  std::optional<ToolReviewer> tool_reviewer;
  ReceivePolicy receive;
  ToolResourceResolver tool_resources;
  std::size_t max_parallel_tools_per_turn = 1;
  WorkspaceResolver workspace;
  ProjectResolver project;
  GoalVerifier verify;
  ArtifactStore artifact_store;
  ChangeAdapters changes;
  std::shared_ptr<SessionStore> store;
  std::function<TimePoint()> now;
  std::function<void(std::exception_ptr)> on_error;
  std::size_t max_delegation_depth = 3;
  std::size_t max_parallel_delegates = 4;
};

class AgentRuntime {
public:
  static AgentResult<std::shared_ptr<AgentRuntime>> Create(RuntimeOptions options);

  AgentResult<void> RegisterModel(ModelRegistration model);
  AgentResult<void> RegisterTool(ToolDefinition tool);
  AgentResult<void> RegisterToolCatalog(ToolCatalogId catalog);
  Async<void> ReplaceToolCatalogAsync(ToolCatalogId catalog, ToolCatalogSnapshot snapshot);
  AgentResult<void> RegisterContextProvider(ContextProviderId id, ContextProvider provider);
  AgentResult<void> RegisterSkillSource(SkillSourceId id, std::shared_ptr<SkillSource> source);
  AgentResult<void> RegisterAgent(AgentRegistration agent);
  AgentResult<void> RegisterContentComponent(ContentComponentRegistration component);
  AgentResult<void> RegisterArtifactAction(ArtifactActionRegistration action);
  AgentResult<void> ValidateRegistration() const;

  Async<void> StartAsync();
  Async<void> CloseAsync();
  Async<void> RefreshAccessAsync();
  Async<std::vector<SkillSummary>> ListSkillsAsync(SkillCatalogQuery query);

  CommandService Commands();
  SessionService Sessions();
  InteractionService Interactions();
  SessionCollaboration Collaboration();
  ProjectService Projects();
  ArtifactChanges Artifacts();
  ForkReviewService ForkReviews();

  Async<CommandReceipt> ReconcileToolAsync(CommandId command, SessionId session,
      ToolCallId call, ToolExecutionResult result);
};
```

宿主在应用线程提供应用级 `TaskScope`，注册模型、固定工具、工具目录、Skill 来源和上下文，再注册角色和渲染器。`ValidateRegistration` 核对目录、Skill 来源和其他注册项的引用、所需模型能力和相互联络的 Agent，允许先登记相互引用的角色。`StartAsync` 校验注册并载入持久状态，成功后受理会话操作。启动前可以登记同一 ID 的更高定义版本。固定注册项的定义在启动后保持不变，活动 Run 持有启动时的 Agent 版本。工具目录通过其发布接口更新。恢复需要原版本或显式声明的宿主恢复处理器。关闭运行时停止接受新命令，保存可恢复检查点并关闭订阅。宿主应在应用级作用域仍可用时调用 `CloseAsync`。未能完成的效果按结果待核对记录。

宿主在 `RuntimeOptions` 提供有效的应用级任务作用域和主体解析器。`access` 和主体解析器是必需回调，缺失时创建运行时返回 `InvalidArgument`。未配置 `receive` 时拒收协作请求。项目解析、资源解析、验证、产物存取和变更适配按需配置。需用的业务回调未配置时返回 `Unsupported`。内存会话可省略 `store`，请求 `Stored` 会话时须配置有效存储。

`AgentRegistration` 中缺省 `run` 使用默认模型循环。宿主需要限定阶段时接入 `RunHandler`，持久模式可用 `RunRestore` 恢复阶段。注册代码以带命名空间的 ID 引用工具。MCP 工具可经动态目录接入，资源和提示模板可通过上下文提供器暴露。Skill 来源按 `AgentDefinition::skill_sources` 接入，宿主编排和默认模型循环共用发现、激活与资源读取契约。实际工具调用使用同一权限和审批流程。

宿主的服务句柄由 `AgentRuntime` 获取。模型经内置工具，宿主编排经 `RunContext` 获取受 Run 约束的句柄。两者调用相同操作，运行时派生可信主体和范围，拒绝失效 Run 或未声明的能力。任何会话动作再次查询当前权限。渲染器获得授权投影和绑定到当前组件的动作入口。组件动作、表单与产物动作按各自绑定校验。实际写入走命令和工具。可信宿主可用 `ReconcileToolAsync` 核对未知结果。核对更新工具和资源记录，已终止 Run 保持终态。

第三方宿主使用 `AgentChat` 时，在 HuxerUI 应用初始化回调中用 `ApplicationContext::Provide` 安装 `std::shared_ptr<AgentChatSelection>` 服务。展示会话前填入已启动的 Runtime 和用于旁聊的助手 ID。

`ReconcileToolAsync` 核对会话访问权和宿主的 `ReconcileTool` 权限，并携带工具、原调用、Run 和被核对的资源。活动 Run 返回 `Busy`，结清后的新核对命令返回 `AlreadyClosed`。提交的 `ToolExecutionResult` 描述已确认的结果、资源效果及原调用已登记的产物。核对只登记证据，新增产物、消息块或模型结果返回 `InvalidArgument`。写入发出前保存的 `issued_targets` 固定核对范围和原批准基线。目标或既有应用记录不符返回 `Conflict`，证据覆盖不足时仍为 `ResultUnknown`。核对命令、原工具结果和对应比较状态共同持久化，`ToolCallSnapshot::reconciliation` 链接到核对命令。相同命令重放返回原回执，变更载荷返回 `Conflict`。已确认产生写入的中断 Run 拒绝整轮重试。

`ArtifactAction` 由运行时依据产物类型与动作名匹配 `RegisterArtifactAction` 项。用户由产物卡或 `Conversation::DispatchArtifactActionAsync` 发起。运行时核对产物在该会话的有效引用、可读权限、`UseArtifactAction` 及动作 schema。宿主的准备回调仅将动作翻译为 `SendInput`，运行时通过同一会话的发送流程登记 Run，或依策略请求确认。操作需要批准时固定原产物版本和准备后的输入，批准后重新核对。活动 Run 存在时直接入口返回 `Busy`。已有任务内的导出可以使用绑定交互，由原 Run 继续调用工具。回执与产物的实际写入分别查询。

### Run 内编排与项目调度

- 项目任务依赖用 DAG 表达跨 Run 或跨会话的交付顺序。项目服务检查环路、判断就绪、按任务修订领取，并将依赖结果交给具体的执行尝试。
- 单个 Run 的模型循环、条件分支、重试及并行委派由默认驱动和 `RunHandler` 组合实现。宿主使用 `SaveCheckpointAsync` 保存阶段 ID、状态版本和恢复所需的命令或尝试引用，并用 `RunRestore` 从已确认的阶段恢复。每个阶段真实执行仍走现有预算、工具、协作及审批边界。 宿主编排在安全边界通过 `ReadPendingSteersAsync` 取得补充，并在采用后以检查点提交应用记录。

自定义编排可用稳定的阶段命令 ID 经 `RequestInputAsync` 登记表单，提交后用 `WaitForInputAsync` 等待。登记只创建一个表单，`InputRequested` 返回交互 ID 和版本，重复命令返回原交互引用。读取已有答复与登记等待在同一提交边界进行。等待仅允许该 `RunContext` 所属 Run 的表单，并使用该表单的已登记期限。表单仍待处理时登记等待，提交、放弃及关闭时按 `InteractionOutcome` 返回。宿主处理本地表单时的 `AskAsync` 是登记并等待的便捷入口。可从中断恢复的阶段先保存带稳定命令 ID 的检查点，再登记表单。恢复时用相同命令重取回执，可覆盖检查点与表单登记之间的中断。重启后 `RunRestore` 同时收到检查点和已登记的等待引用，按原交互 ID、协作请求 ID 或任务尝试 ID 重接。已经提交的答复直接返回。旧等待的期限与结果继续使用原记录。未完成恢复回调前，保存的等待不会自行触发重复操作。

可恢复的宿主工具阶段用 `StartToolAsync` 以稳定命令 ID 登记调用，再用 `WaitForToolAsync` 等待同一 Run 内的调用结果。该回执只确认登记，工具之后仍按计划准备、审查和审批。恢复使用同一命令 ID 和 `ToolCallId` 读取已登记状态或结果。等待返回已提交的 `ToolResult`，包含可直接消费的 `published_artifacts`。效果待核对按原调用处理。`CallToolAsync` 是组合入口。宿主在完成阶段的检查点中保留继续执行所需的模型结果或子任务引用。模型流中断及工具内部提问按各自的中断和核对规则处理。
需要声明式工作流的宿主可以把其流程映射至 `RunHandler` 或项目任务，有独立发布、图形编辑或逐节点恢复需求时再定通用 Graph 契约。

### 自定义对话组件与动作

宿主在启动前用 `RegisterContentComponent` 注册带命名空间的组件类型、schema 版本、数据约束、渲染器及动作表。一次注册固定一个 schema 版本，重复定义返回 `Conflict`。Agent 通过 `AgentDefinition::content_types` 声明它可以发布的类型。应用可在 `AgentDefinition::instructions` 里描述组件何时使用，并在结构化输出或工具参数中提供该类型的数据 schema。类型声明和权限由注册和策略确定。模型提供数据，App 的渲染器构建 HuxerUI 视图。

`ToolExecutionResult::blocks` 可以提交 `CustomBlock`。使用模型结构化输出的宿主 `RunHandler` 通过登记的工具提交相同内容块。执行器提交前核对 Agent 的类型声明、类型版本、数据 schema、引用和大小。块中显式携带的产物引用须已登记，本次另行提交的草稿通过产物交付结果获取引用。未授权类型返回 `PermissionDenied`，非法数据返回 `InvalidArgument` 及字段错误，已发生的工具效果继续记录。持久记录保存类型、schema 版本、数据和纯文本回退，以及关联的交互和产物引用。旧消息缺少对应版本的渲染器时显示回退文本。运行时用 `MessageRef` 和 `BlockId` 标定完整消息中的组件，渲染器收到不可变投影及绑定的动作入口。饼图的图例筛选和 hover 等仅改变视图的操作由宿主组件处理。

运行时登记自定义块的类型、数据 schema 和可提交的动作。宿主从可见消息投影读取业务数据和来源引用，并选择自己的渲染器。组件数据以 UTF-8 JSON 保存，每块上限 256 KiB。纯文本回退须非空，最多 4096 字节，组件最多各绑定 64 个交互和产物。空块 ID 由运行时分配。产物访问撤销后，新快照和历史页隐藏依赖该版本的组件。分叉继续核对原产物版本和目标上下文授权，源会话的交互绑定留在原会话。

宿主从会话快照读取当前用户可见的交互，并与 `CustomBlock::interactions` 匹配。提交动作时仍需携带呈现的版本。产物审阅通过 `Conversation::PreviewArtifactAsync` 获得授权后的指定版本，预览包含名称、类型和当前访问修订。宿主按产物类型选择渲染，动作走 `DispatchArtifactActionAsync`。默认对话组件可以接收宿主的产物渲染回调，并提供通用预览。

宿主使用与所显示区块匹配的 `CustomBlockRef` 构造 `CustomBlockAction`，并调用 `Conversation::DispatchContentActionAsync`。运行时先按当前会话读取权限和 `UseContentAction` 检查主体，再核对完整消息的版本、块、动作 ID 及 payload，随后调用宿主的准备处理器。处理器读取已授权的组件数据，返回用户输入、调整指定活动 Run 的请求、已有交互回复或产物动作。实际提交复用触发目标的授权、版本检查和审批。工具或宿主发布时声明该块可访问的交互及产物引用。运行时核对它们的可见性和归属，实际动作只能使用这些绑定。对活动 Run 的调整须满足该会话现有的 `SteerAsync` 约束。响应表单的操作经 `SubmitAsync`，工具副作用由对应 Run 中的宿主编排通过原工具入口执行。例如，导出卡片可提交绑定到导出表单的决定，随后由等待该决定的 Run 准备并审批导出工具。

策略回调通过 `PolicyRequest::content_action`、`artifact_action` 和 `target.interaction` 识别本次动作及下游目标。产物动作启动任务时只接受该版本的资料引用。组件启动或补充任务时，可引用自身消息及组件绑定的产物。会话公开回执携带初始受理标记或待批准交互。已受理后的实际 Run、补充消息和表单消费记录从原命令进度读取。

产物动作派生的 Run 固定登记 `RunInput::artifact_action`，重开后由宿主编排读取同一版本、动作 ID 和载荷。工具准备收到相同的 `ToolPrepareContext::artifact_action`，准备与执行的策略请求也携带动作。宿主可用这个绑定核对工具参数，另行按原能力检查实际读取和写入。绑定本身不进入模型输入。提交失败可按原命令查询和重放。需要在终态后重新发起动作时，通过产物动作入口提交新命令。若先前写入结果未知，先核对工具结果和实际资源。

Preview 从 PowerPoint 审阅页选择文件名后，用 `DispatchArtifactActionAsync` 启动本地导出。审批显示所选版本和宿主导出目录中的完整目标路径。工具使用原动作的版本和文件名，拒绝覆盖已有文件。等待导出审批时重启，从原检查点恢复调用和交互。拒绝、取消或写入失败后，该版本仍可按权限审阅。

Web Preview 在应用的临时目录生成 PPTX，将完整文件异步写入浏览器持久目录后才登记暂存产物。导出批准后，同一版本复制到审批中显示的浏览器应用目录。工具发布与该版本关联的「下载 PowerPoint」入口，用户点击时再次核对产物访问权，再将已批准的文件交给浏览器下载。下载位置由浏览器管理。无法确认浏览器文件写入结果时保留原调用供核对。 [HTML 下载行为](https://html.spec.whatwg.org/multipage/links.html#downloading-resources)

执行器以原 `CommandId` 去重，在动作实际受理前再次核对组件版本和当前权限，把准备好的请求与提交结果一并记录。由动作启动的后续 Run 或工具结果通过会话投影查看。对原动作的重试返回已有回执，批准时使用已登记的请求。两次提交同一表单仍由交互版本仲裁。消息版本失效返回 `Conflict`，动作未知或载荷不符合 schema 返回 `InvalidArgument`，权限撤回按已有权限规则处理。`UseContentAction` 或实际业务操作需要确认时，在会话内发布交互。批准后重查消息版本和原动作目标，最终结果通过原命令查询。渲染器重新挂载和重新打开历史不会触发动作。需多区域动态界面的宿主可以接入 A2UI，将受支持的目录项及动作映射到本地已登记能力。 [A2UI 组件目录](https://a2ui.org/concepts/catalogs/)

同一命令仍在准备且尚无回执时，重复请求返回 `Busy`，调用方可用原 ID 重试。批准后的补充登记到原 Run 后继续保持 `Scheduled`，在 Run 应用或结束时结清。重启恢复已登记的补充，直到原 Run 到达安全边界。

## 持久化和恢复

```cpp
struct StoreWrite {
  std::string collection;
  std::string key;
  std::optional<Revision> expected;
  std::optional<std::vector<std::byte>> data;
};

struct StoreValue {
  std::string collection;
  std::string key;
  Revision revision;
  std::vector<std::byte> data;
};

struct StoreImage {
  Revision revision;
  std::vector<StoreValue> records;
};

struct StoreTransaction {
  CommitId commit;
  Revision expected_store_revision;
  std::vector<StoreWrite> writes;
};

class SessionStore {
public:
  virtual ~SessionStore() = default;
  virtual Async<StoreImage> LoadAsync() = 0;
  virtual Async<Revision> CommitAsync(StoreTransaction transaction) = 0;
  virtual Async<std::optional<Revision>> FindCommitAsync(CommitId commit) = 0;
};
```

- 存储后端按预期修订原子提交一组不透明记录，所有读者看到完整提交。命令回执、会话事件、项目更新、待调度事项和必要检查点一起提交。相同提交 ID 在恢复时可查询。后端使用运行时生成的编码，宿主负责介质、权限和保留策略。写入缺省 `expected` 表示键应不存在，缺省 `data` 表示删除已存在的键。提交 ID 重复时返回原修订，载荷冲突返回 `Conflict`。
- Preview 在原生目标使用 SQLite，在 Web 使用同源的 IndexedDB。Web 在同一事务检查修订并登记记录和原提交回执，事务成功后返回回执。会话、模型与权限偏好、会话归类都按本浏览器的持久存储范围恢复。并发写入按修订冲突处理，归类的并发更新按最新记录重试。 [IndexedDB 事务](https://www.w3.org/TR/IndexedDB/#transaction-construct)
- 重启先加载已提交快照，恢复队列、当前 Run 的补充及应用状态、消息回复、活跃 Run、目标定义及收尾记录、任务尝试、候选、产物依赖和未结清的工具计划和审查、目标审核及已激活的 Skill 版本。恢复检查点包含阶段和 schema 版本、待处理 ID、绑定的输入与授权范围。按同一适配器和授权输入边界恢复获准落盘的模型续轮状态。先按原命令查找已登记的操作，再以原交互、回复或任务尝试重新登记等待。已消费的请求、已分配的任务尝试和分叉子会话均按原 ID 恢复。会话来源、转交记录和原命令的创建结果在恢复后按当前权限投影。
- 外部写入前持久化调用 ID、计划和开始记录。恢复时先查询效果并重查授权。已提交的工具结果及产物引用按原调用 ID 一起恢复。查询返回空表示没有已知结果，只有明确证明未发出或工具声明且满足幂等条件时才允许调度。无法确认时记录 `ResultUnknown`，由宿主核对。无法恢复的 Run 保留历史和已获得的产物，以中断结清。
- 用户排队的请求保存已解析的模型和预设引用及结论快照，恢复后沿用原队列项并在消费前重新核对。临时旁聊在当前 Runtime 内持有；作为用户输入受理的结论及获准独立登记的产物按各自的保留规则恢复。运行时重放只更新状态。调度在恢复策略允许并核对当前权限、版本、预算及期限后继续。

## 调用流程与验收

### 失败与重试表

| 情形 | 调用结果 | 调用方处理 |
| --- | --- | --- |
| 相同命令重发，且载荷相同 | 按权限投影的原受理回执 | 按原 ID 读取当前命令或对象状态 |
| 同一命令 ID 对应另一请求，目标基线或执行尝试已变更 | `Conflict`，可附当前修订 | 刷新，并在确认新目标后分配新命令 ID |
| 本地提交结果不确定 | `CommitUncertain` | 先查询命令，再用原 ID 重试 |
| 已绑定的目录工具被移除或更换版本 | 原调用结清为 `Conflict`，未执行的批准失效 | 在新轮次重新选择工具并走所需审批 |
| 活动根 Run 占用目标会话 | `Busy`，排队入口可登记后续输入 | 显式排队，或等目标空闲时重试 |
| 授权撤回或目标不可发现 | `PermissionDenied` 或 `NotFound` | 刷新可见状态；新操作按当前身份和策略提交 |
| 数据或表单校验失败 | `InvalidArgument` 及 `fields` | 纠正请求或字段，再提交 |
| 订阅分页游标失效 | `CursorExpired` | 刷新首页，用新游标分页 |
| 外部写入已启动且结果未知 | 工具记录 `ResultUnknown`，后续重试可返回同名错误 | 按调用 ID 核对实际效果 |
| 重试的 Run 或其子 Run 已产生写入 | `RetryAsync` 返回 `Conflict` | 审阅已有变更，再按当前状态发起新请求 |
| 宿主核对未覆盖原写入或试图修改已知结果 | `ResultUnknown`、`Conflict` 或 `AlreadyClosed` | 保留现场和原调用，核实证据后按原命令查询或重试 |
| 仅支持全产物重新生成 | `ScopeExpansionRequired` 及建议范围 | 展示范围，用户选择后提交新请求 |
| 选用的模型修订或预设已变更、已过期、能力不匹配或授权撤回 | `Conflict`、`DeadlineExceeded`、`Unsupported` 或 `PermissionDenied`。入队后遇到同类错误则结清为 `Rejected` 并记录原因 | 保存原输入，显示受影响的配置，用户改选后用新命令提交 |
| 固定的直接发送目标已结束、已被其他 Run 替代或正在取消 | `AlreadyClosed`、`Conflict` 或 `InvalidState`，已受理但未应用的补充结清为 `NotApplied` | 保留草稿或原消息，用户明确选择新任务或排队 |
| 跨会话补充的目标在应用前失效 | 受理前返回对应状态错误；已受理的投递结清为 `DeliveryState::Rejected` 并保留原因 | 从原消息读取处理结果，按当前授权明确选择新的操作 |
| 新增等待成环，或状态变化使已登记的等待成环 | 返回 `ErrorCode::WaitCycle`；已登记的协作回复等待结清为 `ReplyEnd::WaitCycle` | 查看可见的等待原因，由现有工作各自收尾 |
| worktree 预览或基线失效，分支或目标目录被占用 | `Conflict` 或 `DeadlineExceeded`。申请的目标会话未创建 | 保留表单并重新准备，展示新的 commit、路径和分支 |
| worktree 已准备，会话提交回执丢失 | `CommitUncertain`，按原命令核对目录归属和会话记录 | 按原命令查找和恢复。确认未提交且目录仍由该操作独占后才回收 |
| worktree 被其他会话绑定、有用户文件、隔离路径被占用或归属记录不符 | 返回 `Conflict` 或对应的授权错误，保留目录及文件 | 检查当前状态，按原命令继续核对 |
| 选区锚点不可解析、正文仍在流式增长或选区不合法 | `Unsupported`、`InvalidState` 或 `InvalidArgument` | 等正文封存、重新选取或明确确认可引用单元 |
| 源版本被清理、分享撤回，或草稿指向已关闭 Runtime 中的旁聊 | `Conflict`、`PermissionDenied` 或 `NotFound`，按权限隐藏详情 | 保留其他草稿内容，标出失效引用，供用户重新选择或移除 |

### 端到端用例

| 流程 | API 顺序与核对点 |
| --- | --- |
| 新建与打开 | `Sessions().CreateAsync` 一起受理会话和首条输入。直接取得 `SessionCreated`，或原命令通过审批后从 `Commands()` 取得同类型结果，再 `OpenAsync`、`ObserveAsync`。测试重启、回执丢失和重复决定，目录最多出现一个新会话，首条输入最多启动一个 Run。 |
| 首条输入生成标题 | 创建回执到达时列表已有标题。覆盖空会话首次发送、优化失败、两窗口手动改名和迟到的优化，核对标题优先级和列表顺序。 |
| 运行中补充或排队 | 默认补充绑定原 Run，等待安全边界时显示待应用。待批准计划受影响时失效，写入先开始时保留实际效果。覆盖此时 Run 已结束、受理后尚未应用即结束、重启、重复提交和显式排队，核对输入归属和执行顺序。 |
| 会话来源与转交 | 创建、分叉或旁聊后打开 `Conversation`，核对快照中的来源、边界和权限过滤。空闲时转交并核对新绑定、历史系统消息和后续 Run 的权限。测试旧绑定版本、忙时转交和重开会话。 |
| Preview 模型与权限时序 | 用配置 A 启动，改选 B 后直接补充，再用 B 排队。改为 C，检查当前 Run 的补充仍用 A，队列显示并启动 B；空闲新发或新入队用 C。另验收 B 过期、撤销或能力不匹配后队列拒绝、保留输入且无模型请求。 |
| 审批预设、委托审查与权限收紧 | `Ask` 生成用户确认。用规则或独立审查模型验收 `Auto Review` 的批准、拒绝、转人工、超时、计划变化和重启恢复。`Full Access` 核对有效工作区及强制审批。工具即将开始时收紧权限，旧批准失效；已开始的写入保留实际结果。 |
| Preview 目录和 worktree | 打开目录后新建，移动归类并移回根列表，核对同一会话和工作区。活动源会话固定历史边界和 commit 分叉。源 HEAD 移动、目标目录冲突、创建失败及 worktree 创建后重启均须验收，重复命令只有一个目标。 |
| Preview 回收未提交的 worktree | 提交结果未知时保留 checkout。确认原命令未提交后，检查已使用的工作区、忽略文件及隔离路径冲突都能阻止回收。回收前封存原计划，核对旧命令及计划重放无法再创建副本。验证回收中断后继续结清 Git 和工作区登记，源会话和分支仍可使用。 |
| Preview 旁聊与来源定位 | 从中文连续文本、两行 diff 和 PPT 第 3 页元素构造切片，核对实际解析范围和 UTF-8 边界。完成追问后在草稿带回结论，显式提交及排队后重启。校验原会话焦点、源版本与授权过滤，撤权时无向模型发送。 |
| 代码补丁或 PPT 导出 | 模型输出工具调用，执行器完成准备与策略检查。UI 看见 `InteractionSnapshot` 后 `SubmitAsync`，宿主再次核对目标版本才写入。测试两窗口并发、版本变化和部分写入。 |
| 普通代码补丁结果待核对 | Preview 在写入时保存逐文件凭据。结果提交中断后通过 `ReconcileToolAsync` 核对原调用，并保留原 Run 终态、后续用户修改与已提交的审批。凭据不足时继续待核对。核对完成后按文件查看实际效果，已确认写入的文件可另行审批回撤。测试核对期间没有重写目标、启动 CTest 或发布未提交的产物。 |
| Preview 失败重试 | 会话空闲时从普通失败的最新根 Run 触发 `RetryAsync`，保留原 Run、原输入和当前草稿。重发同一 Run 的重试操作使用同一命令。父子 Run 的写入待核对或已确认时，界面显示对应的后续操作，执行器拒绝整轮重试。 |
| 首版代码助手的编辑器缓冲 | Preview 经 `ContextProvider` 将已共享的未保存文本按 `ResourceRef` 版本送入当前代码 Run。差异预览按实际磁盘基线展示将写入的完整变更，并附编辑器来源版本。纳入未保存内容的计划由用户确认，包括 `Full Access`。Preview 在应用本地保存草稿、版本和原磁盘基线。重启后恢复同一待办并继续原 Run。执行前重新核对访问权限、磁盘和草稿版本，任一变化或草稿不可用时拒绝旧计划，用户可保留草稿或丢弃后重新准备。 |
| 生成产物后导出或转发 | `WaitForToolAsync` 直接返回 PPTX 或验证报告的实际版本。用同一引用准备导出或发给另一会话。覆盖新旧引用、草稿登记冲突和结果提交后重启，确认按原调用恢复同一组产物版本。 |
| 模型协议对照 | 各登记组合用同一套流式输出、工具调用和回传续轮用例。覆盖分片和调用 ID、必需能力缺失、提供商私有续轮状态、异常结束及仅停止本地读取的取消。 |
| 自定义消息和动作 | 宿主注册饼图并验证数据，模型通过工具提交。筛选在本地更新，导出通过已绑定的表单进入工具审批。覆盖无对应渲染器时的文本回退、无效 payload、重发和历史版本冲突。 |
| 宿主恢复表单等待 | 受理表单后、答复提交后分别模拟中断。恢复回调按原命令和交互 ID 找到同一待办或已记录的答复，关闭和重开视图不影响 Run。 |
| 动态目录与工具调度 | 在模型流期间更新目录，下一轮使用新快照，旧轮已选中的换版工具报冲突。同一轮分别验收默认串行、允许的并发、共享键读写、待审批及取消。结果按原调用 ID 关联。 |
| Skill 加载与恢复 | 用户选用发布演示 Skill，模型也能在同一 Run 主动激活。核对只向模型公开获准条目的名称与描述，正文和模板按需读取，产物写入走工具审批。覆盖重复激活、同名来源、越界资源、版本撤回以及从检查点恢复原版本。 |
| 跨领域协作 | 通用助手确认目标、登记依赖，创建或找到设计和代码会话。验证报告按请求回复，通用助手附版本化引用给已有 PPT 会话并等待结果。测试接收暂缓、迟到回复、等待循环和断点恢复。 |
| 同会话委派 | 默认模型与宿主分别发起专家团，核对并发、父子关系、逐项失败和结果顺序。验证权限交集、全树预算、父取消，以及等待子审批时重开存储并续接原批次。 |
| 项目任务与目标 | Goal 面板以同一会话入口启用、暂停和恢复，用户输入优先。并发领取只产生一个有效尝试。上游产物发布后下游就绪。分别验收自动完成和人审完成。验证通过后重启可恢复同一审核，拒绝、过期或证据换版后旧审核无法完成目标。覆盖目标暂停和预算用尽。 |
| 运行中修改目标 | 以目标面板预览并把发布演示从 10 页改为 5 页。两窗口竞争只提交一项修改。旧导出和完成批准失效，已发出调用保留实际效果，独立验证会话仍可交付。重启后新 Run 使用新定义，只接受按新条件复核的报告和 5 页 PPTX。 |
| 分叉审阅与回撤 | 从同一历史边界准备两个候选，选中后按当前目标生成计划并授权。已有写入用原调用准备反向计划，测试基线变化、取消和未知效果核对。 |
| 采纳效果核对 | 分别在代码写入完成、部分写入且用户改动另一目标、设计稿和 PPT 已暂存但新版尚未登记时中断结果提交。重启后核对原调用。代码只对已确认的文件提供回撤，保留用户编辑。设计稿和 PPT 继续使用原发布版本，重新采纳通过新命令和审批登记新版。另验证证据缺失、授权撤回、同命令重放及同一 Run 的终态。 |
| 产物局部返工 | 记录新报告版本并查找受影响的页面，以明确范围准备返工。校验其他页面内容和标识，导出使用新批准，旧产物仍引用原版本。 |

用假模型、可控时钟、可注入的权限变化及存储提交故障验证状态归约和幂等。再从 Preview 和独立第三方宿主调用同一公共入口，对照 [完整版端到端验收](design.md#完整版端到端验收)。
