#pragma once

#include <app_resources.h>
#include <huxerui/huxerui.h>

#include <array>
#include <cmath>

namespace agentkit_preview {

enum class PreviewPalette { Graphite, Indigo, Forest, Sandstone };

inline huxerui::Color PreviewTint(huxerui::Color surface, huxerui::Color ink, float weight) {
  return {std::lerp(surface.red, ink.red, weight), std::lerp(surface.green, ink.green, weight),
      std::lerp(surface.blue, ink.blue, weight), 1.0F};
}

inline huxerui::Color PreviewSuccess(huxerui::ThemeBrightness brightness) {
  return brightness == huxerui::ThemeBrightness::Dark ? huxerui::Color::Rgb(158, 217, 179) : huxerui::Color::Rgb(34, 102, 62);
}

inline huxerui::Color PreviewWarning(huxerui::ThemeBrightness brightness) {
  return brightness == huxerui::ThemeBrightness::Dark ? huxerui::Color::Rgb(239, 188, 117) : huxerui::Color::Rgb(146, 84, 25);
}

inline huxerui::ButtonStyle PreviewSecondaryButton(huxerui::ButtonStyle style, const huxerui::ColorScheme& colors) {
  style.background = colors.surface_container_high;
  style.label_style = {huxerui::Font::System(13.0F).WithWeight(huxerui::FontWeight::Medium), colors.on_surface};
  style.disabled_background = colors.surface_container_low;
  style.disabled_label = colors.on_surface_variant;
  style.minimum_height = 34.0F;
  style.padding = huxerui::EdgeInsets::Symmetric(13.0F, 5.0F);
  style.corner_radii = huxerui::CornerRadii{7.0F};
  return style;
}

inline huxerui::ButtonStyle PreviewPrimaryButton(huxerui::ButtonStyle style, const huxerui::ColorScheme& colors) {
  style = PreviewSecondaryButton(std::move(style), colors);
  style.background = colors.primary;
  style.label_style.font = style.label_style.font.WithWeight(huxerui::FontWeight::SemiBold);
  style.label_style.foreground = colors.on_primary;
  style.disabled_background = colors.surface_container_high;
  style.disabled_label = colors.on_surface_variant;
  return style;
}

inline huxerui::IconButtonStyle PreviewChromeIconStyle(const huxerui::ThemeSpec& theme) {
  using namespace huxerui;
  auto style = IconButtonStyle::Default();
  style.foreground = theme.colors.on_surface_variant;
  style.icon_size = 18.0F;
  style.minimum_interactive_size = 34.0F;
  style.state_layer_size = 32.0F;
  style.corner_radius = 7.0F;
  style.indication = theme.interactions.indication;
  style.indication->hover.reset();
  style.indication->press.reset();
  style.indication->ripple.reset();
  return style;
}

inline huxerui::View PreviewChromeAction(huxerui::ImageResource image, huxerui::StringVariant label,
    std::function<void()> action, const huxerui::ThemeSpec& theme) {
  using namespace huxerui;
  return ProvideEnvironment(PreviewChromeIconStyle(theme), IconButton(image, label)
      .OnClick(std::move(action)).With(PointerCursor(PointerCursorKind::Hand), Tooltip(label)));
}

inline huxerui::View PreviewBackAction(huxerui::StringVariant label, std::function<void()> back,
                                      const huxerui::ThemeSpec& theme) {
  return PreviewChromeAction(::app::images::arrow_left, std::move(label), std::move(back), theme);
}

inline huxerui::View PreviewPaneHeader(huxerui::ImageResource image, huxerui::StringVariant title,
    huxerui::StringVariant close_label, const char* close_key, std::function<void()> close, const huxerui::ThemeSpec& theme) {
  using namespace huxerui;
  return Row{
      Image(image).Tint(theme.colors.on_surface_variant).With(Frame{.width = 16.0F, .height = 16.0F}),
      Text(title, TextRole::Title)
          .Style({Font::System(15.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface}).With(Grow()),
      PreviewChromeAction(::app::images::close, std::move(close_label), std::move(close), theme).Key(close_key),
  }.With(Frame{.min_height = 46.0F}, Padding(EdgeInsets{.right = 8.0F, .left = 14.0F}),
      Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

inline huxerui::Indication PreviewRowIndication(const huxerui::ThemeSpec& theme, float radius) {
  using namespace huxerui;
  auto indication = theme.interactions.indication;
  const auto corners = CornerRadii{radius};
  auto hover = theme.colors.on_surface;
  hover.alpha = 0.07F;
  auto press = hover;
  press.alpha = 0.13F;
  indication.geometry = {.clip_corner_radii = corners};
  indication.hover = IndicationLayer{.fill = hover,
      .corner_radii = corners, .placement = IndicationPlacement::BehindContent};
  indication.press = IndicationLayer{.fill = press,
      .corner_radii = corners, .placement = IndicationPlacement::BehindContent};
  if (indication.focus) indication.focus->corner_radii = corners;
  indication.ripple.reset();
  return indication;
}

inline huxerui::TextFieldStyle PreviewFormFieldStyle(huxerui::TextFieldStyle field, const huxerui::ColorScheme& colors) {
  using namespace huxerui;
  field.outlined = {
      .background = colors.surface_container,
      .disabled_background = colors.surface_container_low,
      .border = colors.outline,
      .hovered_border = colors.on_surface_variant,
      .focused_border = colors.primary,
      .disabled_border = colors.outline,
      .corner_radii = CornerRadii{7.0F},
      .minimum_height = 38.0F,
  };
  field.filled = field.outlined;
  field.filled.background = colors.surface_container_high;
  field.filled.corner_radii = CornerRadii::Top(7.0F);
  field.text_style = {field.text_style.font.WithSize(14.0F), colors.on_surface};
  field.placeholder_style.font = field.text_style.font;
  field.padding = EdgeInsets::Symmetric(11.0F, 7.0F);
  field.focused_border_width = 1.0F;
  field.focused_validation_border_width = 1.0F;
  field.caret_width = 1.0F;
  field.validation_error = colors.error;
  field.validation_text_style.foreground = colors.error;
  field.label_style.foreground = colors.on_surface_variant;
  field.floating_label_style.foreground = colors.on_surface_variant;
  field.placeholder_style.foreground = colors.on_surface_variant;
  field.focused_label = colors.on_primary_container;
  field.disabled_text = colors.on_surface_variant;
  field.disabled_label = colors.on_surface_variant;
  field.disabled_placeholder = colors.on_surface_variant;
  field.error_label = colors.error;
  field.caret = colors.on_primary_container;
  field.selection = PreviewTint(colors.surface, colors.primary, 0.25F);
  field.error_caret = colors.error;
  field.composition = colors.on_primary_container;
  return field;
}

inline huxerui::View PreviewLabeledField(huxerui::StringVariant label, huxerui::View field, const huxerui::ColorScheme& colors) {
  using namespace huxerui;
  return Column {
    Text(std::move(label), TextRole::Label)
        .Style({Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface_variant})
        .With(Semantics{.hidden = true}),
    std::move(field),
  }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

inline huxerui::View PreviewFormDialog(huxerui::StringVariant title, huxerui::View body, huxerui::View actions,
                                      const huxerui::ColorScheme& colors, float width) {
  using namespace huxerui;
  return Column {
    Text(std::move(title), TextRole::Title).Style({Font::System(20.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
        .With(Padding(EdgeInsets::Symmetric(20.0F, 15.0F))),
    ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
    std::move(body),
    ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
    Column {std::move(actions)}.With(Padding(EdgeInsets::Symmetric(20.0F, 12.0F)),
        CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container)),
  }.With(Frame{.width = width}, CrossAlign(CrossAxisAlignment::Stretch));
}

inline void ShowPreviewDialog(const huxerui::DialogHandle& dialogs, huxerui::DialogFactory content,
                              huxerui::DialogOptions options = {}) {
  using namespace huxerui;
  dialogs.Show(DialogFactory{[content = std::move(content)](DialogContext dialog) {
    const auto colors = UseTheme().colors;
    return Column {content(dialog)}.With(CrossAlign(CrossAxisAlignment::Stretch),
        Background(colors.surface_container), CornerRadius(12.0F), Border{colors.outline}, ClipChildren{},
        Shadow{.color = Color::Rgb(0, 0, 0, 0.36F), .offset = {0.0F, 12.0F}, .blur_radius = 36.0F})
        .Key("preview-dialog-surface");
  }}, std::move(options));
}

inline huxerui::ThemeSpec PreviewTheme(PreviewPalette palette, huxerui::ThemeBrightness brightness) {
  using namespace huxerui;
  struct Tones {
    std::uint32_t surface;
    std::uint32_t shell;
    std::uint32_t raised;
    std::uint32_t inset;
    std::uint32_t outline;
    std::uint32_t ink;
    std::uint32_t muted;
    std::uint32_t accent;
  };
  const auto color = [](std::uint32_t rgb) { return Color::Rgba32((rgb << 8U) | 0xffU); };
  constexpr std::array dark_palettes{
      Tones{0x181a1f, 0x111318, 0x202329, 0x292d35, 0x363b45, 0xebeef4, 0xaab3c2, 0xa4bedf},
      Tones{0x1b1c25, 0x14151d, 0x232534, 0x2c2e40, 0x393c50, 0xeeeef6, 0xb0b2c6, 0xbcbff1},
      Tones{0x1a201e, 0x131916, 0x222b27, 0x2b362f, 0x36443c, 0xe9efeb, 0xaebdb3, 0xa5cdb7},
      Tones{0x211d1b, 0x181512, 0x2b2522, 0x362e29, 0x483c35, 0xf1e9e2, 0xc3b3a9, 0xe2b694},
  };
  constexpr std::array light_palettes{
      Tones{0xf9fafc, 0xeff2f6, 0xffffff, 0xeef2f7, 0xd9e0e8, 0x232a35, 0x526070, 0x365574},
      Tones{0xf9f8fc, 0xf0eef6, 0xffffff, 0xeeedf7, 0xdedcea, 0x292b3d, 0x585b71, 0x504296},
      Tones{0xf9faf8, 0xeff3ef, 0xffffff, 0xebf1ec, 0xd9e2dc, 0x26342d, 0x506257, 0x33624d},
      Tones{0xfbf9f5, 0xf3eee7, 0xfffefd, 0xf2ece3, 0xe4dbd1, 0x362b23, 0x68594d, 0x804627},
  };
  const auto index = static_cast<std::size_t>(palette);
  const bool dark = brightness == ThemeBrightness::Dark;
  const auto tones = (dark ? dark_palettes : light_palettes).at(index);
  auto theme = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  theme.typography.body_large = 14.0F;
  theme.typography.body_small = 13.0F;
  auto& colors = theme.colors;
  colors.surface = color(tones.surface);
  colors.background = colors.surface;
  colors.on_surface = color(tones.ink);
  colors.on_surface_variant = color(tones.muted);
  colors.primary = color(tones.accent);
  colors.on_primary = dark ? colors.surface : Color::White();
  colors.surface_container_low = color(tones.shell);
  colors.surface_container = color(tones.raised);
  colors.surface_container_high = color(tones.inset);
  colors.surface_container_highest = PreviewTint(colors.surface_container_high, colors.on_surface, dark ? 0.09F : 0.07F);
  colors.outline = color(tones.outline);
  colors.primary_container = PreviewTint(colors.surface_container, colors.primary, dark ? 0.12F : 0.10F);
  colors.on_primary_container = dark ? PreviewTint(colors.primary, colors.on_surface, 0.28F) : colors.primary;
  colors.secondary = colors.on_surface_variant;
  colors.on_secondary = colors.on_primary;
  colors.secondary_container = colors.surface_container_highest;
  colors.on_secondary_container = colors.on_surface;
  colors.tertiary_container = PreviewTint(colors.surface, PreviewWarning(brightness), dark ? 0.21F : 0.12F);
  colors.on_tertiary_container = PreviewWarning(brightness);
  colors.inverse_surface = color((dark ? light_palettes : dark_palettes).at(index).surface);
  colors.inverse_on_surface = color((dark ? light_palettes : dark_palettes).at(index).ink);
  colors.scrim = Color::Rgb(0, 0, 0, dark ? 0.58F : 0.37F);
  colors.error = dark ? color(0xffb7c0) : color(0xa32840);
  theme.interactions.focus_ring = FocusRing{colors.primary, 1.0F, 2.0F};
  theme.interactions.indication = PreviewRowIndication(theme, 7.0F);
  theme.interactions.indication.focus = IndicationLayer{.border = Border{colors.primary, 1.0F}, .corner_radii = CornerRadii{7.0F}};
  return theme;
}

inline huxerui::View PreviewControlsTheme(huxerui::View content) {
  using namespace huxerui;
  return Scope([content = std::move(content)] {
    const auto& theme = UseTheme();
    const auto& colors = theme.colors;
    ThemeDefinition overrides;
    auto button = PreviewSecondaryButton(UseEnvironment<ButtonStyle>(), colors);
    button.indication = PreviewRowIndication(theme, 7.0F);
    overrides.Set(std::move(button));
    overrides.Set(PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), colors));
    overrides.Set(DividerStyle{colors.outline});
    auto menu = UseEnvironment<MenuStyle>();
    menu.background = colors.surface_container;
    menu.foreground = colors.on_surface;
    menu.icon_tint = colors.on_surface_variant;
    menu.separator_color = colors.outline;
    menu.item_indication = PreviewRowIndication(theme, 6.0F);
    overrides.Set(std::move(menu));
    auto tooltip = UseEnvironment<TooltipStyle>();
    tooltip.background = colors.inverse_surface;
    tooltip.text_style.foreground = colors.inverse_on_surface;
    overrides.Set(std::move(tooltip));
    auto scroll = UseEnvironment<ScrollBarStyle>();
    scroll.thumb_color = PreviewTint(colors.surface, colors.on_surface, 0.34F);
    overrides.Set(std::move(scroll));
    auto select = UseEnvironment<SelectStyle>();
    select.background = colors.surface_container;
    select.foreground = colors.on_surface;
    select.border = Border{colors.outline, 1.0F};
    select.indicator = colors.on_surface_variant;
    select.trigger_padding = EdgeInsets::Symmetric(11.0F, 7.0F);
    select.minimum_height = 38.0F;
    select.minimum_width = 72.0F;
    select.corner_radii = CornerRadii{7.0F};
    select.popup_background = colors.surface_container;
    select.popup_corner_radii = CornerRadii{8.0F};
    select.active_item_background = colors.surface_container_high;
    select.selected_item_background = colors.primary_container;
    select.indication = PreviewRowIndication(theme, 7.0F);
    select.item_indication = PreviewRowIndication(theme, 6.0F);
    select.validation_error = colors.error;
    select.validation_text_style.foreground = colors.error;
    overrides.Set(std::move(select));
    auto dialog = UseEnvironment<DialogStyle>();
    dialog.scrim = colors.scrim;
    dialog.background = colors.surface_container;
    dialog.title_style = {Font::System(20.0F).WithWeight(FontWeight::SemiBold), colors.on_surface};
    dialog.message_style = {Font::System(14.0F), colors.on_surface_variant};
    dialog.positive_action_style.foreground = colors.on_primary;
    dialog.negative_action_style.foreground = colors.on_surface;
    dialog.positive_action_background = colors.primary;
    dialog.action_separator_color = colors.outline;
    dialog.action_separator_thickness = 1.0F;
    dialog.minimum_action_height = 34.0F;
    dialog.viewport_margin = 16.0F;
    overrides.Set(std::move(dialog));
    return Theme(std::move(overrides), content);
  });
}

} // namespace agentkit_preview
