#pragma once

#include <huxerui/agentkit/runtime.h>
#include "code_workspace.h"
#include "presentation_tools.h"

namespace agentkit_preview {

inline constexpr std::string_view WorkspaceRelease = "workspace-release";

huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunWorkspaceRelease(
    huxerui::agentkit::RunContext run, huxerui::agentkit::GoalRef goal, huxerui::agentkit::GoalDefinition definition,
    std::optional<huxerui::agentkit::RunCheckpoint> checkpoint);

bool IsWorkspaceRelease(const huxerui::agentkit::GoalDefinition& definition);
std::optional<std::size_t> ReleaseSlideCount(const huxerui::agentkit::JsonValue& conditions);
huxerui::agentkit::Async<huxerui::agentkit::VerificationResult> VerifyWorkspaceRelease(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, std::shared_ptr<CodeWorkspaces> workspaces,
    PresentationFiles files, huxerui::agentkit::VerificationRequest request);

} // namespace agentkit_preview
