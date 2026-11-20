#pragma once

#include <huxerui/modifier.h>

#include <optional>

namespace huxerui::agentkit::detail {

struct AgentChatLayoutStyle {
  float content_width = 672.0F;
  std::optional<float> heading_size;
  bool chat_bubbles = false;

  static constexpr AgentChatLayoutStyle Default() noexcept {
    return {};
  }

  bool operator==(const AgentChatLayoutStyle&) const = default;
};

struct ChatComposerAnchor {
  std::optional<ViewHandle> view;

  static ChatComposerAnchor Default() {
    return {};
  }

  bool operator==(const ChatComposerAnchor&) const = default;
};

} // namespace huxerui::agentkit::detail
