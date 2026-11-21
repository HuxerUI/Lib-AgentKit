#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

class CodeWorkspaces;
class RunSettings;

inline const huxerui::agentkit::AgentId GeneralAgent{"preview.general"};
inline const huxerui::agentkit::AgentId PptAgent{"preview.ppt"};
inline const huxerui::agentkit::ToolId ConfirmPreview{"preview.confirm"};
inline const huxerui::agentkit::ToolId ReadReport{"preview.report.read"};
inline const huxerui::agentkit::ToolId PrepareReleaseOutline{"preview.report.outline"};
inline constexpr std::string_view GeneralAnswerInstruction = "Answer the latest user message directly in plain text or Markdown.";

huxerui::agentkit::AgentResult<void> RegisterGeneralTools(const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime);
huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunGeneral(
    huxerui::agentkit::RunContext run, huxerui::agentkit::RunInput input,
    std::optional<huxerui::agentkit::RunCheckpoint> checkpoint, bool release_available, std::string export_directory,
    std::shared_ptr<CodeWorkspaces> workspaces, std::shared_ptr<RunSettings> settings, bool tests_configured,
    std::weak_ptr<huxerui::agentkit::AgentRuntime> runtime);
huxerui::View GeneralChat(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::AgentChatOptions options = {});

} // namespace agentkit_preview
