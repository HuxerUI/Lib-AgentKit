#pragma once

#include "code_workspace.h"
#include "preview_store.h"
#include <huxerui/agentkit.h>

namespace agentkit_preview {

class RunSettings;

inline const huxerui::agentkit::ToolId CollaborationActivity{"ganwork.collaboration.activity"};

huxerui::agentkit::AgentResult<void> RegisterCollaborationActivity(huxerui::agentkit::AgentRuntime& runtime);

huxerui::agentkit::AgentRegistration WithConversationCollaboration(
    std::weak_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::AgentRegistration agent,
    std::function<NavigationPreferences()> navigation, std::shared_ptr<CodeWorkspaces> workspaces,
    std::shared_ptr<RunSettings> settings, bool tests_configured);

std::string WorkspaceDisplayName(const CodeWorkspace& workspace);

} // namespace agentkit_preview
