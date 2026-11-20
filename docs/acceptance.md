# AgentKit 本地验收记录

验收范围以 [首版端到端验收](design.md#首版端到端验收)、[完整版端到端验收](design.md#完整版端到端验收) 和 [API 调用流程与验收](api-design.md#调用流程与验收) 为准。表中的验证结果仅对应已执行的场景。

## 同会话子 Run 委派

Windows Debug 和 WSL（GCC 14）均通过 `huxerui_agentkit_delegation_tests`。脚本模型调用 `agentkit_delegate` 后，两位专家同时执行，先结束的专家失败也能按请求顺序回传。宿主调用验证单名额下的嵌套委派、深度上限、模型轮次和工具用量共享、已报告 token 超限、排队超时及结构化结果校验。

专项还验证子 Run 无法使用父 Run 未声明的工具。父侧要求批准时，审批归该子 Run。待审批时撤销委派授权，旧批准不会触发工具。父 Run 取消后，两个各自等待输入的子 Run 和一个排队子 Run 均结清。

宿主编排和默认模型驱动分别在子 Run 待工具审批时关闭 Runtime，再用同一 SQLite 数据库创建新 Runtime。恢复后核对原父子 Run ID、审批版本及排队顺序。原批准仅执行一次，默认模型收到原调用的结果。

Windows 的十项相关 CTest 全部通过，覆盖委派、运行时、同轮工具调度、SQLite、模型协议、导航、审批、跨领域审阅、Goal 修订和完整 Preview 发布链。

## Windows Debug

统一产物引用权限后，使用本机安装的 HuxerUI SDK 0.5.0 在独立目录重新构建。13 项相关 CTest 通过，包含 MCP HTTP 短 SSE 通知、MCP stdio、SQLite、审批审阅、分叉及产物旁聊。SQLite 用例覆盖工具结果首次读取、撤权后的回执重放、快照刷新和从原序号续读事件。撤去 v1 的权限后只交付获准的 v2，重开数据库仍沿用原工具调用。完整 Preview 另行串行通过，包含工作区审批与实际 PPTX 导出。

Preview 从补丁生成前的模型失败进入 `RetryAsync`。验收核对原 Run ID、未发送草稿及新 Run 的 `retry_of`。新 Run 重新确认工作区范围和补丁后才写入。另从 SQLite 重开结果待核对的补丁，确认会话提示先核对且没有重试入口。核对确认实际写入后，会话提示审阅已有变更。委派专项还验证子 Run 的外部提交结果未知时，直接重试父 Run 返回 `ResultUnknown`，提交次数保持为一。Windows 的代码失败、补丁恢复、运行时、委派和 SQLite 五项 CTest 均通过。

SDK 消费端执行 `huxerui build windows --profile debug`，并在此前的验收中实际启动 Preview 窗口进程。库及宿主测试使用 Visual Studio x64 工具链，开启 `HUXERUI_AGENTKIT_BUILD_TESTS`，执行 `cmake --build build/acceptance`。本轮顺序执行的 CTest 十项全部通过，MCP 清单验证后的发送限制单独重建运行时目标并复跑通过。此前完整回归中记录过一次等待设计会话的超时，独立复跑通过并保留 `WorkspaceRelease.pptx`。发布链验收分别等待验证收尾和目标会话的审批。

接入同轮调度后，Windows 全部十一项 CTest 通过，包括会话 schema 1 审批迁移到 schema 2。Windows SDK 消费构建通过。MSVC 曾从旧的增量链接状态返回 `LNK1163`，移除相应目标生成的 `.ilk` 文件后重新链接成功。

接入审阅页导出和来源变更记录后，重新构建 Windows SDK 消费端，并以 `ctest --test-dir build/acceptance --parallel 2 --output-on-failure` 运行全套十二项 CTest，全部通过。

接入报告影响导航后，Windows SDK 消费端再次构建通过，十二项 CTest 全部通过。

接入共享资料的来源变更后，Windows SDK 消费端 Debug 构建及十二项 CTest 全部通过。

接入代码文件回撤后，Windows SDK 消费端 Debug 构建及十三项 CTest 全部通过。

接入设计稿版本回撤后，Windows Preview Debug SDK 消费构建及十四项 CTest 全部通过。此前 SDK 构建与专项测试并行时出现过一次 SQLite 测试超时，最新的完整回归通过。

接入分叉候选后，Windows Preview Debug SDK 消费构建与分叉、SQLite 两个专项通过。十五项并行回归中十三项通过，完整 Preview 和权限配置两个界面用例超时。权限配置串行复跑通过。完整 Preview 调整等待目标会话加载的条件后，以显式本地证据目录单独运行通过，保留 SQLite 状态、验证日志及三份导出的 PPTX。一次完整 Windows CTest 全绿仍待复验。

接入候选采纳后，Windows 与 Linux 的 Preview Debug SDK 消费端均重新构建并链接成功。两边通过 SQLite、分叉审阅和设计回撤三个专项。

接入比较入口和并排审阅后，Windows Preview Debug SDK 消费构建通过，分叉界面专项连续通过两次。WSL 的分叉界面专项连续通过六次，随后十五项完整 CTest 全部通过。

接入 PowerPoint 采纳后，Windows 和 WSL 的 Preview Debug SDK 消费构建均通过。Windows 串行 CTest 与 WSL 双任务并行 CTest 各通过全部十五项，包含完整 Preview、候选审阅、演示返工和 SQLite 恢复。

接入代码候选后，Windows 和 WSL 的 Preview Debug SDK 消费构建均通过。Windows 通过代码候选、worktree、分叉审阅、PPT 返工和代码回撤五个专项。WSL 重新构建并通过全部十六项 CTest。

接入比较创建审批后，两端的 Preview Debug SDK 消费构建均通过。Windows 通过分叉审阅、代码候选和 SQLite 三项回归，WSL 通过全部十六项 CTest。

接入候选 checkout 恢复后，Windows 和 WSL 的 Preview Debug SDK 消费构建均通过。Windows 通过候选恢复和代码候选两项专项。WSL 通过全部十七项 CTest。

接入采纳效果核对和按文件回撤后，Windows 和 WSL 的 Preview Debug SDK 消费构建均通过。Windows 通过代码采纳恢复和 SQLite 专项。WSL 通过全部十八项 CTest。

补齐部分写入、暂存产物及人工批准后 checkout 中断的用例后，Windows 和 WSL 的 Preview Debug SDK 消费构建均通过。Windows 通过六项采纳和候选工作区恢复专项。WSL 通过全部二十二项 CTest。

接入会话归类、统一待办和窄窗口导航后，Windows 和 WSL 的 Preview Debug SDK 消费构建通过。WSL 执行 `ctest --parallel 2 --timeout 145 --output-on-failure`，二十三项全部通过。Windows 重新构建后串行复跑完整 Preview、导航和 worktree 三项，全部通过。完整 Preview 验证从 Goal 面板切换到设计会话后继续发布链，产出实际 PPTX。

接入独立审阅和审批版本绑定后，两端的 Preview Debug SDK 消费构建通过。WSL 全部二十四项 CTest 通过。Windows 的完整 Preview、导航、审阅审批、设计回撤和旁聊五项通过，接入共享的对话草稿后复跑审阅审批、设计回撤和旁聊三项也通过。两端均执行 `--native-review-approval`，核对真实窗口的创建和独立关闭。

接入历史范围订阅、流式滚动和 PPT、设计审阅验收后，Windows 和 WSL 的 Preview Debug SDK 消费构建通过。WSL 全部二十六项 CTest 通过。Windows 通过导航、长对话、跨领域审阅、旁聊、SQLite 和分叉审阅六项。两端的 `--native-review-approval` 覆盖代码、PPT 和设计窗口，核对实际代码写入、PPTX 导出、设计发布和独立关闭。

对话产物卡片改为按原会话和确切版本读取有权访问的名称。Windows 原生 Preview 用旧 SQLite 打开 PPT 对话，核对 v1、v2 卡片和无障碍名称均含文件名及版本。从时间线点击 v2，审阅页显示同一名称和版本 2，核对采纳后的首页。使用构建前已有的会话及暂存文件复验，保持原存储格式。

Windows 的跨领域审阅、PPT 局部返工、分叉和旁聊专项通过。分叉前两次在第二个 PPT 候选的表单加载时超时，单独复跑完成采纳、导出和重启后键盘打开新版本。旁聊回归在宽窗发送完整结论，再切到 420 逻辑像素，待布局更新后重新打开原旁聊，核对输入框可见，并选取中文回复片段带回和提交。

Windows 从 520×680 逻辑像素的原生窄窗实际操作代码、PPT 和设计审阅。PPT 用页签切到第 3 页，返回对话后焦点在原卡片，按 Enter 重开保持第 3 页。独立窗口继承第 3 页，切到第 2 页并关闭后原窗仍在第 3 页。窄窗导出 v1 时在会话里确认完整路径，批准前无目标文件，批准后的 PPTX 与暂存和原导出逐字节一致，归档内三页标题正确。设计卡片直接打开同一版本的节点审阅，选中 `Increase contrast` 后带回引用，返回恢复原卡片焦点，再次打开恢复所选节点及标注。代码回撤列表显示 `config.txt` 和 `settings.html`，无障碍说明保留完整路径。仅勾选 `config.txt`，按反向差异的行号审阅并批准，实测恢复 `mode=disabled`，排除的文件哈希不变。

在同一原生窄窗的消息输入框用 Windows 简体中文拼音输入法键入 `ni`，观察实际候选窗和组字期间禁用的发送按钮。空格确认候选「你」后才可发送，核对同名会话、单条原消息与本地模型回复。模型流式更新时的中文组字与选区由 IME 专项验证。

复跑可先用 `huxerui_agentkit_preview_tests --native-review-approval <新验收目录>` 生成数据库、工作区及三类产物，再并排准备同名的验收可执行文件和 `.resources` 目录。资源目录使用 Windows SDK 消费构建生成的 `example_agent_kit.resources`。启动验收可执行文件时传入 `--native-preview-compact <sessions.sqlite3> <workspace>`，第二个路径用于加载与原验收一致的本地代码模型夹具。只传数据库可用来验收缺少原 Agent 修订的路径。

Windows 原生窄窗在缺少原 Agent 修订时从旧代码会话选择单文件回撤。队列拒绝后，回撤页显示实际原因、保留文件选择并恢复重试按钮。会话及重启后仍显示该原因，两份文件维持原字节。恢复代码模型后，显式提交同一单文件范围，审批出现才提示返回审阅。批准后只有 `config.txt` 恢复原值，`settings.html` 哈希保持一致。SQLite 核对首条队列项和命令保持 `Rejected`、`Failed` 且无 Run，重试以新队列项和命令完成。Windows 消费构建、代码回撤、回撤恢复和 SQLite 专项通过。SQLite 另验证有会话读取权限的另一身份无法读取原提交者的产物变更队列。

Windows SDK Debug 消费构建通过。原生窄窗修正后的设计回撤、分叉、跨领域审阅、代码回撤和 IME 专项通过。完整 Preview 回归修正 v1 和 v2 卡片的选择后，在发布验证截图阶段超过原 8 秒等待。将验证结束和后续 PPT 待办的等待与其他跨会话步骤统一为 25 秒，在独立目录从头复跑通过。保留的发布链证据包含实际文件写入、CTest 成功记录和实现截图。最终 `WorkspaceRelease.pptx` 的五页 XML 与内嵌图片完整，图片与实现截图逐字节相同，旧十页导出未生成。

回撤队列拒绝及重试修正后，单独运行 `ctest --test-dir build/acceptance -R '^huxerui_agentkit_preview_tests$' --output-on-failure`，完整 Preview 用例在 212 秒通过。本次仅运行该用例。

随后集中复跑 Windows 的 37 项专项，34 项通过。worktree 用例仍查找旧省略号按钮，在 Git 操作前失败。改为按源会话 ID 定位当前菜单后，专项通过。分叉审阅在并行回归中出现一次访问异常，同一二进制独立复跑三次通过，原因待定位。MCP HTTP 仍复现该 SDK 在 SSE 连接保持打开时未及时交付短通知的问题。

修复 Goal 暂停影响接收会话独立工作的队列问题后，重建 Windows 测试目标。运行时、SQLite、模型协议、PPTX 和 worktree 五项全部通过。

调整代码会话标题和编辑器后，从 Preview 目录执行 `huxerui build windows --config debug`，SDK 消费端成功生成 `example_agent_kit.exe`。

按桌面稿核对新建和侧栏后，Windows 原生 520 逻辑像素窗口用本地演示模型提交一条待审批消息。抽屉显示待办数量 1，并在会话行显示通用助手图标和待处理状态。重启后从抽屉恢复同一会话和审批。实测原会话菜单的第三个圆点贴边裁切，改为将图标置于独立的点击区域。重建后原生窄窗显示完整三点，点击可打开改名、移入归类和复制入口。导航回归在 1120、760 和 520 逻辑像素核对所选目录的完整路径位于工作区控件与归类之间，清除目录选择时路径一并移除。两条不同会话的待办在打开待办页前显示为 2，处理一条后显示为 1。

Windows SDK 消费端 Debug 构建通过。Windows 和 WSL 的导航、IME、模型与权限专项通过。调整三点菜单后在两端复跑导航和 worktree，均通过，并在 Windows 原生窗口复验点击。

Windows 原生 520×680 对话中选取 `approval`，打开旁聊并输入未提交的问题。关闭后保留原词选区，焦点返回原旁聊按钮。按 Enter 重开时，问题及输入框焦点恢复。Windows 和 WSL 的消息旁聊回归另验证不同选区各有自己的问题草稿。首次发送后焦点切到追问编辑器，切离原主会话再打开，在 420 逻辑像素宽度恢复同一个旁聊及未发送的追问。带回结论仍先更新主对话草稿，之后由用户提交。产物旁聊专项同时通过。

代码审批回归将活动的双文件补丁放到 520×420 逻辑像素的窗口。对话内审批按钮与输入框完整可见。打开附件后，文件标题、页签、实际差异、决定和返回操作都有可用空间。切换到第二个文件核对同一审批与磁盘内容，返回后焦点在原附件按钮上，输入草稿保持。原生 520×680 窄窗复验代码审阅的标题和页签不再裁切。审阅标题已显示文件名的情况下，差异区沿用同一标题，保留变更行数和补丁原文。Windows SDK Debug 消费构建通过，Windows 和 WSL 的审阅审批、跨领域审阅和代码编辑器专项均通过。

| 文档用例 | 实现与验证入口 | 已验证结果 | 仍需验收 |
| --- | --- | --- | --- |
| Preview 桌面导航和新建草稿 | `App`、`DraftChoice`、`SessionRow`、`RunSettingsControls`、`huxerui_agentkit_navigation_tests` | 按最新桌面交互稿将助手、工作区和归类纵向排列，共用标题和输入栏所在列。已绑定目录显示实际路径，原生同名目录的侧栏条目显示上级目录名并提供完整路径提示。Windows 重新构建 SDK 原生应用，在约 1080 和 540 逻辑像素核对平面新建入口、三项选择及底部输入栏。助手菜单标示当前项，改选 PPT 后输入提示和导入报告入口随之变化。首条消息生成标题并进入统一列表。缩窗后从单一导航入口打开抽屉，再新建恢复默认通用助手。布局回归覆盖 1120、760 和 520 逻辑像素。 | 深色宽窗的原生视觉对照。 |
| Preview 正式对话页 | `AgentChat`、`SessionWorkspace`、导航、IME、长对话、模型权限、旁聊及审阅专项 | Windows 原生窗口在宽窗和窄窗检查标题、消息与输入区。Goal 和比较入口在顶部工作区栏。重启后打开原 PPT 对话，核对待处理简报及整宽字段。运行中默认直接发送，可改为排队。实际停止 Run 后收起停止按钮和发送方式，回到普通发送。Windows 和 WSL 的 IME 用例在 1120 和 520 逻辑像素核对居中列及流式更新时的焦点、选区和中文组字。Windows 原生窗口确认导出的长路径可以换行，路径开头和文件名都可见。双击选词后出现引用和旁聊入口，带回的引用保留所选原文。旁聊回归另覆盖长路径后的自动换行和原中文选区。 | Windows 深色对话、原生窄窗旁聊及系统 IME 对照。 |
| 代码工作区标题与编辑器 | `AgentChatOptions::header_actions`、`CodeChat`、`VerifyCodeEditorBaseline` | Windows 原生宽窗中，工作区编辑入口与助手状态位于标题下方同一行。520 逻辑像素窗口将它移到下一行，待批准的测试卡片和输入栏可见。打开已应用变更审阅时，编辑入口从当前页及可访问树中移除。返回时焦点恢复到原产物卡片。窄窗从编辑入口打开有独立表面的弹窗，初始焦点落在路径上，空路径按钮禁用。输入 `config.txt` 后实际读到 `mode=enabled`，关闭后焦点回原入口，原测试审批仍待处理。Windows 和 WSL 最终重建后的编辑器及恢复专项均通过。 | Windows 深色原生编辑器对照。 |
| Preview 审阅布局与页签 | `AgentChat`、`CodeDiffReview`、`StagedPresentationReview`、`DesignDetails` | Windows 和 WSL 在同一个待审批变更内切换代码文件，核对两个目标仍保持原内容，返回恢复原附件焦点及草稿。PPT 用页签定位到第 3 页，另一个窗口独立定位到第 1 页。设计稿的选中节点和标注也随窗口保存。Windows 原生验收打开并分别关闭代码、PPT 和设计审阅窗口，实际写入获批文件并导出 PPTX。原生浅色代码审阅确认新增和删除行的整行底色及新旧行号。鼠标跨行选中补丁文本后显示引用和旁聊入口。新版按稿在同一标题行显示产物名称和版本，Windows 原生重开同一 PPTX，在主窗口和独立窗口核对名称与 v1，关闭独立窗口后原窗口仍可用。导出文件名与提交放在滚动预览外，PPT 导出失败与局部返工专项复跑通过。固定深色的 Windows 原生验收构建核对比较列表、候选画板、PPTX v2 和导出表单。主窗在第 2 页打开同色独立窗口，独立窗口切到第 1 页并关闭，主窗仍在第 2 页。实际从 v2 审阅页批准导出，批准前无目标文件，批准后导出与采纳的 PPTX 逐字节一致。复验时可辨认导出文件名和新建比较表单中的正文、焦点及光标。Windows 与 WSL 的独立审阅、审批和返工专项复跑通过。 | Windows 的系统主题实时切换，以及部分领域审阅在原生窄窗中的逐页对照。 |
| 跨会话审批附件 | `Conversation::PreviewArtifactAsync`、`VerifyApprovalAttachment`、`StartWorkspaceRelease` | Windows 和 WSL 的 SQLite 回归用报告夹具打开同项目原版本附件，拒绝未绑定的新版本及跨项目引用。读取进行中撤权后，会话和待办目录移除附件，在途预览失败，原会话仍可读取该产物，审批的 ID 和版本保持一致。Windows 和 WSL 完整 Preview 从发布链的代码差异切到设计画板，再回到指定文件，确认此前两个工作区文件都未改变。批准后继续验证和 PPTX 交付。Windows 将发布链跨会话事件的测试等待上限统一为 25 秒，串行复跑全程通过并保留三份 PPTX。Windows SDK 消费端 Debug 重建成功。审阅、跨领域审阅、分叉审阅和候选恢复回归通过。 | 其他桌面环境实机验收。 |
| Preview 会话归类与统一待办 | `huxerui_agentkit_navigation_tests`，`ConversationNavigationStore`，`Interactions().ObservePendingAsync`，`AgentChatInteractionRequest` | Windows 和 WSL 用本地 SQLite 创建、排序和折叠归类。启动时预选已登记的工作区，可显式选择无工作区并发送，核对会话仍由通用助手处理。从侧栏点击已登记目录，新草稿恢复该目录。重开后复用同一会话，键盘打开菜单改名，再通过行尾菜单移至根列表并移回。移除归类保留会话，明确删除后保留工作区文件及另一会话的请求。待办按原 ID 打开折叠归类中的设计审批和独立 PPT 表单，完成设计请求后从待办移除，返回时保留草稿。将窗口缩为 560 逻辑像素，验证同一导航抽屉和重开后恢复的 PPT 请求。用 SQLite 触发器拒绝归类保存，新会话仍可从根列表打开。修复写入条件后重试，沿原 ID 补写关联并消费后续修改。两个独立 UI 测试窗口共用 SQLite。重新激活后可见彼此对不同归类的修改，过期的重命名和移动会话提示冲突。放弃旧操作并重开后，核对最新名称和会话归属。长归类名的行尾菜单可点击。 | 原生窗口切换及 Web 跨标签页实测。 |
| Preview 原生目录选择器 | Windows SDK Debug 消费构建、`FilePicker::OpenDirectoryAsync`、`CodeWorkspaces::AddAsync` | 用隔离的应用数据运行实际 Preview。从系统对话框取消后保留独立草稿，会话和工作区数仍为零。选中目录只预选工作区，首条消息发给本地演示模型后才创建会话。重复选择沿用原工作区 ID。两个含空格的同名目录各有自己的 ID，SQLite 确认两个会话分别绑定正确的路径。重启窗口后可重新打开原会话和回复。选择带 AgentKit 待回收标记的目录时显示失败，原会话仍打开，工作区和会话数量不变。新版的同名目录标签和实际工作区路径另见桌面导航验收。 | 其他平台的系统目录选择器实测。 |
| Preview 独立审阅与版本化审批 | `huxerui_agentkit_review_approval_tests`，`AgentChatReviewRequest`，`huxerui_agentkit_preview_tests --native-review-approval <验收目录>` | Windows 和 WSL 从实际代码差异返回对话，核对草稿和原附件按钮焦点。两个独立审阅视图观察同一会话。错误版本仅显示产物。选中差异的两行后，引用的文字和精确版本进入共享的会话草稿。正确版本提交后两个视图均显示已批准，竞争命令被拒绝，双文件内容与计划一致。原生验收通过 Preview 入口创建两个审阅窗口，程序从第一个会话句柄提交审批，第二个句柄收到决定。关闭一个审阅窗口后另一窗口仍有效。 | 原生窗口中的系统键盘操作与跨显示器焦点。 |
| 长对话的分页、审阅回位和流式更新 | `huxerui_agentkit_long_conversation_tests`、`SessionHistoryWindow`、`huxerui_agentkit_sqlite_tests` | Windows 和 WSL 走通超过默认 50 条投影的分叉会话。首次显示最新轮次，加载早期消息时保留原页边界位置。打开继承的旧 PPT 后继续接收新轮次，返回时核对原附件位置、焦点与草稿。阅读旧消息时继续接收输出，位置保持。跳回最新后随增量跟到末尾。SQLite 回归验证扩大历史窗口后撤回授权会隐藏继承的产物卡片。 | 其他平台的实际运行。 |
| 多行输入、中文 IME 和流式焦点 | `huxerui_agentkit_composer_ime_tests`、HuxerUI `TextInputCommandBatch` | Windows 和 WSL 确认新建时 Enter 插入换行，首条消息组字提交前不可发送。已有会话在模型增量和 Run 结束时保留输入会话、含补充平面字符的 UTF-16 选区、组字范围、草稿及焦点。取消组字恢复原文和光标，确认中文后可正常发送。旁聊选区与带回结论的回归也通过。 | 原生系统中文输入法的候选窗和平台键盘事件。 |
| 助手 Markdown 回复 | `TextBlock::markdown`、`MarkdownMessageBlock`、`AgentChatLinkHandler`、`VerifyComposerIme`、`VerifyMarkdownSource`、`huxerui_agentkit_markdown_images_tests` | 按消息块持有 Lib-Markdown 增量解析会话。Windows 原生 520 逻辑像素窗口用本地助手显示标题、加粗文本、表格和代码块。Windows 与 WSL 验证流式更新保持组字、草稿和焦点，已完成的排版回复支持直接引用和旁聊。回归覆盖反向拖选、重复中文和 emoji、链接标签与实体，核对 SQLite 中的引用及旁聊切片的消息版本、块 ID 和原文 UTF-8 字节区间。扩展选区从前往后和从后往前均可引用和旁聊。两种方向提交的 SQLite 引用具有相同的消息版本、块 ID、原文 UTF-8 切片和阅读顺序。旁聊展示排版文本，模型收到精确原文切片，链接地址和排版语法未进入上下文。两端通过 IME、长对话、消息旁聊及产物旁聊四项。图片专项用真实回环 HTTP 验证 PNG 下载、404 和损坏像素数据。界面测试验证独立图片渲染及混排图片展示，流式回复不发起加载，失败保留替代文字。图片异步加载时可继续用键盘扩展选区，正反向引用的 SQLite 来源及旁聊模型切片仍对应原版本和 UTF-8 区间。链接测试用 Preview 的地址策略，确认流式回复、文件地址及跨链接拖选均无链接动作。已完成回复在用户激活时复核策略，拒绝时宿主不接收请求。宿主每次激活只收到一次精确地址及原消息版本和块 ID，打开失败在原消息显示，成功重试后清除。此前 Windows 和 Web 的 SDK 消费构建通过。 [Lib-Markdown](https://github.com/HuxerUI/Lib-Markdown) | 原生系统浏览器联调、macOS 和移动端导航、其他平台的图片加载及长文增量性能待验收。 |
| PPT 和设计稿的独立审阅位置 | `huxerui_agentkit_domain_review_tests`、`DetachedArtifactReview`、`OpenPreviewDesignReview` | Windows 和 WSL 的两个独立审阅视图都从指定 PPT 页或设计节点打开，各自改动页码和节点，随后观察同一审批决定。PPT 真实导出并核对批准的页内容，设计稿发布后另一视图保留所选节点及关闭状态。两端还用同一 Preview 入口打开真实 PPT 和设计窗口，程序通过会话接口批准，通过产物核对导出，分别关闭窗口。 | 真实窗口在不同显示器上的 DPI 切换和焦点，以及其他平台实机验收。 |
| 首版代码助手 | `RunCode`、`RegisterCodeTools`、`RegisterCodeTests`，`CodeRoundTrip`、`RunConfiguredPreview`、`huxerui_agentkit_code_failure_tests`、`huxerui_agentkit_code_editor_tests`、`huxerui_agentkit_code_editor_recovery_tests` | 测试模型和真实中转模型均提出两文件补丁，重启恢复原审批 ID。Windows 和 WSL 验证部分写入、CTest 非零退出、编辑器来源及冲突。在 Preview 实际键入未保存内容，核对发给模型的文本、实际写入差异和源文件定位。两个独立进程核对同一会话、Run、工具调用、计划及审批 ID 和版本。当前草稿获批后继续原 Run，未发起新模型轮次，实际写入两文件并运行 CTest。草稿在重启前更新后，批准旧计划仍无写入，不启动 CTest，用户可丢弃草稿并重读磁盘。`Full Access` 用例核对实际运行预设，含草稿的补丁仍在人工批准后才写入。Windows 五项相关回归与 WSL 全部三十一项 CTest 通过。 | 平台矩阵的其他环境。 |
| 版本化宿主上下文 | `RegisterContextProvider`、`AgentDefinition::context`、`RunContext::CompleteAsync`、`VerifyHostResourceContext` | Windows 和 WSL 的运行时回归用宿主夹具解析编辑器选区与磁盘文件，验证默认对话和宿主编排均按输入顺序发送指定版本。版本错配、资源缺失、超预算和撤权时没有模型请求，读取中撤权还会阻止后续提供器读取。WSL 全部二十九项 CTest 通过。Windows 使用本地兼容 SDK 包，通过运行时、代码故障、旁聊和 SQLite 四项回归。拆分后的 Preview 主线在 WSL 全程通过。Windows 组合回归达到 240 秒上限时，SQLite 中发布 Goal 已完成，五页 PPTX 归档有效。重开同一验收目录后，单独执行发布后影响导航通过。两条发布阻断专项在 Windows 和 WSL 均通过。 | Windows 组合回归的整体耗时。 |
| 普通代码补丁结果核对 | `ApplyCodePatch`、`ReconcileToolAsync`、`huxerui_agentkit_code_patch_recovery_tests` | Windows 和 WSL 分别在两文件写入后及部分写入后中断 SQLite 结果提交。重开会话按原调用核对逐文件凭据。缺少凭据保持待核对，恢复凭据后显示已写入和跳过的文件。用户后续编辑原样保留，原 Run 终态保持，未发布未提交的产物，也未启动 CTest。部分写入用例从核对结果仅选择已确认写入的文件，单独批准后实际回撤并核对原字节。Windows 三项代码故障与恢复专项及 WSL 全部三十三项 CTest 通过。 | 平台矩阵的其他环境。 |
| 首版 PPT 助手 | `RunPresentation`、`RegisterPresentationTools`，`PreparePresentation`、`RestartAndExport`、`RunConfiguredPreview`、`huxerui_agentkit_presentation_failure_tests` | 界面回归逐页查看编辑后的大纲。真实中转模型起草两页，原生验收通过 Preview 的表单接口编辑标题并核对预览载荷。重启恢复原导出审批 ID，导出文件和暂存版本字节相同。PowerPoint 打开实际文件，确认两页标题符合批准的大纲。Windows 和 WSL 验证暂存目录不可用时显示生成失败，目录中没有 PPTX，也没有审阅或导出入口。审批期间目标被占用时，原文件保持完整，重开会话仍能审阅原暂存版本和批准过的第一页。 | 平台矩阵的其他环境。 |
| 会话、运行及持久化 | `AgentRuntime::Sessions()`、`RunContext`、SQLite `SessionStore`，`huxerui_agentkit_runtime_tests`、`huxerui_agentkit_sqlite_tests` | 运行时和存储回归通过，含可恢复表单、待批准工具、提交回执、外部效果核对、协作等待、项目状态与 Goal 修订。 | Preview 的跨窗、输入、订阅及配置时序按两份正式文档继续验收。 |
| Goal 暂停与跨会话排队 | `Conversation::Goals()`、`SendMessageAsync`、SQLite 的 `VerifyPausedGoalDispatch` | 接收会话忙时先排入带 Goal 版本的请求。暂停该 Goal 后，发起侧原 Run 收尾，接收侧先执行后到的独立协作，再启动自己的 Goal。原请求仍处于排队且无目标 Run。恢复原 Goal 后，同一请求只启动一个 Run，并沿用原 Goal 版本。WSL 用例在修复前复现阻塞，修复后连续三次通过。Windows 重建后通过。 | Linux 原生面板已验证，其他目标平台待复验。 |
| 复制到 worktree | `Sessions().ForkAsync`、`WorkspaceResolver`、`WorktreeForks`，`huxerui_agentkit_worktree_tests` 和 SQLite 的工作区分叉用例 | 在真实 Git 仓库中预览固定 commit，确认前无 checkout。预览后移动源 HEAD，覆盖目标目录和分支冲突。注入 Git 已创建而会话提交未确认的故障，重启用原命令接回同一个 checkout，保留同一项目和独立工作区。源文件、暂存和未跟踪文件留在源侧，源审批继续待办。副本新写入经自己的审批，仅修改目标。SQLite 另验证已提交回执丢失、审批恢复、重复计划冲突及目标撤权。 | 其他桌面平台实机验收。 |
| 回收未完成的 worktree 副本 | `Sessions().AbandonWorkspaceForkAsync`、`WorktreeForks::DiscardAsync`、`VerifyWorktreeCopy`、SQLite 工作区回归 | 真实 Git 验收覆盖原提交仍不确定、存在忽略文件和隔离路径被占用，三个场景均保留 checkout 和现有文件。明确未提交后回收专属 checkout 并移除对应登记。模拟物理移除后尚未结清登记即退出，重启可续清。直接查 SQLite 和 Git 确认原命令已封存，源会话、审批及 Git 分支保留。SQLite 回归覆盖已被会话使用的工作区拒绝回收、重启后原命令与计划无法再分叉、旧 Runtime 无法提交会话或把重复提交的封存请求报告为成功。 | 其他桌面平台实机验收。 |
| 分叉历史和产物授权 | `Conversation::PreviewArtifactAsync`、`Conversation::ReadSelectionAsync`、`ReadHistoryAsync`、`AssembleInheritedHistory`，`VerifyArtifactFork`、`ReviewCopiedPresentation` | 源报告升为 v2 后，副本在 SQLite 重开仍从原消息解析 v1 的预览和文本切片。拒绝在目标使用的引用不会继承。模型分别验证自动继承的版本信息、显式引用的 v1 内容及收紧模型权限后的输出范围。并发撤权会拦截已开始读取的预览和选区，订阅和分页隐藏该引用，后续 Run 继续使用可见历史。Preview 从复制对话打开原 PPT 产物预览。 | 正式协议矩阵中的提供商续轮。 |
| 分叉候选的创建、暂存和恢复 | `ForkReviews().CreateComparisonAsync`、`ListComparisonsAsync`、`ObserveComparisonAsync`、`CloseComparisonAsync`，SQLite 比较故障注入 | Windows 和 WSL 验证同一事务登记比较和两个分支的首个 Run。提交后丢失回执时，重启恢复原 Run ID 和待填写表单。订阅报告分支成功及失败。共享工作区的写入工具未执行，另一分支在自身预算内暂存产物。会话撤权后读取和命令回执按权限投影。目录重开可找到已关闭的原比较，源会话撤权后目录不可读。 | 真实 Git 验收见候选 checkout 恢复用例。 |
| 比较创建审批 | `StageComparisonApprovals`、`DecideComparisonApprovals`、`RecoverComparisonApproval`、`huxerui_agentkit_fork_review_tests` | Windows 和 WSL 验证批准前没有分支、Run 或工作区分配。重复提交返回同一审批。在重启后拒绝的命令结清为拒绝。权限变化引入新的确认要求时，旧审批结清为冲突。批准后注入比较提交回执丢失，Runtime 在确认前不启动分支，重启后通过原命令取回比较和两个分支，分配器未重复调用。源会话按提交顺序列出直接与审批创建的比较，回执会过滤撤权分支。Preview 实际从比较表单返回源会话审批，再从列表打开已批准的设计或演示比较。真实 Git 分配验收见候选 checkout 恢复用例。 | 平台矩阵的其他环境。 |
| 候选采纳与设计版本 | `ForkReviews().PrepareAdoptionAsync`、`StoreCandidateAdoption`、`RecoverCandidateAdoption` | Windows 和 WSL 验证拒绝旧比较修订，等待审批时不能用新候选替换已准备计划。真实目标文件在审批等待时发生修改，批准旧计划返回冲突并保留用户编辑。按新基线重备后，SQLite 重开可取回原命令、调用与审批，批准只写一次，另一轮重开后仍能查到精确的候选来源。发布登记的中断见暂存产物采纳恢复用例。 | 平台矩阵的其他环境。 |
| Preview 比较入口、并排审阅和采纳 | `ComparisonWorkspace`、`huxerui_agentkit_fork_review_tests` 和 `huxerui_agentkit_long_conversation_tests` | 从会话工具栏以同一个已发布设计为输入，实际创建两个 PPT 方案和两个设计方案。用户从比较页进入分支确认内容，再返回同一比较。桌面宽度检查两栏位置和演示同步翻页，验证两份真实 PPTX 的首页内容且导出目录为空。从并排页打开主会话的采纳审批，审阅选定画板并批准，检查实际升版与来源。从窄窗口重开原比较，核对两个候选纵向排列和已应用状态。Windows 和 WSL 界面回归补验进入比较时保留已加载历史，新消息写入后返回仍停在原附件位置。实际访问候选后返回源会话，草稿保留，焦点回到工具栏入口，重新打开仍为同一比较。 | 移动端触控采纳实测。 |
| Preview 采纳 PowerPoint 候选并导出 | `huxerui_agentkit_fork_review_tests`、`huxerui_agentkit_ppt_adoption_baseline_tests`、`PresentationChanges` | Windows 和 WSL 从已有演示启动比较，确认两个候选并打开选中演示的审批附件。批准前只有原版本，未写入采纳文件。批准后同一产物新增一版，逐页引用候选，字节与候选一致，原始和未选版本仍可读。新版本通过独立审批实际导出，核对归档内容、新版字节和未变化的原始导出。关闭并重开应用后仍可预览新版。Windows 在约 760 逻辑像素的原生窗口从已保存的设计比较切回目标演示，实际点击 v2 卡片后确认版本、首张幻灯片及独立窗口入口。Windows 和 WSL 的重启专项滚动到同一 v2 卡片，通过无障碍焦点和 Enter 打开已采纳版本，复跑均通过。两端专项验证局部返工先发布 v2 时，排队的 v1 采纳结清为冲突，未创建 Run、工具或文件。SQLite 重开后原命令仍返回原回执。按 v2 重备并另行批准后发布 v3，v3 与选中的候选文件字节一致，v2 内容完整保留。 | 其他平台实测。 |
| Preview 代码候选的隔离、比较及合入 | `InspectCodeAdoption`、`PrepareAdoptionAsync`、`huxerui_agentkit_code_candidate_tests` | 两个 checkout 均从已审阅的固定提交创建，工作区 ID 独立，未跟踪的用户文件留在主工作区。分别应用不同候选补丁并运行 CTest，并排查看逐文件差异。Windows 和 WSL 在主工作区审批期间修改第二个文件，旧批准失效且两个文件保持当时内容。重叠编辑会在新审批前提示文件。两份修改都已存在时提示无需重复采纳。只有一份已存在时，新审批仅包含另一份。换成可合并的用户编辑后，新差异以用户编辑后的内容为上下文。批准前两个文件保持原内容，批准后保留用户标题并应用两个候选修改，逐文件记录候选来源。重启后从比较入口看到已应用状态。 | 其他目标桌面平台实机验收。 |
| 采纳效果核对与回撤 | `AgentRuntime::ReconcileToolAsync`、`huxerui_agentkit_code_adoption_recovery_tests`、`huxerui_agentkit_partial_adoption_tests`、`RecoverInterruptedEffect`、`VerifyReconciledEffect` | Windows 和 WSL 在代码采纳实际替换两个文件后中断 SQLite 结果提交。重开比较页，缺少一项替换凭据时继续待核对。补回凭据后用原调用结清，再从已恢复效果中选择一个文件，独立审批并实际回撤。另在首个文件替换后注入对第二个文件的编辑，重启后标明部分应用，回撤清单仅包含原调用实际替换的文件。两个用例均保留用户编辑。SQLite 验证撤权、范围校验、重开后原命令回执、同命令的结果冲突及原 Run 终态。 | 平台矩阵的其他环境。 |
| 暂存产物的采纳恢复 | `huxerui_agentkit_design_adoption_recovery_tests`、`huxerui_agentkit_ppt_adoption_recovery_tests` | Windows 和 WSL 在候选 JSON 或 PPTX 实际暂存后中断对应的 SQLite 登记。重开后原版本仍可读，准备登记的版本返回 `NotFound`。比较页以原调用确认未应用。重新采纳经新审批后发布同一产物的下一个版本，保留原版、选中候选来源及新调用。核对新设计内容和 PPTX 字节。 | 平台矩阵的其他环境。 |
| 候选 checkout 的中断恢复与回收 | `ForkReviews().AbandonComparisonWorkspaceAsync`、`huxerui_agentkit_candidate_recovery_tests`、`huxerui_agentkit_approved_candidate_recovery_tests` | Windows 和 WSL 在真实 Git 仓库中注入 checkout 已分配、比较尚未提交的故障。直接提交和人工批准两条路径均在重开后用原命令复用两个 checkout，分支在提交前保持未启动。人工路径另核对批准前没有实际 checkout，恢复后同一命令与审批关联原两条分支。另一比较因路径被占用而失败，可选择回收首个 checkout。忽略文件阻止回收，移除该测试文件后重开应用可清除干净的 checkout，并保留 Git 分支、被占用目录、源工作区及另一比较。 | 平台矩阵的其他环境。 |
| 通用助手直接调用及转交 | `RunGeneral`、`PlanBriefing`、`DeliverBriefing`、`huxerui_agentkit_navigation_tests` | 同一通用会话直接生成报告大纲，再调度独立设计会话。重启后继续等待设计审阅，用确认设计的实际数据驱动已有 PPT 会话。Windows 和 WSL 在 Preview 会话菜单中将空闲的设计会话转交通用助手。转交后同一会话与工作区保留原设计 Run、产物历史和输入草稿，发送草稿后新 Run 绑定通用助手。 | 其他桌面环境。 |
| 完整版发布链 | `RunWorkspaceRelease`、`RunVerification`、`StartWorkspaceRelease`、`FinishWorkspaceRelease`、`RunConfiguredRelease`，`huxerui_agentkit_release_test_failure_tests`、`huxerui_agentkit_release_missing_screenshot_tests` | 测试模型和真实中转模型均从通用会话生成设计、实际修改配置和设置页，并在重启后恢复原验证审批。独立会话运行 CTest，由 Chrome 渲染修改后的设置页并截图。通用会话将带版本的证据交给已有 PPT 会话，SQLite 核对报告和截图来自同一次调用。PowerPoint 打开并渲染验证页，内嵌图片与截图字节一致。Windows 和 WSL 的两个独立专项从各自的项目工作区启动新发布流程。CTest 失败时目标保持阻断。缺少截图时 CTest 通过，验证会话显示缺少实现截图的原因，目标仍阻断。两个场景的已有 PPT 会话均无发布待办，也没有完成审批。 | 按正式设计的平台能力矩阵补实机验收。 |
| 发布 Goal 中途修改 | `ReviseWorkspaceRelease`、`FinishWorkspaceRelease`、`RunConfiguredRelease`、`huxerui_agentkit_goal_completion_revision_tests`、`GoalEditImpact` | 十页导出待审批时改为五页，经界面和真实模型验收。重开后旧导出审批为 `Invalidated`，实际导出五页文件，设计、实现和 CTest 各成功一次。Windows 和 WSL 用确定性模型走到十页已导出且等待最终审批，从 Preview 打开原审批，再进 Goal 面板把页数改为五页。提交前显示原待办将关闭、证据需复核。提交后界面移除旧批准入口，SQLite 旧审批为 `Invalidated`，证据命令为 `Withdrawn`，重开仍拒绝旧批准。新版本保留同一 Goal ID 和四份已验证的输入证据，恢复原五页简报请求。在统一待办打开新版审批并批准，Goal 面板显示完成。两份 PPTX 均包含原验证截图。十页原件字节和 CTest 结果的写入时间均与改版前一致。 | 原生窗口和其他平台的界面实测。 |
| 设计节点、文件版本和影响查询 | `RegisterCodeTools`、`ArtifactChanges::InspectImpactAsync`、`ReviewReleasedDesignImpact`、`--release-impact` | 批准后重开发布设计，保存节点只定位到实际改动的设置页文件，启用节点定位到设置页及配置文件。检查 SQLite 中设计节点到实现文件的边、文件版本指纹，以及截图和 PPT 页的关联。Windows 用已完成的五页发布记录重启单独复验，WSL 在完整 Preview 中验收。修改工作区现场文件后点击影响链接，核对原批准的差异和现场文件原样保留。近似影响打开原验证截图，带页码的链接打开演示的第 5 页。关闭详情后恢复到原列表，代码链接验证了焦点恢复。WSL 另通过设计回撤、PPT 局部返工和代码候选三个专项。 | 不同类型来源的故障用例和原生窗口焦点。 |
| 来源变更记录 | `RecordSourceChangeAsync`、`ArtifactImpactQuery::change_command`、`VerifyRework` | Windows 和 WSL 的 SQLite 用例登记报告修订，以原命令查询项目提交。相同请求重放取得原回执，同一个 ID 绑定不同变更时报冲突。重开后按命令得到原变更并定位引用旧报告的两个页面。撤销替换报告的读取权限后原查询按权限失败。新项目记录使用 schema 2，读取入口保留 schema 1。普通代码回撤的部分写入在结果核对后只登记已确认文件的来源变更，并能定位原实现证据。 | 平台矩阵的其他环境。 |
| 产物升版与旧计划失效 | `ArtifactRecord::source_change`、`VerifyRework`、`VerifySourceApproval`、SQLite `SessionStore` | Windows 和 WSL 验证同项目另一会话使用报告结果片段。只变更概览时，原人工审批和自动审查继续等待。报告升版时，待批交互失效，运行中的审查收到停止信号，尚未启动的审查不会发出。人工已批准、调用尚未发出时，注入冲突结清写入失败。重开后保留原 Run、调用、计划和批准回执，按已提交的新来源结清为冲突，无二次批准或工具执行。产物与来源变更共同提交后丢失回执时，按原提交 ID 核对并定位引用旧报告的两个页面。局部返工只登记批准的结果页。 | 平台矩阵的其他环境。 |
| 项目共享资料发布和撤回 | `Projects().UpdateContextAsync`、`ArtifactChanges::InspectImpactAsync`、SQLite 的 `SetUpProject`、`ReopenProject`、`RecoverProjectWithdrawal`、Preview 的 `VerifySharedReports` | Windows 和 WSL 验证首次发布和送审前后的状态。运行时专项通过原命令查询对应变更和受影响报告，覆盖重开、重放、撤权及已领取任务拒绝启动。Preview 专项从通用会话的菜单保存新草稿、送审、发布及撤回，并检查另一 PPT 会话的返工审批。草稿和送审保留待批计划，发布和撤回分别使对应计划失效，两次均未生成新版 PPTX，旧审批无法提交。撤回 v2 后从仍可用的 v1 继续编辑，分配 v3。 | Web 和移动端的共享报告操作实测。 |
| 代码文件回撤 | `PrepareRevertAsync`、`WithCodeChanges`、`VerifyCodeRevert`、`huxerui_agentkit_code_revert_recovery_tests` | 从已应用变更进入 Preview 审阅，验证审批期间目标变化、按文件选择、审批重开和原命令回放。Windows 和 WSL 在第一个文件实际回撤后编辑第二个文件，验证只登记实际回撤的文件。另在同一场景注入 SQLite 结果提交中断，从原变更记录核对原反向调用。恢复前以 API 请求回撤重叠文件会返回 `ResultUnknown`。缺少写入凭据时仍待核对，恢复凭据后以 `ReconcileToolAsync` 结清为部分完成。核对保留用户后续修改及原 Run 终态，并由唯一一条来源变更定位原实现证据。 | 平台矩阵的其他环境。 |
| 设计稿版本回撤 | `WithDesignChanges`、`ToolDefinition::recover_published_effect`、`VerifyDesignRevert`、SQLite 的 `VerifyRework` | 排队中的 v2 回撤在 v3 发布后报冲突。只读核对六份早期验收库的 schema 1 设计发布记录，均保留原调用、成功发布结果和前一版本依赖，且效果为空。Windows 和 WSL 的 SQLite 夹具用这一结构重开旧会话。删去前一版本依赖时仍可审阅 v3，且回撤入口不可用。保留依赖后按原调用恢复凭据，审批可打开 v3 和 v2，重启保留同一审批 ID、版本和计划。批准后发布同一设计的 v4，字节与 v2 一致，v2 和 v3 原件仍可读取。新产物引用 v2，来源变更记录 v3 到 v4 的 `Reverted`。SQLite 故障回归核对条件发布冲突时不会登记产物或效果。 | 其他目标平台实测。 |
| 验证报告定位受影响的演示和幻灯片 | `VerificationReportSummary`、`OpenPresentationReviewAsync`、`AgentChatOptions::review_request`、`huxerui_agentkit_ppt_adoption_baseline_tests` | 可精确定位时，报告列出两张受影响的幻灯片。撤销读取权限后点击旧链接会清除结果并留在报告。重新授权后可直达目标会话第 2 页，换源、审批、重开并导出实际文件。旧版链接返回 `Conflict`，无效页返回 `NotFound`。Windows 和 WSL 的整份演示专项使用实际采纳生成的 v3。报告标明范围待复核，只提供 v3 的整份审阅入口。撤权后的点击留在报告页，恢复授权后打开目标会话的 v3，可浏览两页。 | Web 和移动端实测。 |
| 局部返工命令和恢复 | `ArtifactChanges::PrepareReworkAsync`、`VerifyRework` | SQLite 用例在活动 Run 后排队两条返工命令。重发返回原受理回执。重启保留审批和调用 ID，批准后仅执行一次。获得新产物版本后，后续排队的旧基线返回冲突。未选片段保持原 ID 和来源，选中片段绑定新报告版本。缺少选定范围时先返回 `ScopeExpansionRequired`。 | 其他业务产物适配器。 |
| PPTX 局部返工和版本导出 | `PresentationChanges`、`ReworkPptxText`、`DispatchArtifactActionAsync`、`huxerui_agentkit_presentation_rework_tests` | Windows 和 WSL 从审阅页进入第 2 页，使用同项目另一会话的报告换源。不同项目的报告未显示。返工批准后打开 v2 并从审阅页提交文件名。等待导出审批时关闭并重开 SQLite 运行时，恢复同一 Run、工具调用、计划、审批和版本，批准前无导出文件。此用例禁止把 PPTX 发送给模型，导出完成时模型轮次为零。导出字节与批准版本一致，旧版保留来源和内容。Windows SDK 消费构建通过，PowerPoint 实际打开文件并核对两页。 | 更多版式与不同类型的来源换页。 |
| Chat Completions | `MakeChatCompletionsModel`，`huxerui_agentkit_model_tests` | 协议夹具覆盖分片、工具回传、续轮、`json_schema` 与 `json_object` 结构化输出、中断及取消。通过用户提供的 OpenAI 兼容端点，使用 `gpt-4.1-mini-2025-04-14` 和 Windows 原生 HTTP 实测流式工具调用与回传。该端点也为 Preview 代码、设计和 PPT 提供实际结构化输出。共享 HTTP 接入重构后，Windows 本地协议夹具再次验证 Runtime 调用并回传同一工具结果。 | 按正式协议矩阵验证各兼容端点的模型能力。 |
| OpenAI Responses | `MakeResponsesModel`，`huxerui_agentkit_model_tests`，Preview 的 `--configured-preview` | Windows 和 WSL 的协议夹具覆盖 UTF-8 分片、同轮工具顺序、`item_id` 与 `call_id` 的关联、加密推理续轮、授权前缀失效、结构化结果、拒绝、提前 EOF 和本地取消。通过用户提供的中转端点，Windows 原生 HTTP 用 `gpt-4.1-mini-2025-04-14` 和 `gpt-5-mini-2025-08-07` 实测工具流式调用及结果回传。Windows 两进程实测在工具待审批时保存加密推理，重开 SQLite 后恢复原 Run、调用、计划及审批，获批后发送一次续轮请求，加密项与本地记录一致，`store` 为 `false`。Preview 用真实 `gpt-4.1-mini-2025-04-14` 通过三进程首版及三进程完整版发布链，恢复原补丁、验证和导出审批，实际修改两文件、运行 CTest、将 Goal 从十页改为五页并使旧导出失效。PowerPoint 实际打开两页首版和带验证截图的五页发布稿。 | 其他目标平台实测。 |
| DeepSeek 与 GLM Responses | `MakeResponsesModel` 的 `ResponsesDialect`，`CompatibleResponsesProtocol` 和两个配置验收入口 | 协议夹具验证各自的请求格式、工具参数分片和调用 ID、结构化输出、私有推理回放及跨 Run 和前缀边界。GLM 覆盖显式服务端续轮、引用过期后回退和撤回服务器状态授权。使用用户提供的中转端点，Windows 原生 HTTP 以对应方言连接 `deepseek-v4-flash-0731` 和 `glm-5`，均完成实际工具流式调用、回传及结果核对。两家 Preview 回环各完成代码、PPT 和发布链，PowerPoint 实际打开并核对两页首版与五页发布演示。WSL 用同一中转的原生 HTTP 验收两种 Responses 方言的结构化输出。DeepSeek 请求 JSON Schema，GLM 请求 JSON 对象并在提示中提供 schema。首次运行中，两者均由 `RunContext::CompleteAsync` 验证包含布尔值和整数的嵌套数据，结构化正文留给宿主。同一端点复跑时，DeepSeek 返回数据未通过 schema 校验，GLM 返回内容未通过 JSON 解析，两个 Run 均报告失败。 | 结构化输出的稳定性、私有状态跨进程回放和 GLM 的服务端续轮。 |
| Claude Messages 与兼容端点 | `MakeMessagesModel`，`huxerui_agentkit_model_tests`，Preview 的 `--configured-preview` | 协议夹具覆盖文本和 JSON 参数分片、同轮工具顺序、按 ID 回传工具结果及错误、原样续轮的思考块和签名、授权前缀、结构化结果、截断、EOF 与本地取消。Windows 原生 HTTP 分别使用 Claude、DeepSeek、GLM 回环夹具驱动完整工具轮次和 Preview 的代码、PPT 流程。Claude 配置另走通跨领域发布链，验收十页改五页、旧导出失效、审批恢复和截图进入实际 PPTX。 | 真实 Claude、DeepSeek 和 GLM Messages 端点及各模型思考能力。 |
| 会话内联附件 | `SendInput::attachments`、`SendInput::staged_attachments`、`ArtifactStore::resolve_staged_attachment`、`VerifyModelAttachments` | 运行时先验证模型权限，再解析草稿附件。首条消息和 v1 同一次提交，重放原命令只登记一次。引用已登记的 v1 两次时只读一次原字节，历史保留两处引用。活动 Run 的补充可选 v2。模型发送权限撤回后，排队拒绝启动、在途读取不会发给模型，重试会重新校验已应用的附件。Windows 实际 Preview 从系统选择器选取 PNG。选完后修改源文件，首条纯附件消息仍送出原字节，和私有目录 v1 副本逐字节匹配。Gemini GenerateContent 回环只收到一次 POST 并返回回复。重启后从列表打开原会话，核对占位标题、文件名、v1 和原回复。最终版本在 Windows 和 WSL 的 Runtime、SQLite 和 IME 三项回归均通过。 | 其他平台的实际文件选择器、真实 Gemini 端点与活动输入的原生界面验收。 |
| Gemini GenerateContent | `MakeGeminiGenerateContentModel`，`huxerui_agentkit_model_tests`，Preview 的 `--configured-preview` | 协议夹具验证调用顺序、带 ID 自动关联、省略 ID 的旧调用、原 `Part` 的思考签名、空文本签名、尾部用量、结构化输出、截断和本地取消。内联文件夹具验证 PNG 和 PDF 的原始字节、版本和媒体约束、重复引用的预算。Windows 原生 HTTP 两种工具格式均完成同一调用回传。Gemini 配置走通 Preview 代码、PPT 以及跨领域发布链，核对实际代码与 CTest、审批重启恢复、被替代的导出和五页 PPTX。 | 真实 Gemini 端点的选型和能力验收。 |
| Gemini Interactions | `MakeGeminiInteractionsModel`，`huxerui_agentkit_model_tests`，Preview 的 `--configured-preview` | 协议夹具验证文本及参数分片、空文本块和步骤顺序、工具调用 ID、思考签名、结构化输出、截断和本地取消。覆盖服务端续轮显式启用、跨 Run 及前缀边界、撤回服务器状态授权后重建输入，以及过期引用回退。Windows 原生 HTTP 回环在两种续轮模式下均执行一次宿主工具。Preview 回环走通代码、PPT 和发布链，核对审批重启恢复、实际补丁和 CTest、十页计划失效。PowerPoint 打开两页首版演示和五页发布演示，核对批准的文字。 | 真实 Interactions 端点、模型实际能力及协议私有状态的重启续轮。 |
| 私有续轮状态的本地恢复 | `SessionStore`、`ModelRegistration::allow_persist_resume_state`，SQLite 的 `StoredContinuation`、`VolatileContinuation`，模型测试的 `--configured-responses-recovery` | 在待审批阶段重启。显式启用落盘时保留原审批和调用，重启后只执行一次，匹配模型修订的适配器接回原始私有字节及其授权前缀。默认配置重启后将依赖该状态的 Run 结清为中断，原审批无法执行。真实 OpenAI Responses 的加密项已通过 Windows 原生跨进程续轮。 | 其他支持私有续轮的真实端点。 |
| 消息选区旁聊与结论 | `Sessions().CreateAsync`、`ResolveSelection`、`SendAsync`、`SteerAsync`、`QueueAsync`、运行时及 SQLite 测试、`huxerui_agentkit_side_chat_tests` | Preview 的界面回归从中文片段打开临时旁聊，脚本模型只收到所选片段。打开弹窗时焦点落在满宽的提问框，收到回复后跟进输入与带回入口都处于弹窗内。可先带回完整回复并从草稿移除，再重新打开同一旁聊，选取回复片段带回并显式发送。Windows 原生浅色窗口从 PPT 第一页标题打开 440 逻辑像素的弹窗，核对来源版本、页位置、提问焦点及底部操作。带回后仍在原页，返回对话时焦点回到原产物卡片，结论保留在草稿。Windows 和 WSL 的界面用例从后往前选中两条消息，核对表情和中文边界、弹窗版本、模型收到的阅读顺序。带回结论后恢复端点位置及焦点，引用同一范围后可一并发送两项草稿。保留的 SQLite 证据中，送出的引用和结论来源均依次记录同一会话的两条消息、v1 与 v2 及原 UTF-8 区间。Windows 四项、WSL 六项相关回归通过。运行时的 `ReadSelectionsAsync` 用例覆盖第二个切片失效及单独撤权，整批读取失败。撤权后保留仍有权查看的来源位置，隐藏由整组选区生成的结论，原队列项被拒绝。SQLite 重启后原临时会话消失，队列内的结论文本和原版本来源仍可交付给主会话 Run。 | Windows 深色原生窄窗。 |
| 产物选区旁聊与审阅位置 | `Conversation::ReadSelectionAsync`、`AgentChatSourceActions`、`CodeRoundTrip`、`ReviewCopiedPresentation`、`ApproveAndReviseDesign`、`huxerui_agentkit_artifact_side_chat_tests` | 重启后从待审批补丁选中两行差异，带回结论后工作区仍未改动。复制会话从原 PPTX 第 3 页选中带 XML 转义的文字。设计 v2 发布后仍可从 v1 选择已确认节点。脚本模型三次均只收到指定片段，结论回到原对话草稿。关闭旁聊保留原审阅页，再次打开 PPT 或设计版本恢复页码或节点。另在待导出审批的 PPT 会话选中 v1 第 3 页文字，带回后显式排队。Windows 两个独立进程前后核对原队列、导出审批及 Run、产物版本、页 ID 和 UTF-8 区间。临时旁聊没有落盘，批准前无导出文件，批准原导出后队列只提交一次相同结论。导出的 PPTX 归档完整且保留转义后的第 3 页文字。WSL 的同流程运行时重开回归通过。Linux 原生窄窗的真实模型验收见下文。 | Windows 深色原生窗口和其他目标平台实测。 |
| PPT 图片选区和空闲发送 | `PresentationArtifactStore`、`LoadPresentationScreenshot`、`Conversation::SendAsync`、`huxerui_agentkit_goal_completion_revision_tests` | Windows 与 WSL 在新版五页 PPTX 的第 5 页显示实际内嵌实现截图，并以 `slide-5/picture-6` 绑定所选产物版本。打开旁聊时模型按文本选区契约收到图片名称和说明。带回结论后审阅仍停在第 5 页。用户回到空闲会话显式发送，新 Run 的消息只保存一次该结论和图片来源，没有对应的队列项。Windows 重开 SQLite 查证消息引用的是已完成 Goal 的最终产物。早期已保存的 PPTX 通过 Windows 影响导航复验，继续支持整页审阅与选区。独立图片选区适用于登记了元素 ID 的版本。 | 图片选区的其他目标平台实测。 |
| 自定义组件发布与渲染 | `RegisterContentComponent`、`ToolExecutionResult::blocks`、`Conversation::RenderContent`、`VerifyContentApplication` 和 `VerifyArtifactFork` | Windows 独立 CMake 宿主链接已构建的 AgentKit 静态库及公开 SDK。脚本模型发出工具调用，在默认对话页显示经 schema 校验的报告组件。界面本地切换图例，会话 Run 数不变。版本无对应渲染器时显示回退文字，非法数据和未声明类型被拒绝。SQLite 重启、快照与分页保留原产物版本，分叉清除源交互绑定，产物撤权后隐藏关联组件。 | 其他目标平台。 |
| 自定义组件与产物动作 | `DispatchContentActionAsync`、`DispatchArtifactActionAsync`、`RegisterArtifactAction`、`VerifyContentApplication`、`StageCardExport` 和 `FinishCardExport` | 默认对话页的已绑定组件动作启动一次新 Run。SQLite 用例拒绝过时的消息版本和非法数据，原命令重发取回同一回执，批准后撤权时原表单保持待处理。已批准补充在重启后由原 Run 读取和应用一次，待批准的导出动作复用原准备结果。表单消费后另行审批实际文件写入，核对 CSV 内容和一次执行记录。分叉会话内验证从组件发起以及直接发起产物动作，两者均使用原产物版本。 | 其他目标平台。 |
| Preview 交互饼图 | `RunReportChart`、`RegisterReportChart`、`huxerui_agentkit_report_chart_tests` | 通用对话用脚本模型生成结构化报表，界面显示饼图。取消一个图例后，另一分类的比例从 60% 更新到 75%。越界文件名显示校验反馈，随后可更正并提交。有效表单只提交选中项，分别核对审批预览、重开应用后恢复的审批和批准后的 CSV 字节。 | 真实提供商和其他目标平台。 |
| 表单长度、项数和数值校验 | `Conversation::SubmitAsync`、`VerifyFormConstraints` | Windows 和 WSL 运行时回归核对六种边界约束及对应字段路径。两字中文标签按码点通过，含超过双精度整数精度的边界比较。校验失败保留原表单版本，更正后可提交。 | 其他目标平台。 |
| 宿主工具自动审查 | `RuntimeOptions::tool_reviewer`、`VerifyToolReview`、`SaveToolReview`、`ReopenToolReview` | Windows 和 WSL 运行时覆盖批准、拒绝、转人工、审查器缺失、超时、失败、强制人审、撤权、取消、用户补充和资源基线变化。仅符合当前策略和原计划的调用执行写入。SQLite 故障注入在写入标记前中断。重启沿用同一调用与计划，复用有效批准，或在未决与审查器换版时要求人工确认。每个成功场景仅执行一次。 | 其他目标平台实测。 |
| Preview 独立模型审查 | `ChartExportReviewer`、`RunConfiguredChartReview` | Windows 原生 Preview 通过 HTTP 回环连接不同配置的对话模型和审查模型。Responses 和 Chat Completions 的批准均导出与所选数据一致的 CSV，拒绝无文件。主动转人工、额外输出字段、截断、HTTP 失败、超时和无效配置均保留原调用和计划，确认前无文件写入。Ask 模式无审查请求。逐项核对审查请求仅包含允许的计划摘要及结构化约束。另用已授权的 Chat Completions 端点和 `gpt-4.1-mini-2025-04-14` 实际批准同类计划，核对审查记录及导出的 CSV。 | 其他目标平台。 |
| Skill 来源、激活与恢复 | `RegisterSkillSource`、`RunContext::ActivateSkillAsync`、`VerifySkillApproval` 和模型协议回归 | Windows SQLite 重开覆盖首条消息用户选用和模型主动激活。只发现可见元数据，批准前未读取正文。原 Run 恢复同一审批，目录升级后仍使用原版正文和模板。拒绝未提供的引用和越界路径，缺失原版时以冲突结束。撤销资源的模型发送授权后，下一轮不再收到已读取的内容。审批结清回执可按提交 ID 在 SQLite 查证。模型协议夹具核对目录和正文区块的编码。Preview 中选用发布演示 Skill、读取模板和审批实际 PPTX 的结果见下一行。 | 同下一行的目标平台复验。 |
| Preview 发布演示 Skill | `VerifyReleaseSkill`、`RunConfiguredReleaseSkill`、`RunSkillPresentation` | Windows 与 WSL 的 Preview 界面覆盖输入栏选用和模型主动激活，核对同一 Run、版本、激活和导出审批。正常关闭再重开后才批准，批准前未写入最终文件。两条路径均读取验证报告，按包内模板起草，实际导出保留用户编辑及报告内容。Windows 原生 Preview 通过真实 Chat Completions 模型完成激活与跨进程恢复，PowerPoint 打开实际文件，核对两页标题、七项测试结论及待完成的无障碍检查。 | 其他目标平台的原生实测。 |
| Preview 模型和权限时序 | `AgentChatRunControls`、`RunSettingsControls`、`huxerui_agentkit_run_settings_tests` | 输入栏以当前选项值显示模型和权限，保留各自的无障碍名称。仅对与当前 Run 不同的选项显示「下次」。Windows 和 WSL 的设置回归通过，重开读取保存的模型和权限值。用三个脚本模型从界面启动 A/Ask，再改选 B/Auto Review。直接补充由 A 收到，B 排队后改选 C，启动时仍使用 B。A 和 B 的补丁都要人工批准。另排入的 C/Full Access 授权到期后保留输入及原因，且没有创建 Run 或调用模型。重新确认路径和权限后从草稿发送，C 只写入范围内的文件。Auto Review 对新的饼图 CSV 留下批准记录并实际导出。往返切换会话保留草稿。额外命名模型注册与默认 Responses 配置共存时，Windows 原生 HTTP 回环仍通过代码与 PPT 流程。 | 更多提供商及平台实测。 |
| 重启后的配置偏好和队列草稿 | `ComposerPreferences`、`ReadQueuedDraftAsync`、`VerifyRunSettingsReload`、`RecoverRejectedReferences` | Windows 独立进程从 SQLite 重开代码会话，输入区恢复已保存的 B/Auto Review，原队列、Run 和过期 C 配置保持原绑定。队列拒绝后取回原文字和准确选区，脚本模型收到复核过的引用，用户也可移除引用并保留文字。SQLite 用例验证旁聊已关闭后仍能取回存好的结论，另一主体无法读取或复用，来源撤销时只返回不可用提示并拒绝提交。恢复授权再排队后，原拒绝记录保留，新的命令重发仍只有一个队列项。 | 其他目标平台上的 UI 实测。 |
| 未生效补充的取回和重新提交 | `ReadUnappliedSteerDraftAsync`、`VerifyRunSettingsReload`、`RecoverUnappliedSteer` | Windows 和 WSL 的 Preview 在代码任务等待范围表单时受理带选区的补充，停止后从 SQLite 重开原会话。取回仅填空草稿并恢复输入焦点。空闲时提供作为新任务发送，活动 Run 期间只提供排队。切换会话后保留草稿及提交方式。显式排队等当前任务结束后启动，模型收到原文和准确选区。SQLite 专项验证待应用状态不可取回，另一主体不可读取或复用，撤权只影响对应结论。恢复授权后作为新任务发送，使用原 Skill 版本及已关闭旁聊的结论。相同新命令重发只创建一个 Run，原消息仍归原 Run 并保持 `NotApplied`。 | 其他平台实测及两个原生窗口同时操作的焦点和草稿验收。 |
| 动态工具目录和审批恢复 | `RegisterToolCatalog`、`ReplaceToolCatalogAsync`、`VerifyToolCatalog`、`VerifyCatalogApproval` | Windows 和 WSL 核对流式轮次绑定原目录、下一轮读取新版、未执行的旧审批失效，以及已发出的调用保留原结果。同版本换定义、目录命名冲突和已移除版本的重新发布被拒绝。SQLite 重开从同一调用、计划和审批恢复。同版本只执行一次，换版后按原调用返回冲突且无工具执行。实际 MCP HTTP 连接的换版结果见下一行。 | 其他宿主环境。 |
| MCP 工具协议与宿主绑定 | `McpToolCatalog`、`VerifyMcpCatalog`、`huxerui_agentkit_mcp_http_tests` | 宿主传输夹具核对 JSON-RPC 元数据、唯一请求 ID、分页游标和宿主对工具效果的声明。WSL 和含 WinHTTP 修复的隔离 Windows Debug SDK 通过回环 HTTP 验收。SSE 订阅仍打开时接收换版通知并刷新两页清单，旧审批按原调用和计划结清为冲突。重放批准回执只执行一次。单次提交和随后同轮的两次提交都使用获批的 v2，进度令牌和结构化结果按各自请求关联。重新订阅后刷新返回 HTTP 403，目录清空，剩余待审批调用失效。用相同 AgentKit 源码和正式 Windows SDK v0.4.0 从空目录构建，复现订阅保持打开时短通知滞留。 | WinHTTP 修复进入正式 Windows SDK 后复验发行包。 |
| MCP stdio 子进程 | `huxerui_agentkit_mcp_stdio_tests`、`McpToolExchange`、`Conversation::RetryAsync` | Windows 标准 SDK 和 WSL 使用真实子进程及 stdin/stdout 管道。清单换版通知使旧审批失效，批准的新调用按原 ID 收到进度与结果。子进程登记写入后、回复前退出，工具保留原调用 ID 并标记 `ResultUnknown`，拒绝重试，另一个尚未执行的审批随目录撤回失效。宿主在同一 Runtime 连接新进程，使用新 ID 订阅并批准新版调用。子进程的写入记录确认原请求未重放。宿主发送针对新订阅的取消通知并关闭 stdin，子进程正常退出。 [MCP stdio](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio) | 其他桌面环境。 |
| 同轮工具调度和恢复 | `RuntimeOptions::max_parallel_tools_per_turn`、`huxerui_agentkit_tool_schedule_tests`、`huxerui_agentkit_mcp_http_tests`、`VerifyStoredToolTurn` | Windows 和 WSL 核对默认串行、未知范围独占、共享读和资源冲突。等待审批不占执行名额，未就绪的前序调用保持顺序。执行前复核资源与基线，取消阻止新调用并区分已发出的读和结果待核对的写。SQLite 重开保留两条原审批、结果和预算，已完成的调用不重跑。WSL 和含修复的 Windows SDK 以实际 HTTP 连接验证换版后同一模型轮次的两次调用各自审批。测试服务器保持第一条请求打开，待第二条完成后才放行第一条。运行时等待两条都结束，将结果按模型原调用顺序回传，并核对原调用 ID、作用范围和 v2 结构化结果。 | 正式 Windows SDK 的 HTTP 路径随短 SSE 修复复验。 |
| 独立第三方宿主 | `tests/third_party_host`、`HuxerUI::AgentKit`、`RegisterArtifactRenderer`、`AgentChat` | Windows x64 与 Ubuntu 24.04（WSL）分别用已安装的 HuxerUI SDK 配置独立 CMake 项目。器材预约宿主登记两个 Agent、本地脚本模型、版本化手册、预约工具、权限策略及两种业务渲染器。通用助手在批准后仅变更库存一次，拒绝的预约无副作用。受限项目及会话无法访问，会话也未出现在列表。实际操作自定义消息卡片和产物审阅，核对版本不符的动作被拒绝，撤权后审阅关闭且已有动作入口无法继续提交。Windows 的审批、独立审阅及 PPT 局部返工三项回归同时通过。 | 使用 HuxerUI 的窗口测试环境运行，平台实测见对应平台条目。 |
| SSH 宿主工具 | `tests/third_party_host/ssh_task_host.cpp`、`ToolEffect::ExternalSubmit`、`ToolContext::Resources()`、Lib-Terminal | Windows x64 和 WSL 分别用独立 SDK 工程及实际 OpenSSH 客户端验证。临时回环服务固定主机密钥和客户端公钥，只接受宿主配置的验证命令。批准前、拒绝后及批准前撤权时均无连接。批准并重放审批命令只启动一次。服务端逐次返回 0、17 及无退出状态的断开，AgentKit 分别记录成功、失败和待核对。两个平台的 SSH 和器材预约宿主验收均通过。 | macOS 桌面客户端。 |

候选审阅、回撤和影响定位到返工的完整流程继续按正式文档记录实际验收证据。

独立宿主可以用 `cmake -S tests/third_party_host -B build/third-party-host -DCMAKE_BUILD_TYPE=Debug -DHuxerUI_DIR=<SDK 的 CMake 包目录>` 配置，接着运行 `cmake --build build/third-party-host --config Debug` 和 `ctest --test-dir build/third-party-host -C Debug --output-on-failure`。该验收项目使用 SDK 的 `testing` 组件，业务代码只包含公开头文件。

代码差异改用只读 `CodeEditor`。审阅专项使用键盘跨行选择，检查引用中的补丁版本、文件 `PartId`、字节范围和原文。Windows 的审批、回撤、编辑器基线、编辑器重启恢复和旁聊五项通过。WSL 的同五项及完整 Preview 发布链通过。Windows 的完整流程在统一 155 秒总时限下超时。独立复跑确认五页简报已提交，旧导出审批已失效，但大纲未在原来单步的 8 秒内出现。将该等待调为与其他跨会话步骤一致的 25 秒后，Windows 独立复跑通过，包含测试失败及缺少截图两条阻断路径。导出的 `WorkspaceRelease.pptx` 有五页，包含原验证截图且归档完整。

复验时 GitHub 正式发布仍为 v0.4.0。分别用已安装的 Windows v0.4.0 和基于 `5456e3e8` 应用短 SSE 修复并独立安装的静态 Debug SDK，从空目录配置和编译同一版 AgentKit。正式 SDK 的运行时、同轮工具调度和 MCP stdio 三项通过。HTTP 专项在服务端保持 SSE 订阅打开期间等不到客户端刷新。WinHTTP 的固定大小读取会尝试填充缓冲区，修复先取得已可读的字节量。隔离 Windows SDK 和 WSL 的四项全部通过，覆盖换版、已批准的同轮并行请求和撤权。两个旧构建目录的 Ninja 依赖日志未记录 MSVC 头文件，增量链接混入旧对象并崩溃，干净构建验证后不再使用该次增量结果。 [HuxerUI 发布包](https://github.com/HuxerUI/HuxerUI/releases/tag/v0.4.0) · [WinHTTP 读取说明](https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpreaddata)

发布链截图用例使用 CMake 检测到的 Chromium 浏览器，也可显式设置 `HUXERUI_AGENTKIT_TEST_BROWSER`。缺少浏览器时，测试输出会注明截图用例跳过。构建后可把一个新的本地目录路径传给 `huxerui_agentkit_preview_tests.exe`，保留 SQLite、测试日志、截图和导出的 `exports/WorkspaceRelease.pptx`。

发布后的影响导航可用 `huxerui_agentkit_preview_tests.exe --release-impact <已完成的验收目录>` 复验。该用例读取已完成发布的 SQLite 和产物，并在夹具的 `code/settings.html` 写入模拟的发布后编辑，检查审阅过程保留这次编辑。

发布验证阻断专项可执行 `ctest --test-dir build/acceptance -R 'huxerui_agentkit_release_(test_failure|missing_screenshot)_tests' --output-on-failure`。单独保留证据时，用 `huxerui_agentkit_preview_tests.exe --release-verification-failure <test-failure 或 missing-screenshot> <新目录>`。两个专项均使用实际 CTest，无需浏览器。Windows 分别用时 41 秒和 34 秒，WSL 两项各约 4 秒。

实际模型工具回传用例使用 `huxerui_agentkit_model_tests.exe --configured-provider`。从进程环境读取 `HUXERUI_AGENTKIT_BASE_URL`、`HUXERUI_AGENTKIT_MODEL` 和 `HUXERUI_AGENTKIT_API_KEY`。`HUXERUI_AGENTKIT_PROTOCOL` 可选 `responses`、`messages`、`generate-content` 或 `interactions`，缺省使用 Chat Completions。Responses 用 `HUXERUI_AGENTKIT_PROVIDER` 选择 `openai`、`deepseek` 或 `glm`。Messages 可另选提供商和 `HUXERUI_AGENTKIT_ANTHROPIC_VERSION`。Interactions 和 GLM Responses 可用 `HUXERUI_AGENTKIT_ALLOW_SERVER_STATE=1` 单独验收服务端续轮。Preview 的配置验收入口使用同一协议选项。

OpenAI Responses 推理模型的实测可设置 `HUXERUI_AGENTKIT_REQUIRE_ENCRYPTED_REPLAY=1`。用例核对后续原生 HTTP 请求同时发送工具结果与加密推理项，并核对关闭服务端存储且省略服务端响应引用。工具仍须由真实模型触发并回应宿主返回的标记。

实际 Responses 结构化输出使用同一可执行文件的 `--configured-structured` 入口，将 `HUXERUI_AGENTKIT_PROTOCOL` 设为 `responses` 并选择提供商方言。用例核对 `RunContext::CompleteAsync` 返回的字段和值，确认宿主读取数据且对话只显示核对结果。

跨进程验收使用同一 OpenAI Responses 端点和支持加密推理的模型，先运行 `huxerui_agentkit_model_tests.exe --configured-responses-recovery <新 SQLite 文件路径> prepare`，再以同一路径运行 `resume`。两个进程打印 Run、调用、计划及审批 ID 和审批版本。续轮阶段先确认没有调用模型或执行工具，再批准原交互。用例在内存中比较 SQLite 中的加密项与实际原生 HTTP 请求，确认工具只执行一次且模型返回宿主提供的标记。

Responses 完整版首轮在通用助手路由时收到 HTTP 400。本地转发保留原请求并取得错误字段，端点要求 `text.format` 为 `json_object` 时，`input` 的消息须指明 JSON 输出。修正请求装配后，通过同一真实端点在新目录完成三段发布链。协议回归核对 OpenAI 和 GLM 的格式消息，并覆盖 GLM 的续轮与回退。 [Responses JSON 模式](https://developers.openai.com/api/docs/guides/structured-outputs?api-mode=responses#json-mode)

真实模型下的两条 Preview 业务流程使用 `huxerui_agentkit_preview_tests.exe --configured-preview <验收目录> <阶段>`，在同一个目录依次运行 `prepare-code`、`prepare-ppt` 和 `finish`。该入口使用原生应用循环、生产配置、Preview 的业务工具和 SQLite。阶段驱动器提交与界面相同的表单和审批命令。验收目录的 `code` 子目录使用隔离夹具，包含初始状态为 Disabled 的 `config.txt` 和 `settings.html`，以及检查两处改动并写入 `tested.txt` 的 CTest。测试阶段会打印补丁和保存交互的 ID，供重启前后核对。导出文件为 `exports/LiveAcceptance.pptx`。

真实模型发布链使用同一入口。在一个新的验收目录中依次运行 `release-prepare`、`release-draft`、`release-finish`。首阶段自动创建隔离工作区并要求构建时配置 Chromium。完成设计与补丁审批后停在独立会话的测试审批。第二阶段恢复同一审批，运行测试并起草十页演示，在导出前修改 Goal。第三阶段从新的五页简报继续，确认导出及目标审批，检查 `exports/LiveRelease.pptx`、实际工作区内容和截图。验收简报要求每页使用简短要点，以控制实测端点的生成时间。

Git worktree 用例可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_worktree_tests --output-on-failure`。使用 `huxerui_agentkit_preview_tests.exe --worktree <验收目录>` 可将证据保留在新的隔离目录，包含源仓库、SQLite、工作区登记、原命令和 worktree 归属记录。使用 `--worktree-discard <验收目录>` 可单独复跑回收用例。

消息旁聊可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_side_chat_tests --output-on-failure`。使用 `huxerui_agentkit_preview_tests.exe --side-chat <验收目录>` 可保留 SQLite 中已提交的主对话和结论来源。

产物旁聊队列可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_artifact_side_chat_tests --output-on-failure`。跨进程验收先运行 `huxerui_agentkit_preview_tests.exe --artifact-side-chat prepare <新目录>`，再用同一目录运行 `--artifact-side-chat resume`。第一阶段保留原导出审批和队列，第二阶段批准并核对原结论进入后续对话。SQLite 中的 `agentkit.session` 记录可用于比对来源版本、页 ID、区间、审批及队列状态。

图片选区和空闲直接提交可运行 `huxerui_agentkit_preview_tests.exe --goal-completion-revision <新验收目录>`。用例使用 Chromium 采集实际实现截图，检查两个批准版本中的图片字节，并在最终五页版本进入图片旁聊。保留的 SQLite 可查证直接发送的新 Run 及 `slide-5/picture-6` 是否引用已完成 Goal 的最终 PPTX。

饼图界面回归可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_report_chart_tests --output-on-failure`。使用 `huxerui_agentkit_preview_tests.exe --report-chart <验收目录>` 可保留 SQLite、原表单与审批记录和实际导出的 `exports/团队构成.csv`。

独立模型审查使用 `huxerui_agentkit_preview_tests.exe --configured-preview <验收目录> <阶段>`。先在新的验收目录创建 `code` 子目录，分别配置能完成饼图任务的对话端点和独立审查端点。阶段为 `review-approve`、`review-deny`、`review-manual` 或 `review-ask`，每次使用独立目录。回环验收用结构化的 `approve`、`deny` 或 `defer` 控制决定，也可模拟截断、过量字段、HTTP 失败和超时。核对 SQLite 中的计划、审查结果及导出实际内容。

发布演示 Skill 的界面回归可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_release_skill_tests --output-on-failure`。用 `huxerui_agentkit_preview_tests.exe --release-skill <新验收目录>` 可保留两条路径的 SQLite 与导出文件。真实模型实测通过 `--configured-preview <验收目录> skill-prepare` 保存等待中的激活，再在同一目录运行 `skill-finish`，核对两个进程打印的激活 ID 和最终 `exports/LiveReleaseSkill.pptx`。

模型和权限时序回归可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_run_settings_tests --output-on-failure`。使用 `huxerui_agentkit_preview_tests.exe --run-settings <验收目录>` 可保留 SQLite、工作区修改后的 `project/config.txt` 和规则审查生成的 `exports/Report chart.csv`。
在另一进程用 `huxerui_agentkit_preview_tests.exe --reload-run-settings <同一验收目录>` 可单独核对持久化的偏好、原拒绝记录和可编辑引用。

PPTX 局部返工可执行 `ctest --test-dir build/acceptance -R huxerui_agentkit_presentation_rework_tests --output-on-failure`。用 `huxerui_agentkit_preview_tests.exe --presentation-rework <新验收目录>` 可保留两份同项目报告和一份其他项目报告、原版和新版暂存 PPTX、SQLite 审批记录及 `exports/ReworkedRelease.pptx`。

新版导出的专项同时在 Windows 和 WSL 运行了 Runtime、SQLite 和 Preview 返工三项 CTest。保留一份从审阅页导出的 Windows 文件，PowerPoint 打开后第 1 页仍显示旧报告结论，第 2 页显示新结论与原观察项。

消息选区操作调整之前的 Windows 串行基线跑完全部 41 项，40 项通过。唯一失败为正式 SDK 的 MCP HTTP 短 SSE 通知在订阅保持打开时未触发清单刷新。完整 Preview、Goal 修订、测试失败与截图缺失阻断、长对话及回撤用例均通过。

## Linux Debug（Ubuntu 24.04 / WSL）

接入 Lib-CodeEditor 后，Linux Preview 通过公开 SDK 构建为 x86-64 ELF 应用。全部三十项 CTest 通过，包括编辑器缓冲审批、普通代码回撤、分叉候选和模型权限时序。

普通代码回撤加入部分写入与结果核对后，WSL 全部三十二项 CTest 通过。Windows 在同一源码上通过普通代码回撤、结果核对和设计回撤三项。共享文件写入凭据读取路径的代码采纳与部分采纳也通过专项回归。

使用 GCC 14 和 WSL 内的 HuxerUI SDK 构建 AgentKit 核心库。运行时、SQLite 和模型协议夹具三个测试目标通过，覆盖消息选区及旁聊结论的重启恢复、继承产物的历史切片和产物选区读取时撤权。GCC 13 重新编译了最初触发编译器内部错误的协作单元，整库和测试仍待用该编译器验证。

原 WSL SDK 缺少 Preview 依赖的 `File::OpenRandomAccess`、`ViewHandle` 和 `AsyncInputStream::FromReader`。从 HuxerUI 提交 `c67c42c2` 构建并隔离安装匹配 SDK，使用 GCC 14 重新编译后通过全部十项回归，含窗口外 Preview UI、实际 Git worktree、Skill 模板和实际 PPTX 导出。

使用该 SDK 从 Preview 目录执行 `huxerui build linux --profile debug --generator Ninja --sdk <SDK 安装目录>`，动态目录接入后的版本成功链接为 Linux x86-64 的 Preview 可执行文件。

本轮另将 Chrome for Testing Headless Shell 157.0.8103.78 配置为 `HUXERUI_AGENTKIT_TEST_BROWSER`，重新构建并通过全部十项回归。用新目录运行 `huxerui_agentkit_preview_tests <验收目录>` 保留发布链证据。验证进程生成 900 × 570 的 PNG，`exports/WorkspaceRelease.pptx` 包含五页且压缩包完整，归档里的 `ppt/media/implementation.png` 与验证目录里的实际截图 SHA-256 一致。

接入同轮调度及会话 schema 2 后，沿用相同浏览器配置重新构建，全部十一项 CTest 通过，含同轮执行约束及 SQLite 旧审批迁移。

接入局部返工和新版本导出后的首轮十二项 CTest 中十项通过。旁聊与 worktree 的间歇崩溃在单项重复执行中复现，Windows 的两个对应专项通过。在 WSL 上用 AddressSanitizer 对 AgentKit 及 HuxerUI 的测试快照单元插桩，两个用例均捕获到快照构建或释放阶段的异常访问。独立 SDK 探针完成 5,000 帧并多次重绘，崩溃根因仍待定位。

接入 PowerPoint 审阅页的换源入口后，WSL 十二项 CTest 单次通过。返工用例从同项目另一会话选取新报告，其他项目的报告不出现在选择界面。随后单项重跑旁聊八次均通过，worktree 前五次通过，第六次再次崩溃。

接入审阅页导出和来源变更记录后，WSL 的 Runtime、SQLite 和 Preview 返工专项通过，随后十二项 CTest 在本次运行中全部通过。旧的 worktree 间歇崩溃仍待单独定位。

接入共享资料的来源变更后，WSL 的 SQLite 专项与十二项 CTest 全部通过。使用同一 SDK 重新构建 Linux Preview Debug 应用，成功链接 `example_agent_kit`。

接入代码文件回撤后，WSL 的十三项 CTest 全部通过。用同一 SDK 在 WSL 本地文件系统重新配置并构建 Linux Preview Debug，确认生成 x86-64 ELF 应用。

接入设计稿版本回撤后，WSL 的十四项 CTest 全部通过，Linux Preview Debug SDK 消费构建成功。

接入分叉候选后，WSL 的十五项 CTest 全部通过，Linux Preview Debug 消费端重新链接为 x86-64 ELF 应用。调整 Preview 回归等待会话加载的条件后，复跑完整 Preview 用例通过。

接入报告影响导航后，WSL 十二项 CTest 本次通过，报告导航和返工专项连续八次通过。

接入多行输入和中文 IME 验收后，WSL 全部二十七项 CTest 通过。Windows 的导航、模型和权限针对性用例，以及 IME 和旁聊专项通过。Windows 使用 `huxerui build windows --profile debug` 重新链接 Preview SDK 消费端。

补齐 PowerPoint 候选采纳排队冲突后，Windows 与 WSL 的基线竞争、采纳恢复及局部返工专项均通过。Windows SQLite 专项通过。WSL 的 SQLite 专项首轮以 `free(): invalid pointer` 中止，隔离复跑通过，GDB 下正常退出，随后连续四次单项执行均通过。首次中止原因待定位。

使用匹配 SDK 重新构建 Preview 测试宿主，通过 `--native-review-approval` 初始化隔离会话，再以 `--native-preview-compact` 打开原生窄窗。在 Ubuntu 24.04 的独立 D-Bus 会话和 dconf 配置中，用系统的 XDG Settings portal 和 GTK 后端触发浅色、深色和返回浅色。核对实际 `SettingChanged` 通知及窗口截图。主窗 520 × 680 和独立 PPT 审阅窗 900 × 760 随系统主题切换，两个窗口均保持原 ID。分别选择第 3 页和第 2 页后，切换期间各自保留页码和 v1 内容，幻灯片底色保留。此用例覆盖 Linux 的在途主题更新，Windows 的系统主题切换需要另验。 [XDG Settings](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.Settings.html)

重建当前源码后，全部 41 项 CTest 于 104 秒内通过。全套回归前，修正菜单定位后的 worktree 和新增暂停场景后的 SQLite 各连续通过三次。修正前独立复跑 SQLite 和旁聊各八次也通过。当前复跑未再出现此前的间歇崩溃。

更新侧栏待办订阅和会话菜单后，重建测试宿主并运行全部 41 项 CTest。首轮完整 Preview 用例仍查找未排版的 Markdown 大纲，其他 40 项通过。将该断言改为核对实际显示的标题和列表文本，复跑全部 41 项于 111 秒内通过。完整 Preview 用例通过通用助手直接调用、跨会话发布流程和实际 PPTX 导出。

旁聊草稿恢复及短窗审批布局接入后，重建并通过全部 41 项 CTest，耗时 122 秒。随后修正原生窗口发现的标题和文件页签裁切，复跑审阅审批、跨领域审阅和代码编辑器三项均通过。

在 Ubuntu 24.04 的隔离 X11 会话中，用默认 GTK 渲染器和实际输入操作复验 520 × 680 主窗和 900 × 760 独立代码审阅窗。由深色切到浅色再切回深色，两窗以原窗口 ID 分别保留 v1 的 `settings.html` 和 `config.txt` 页签。返回对话后，原附件获回焦点，未发送草稿和两份具名的 v1 待审批附件仍在。深色窄窗也验证了新建页、导航抽屉及工作区编辑器读取未改动的 `config.txt`。Windows SDK Debug 消费端重新构建成功。具名附件变更后的审阅和跨领域审阅专项在 Windows 和 WSL 均通过，WSL 的长对话和分叉审阅也通过。

同一 Linux 测试程序强制使用 GTK Cairo 时，新建页截图出现 18,801 个粉色像素，与 GTK 4.14.5 对未支持的变换绘制的错误标记一致。在隔离环境取消 `GSK_RENDERER` 后，新建页和双窗审阅的浅色与深色截图均为零。此次对照使用软件 OpenGL 可用的 X11 环境。 [GTK 4.14.5 源码](https://github.com/GNOME/gtk/blob/4.14.5/gsk/gskrendernodeimpl.c#L3220-L3233)

在 Linux 520 × 680 深色原生窄窗中，鼠标拖选通用助手对话内的消息，引用与旁聊操作显示在所选正文下方。引用只把选中的两个词加入草稿。旁聊聚焦独立提问框，本地模型回显相同片段。带回结论后，主草稿的文字、引用、选区及操作焦点保持。再次拖选后用 Tab 和 Enter 重开同一旁聊，Escape 返回原位置。修订后的 IME、消息及产物旁聊专项在 Windows 和 WSL 通过，WSL 另通过长对话专项。Windows Preview SDK Debug 消费构建成功。

Windows 和 WSL 的报告返工与候选基线回归核对概要在原报告卡片下展开、收起回到原卡片焦点、草稿保留，并继续通过影响跳转和授权变化用例。Linux 520 × 680 深色原生窄窗从存档的成功验证会话展开 `Workspace verification` v1，查找受影响的演示，打开 `Design briefing.pptx` v1 的第 3 页，核对页码和页面标题。Windows 四项专项、WSL 五项专项通过，覆盖独立审批和跨领域审阅，WSL 同时覆盖长对话。

Windows 和 WSL 各通过四项 CTest 输出与对话专项。实际 CTest 的失败诊断在关闭、重开会话后仍可原位展开，文本中保留失败原因并去掉分段标记与颜色控制字符。测试同时从原工具调用 ID 对应的本地日志读到注入的 ANSI 字节。测试成功但缺少截图时，继续显示退出码 0，发布保持阻塞。Linux 520 × 680 深色原生窄窗用变更前的已保存会话，核对了成功及失败日志的默认收起和展开，失败文件及原因可读。Windows SDK Debug 消费构建成功。

Linux 520 × 680 深色原生窄窗从设计会话新建并启用 Goal。目标触发的 Run 等待输入时暂停，核对目标显示 Paused、Run 仍等待输入。修改说明前预览将关闭的一条待办，应用后目标保持暂停并升至 v2，原 Run 收尾。恢复后新 Run 等待输入，累计轮次和调用均保留。再次暂停并退出后，用同一库重启，原目标仍处于 Paused v2，保留累计用量，恢复后的 Run 显示等待输入。另打开已完成的发布 Goal，核对目标 v3、Run 空闲和五项证据的名称及版本。Windows、WSL 的完成修订用例核对 Goal 面板按原 `InteractionId` 打开请求，修改后旧请求失效。WSL 同时通过测试失败和截图缺失阻断专项，Windows Preview SDK Debug 消费构建成功。

在 Linux 520 × 680 深色原生窗口编辑处于暂停状态的设计 Goal，输入两行完成条件，将策略改为验证通过即完成。影响预览显示原 Run 将收尾且关闭一项待办。应用后保留目标标识、暂停状态和累计用量，定义从 v2 升到 v3，两行条件与新策略均可查看。通用文字条件的完成判定需要宿主配置相应验证器。

Windows 和 WSL 分别通过发布 Goal 的人工确认与自动完成两项回归。两条路径均从待完成确认的 10 页发布目标修改为 5 页，关闭旧确认并在重启后使用修订版本，核对实际测试、截图和新旧 PPTX。人工路径等待新的确认，自动路径通过同一发布验证后直接完成。WSL 全部 42 项 CTest 通过，Windows Preview SDK Debug 消费构建成功。Windows 完整 Preview 回归原先两次在单页大纲工具仍处于就绪时达到 8 秒等待上限。将这一检查的等待上限设为 25 秒，保留结果断言和执行条件，使用独立证据目录串行复跑，完成大纲、协作、补丁、测试和实际发布导出。

Linux 原生 520 × 680 深色窗口通过用户提供的 Chat Completions 中转调用 `gpt-4.1-mini-2025-04-14`。用键盘逆向选取两条消息，结束选区后撤下端点提示。实际 SSE 请求在最新问题之前按阅读顺序提供 v1 和 v2 的两个精确片段，旁聊的上下文限于选区与自身会话。移动资料顺序后，同一句问题获得包含两段的回复。回到主对话时恢复选区端点焦点，显式发送并在重启后读取原结论。请求将结论放在当前问题之前，并在引用正文标明来源类型。重启后的实际模型回复正确复述结论。SQLite 保留两条消息原版本及 UTF-8 区间，仅保存主会话。最新代码的 Windows 运行时、消息旁聊、产物旁聊三项通过，WSL 另通过 IME 专项，共四项。

Linux 520 × 680 深色原生窗口从已保存的 `Product rollout.pptx` v1 打开第 3 页，选中 `Brief & revise <option>`。实际 Chat Completions 请求仅包含选段和追问。为选段增加引用标签后，同一句问题得到完整复述。带回结论保留原页，返回对话后焦点回到 v1 卡片，用 Enter 再次打开仍为第 3 页。显式发送只登记一次结论，SQLite 核对其来源为 v1、`slide-3` 和 UTF-8 字节区间 [14, 37)。原暂存 PPTX 的 SHA-256 一致。Windows 的运行时、消息旁聊、产物旁聊三项通过，WSL 另通过 IME 专项，共四项。

按 Preview 桌面设计稿对照输入栏，模型和发送方式使用下拉图标，权限显示盾牌，发送按钮保留文字和上箭头。Preview 关闭窗口调试叠层。Linux 1120 × 940 深色原生窗口点击「新建对话」后直接键入文字并发送，核对首条消息和回复。Windows 和 WSL 的导航专项验证窄窗口从侧栏新建时收起侧栏并把焦点交给输入框，WSL 同轮五项专项通过，覆盖导航、IME、模型和权限、消息及产物旁聊。Windows SDK 消费端 Debug 构建成功。同轮 Linux 520 × 680 原生窗口观察到焦点回到侧栏入口，后续在同尺寸窗口修复并复验。

Preview 在同一窗口按工作区保留新建草稿。Linux 1120 × 940 深色原生窗口分别键入独立对话和工作区的草稿，通过侧栏与工作区选择器往返，正文各自恢复。Windows 和 WSL 的导航专项同时核对助手和权限恢复。在 SQLite 创建提交前暂挂独立草稿的请求，切回工作区后再放行。原会话以通用助手和空工作区出现在目录，当前工作区草稿继续可编辑，原会话可随后打开查看回复。启动期间已输入的正文随首次预选工作区保留。Windows SDK 消费端 Debug 构建通过。WSL 通过导航、IME、模型权限、worktree 和完整 Preview 集成用例。

Preview 将 Goal 和方案比较放入对话标题区。审阅时保留返回、独立打开和当前产物的操作。Linux 1120 × 940 深色原生窗验收 `Product rollout.pptx` v1 第 3 页。关闭旁聊仍在第 3 页，带回助手回复后回到源对话，直接键入的文字进入带结论的草稿。再次打开同一产物定位 v1 第 3 页。WSL 九项 Preview 回归通过，包含长对话返回时的滚动和焦点、跨消息引用、差异和截图选区、PPT 结论排队并在重启后恢复、同窗口和独立窗口审阅。Windows 长对话、消息旁聊和产物旁聊回归，以及 SDK 消费端 Debug 构建通过。WSL 独立第三方宿主消费回归通过。

Windows 和 WSL 的独立宿主从已授权的产物打开旁聊，核对模型收到的收据片段及其回复。弹窗打开期间通过 `RefreshAccessAsync` 撤销产物读取权，界面移除选段、回复、追问和带回结论入口，提示关闭并重新选择。原对话的文字草稿保留。相同的产物选区和已保存的旧回复引用现在均返回 `PermissionDenied`。修复前分别复现弹窗未收起和旧回复可读。WSL 六项相关 CTest 通过，覆盖运行时、SQLite、完整 Preview、审批审阅和两类旁聊。Windows SDK 消费端 Debug 构建通过。

Linux 520 × 680 深色原生窗口用 X11 输入复现侧栏在输入框获焦后的下一帧把焦点还给侧栏入口。Preview 在后续帧确认焦点，侧栏恢复焦点时重新交给输入框。修复后分别用鼠标和键盘从侧栏新建，保持窗口原有焦点并直接键入，均进入新建输入框。鼠标路径发送首条消息，核对标题、消息和回复。键盘路径从已有对话打开侧栏，按 Enter 新建后直接键入。Windows 和 WSL 的导航与 IME 专项通过，导航回归在侧栏收起后再推进一帧并向当前文本输入会话提交文字。Windows SDK 消费端 Debug 构建通过。

本轮 WSL 完整回归通过 42 项中的 41 项。SQLite 恢复测试以段错误结束，WSL 内核记录了空指令地址。原测试入口连续通过十二次，两批 GDB 复跑分别通过七次和十四次，Windows SQLite 专项也通过。用 AddressSanitizer 在恢复已批准的会话创建场景复现，调用栈停在 SDK 的测试快照生成与 AddressSanitizer 的栈分配代码中。独立诊断副本关闭快照采集后，在会话 CBOR 序列化中再次触发该栈分配错误。只关闭 AddressSanitizer 的返回后栈检查时，保留堆检查的原测试连续通过五次。只链接 SDK 的窗口与顺序后台任务探针各通过 110 次。Valgrind 检查在返工准备场景达到测试自身的等待上限，超时前报告零个内存错误，未覆盖后续场景。普通构建首次段错误的调用栈仍缺失，后续复现时采集原构建的调用栈。

通过用户提供的 OpenAI 兼容中转测试 Claude 和 Gemini 的工具调用。Claude Haiku 4.5 结束后未取得宿主工具返回的标记，独立请求看到它直接以文本结束。Gemini 2.5 Flash 的工具调用流同时出现 tool_calls 和 stop 两种结束原因，对应的 AgentKit Run 报协议错误。两组都未通过该中转的工具回传验收。Claude Messages 和 Gemini 原生协议的结果仍以独立回环夹具为证据。

## Android 和 Web Debug

使用当前的 Web Debug SDK 构建，在 Chromium 通过通用助手的本地演示模型生成包含 HTTP 链接及 `file:` 地址的 Markdown 回复。完成前后均未访问目标。点击回复中的文件地址保留原对话。点击获准的本地报告链接后，新标签页只请求一次目标，保留查询参数和片段，且无 opener 和 Referer。原会话与已完成 Run 仍可读取。 [Web 链接行为](https://html.spec.whatwg.org/multipage/links.html#link-type-noopener)

从 Preview 目录用已安装的 HuxerUI SDK 和 Android Studio JBR 执行 `huxerui build android --config debug --abi <ABI> --java-home <JBR 路径>`，分别构建 `x86_64` 和 `arm64-v8a`。CMake 暂存目录在仓库根下的 `.huxerui/cxx`。两份 APK 通过 `apksigner verify`，`aapt dump badging` 核对包名 `org.huxerui.lib.agentkit.preview`、`MainActivity` 及各自的 ABI。 [Android Gradle CMake 配置](https://developer.android.com/reference/tools/gradle-api/8.13/com/android/build/api/dsl/Cmake)

Android Emulator 36.5.11 的 WHPX 检查通过。Pixel 10 只读冷启动报告启动完成，ADB 未授权，随后模拟器退出。用 API 36 x86_64 镜像新建的专用 AVD 在 SwiftShader 模式下以 `0xC0000005` 退出。改用 ANGLE 并关闭 Vulkan，ADB 可以列出设备，但 Shell、直接读取和模拟器控制台均超时，未到可安装阶段。 [Android Emulator 启动选项](https://developer.android.com/studio/run/emulator-commandline) · [图形故障排查](https://developer.android.com/studio/run/emulator-troubleshooting)

另用 Pixel 9 的 Android 16 x86_64 Play Store 镜像、宿主 GPU、只读模式及无快照执行冷启动。重新构建 SDK Debug 消费端，实际安装 APK 并启动。通过触控与 Android 输入发送首条消息，核对自动标题、本地通用助手回复和会话列表。发送 `approval` 后在待审批时强制结束应用进程，再冷启动并从列表打开原对话。原消息、回复和待审批操作均恢复，点击批准后显示完成回复。比对两个进程的 SQLite 检查点，只有同一个会话和两个 Run。原审批由 `Pending` 到 `Approved`，对应 Run 由 `WaitingForInput` 到 `Succeeded`，ID 均保持。此轮验证 x86_64 模拟器上的会话、触控和审批恢复，ARM64 实机、领域审阅及移动端权限交互仍待验收。

使用同一设备和新构建的 x86_64 Debug APK 对照窄窗旁聊。长按本地助手回复中的 `Received`，打开旁聊并输入问题。软键盘打开时问题框获得焦点，操作按钮可触及。在进程保持运行时切到系统深色，选区、问题和焦点仍在。旧版问题文字接近背景色，新版截图核对了深色主题的正文前景和可见光标。提交后本地助手收到准确选词，带回结论只更新原对话草稿。切回浅色后原对话和结论仍在。

Web 的最初构建能绘制界面，SQLite 初始化时因 SDK 缺少 `WorkerSequence::Run()` 而失败。改用 IndexedDB 后，执行 `huxerui build web --profile debug` 和 `huxerui package web --profile debug` 均成功。将 `dist/web` 用本地 HTTP 服务加载到 Chromium，在默认通用助手发送 `Web proof` 并收到回复。刷新后从目录打开原会话，读取同一条消息和回复。接着发送 `approval`，在审批待处理时刷新，打开会话并批准。解码浏览器持久化记录，核对原 `RunId` 和交互 ID、审批状态由 `Pending` 到 `Approved`、原 Run 到 `Succeeded`，Run 数量一致。

在 Chromium 对同一测试库打开两条 IndexedDB 连接。并发使用同一存储基线的提交只有一个成功。相同 ID 和载荷重放返回原修订，换载荷报冲突。注入提交事务在写记录后、更新总修订前失败，确认记录与原修订保持一致且没有新回执，重试后更新和删除一起生效。另核对模型偏好可跨连接读取，以及相同基线的两个归类更新只有一个写入。Windows SQLite、导航和 IME 三项回归通过，WSL 全部二十七项 CTest 通过。

从 `dist/web` 在 Chromium 走通 PPT 助手简报、编辑大纲、生成、逐页审阅及导出。将第 1 页标题改为 `Approved scope`，审阅实际 PPTX 的第 1 页和第 2 页。提交文件名 `WebRelease.pptx` 后，在审批待处理时刷新。持久记录确认刷新前只有暂存文件，刷新后的审批沿用原 Run 与交互 ID。点击批准后原 Run 成功结束。点击「下载 PowerPoint」得到实际文件，暂存、浏览器导出目录及下载文件的字节一致。PowerPoint 打开文件，核对两页及批准后的页标题。再次导出到同名目标返回冲突，两个已有文件内容保持一致。

Web Debug SDK 包复验通过。Windows 的 PPTX、局部返工、PPT 采纳恢复和独立审阅四项回归通过，WSL 全部二十七项 CTest 通过。

Chromium 从 Web Debug SDK 包实际打开单文件选择器。缺少 `summary` 的 JSON 显示校验错误，随后选取有效报告并将摘要发布为本项目共享项。PPT 大纲的两页分别插入该摘要，生成 v1 后从第 2 页导入另一份报告并准备替换。刷新前仅存在 v1，重开后保持原 Run、审批 ID 和计划。批准后原 Run 成功结束，模型轮次为零，原 PPTX 保持完整。v2 的 ZIP 内容只有 `ppt/slides/slide2.xml` 发生变化，两页的稳定 ID 保持一致。持久记录确认第 1 页继续引用原报告，第 2 页引用新报告。下载的 `RevisedReport.pptx` 与暂存 v2 字节一致，PowerPoint 打开后核对两页摘要。

Windows 的导航、独立审阅和 PPTX 局部返工三项回归通过，WSL 全部二十七项 CTest 通过。

### Web 工作区与来源返工

使用 `huxerui package web --profile debug` 生成 SDK 消费包，在 Chromium 中通过实际 Preview 界面打开目录。测试的目录选择器返回真实的浏览器源私有文件系统句柄，记录了触发选择器的用户手势，并注入拒绝、过期和取消状态。两处名为 `Same` 的目录持久化为不同的 `WorkspaceId`，重复打开同一目录时沿用原 ID。选择文件夹和浏览子目录后，仅显式选择的有效 JSON 报告进入项目共享上下文。无摘要的报告和读权限失效均显示对应错误，且未写入项目。

另在 Chromium 用独立 IndexedDB 验证版本 1 升级到版本 2 后保留会话记录、提交记录、导航和模型偏好。两个存储连接并发登记同一目录只获得一个工作区 ID。授权内的文件读取成功，`../` 路径被拒绝。

使用第一处目录的报告生成并下载 `Presentation.pptx`。刷新后打开第二处同名目录，再回到原 PPT 会话。返工入口恢复了会话绑定的第一处目录，在用户重新授权后从原目录选择 `updated.json`，并为第 1 页准备替换审批。再次刷新后审批仍可处理。批准后生成和下载 `Presentation-v2.pptx`。两次下载分别与对应的暂存和导出文件逐字节一致。ZIP 内仅 `ppt/slides/slide1.xml` 改变，PowerPoint 实际打开两个文件并核对两页及指定摘要。项目记录中的更新报告关联第一处目录的工作区 ID。

Windows 的导航、模型与权限、worktree、PPTX 局部返工和 PPT 采纳恢复五项 CTest 通过，WSL 全部二十七项通过。浏览器验收使用了测试目录选择器，系统目录对话框和真实权限提示仍待实测。

### Web 模型配置与流式传输

Web Preview 从同源的 `agentkit-models.json` 读取公开配置，界面显示首选模型及其他适用配置。用 Chromium 加载 Web Debug SDK 消费包。同源 Chat Completions 校验宿主会话 Cookie，服务端将 SSE 事件分段写回，Preview 显示完整回复。跨源 Responses 通过浏览器 CORS 预检，返回分片的 Skill 激活参数。刷新后原会话仍显示待审批的 Skill 和所选模型。点击批准后，服务收到原 `call_id` 与 `activated: true`，Preview 进入演示简报。网络记录只有一次激活调用。

从相同 SDK 消费包在 Chromium 新建通用会话，经同源测试代理向真实 OpenAI 兼容端点的 `gpt-4.1-mini-2025-04-14` 发送单条测试消息。代理核对浏览器请求含宿主会话 Cookie，提供商凭据由代理加入，收到 HTTP 200 和 `text/event-stream` 后把响应流传回。Preview 收到并显示预期的 `WEB-RELAY-OK`。此用例验证真实 Chat Completions，其他协议的浏览器联调覆盖以上回环用例。

在修复相同上传边界的诊断构建中另验收 CORS 拒绝、用户停止及配置错误。CORS 拒绝只发送预检，Run 显示连接错误。停止活动 SSE 时服务记录请求关闭。无效配置显示错误且会话仍可打开，旧模型不可用时保留草稿，配置文件返回 404 时新建会话显示本地演示模型。

分别用已安装的 HuxerUI 0.4.0 和重建后的隔离 SDK 执行 `huxerui package web --config debug --sdk <SDK>`，在 Chromium 从实际 Preview 发送 Chat Completions 和 Responses。已安装 SDK 的两条 POST 均到达同源测试服务，Chromium 捕获上传回调将 JavaScript `Number` 传给 Wasm `i64` 时的 `TypeError`，IndexedDB 中两个 Run 均为 `Failed`。在 HuxerUI 提交 `5456e3e8` 的隔离源码中将上传长度参数转换为 `BigInt`，用 Emscripten 4.0.19 完整重建 Web 目标库及 Testing 库，并按 SDK 目录结构安装。两个修复版用例均在结束事件放行前收到首段 SSE 数据，IndexedDB 中 Run 为 `Succeeded`，最终回复与完整结构化响应一致，各发送一次 POST。正式 Web SDK 仍需合入该修复后复验发行包。 [Emscripten 的 JS BigInt 说明](https://emscripten.org/docs/compiling/WebAssembly.html#webassembly-feature-extensions)

Windows 模型协议、模型与权限、报告图表三个 CTest 通过。完整 Preview 并行用例首次超时，随后对同一二进制指定独立证据目录串行执行，通过代码补丁、PPTX 导出和发布链检查。WSL 全部二十七项测试通过。

模型增量区分对话正文和供宿主解析的结构化载荷后，Chromium 复验了正常完成、取消和连接中途结束。刷新后正常回复仅显示一次，IndexedDB 中保留最终回复与失败状态。旧记录仍可打开。Windows 模型协议覆盖五种适配协议，输入专项验证可见增量仍会显示且中文组字不会丢失。420 像素视口验证了模型、权限和 Skill 控件换行，改选的模型和权限写入会话偏好。此轮验证范围是交互和持久化，视觉稿对照留待 Preview 界面验收。

PPT 失败路径纳入 CTest 后，Windows 串行通过生成与导出失败、局部返工和 PPTX 归档三项用例。WSL 的二十八项完整回归通过，包括最新的模型与权限偏好重启恢复。
