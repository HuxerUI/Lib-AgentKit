#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

struct PreviewSideChats {
  struct Draft;
  std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime;
  huxerui::agentkit::AgentId assistant;
  std::vector<std::shared_ptr<Draft>> drafts;
  std::function<huxerui::agentkit::AgentChatRunControls(huxerui::State<huxerui::agentkit::UserRunOptions>,
      huxerui::State<huxerui::agentkit::AgentChatDraftSources>)> composer_controls;
};

huxerui::View PreviewSideChatWorkspace(huxerui::View content, std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    huxerui::agentkit::AgentChatTextSelection::Binding selection, huxerui::State<huxerui::agentkit::AgentChatDraftSources> draft,
    huxerui::ViewHandle composer);

huxerui::View PreviewSourceActions(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::optional<huxerui::agentkit::SourceSlice> source, huxerui::State<huxerui::agentkit::AgentChatDraftSources> draft,
    std::function<void()> on_citation_added = {});

} // namespace agentkit_preview
