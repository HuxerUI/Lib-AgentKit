#pragma once

#include <app_resources.h>
#include <huxerui/huxerui.h>

namespace agentkit_preview {

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
      .background = colors.surface,
      .disabled_background = colors.surface_container_low,
      .border = colors.outline,
      .hovered_border = colors.on_surface_variant,
      .focused_border = colors.primary,
      .disabled_border = colors.outline,
      .corner_radii = CornerRadii{7.0F},
      .minimum_height = 38.0F,
  };
  field.filled = field.outlined;
  field.filled.background = colors.surface_container_low;
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
  field.selection = colors.primary_container;
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
        Background(colors.surface_container_low), CornerRadius(12.0F), Border{colors.outline}, ClipChildren{},
        Shadow{.color = Color::Rgb(0, 0, 0, 0.36F), .offset = {0.0F, 12.0F}, .blur_radius = 36.0F})
        .Key("preview-dialog-surface");
  }}, std::move(options));
}

inline huxerui::ThemeSpec PreviewTheme(huxerui::ThemeBrightness brightness) {
  using huxerui::Color;
  const bool dark = brightness == huxerui::ThemeBrightness::Dark;
  auto theme = dark ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
  theme.typography.body_large = 14.0F;
  theme.typography.body_small = 13.0F;
  auto& colors = theme.colors;
  colors.surface = dark ? Color::Rgb(20, 21, 24) : Color::Rgb(249, 247, 244);
  colors.background = colors.surface;
  colors.surface_container_low = dark ? Color::Rgb(25, 27, 31) : Color::Rgb(242, 239, 235);
  colors.surface_container = dark ? Color::Rgb(30, 32, 37) : Color::Rgb(255, 253, 249);
  colors.surface_container_high = dark ? Color::Rgb(37, 39, 45) : Color::Rgb(235, 231, 225);
  colors.surface_container_highest = dark ? Color::Rgb(46, 49, 56) : Color::Rgb(227, 222, 214);
  colors.on_surface = dark ? Color::Rgb(239, 240, 243) : Color::Rgb(40, 37, 32);
  colors.on_surface_variant = dark ? Color::Rgb(171, 174, 184) : Color::Rgb(88, 80, 71);
  colors.outline = dark ? Color::Rgb(52, 55, 63) : Color::Rgb(217, 211, 201);
  colors.primary = dark ? Color::Rgb(224, 183, 138) : Color::Rgb(128, 84, 47);
  colors.on_primary = dark ? Color::Rgb(42, 32, 22) : Color::White();
  colors.primary_container = dark ? Color::Rgb(49, 42, 38) : Color::Rgb(234, 220, 201);
  colors.on_primary_container = dark ? Color::Rgb(246, 212, 174) : Color::Rgb(98, 59, 28);
  colors.error = dark ? Color::Rgb(255, 180, 189) : Color::Rgb(150, 50, 64);
  theme.interactions.focus_ring = huxerui::FocusRing{colors.primary, 1.0F, 2.0F};
  return theme;
}

inline huxerui::View PreviewControlsTheme(huxerui::View content) {
  using namespace huxerui;
  return Scope([content = std::move(content)] {
    const auto& theme = UseTheme();
    const auto& colors = theme.colors;
    ThemeDefinition overrides;
    overrides.Set(PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), colors));
    auto select = UseEnvironment<SelectStyle>();
    select.background = colors.surface;
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
    overrides.Set(std::move(select));
    auto dialog = UseEnvironment<DialogStyle>();
    dialog.scrim = Color::Rgb(9, 11, 15, theme.brightness == ThemeBrightness::Dark ? 0.56F : 0.40F);
    dialog.background = colors.surface_container_low;
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
