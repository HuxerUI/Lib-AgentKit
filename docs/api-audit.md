# Lib-AgentKit 公开接口审计

## 调用流程

| 阶段 | 入口与数据流 | 调用方要保留的绑定 |
| --- | --- | --- |
| 宿主启动 | 应用作用域创建 `AgentRuntime`，注册模型、静态工具、动态目录、Skill、上下文和业务渲染器，再注册 `AgentRegistration`。`StartAsync` 核对注册关系、读取 `SessionStore`，重接可恢复的工作。 | `AgentRef`、`ModelProviderRef`、工具及目录修订。带持久工作流时还需注册对应的 `RunRestore`。[运行时注册](../src/agent_kit.cpp#L267) |
| 会话与命令 | `SessionService` 创建、分叉、打开和分页。已打开的 `Conversation` 通过 `SendAsync`、`QueueAsync` 或 `SteerAsync` 受理输入。一次受理可返回工作 ID 或待批准交互。目录、会话和待处理交互分别观察。 | 保存原 `CommandId`，以 `Commands().ReadAsync` 或 `ObserveAsync` 处理待批准和不确定提交。直接补充绑定原 Run，排队保留受理时的 `RunStartBinding`。[会话命令](../src/session_commands.cpp#L1043) · [观察接口](../src/session_queries.cpp#L716) |
| 输入与模型 | Runtime 解析原版本的上下文、内联附件、共享资料和活跃 Skill，按接收模型复核授权。默认驱动组装 `ModelTurnRequest`，宿主流程则通过 `RunContext::CompleteAsync` 发起受限轮次。适配器交回文本、调用参数、用量、结构化结果和结束事件。 | `SourceSlice` 中的原版本与 UTF-8 范围，提供商调用 ID，以及已授权历史前缀对应的私有续轮状态。[模型装配与读取](../src/run.cpp#L97) · [来源解析](../src/selection.cpp#L162) |
| 工具与审批 | 参数经过 schema 和权限校验，准备回调产出 `PreparedChange`。人工或自动审批绑定原 `PlanId`，资源与版本复核通过后才调用执行回调。提交实际效果时登记新产物版本，按原模型调用顺序回传结果。 | `ToolCallId`、`PlanId`、交互版本和资源基线。结果不确定时核对原调用，读取 `ToolResult::published_artifacts` 获取真正发布的版本。[工具执行](../src/tool_execution.cpp#L138) |
| 协作、委派与调度 | 独立会话通过 `SessionCollaboration` 投递通知、工作或补充，并以请求 ID 接回复。同会话子 Run 通过 `DelegateAsync` 排并发和继承约束。跨会话任务依赖、领取和证据验证由 `ProjectService` 管理，持续目标的定义和完成确认归 `SessionGoals`。 | 投递、执行和等待各有状态。保留原协作请求、`AttemptId`、`GoalRef`、委派批次命令和检查点。[协作等待](../src/collaboration_wait.cpp#L10) · [子 Run](../src/delegation.cpp#L195) · [项目任务](../src/project_tasks.cpp) · [目标状态](../src/goals.cpp#L350) |
| 产物、界面与来源 | `ArtifactStore` 根据原版本和存储句柄读取字节、结构化预览及规范文本。`AgentChat` 与自定义渲染器使用授权快照。引用和旁聊提交同一组有序切片。比较、采纳、局部返工和回撤通过各自的准备及审批入口形成新操作。 | 原 `MessageRef` 或 `ArtifactRef`、块或部件 ID、精确字节范围，以及采纳与返工的目标基线。[产物预览](../src/artifacts.cpp#L426) · [来源选区](api-design.md#来源选区与旁聊) |
| 关停与恢复 | `SessionStore` 以全局修订和记录修订原子提交。重启按原 ID 恢复已受理命令、交互、工具、队列、子 Run 与宿主阶段。已发出的工具如缺少可信结果，保持待核对。 | 保留原 `CommitId` 和 `CommandId`，使用回执或原调用续接。重试和再生成均检查待核对效果，重试还有已执行写入限制。[装载与恢复](../src/persistence.cpp#L1048) · [重试与再生成](../src/session_commands.cpp#L872) |

## 字段及符号使用

事件流接入前的盘点覆盖 15 个公开头文件中的类型、别名、函数、字段和枚举成员。使用 Clang 解析 Windows 验收构建的 72 个翻译单元，覆盖库实现、Preview 和回归代码，结合定义、构造、持久化和对外回调核对用途。扫描在 1,383 个字段或枚举成员中定位到 1,339 个有直接带类型引用的成员。引用计数包括序列化，位置初始化、整对象比较和宿主扩展则需查生产和消费链路，不能由计数直接判断是否在用。

| 实际状态 | 符号及影响 |
| --- | --- |
| 事件流已接入 | `SessionEvent` 由提交路径生成，与持久会话快照一并保存。`Conversation::ObserveEventsAsync` 支持续读和恢复，`MessageDelta` 和 `ArtifactRegistered` 作为具体事件交付。现有快照观察独立提供界面投影。[事件设计](api-design.md#run事件和交互) · [观察实现](../src/session_queries.cpp) |
| 对外类型存在，默认 UI 不消费 | `InteractionBlock` 和 `ToolBlock` 在 `ContentBlock::body` 与存储编解码中保留。工具结果校验会拒绝这两类块，库没有专门的生产入口，`AgentChat` 也没有对应渲染分支。当前表单和工具状态分别来自 `SessionSnapshot::interactions`、`tools`。`ArtifactPreview::thumbnail` 可由宿主预览回调填入，默认 Preview 没有渲染路径。[块持久化](../src/persistence.cpp#L168) · [块校验](../src/content_components.cpp#L144) · [默认消息渲染](../src/agent_chat.cpp#L410) · [预览回传](../src/artifacts.cpp#L426) |
| 对外字段留空 | `CandidateVerification::workspace_revision` 目前参与序列化和 Preview 比较，没有找到比较运行时填入它的路径。`AgentError::retry_after` 可从宿主透传和持久化，内置 HTTP 适配器未解析重试延迟。`SessionTitleRequest::stop` 目前以默认令牌调用，实际取消由宿主任务生命周期管理。[候选构造](../src/fork_reviews.cpp#L335) · [HTTP 错误](../src/model_provider.cpp#L193) · [标题优化](../src/session_commands.cpp#L153) |
| 模型产物已接入 | `ModelReply::artifacts` 返回本次 `CompleteAsync` 中的工具已提交且当前可读的产物版本，恢复沿用原调用及发布记录。[工具结果与产物交付](api-design.md#工具结果与产物交付) |
| 配置与执行的边界 | `ToolDefinition::idempotent_by_call_id` 目前用于同版本工具目录的等价比较。恢复依靠结果查询或核对，该字段本身不触发重发。`ModelCancellation::StopReading` 是默认能力，注册校验按实际能力匹配 `ProviderCancel` 要求。`ModelStopOutcome::ProviderConfirmed` 可由自定义适配器返回，Runtime 保存该结果但尚无公开读出字段。[目录等价](../src/tool_catalog.cpp#L24) · [恢复](../src/persistence.cpp#L1048) · [模型能力](../src/agent_kit.cpp#L51) · [取消](../src/run.cpp#L350) |
| 枚举值尚无内置生产路径 | 当前缺少 `ArtifactAvailability::Missing`、`Withdrawn` 的状态更新入口，产物读取会拒绝非 `Available`。缺少回复目前落在 `ReplyEnd::MissingReply`，无 `RunEndReason::ReplyMissing` 生产者。取消本地等待通过任务取消收尾，无 `ReplyEnd::WaitCancelled` 结果生产者。`ReplyStatus::Refused` 和 `UndoSupport::Compensate` 是宿主可提交的值，前者随回复保留，后者进入通用补偿准备路径。[产物读取](../src/artifacts.cpp#L111) · [回复结清](../src/collaboration.cpp#L654) · [等待](../src/collaboration_wait.cpp#L66) |
| 由宿主消费或通过对象整体使用 | `AgentChatImage::message/block`、`ReceiveRequest::sender/target/message`、`VerificationRequest::project/expected`、`SessionTitleRequest`、`ContentActionContext::actor/command/target`、`ArtifactActionContext::actor/command`、`SkillSourceRequest::project/workspace/stop` 都由框架构造给宿主。Preview 可只读取自己需要的字段。`ResourceAccess::file` 随授权交给工具处理器，Runtime 仲裁使用资源 ID、访问方式和基线。`AgentChatInteractionRequest::ticket` 通过整个请求的依赖比较触发重复导航。[宿主契约](../include/huxerui/agentkit/runtime.h) · [导航依赖](../src/agent_chat.cpp#L1123) · [工具授权](../src/tool_execution.cpp#L523) |
| 独立调用接口 | `AgentChatArtifactText` 提供可独立组合的产物正文组件。Preview 的各领域审阅自行绑定选区，仓库内没有调用该组件，函数本身有实现。`Install(ApplicationContext&)` 当前是空函数，正常接入直接使用 `AgentRuntime::Create`、注册与启动。[独立组件](../src/agent_chat.cpp#L918) · [安装空入口](../src/agent_kit.cpp#L506) |

通过快照、回执或序列化消费也属于有效使用。例如 `InteractionConsumed::version`、`TaskUpdated::revision`、`AgentTransferred::from` 和 `CommitStamp` 的条件字段用于交给调用方，内置 UI 读取其中一部分。`ForkReviewSummary::revision` 随摘要构造并参与整对象比较。`MessagesThinking::mode` 决定实际请求字段。自定义适配器可选择使用 `ModelEndpoint::base_url` 描述实际连接配置，协议实现使用各自 Options 中的地址。

## 命名评估

| 当前名称 | 与实际职责的偏差 | 可考虑的命名或形状 |
| --- | --- | --- |
| `ModelProviderRef` | 锁定一个模型配置的注册 ID 和版本，provider 品牌只是 `ModelEndpoint` 的描述。 | `ModelRegistrationRef` 更准确。[定义](../include/huxerui/agentkit/session.h) |
| `GoalVerifier` | 同一回调接收目标或任务尝试，两条流程都使用。 | `EvidenceVerifier` 直接表达通用证据校验职责。[验证请求](../include/huxerui/agentkit/project.h) |
| `Budget::deadline` | 字段是相对 `Duration`，其他请求的 `deadline` 通常是绝对 `TimePoint`。 | `time_limit` 或 `max_duration`，保留绝对 deadline 的术语区分。[定义](../include/huxerui/agentkit/session.h) |
| `QueuedDraftReference`、`QueuedDraft`、`RecoveredDraft` | 排队拒收和未生效补充共用字段，后者通过别名复用了前者。 | 以 `RecoveredDraft`、`RecoveredDraftReference` 作为通用形状，旧名可保留兼容。[定义](../include/huxerui/agentkit/session.h) |
| `DelegateOutcome::result` 与 `structured` | `structured` 当前是 `result->structured` 的复制，两份公开结果容易被误解为独立数据。 | 统一从 `RunCompletion` 取结构化结果，或明确提供只读便捷访问。[结果构造](../src/delegation.cpp#L300) |
| `AgentChatSelection::side_chats` | 接入字段公开了一个 `detail` 类型的 UI 缓存，注册时只需要 runtime 和 assistant。 | 将缓存收回内部，由宿主选择 API 持有对话所需绑定。[定义](../include/huxerui/agentkit.h) |

其余收敛项包括从不填充的返回项、只作声明的配置及命名。修改枚举、持久化成员或公开类型前需核对已有记录的读取与迁移。
