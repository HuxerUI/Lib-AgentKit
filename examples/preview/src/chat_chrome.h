#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/theme.h>

namespace agentkit_preview {

using ChatHeader = std::function<std::optional<huxerui::View>(
    const huxerui::agentkit::AgentChatPage&, std::optional<huxerui::View>)>;

struct ChatChrome {
  std::shared_ptr<const ChatHeader> header;

  static ChatChrome Default() { return {}; }

  bool operator==(const ChatChrome&) const = default;
};

huxerui::agentkit::AgentChatReviewHeader PreviewArtifactHeader(const huxerui::ThemeSpec& theme,
    std::function<void(huxerui::agentkit::AgentChatReviewRequest)> open_window = {},
    std::optional<huxerui::State<std::string>> error = {});

huxerui::View PreviewDraftComposer(huxerui::State<huxerui::TextEditingValue> draft, huxerui::ViewHandle focus,
    huxerui::State<bool> focused, huxerui::StringVariant placeholder, const char* input_key,
    std::vector<huxerui::View> extras, std::optional<huxerui::View> controls, bool editing,
    std::function<bool()> send_available, std::function<void()> submit, bool dense = false);

huxerui::View PreviewChat(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    huxerui::agentkit::AgentChatOptions options, std::optional<huxerui::View> actions = {});

} // namespace agentkit_preview
