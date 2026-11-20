#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

huxerui::View SessionWorkspace(std::shared_ptr<huxerui::agentkit::Conversation> conversation, huxerui::View chat,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, std::optional<huxerui::agentkit::RunState> run,
    bool show_goal, std::function<void()> close_goal,
    std::function<void(huxerui::agentkit::PendingApproval)> open_completion);

} // namespace agentkit_preview
