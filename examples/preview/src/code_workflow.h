#pragma once

#include "code_workspace.h"

#include <huxerui/agentkit.h>

namespace agentkit_preview {

class RunSettings;

huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunCode(
    huxerui::agentkit::RunContext context, huxerui::agentkit::RunInput input,
    std::optional<huxerui::agentkit::RunCheckpoint> checkpoint, bool tests_configured,
    std::weak_ptr<huxerui::agentkit::AgentRuntime> runtime, std::shared_ptr<CodeWorkspaces> workspaces,
    std::shared_ptr<RunSettings> settings);

huxerui::View CodeChat(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    huxerui::agentkit::AgentChatOptions options = {}, std::optional<CodeWorkspace> workspace = {},
    std::optional<huxerui::File> evidence = {});

huxerui::View AppliedFileDiff(huxerui::agentkit::ArtifactRef artifact, huxerui::agentkit::PartId file, std::string diff);

std::optional<huxerui::View> RenderWorkspaceChangeReview(const huxerui::agentkit::ArtifactPreview& preview,
    std::shared_ptr<huxerui::agentkit::Conversation> conversation, std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    std::optional<CodeWorkspace> workspace, std::optional<huxerui::File> evidence);

huxerui::View WorkspaceChangeActivity(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::ToolCallSnapshot call,
    std::optional<CodeWorkspace> workspace, huxerui::File evidence);

std::string CodeReviewAttachmentLabel(
    const huxerui::agentkit::ApprovalPreview& preview, const huxerui::agentkit::ArtifactRef& artifact);

huxerui::View CodeRevertReview(std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    std::shared_ptr<huxerui::agentkit::Conversation> conversation, huxerui::agentkit::ToolCallSnapshot original,
    std::optional<CodeWorkspace> workspace, std::optional<huxerui::File> evidence);

} // namespace agentkit_preview
