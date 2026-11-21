#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

using ChatHeader = std::function<std::optional<huxerui::View>(
    const huxerui::agentkit::AgentChatPage&, std::optional<huxerui::View>)>;

struct ChatChrome {
  std::shared_ptr<const ChatHeader> header;

  static ChatChrome Default() { return {}; }

  bool operator==(const ChatChrome&) const = default;
};

huxerui::View PreviewChat(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    huxerui::agentkit::AgentChatOptions options, std::optional<huxerui::View> actions = {});

} // namespace agentkit_preview
