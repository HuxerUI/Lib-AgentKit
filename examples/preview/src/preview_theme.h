#pragma once

#include <huxerui/theme.h>

namespace agentkit_preview {

inline huxerui::ButtonStyle PreviewSecondaryButton(huxerui::ButtonStyle style, const huxerui::ColorScheme& colors) {
  style.background = colors.surface_container_high;
  style.label_style = {huxerui::Font::System(13.0F).WithWeight(huxerui::FontWeight::Medium), colors.on_surface};
  style.minimum_height = 34.0F;
  style.padding = huxerui::EdgeInsets::Symmetric(13.0F, 5.0F);
  style.corner_radii = huxerui::CornerRadii{8.0F};
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

inline huxerui::TextFieldStyle PreviewFormFieldStyle(huxerui::TextFieldStyle field, const huxerui::ColorScheme& colors) {
  field.outlined.background = colors.surface;
  field.outlined.border = colors.outline;
  field.outlined.hovered_border = colors.on_surface_variant;
  field.outlined.focused_border = colors.on_primary_container;
  field.text_style.foreground = colors.on_surface;
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
  field.composition = colors.on_primary_container;
  return field;
}

inline huxerui::ThemeSpec PreviewTheme(huxerui::ThemeBrightness brightness) {
  using huxerui::Color;
  const bool dark = brightness == huxerui::ThemeBrightness::Dark;
  auto theme = dark ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
  auto& colors = theme.colors;
  colors.surface = dark ? Color::Rgb(23, 24, 25) : Color::Rgb(249, 247, 244);
  colors.background = colors.surface;
  colors.surface_container_low = dark ? Color::Rgb(28, 29, 30) : Color::Rgb(242, 239, 235);
  colors.surface_container = dark ? Color::Rgb(36, 37, 38) : Color::Rgb(255, 253, 249);
  colors.surface_container_high = dark ? Color::Rgb(43, 44, 46) : Color::Rgb(235, 231, 225);
  colors.surface_container_highest = dark ? Color::Rgb(53, 54, 56) : Color::Rgb(227, 222, 214);
  colors.on_surface = dark ? Color::Rgb(242, 239, 234) : Color::Rgb(40, 37, 32);
  colors.on_surface_variant = dark ? Color::Rgb(191, 186, 178) : Color::Rgb(88, 80, 71);
  colors.outline = dark ? Color::Rgb(57, 58, 61) : Color::Rgb(217, 211, 201);
  colors.primary = dark ? Color::Rgb(235, 190, 139) : Color::Rgb(128, 84, 47);
  colors.on_primary = dark ? Color::Rgb(42, 32, 22) : Color::White();
  colors.primary_container = dark ? Color::Rgb(52, 45, 42) : Color::Rgb(234, 220, 201);
  colors.on_primary_container = dark ? Color::Rgb(246, 212, 174) : Color::Rgb(98, 59, 28);
  colors.error = dark ? Color::Rgb(255, 180, 189) : Color::Rgb(150, 50, 64);
  theme.interactions.focus_ring = huxerui::FocusRing{colors.on_primary_container, 2.0F, 2.0F};
  return theme;
}

} // namespace agentkit_preview
