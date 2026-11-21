#pragma once

#include <huxerui/huxerui.h>
#include <huxerui_agentkit_resources.h>

#include <optional>

namespace huxerui::agentkit::detail {

enum class ChatControlIcon { Chevron, Shield, Send };

inline View ChatControlGlyph(ChatControlIcon icon, Color color) {
  const auto& image = icon == ChatControlIcon::Chevron ? huxerui_agentkit::images::chevron_down :
      icon == ChatControlIcon::Shield ? huxerui_agentkit::images::shield : huxerui_agentkit::images::send;
  return Image(image).Tint(color).With(Frame{.width = 14.0F, .height = 14.0F});
}

inline View ChatControlWithIcons(View button, ButtonStyle style, Color ink, ChatControlIcon trailing,
                                std::optional<ChatControlIcon> leading = {}) {
  const auto inset = style.padding;
  style.padding.right += 18.0F;
  if (leading) style.padding.left += 19.0F;
  View control = ProvideEnvironment(style, std::move(button));
  if (leading) {
    control = Stack{std::move(control), ChatControlGlyph(*leading, ink).With(Padding(EdgeInsets{.left = inset.left}))}
        .With(Align(HorizontalAlignment::Start, VerticalAlignment::Center));
  }
  return Stack{std::move(control), ChatControlGlyph(trailing, ink).With(Padding(EdgeInsets{.right = inset.right}))}
      .With(Align(HorizontalAlignment::End, VerticalAlignment::Center));
}

inline View ChatSendAction(StringVariant label, std::string key, std::function<void()> send, bool enabled, const ColorScheme& colors,
                           IconButtonStyle style) {
  style.icon_size = 20.0F;
  style.minimum_interactive_size = 38.0F;
  style.state_layer_size = 38.0F;
  style.corner_radius = 11.0F;
  style.foreground = colors.on_primary;
  style.disabled_foreground = colors.on_surface_variant;
  return ProvideEnvironment(style, IconButton(huxerui_agentkit::images::send, label).OnClick(std::move(send))
      .With(Enabled(enabled), Background(enabled ? colors.primary : colors.surface_container_high),
          CornerRadius(11.0F), Tooltip(label)).Key(std::move(key)));
}

} // namespace huxerui::agentkit::detail
