#pragma once

#include <huxerui/huxerui.h>

#include <optional>

namespace huxerui::agentkit::detail {

enum class ChatControlIcon { Chevron, Shield, Send };

inline View ChatControlGlyph(ChatControlIcon icon, Color color) {
  return Canvas([icon, color](PaintContext& context, Size) {
    const StrokeStyle stroke{.width = 1.35F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
    const auto line = [&](float x1, float y1, float x2, float y2) {
      context.DrawLine({x1, y1}, {x2, y2}, color, stroke);
    };
    switch (icon) {
      case ChatControlIcon::Chevron:
        line(4, 6, 7, 9);
        line(7, 9, 10, 6);
        break;
      case ChatControlIcon::Shield:
        line(7, 1.5F, 11.5F, 3.5F);
        line(11.5F, 3.5F, 11.5F, 8);
        line(11.5F, 8, 10, 10.5F);
        line(10, 10.5F, 7, 12.5F);
        line(7, 12.5F, 4, 10.5F);
        line(4, 10.5F, 2.5F, 8);
        line(2.5F, 8, 2.5F, 3.5F);
        line(2.5F, 3.5F, 7, 1.5F);
        line(5.2F, 7, 6.5F, 8.3F);
        line(6.5F, 8.3F, 9.2F, 5.6F);
        break;
      case ChatControlIcon::Send:
        line(7, 11, 7, 3);
        line(3.5F, 6.5F, 7, 3);
        line(7, 3, 10.5F, 6.5F);
        break;
    }
  }).With(Frame{.width = 14.0F, .height = 14.0F});
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

inline const VectorAsset& ChatSendIcon() {
  static const auto icon = VectorAsset::Create(Size{20.0F, 20.0F}, [](VectorBuilder& builder) {
    Path arrow;
    arrow.MoveTo({5.5F, 9.5F}).LineTo({10.0F, 5.0F}).LineTo({14.5F, 9.5F});
    arrow.MoveTo({10.0F, 5.0F}).LineTo({10.0F, 15.0F});
    builder.StrokePath(std::move(arrow), Color::White(),
        StrokeStyle{.width = 1.8F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
  });
  return icon;
}

inline const VectorAsset& ChatSendModeIcon(bool queued) {
  static const auto steer = VectorAsset::Create(Size{16.0F, 16.0F}, [](VectorBuilder& builder) {
    Path bolt;
    bolt.MoveTo({9, 2}).LineTo({4, 9}).LineTo({8, 9}).LineTo({7, 14}).LineTo({12, 7}).LineTo({8, 7}).Close();
    builder.StrokePath(std::move(bolt), Color::White(), StrokeStyle{.width = 1.4F, .join = StrokeJoin::Round});
  });
  static const auto queue = VectorAsset::Create(Size{16.0F, 16.0F}, [](VectorBuilder& builder) {
    Path list;
    list.MoveTo({3, 4}).LineTo({13, 4}).MoveTo({3, 8}).LineTo({13, 8}).MoveTo({3, 12}).LineTo({10, 12});
    builder.StrokePath(std::move(list), Color::White(), StrokeStyle{.width = 1.5F, .cap = StrokeCap::Round});
  });
  return queued ? queue : steer;
}

inline View ChatSendAction(std::string label, std::function<void()> send, bool enabled, const ColorScheme& colors,
                           IconButtonStyle style) {
  style.icon_size = 20.0F;
  style.minimum_interactive_size = 38.0F;
  style.state_layer_size = 38.0F;
  style.corner_radius = 11.0F;
  style.foreground = colors.on_primary;
  style.disabled_foreground = colors.on_surface_variant;
  const auto key = "chat-send/" + label;
  return ProvideEnvironment(style, IconButton(ChatSendIcon(), label).OnClick(std::move(send))
      .With(Enabled(enabled), Background(enabled ? colors.primary : colors.surface_container_high),
          CornerRadius(11.0F), Tooltip(label)).Key(key));
}

} // namespace huxerui::agentkit::detail
