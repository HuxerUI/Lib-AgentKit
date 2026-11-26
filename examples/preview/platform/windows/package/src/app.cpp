#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <installer_resources.h>
#include <huxerui/huxerui.h>
#include <huxerui/windows/installer.h>

using namespace huxerui;
using namespace huxerui::windows;
namespace installer_strings = installer::strings;

namespace {

Color TintColor(Color surface, Color ink, float weight) {
  return {std::lerp(surface.red, ink.red, weight), std::lerp(surface.green, ink.green, weight),
      std::lerp(surface.blue, ink.blue, weight), 1.0F};
}

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

Color SuccessColor(ThemeBrightness brightness) {
  return brightness == ThemeBrightness::Dark ? Color::Rgb(158, 217, 179) : Color::Rgb(34, 102, 62);
}

ThemeSpec InstallerThemeSpec(ThemeBrightness brightness) {
  const bool dark = brightness == ThemeBrightness::Dark;
  const auto color = [](std::uint32_t rgb) { return Color::Rgba32((rgb << 8U) | 0xffU); };
  ThemeSpec theme = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
  theme.typography.body_large = 14.0F;
  theme.typography.body_small = 13.0F;
  auto& colors = theme.colors;
  colors.surface = color(dark ? 0x181a1fU : 0xf9fafcU);
  colors.background = colors.surface;
  colors.on_surface = color(dark ? 0xebeef4U : 0x232a35U);
  colors.on_surface_variant = color(dark ? 0xaab3c2U : 0x526070U);
  colors.primary = color(dark ? 0xa4bedfU : 0x365574U);
  colors.on_primary = dark ? colors.surface : Color::White();
  colors.surface_container_low = color(dark ? 0x111318U : 0xeff2f6U);
  colors.surface_container = color(dark ? 0x202329U : 0xffffffU);
  colors.surface_container_high = color(dark ? 0x292d35U : 0xeef2f7U);
  colors.surface_container_highest = TintColor(colors.surface_container_high, colors.on_surface, dark ? 0.09F : 0.07F);
  colors.outline = color(dark ? 0x363b45U : 0xd9e0e8U);
  colors.primary_container = TintColor(colors.surface_container, colors.primary, dark ? 0.12F : 0.10F);
  colors.on_primary_container = dark ? TintColor(colors.primary, colors.on_surface, 0.28F) : colors.primary;
  colors.secondary = colors.on_surface_variant;
  colors.on_secondary = colors.on_primary;
  colors.secondary_container = colors.surface_container_highest;
  colors.on_secondary_container = colors.on_surface;
  colors.inverse_surface = color(dark ? 0xf9fafcU : 0x181a1fU);
  colors.inverse_on_surface = color(dark ? 0x232a35U : 0xebeef4U);
  colors.scrim = Color::Rgb(0, 0, 0, dark ? 0.58F : 0.37F);
  colors.error = dark ? color(0xffb7c0U) : color(0xa32840U);

  const CornerRadii corners{7.0F};
  Indication indication;
  indication.geometry = {.clip_corner_radii = corners};
  indication.hover = IndicationLayer{
      .fill = WithAlpha(colors.on_surface, 0.07F),
      .corner_radii = corners,
      .placement = IndicationPlacement::BehindContent,
  };
  indication.press = IndicationLayer{
      .fill = WithAlpha(colors.on_surface, 0.13F),
      .corner_radii = corners,
      .placement = IndicationPlacement::BehindContent,
  };
  indication.focus = IndicationLayer{.border = Border{colors.primary, 1.0F}, .corner_radii = corners};
  theme.interactions.focus_ring = FocusRing{colors.primary, 1.0F, 2.0F};
  theme.interactions.indication = std::move(indication);
  return theme;
}

ButtonStyle PrimaryButtonStyle(const ThemeSpec& theme) {
  ButtonStyle style = ButtonStyle::Default();
  style.background = theme.colors.primary;
  style.label_style = {Font::System(13.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_primary};
  style.disabled_background = theme.colors.surface_container_high;
  style.disabled_label = theme.colors.on_surface_variant;
  style.minimum_width = 96.0F;
  style.minimum_height = 36.0F;
  style.padding = EdgeInsets::Symmetric(16.0F, 7.0F);
  style.corner_radii = CornerRadii{7.0F};
  style.indication = theme.interactions.indication;
  return style;
}

ButtonStyle SecondaryButtonStyle(const ThemeSpec& theme) {
  ButtonStyle style = PrimaryButtonStyle(theme);
  style.background = theme.colors.surface_container_high;
  style.label_style = {Font::System(13.0F).WithWeight(FontWeight::Medium), theme.colors.on_surface};
  style.disabled_background = theme.colors.surface_container_low;
  return style;
}

TextFieldStyle InstallerTextFieldStyle(const ThemeSpec& theme) {
  const auto& colors = theme.colors;
  TextFieldStyle field = TextFieldStyle::Default();
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
  field.selection = TintColor(colors.surface, colors.primary, 0.25F);
  field.error_caret = colors.error;
  field.composition = colors.on_primary_container;
  return field;
}

View InstallerControlsTheme(View content, const ThemeSpec& theme) {
  ThemeDefinition overrides;
  overrides.Set(PrimaryButtonStyle(theme));
  overrides.Set(InstallerTextFieldStyle(theme));
  overrides.Set(DividerStyle{theme.colors.outline});
  return Theme{std::move(overrides), std::move(content)};
}

std::string InstallPathText(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {value.begin(), value.end()};
}

std::optional<std::filesystem::path> ParseInstallPath(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  try {
    std::filesystem::path path(std::u8string(value.begin(), value.end()));
    return path.is_absolute() ? std::optional<std::filesystem::path>{path.lexically_normal()} : std::nullopt;
  } catch (const std::filesystem::filesystem_error&) {
    return std::nullopt;
  }
}

int InstallerStepIndex(InstallerPhase phase) {
  switch (phase) {
  case InstallerPhase::Detecting:
    return 0;
  case InstallerPhase::Ready:
    return 1;
  case InstallerPhase::Planning:
  case InstallerPhase::Applying:
  case InstallerPhase::Canceling:
    return 2;
  case InstallerPhase::Completed:
  case InstallerPhase::Canceled:
  case InstallerPhase::Failed:
    return 3;
  }
  return 0;
}

View InstallerMark(const ColorScheme& colors) {
  return Column{
      Image(installer::images::ganwork_mark)
          .Tint(colors.on_primary_container)
          .Fit(ImageFit::Contain)
          .With(Grow()),
  }.With(
      Frame{.width = 64.0F, .height = 64.0F},
      Padding(12.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(colors.primary_container),
      Border{colors.outline, 1.0F},
      CornerRadius(16.0F)
  );
}

View StepRow(int number, StringResource label, bool done, bool active, const ColorScheme& colors) {
  const Color badge_bg = done || active ? colors.primary_container : colors.surface_container;
  const Color badge_border = done || active ? colors.primary : colors.outline;
  const Color badge_ink = done || active ? colors.on_primary_container : colors.on_surface_variant;
  const Color text_ink = active ? colors.on_surface : colors.on_surface_variant;
  const FontWeight text_weight = active ? FontWeight::SemiBold : FontWeight::Medium;

  return Row{
      Column{
          Text(done ? "\xe2\x9c\x93" : std::to_string(number), TextRole::Label)
              .Style({Font::System(11.0F).WithWeight(FontWeight::SemiBold), badge_ink}),
      }.With(
          Frame{.width = 22.0F, .height = 22.0F},
          MainAlign(MainAxisAlignment::Center),
          CrossAlign(CrossAxisAlignment::Center),
          Background(badge_bg),
          Border{badge_border, 1.0F},
          CornerRadius(11.0F)
      ),
      Text(label).Style({Font::System(13.0F).WithWeight(text_weight), text_ink}),
  }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center));
}

View BrandPanel(const ThemeSpec& theme, InstallerPhase phase) {
  const auto& colors = theme.colors;
  const int current_step = InstallerStepIndex(phase);
  const std::array steps{
      installer_strings::welcome,
      installer_strings::installation,
      installer_strings::installing,
      installer_strings::complete,
  };

  std::vector<View> step_list;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const int step_num = static_cast<int>(i);
    const bool done = step_num < current_step || (phase == InstallerPhase::Completed && step_num == 3);
    const bool active = step_num == current_step && !done;
    step_list.push_back(StepRow(step_num + 1, steps[i], done, active, colors));
  }

  return Column{
      InstallerMark(colors),
      Column{
          Text(installer_strings::application_setup)
              .Style({Font::System(11.0F).WithWeight(FontWeight::SemiBold), colors.on_primary_container}),
          Text("GanWork")
              .Style({Font::System(26.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
          Text(installer_strings::guided_setup)
              .Style({Font::System(13.0F), colors.on_surface_variant}),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Start)),
      ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
      Column(std::move(step_list)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Start)),
      Spacer().With(Grow()),
      Column{
          Text(installer_strings::platform_windows, TextRole::Label)
              .Style({Font::System(11.0F).WithWeight(FontWeight::SemiBold), colors.on_surface_variant}),
      }.With(
          Padding(EdgeInsets::Symmetric(10.0F, 4.0F)),
          Background(colors.surface_container),
          Border{colors.outline, 1.0F},
          CornerRadius(6.0F)
      ),
  }.With(
      Frame{.width = 248.0F},
      Padding(EdgeInsets{.top = 28.0F, .right = 24.0F, .bottom = 24.0F, .left = 24.0F}),
      Spacing(20.0F),
      Background(colors.surface_container_low),
      CrossAlign(CrossAxisAlignment::Start),
      WindowDragRegion{}
  );
}

View CompletionMark(const ThemeSpec& theme) {
  const Color foreground = SuccessColor(theme.brightness);
  const Color background = WithAlpha(foreground, 0.14F);
  return Canvas([background, foreground](PaintContext& paint, Size) {
    paint.DrawCircle({26.0F, 26.0F}, 26.0F, background);
    const StrokeStyle stroke{.width = 3.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
    paint.DrawLine({15.0F, 27.0F}, {23.0F, 35.0F}, foreground, stroke);
    paint.DrawLine({23.0F, 35.0F}, {39.0F, 18.0F}, foreground, stroke);
  }).With(Frame{.width = 52.0F, .height = 52.0F});
}

View SecondaryAction(View action, const ThemeSpec& theme) {
  ThemeDefinition definition;
  definition.Set(SecondaryButtonStyle(theme));
  return Theme{std::move(definition), std::move(action)};
}

std::vector<View> PromptActions(const InstallerHandle& installer, const InstallerPrompt& prompt,
                                const ThemeSpec& theme) {
  std::vector<View> actions;
  for (const InstallerPromptChoice choice : prompt.choices) {
    StringVariant label = installer_strings::continue_action;
    switch (choice) {
    case InstallerPromptChoice::Ok:
      label = installer_strings::ok;
      break;
    case InstallerPromptChoice::Cancel:
      label = installer_strings::cancel;
      break;
    case InstallerPromptChoice::Abort:
      label = installer_strings::abort;
      break;
    case InstallerPromptChoice::Retry:
      label = installer_strings::retry;
      break;
    case InstallerPromptChoice::TryAgain:
      label = installer_strings::try_again;
      break;
    case InstallerPromptChoice::Ignore:
      label = installer_strings::ignore;
      break;
    case InstallerPromptChoice::Yes:
      label = prompt.kind == InstallerPromptKind::Restart ? installer_strings::restart_now : installer_strings::yes;
      break;
    case InstallerPromptChoice::No:
      label = prompt.kind == InstallerPromptKind::Restart ? installer_strings::restart_later : installer_strings::no;
      break;
    case InstallerPromptChoice::Continue:
      label = installer_strings::continue_action;
      break;
    }
    View action = Button(std::move(label)).OnClick([installer, id = prompt.id, choice] {
      installer.Respond(id, choice);
    });
    if (!prompt.recommended || choice != *prompt.recommended) {
      action = SecondaryAction(std::move(action), theme);
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

[[huxerui::composable]]
View RestartDialog(const InstallerHandle& installer, const InstallerPrompt& prompt) {
  const ThemeSpec& theme = UseTheme();
  const auto& colors = theme.colors;
  return Column{
      Text(installer_strings::restart_title, TextRole::Title)
          .Style({Font::System(18.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
      Text(installer_strings::restart_confirmation).With(Foreground(colors.on_surface_variant)),
      Row(PromptActions(installer, prompt, theme)).With(Spacing(10.0F), MainAlign(MainAxisAlignment::End)),
  }.With(
      Frame{.width = 420.0F},
      Padding(24.0F),
      Spacing(16.0F),
      Background(colors.surface_container),
      Border{colors.outline, 1.0F},
      CornerRadius(12.0F),
      Shadow{.color = Color::Rgb(0, 0, 0, 0.36F), .offset = {0.0F, 12.0F}, .blur_radius = 36.0F}
  );
}

[[huxerui::composable]]
View InstallerContent() {
  const InstallerHandle installer = UseInstaller();
  const WindowHandle window = UseWindow();
  const TaskScope tasks = UseTaskScope();
  const ThemeSpec& theme = UseTheme();
  const auto& colors = theme.colors;
  const InstallerStatus status = installer.Status();
  auto destination = UseState<std::optional<TextEditingValue>>(std::nullopt);
  auto desktop_shortcut = UseState<std::optional<bool>>(std::nullopt);
  StringVariant eyebrow;
  StringVariant heading;
  std::vector<View> details;
  std::vector<View> actions;
  MainAxisAlignment panel_alignment = MainAxisAlignment::Start;

  const bool restart_prompt = status.prompt && status.prompt->kind == InstallerPromptKind::Restart;
  if (status.prompt && !restart_prompt) {
    eyebrow = installer_strings::action_required;
    heading = installer_strings::setup_needs_attention;
    if (status.prompt->kind == InstallerPromptKind::FilesInUse) {
      details.push_back(Text(installer_strings::files_in_use).With(Foreground(colors.on_surface_variant)));
      if (!status.prompt->message.empty()) {
        details.push_back(
            Column{
                ScrollView{
                    Text(status.prompt->message).With(Foreground(colors.on_surface_variant)),
                }.With(Frame{.max_height = 144.0F}, ScrollBar()),
            }.With(
                Padding(12.0F),
                Background(colors.surface_container_low),
                Border{colors.outline, 1.0F},
                CornerRadius(8.0F),
                CrossAlign(CrossAxisAlignment::Stretch)
            )
        );
      }
    } else {
      details.push_back(Text(status.prompt->message).With(Foreground(colors.on_surface_variant)));
    }
    actions = PromptActions(installer, *status.prompt, theme);
  } else if (status.phase == InstallerPhase::Detecting) {
    eyebrow = installer_strings::welcome;
    heading = installer_strings::preparing_setup;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Text(installer_strings::checking_installed_version).With(Foreground(colors.on_surface_variant))
    );
    details.push_back(ProgressBar(status.progress));
  } else if (status.phase == InstallerPhase::Ready &&
             status.product == InstallerProductState::NewerVersion) {
    eyebrow = installer_strings::version_check;
    heading = installer_strings::newer_version_installed;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(Text(installer_strings::remove_newer_version).With(Foreground(colors.on_surface_variant)));
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Ready && status.product == InstallerProductState::SameVersion) {
    eyebrow = installer_strings::maintenance;
    heading = installer_strings::manage_installation;
    details.push_back(
        Column{
            Text(installer_strings::already_installed).With(Foreground(colors.on_surface_variant)),
        }.With(
            Padding(14.0F),
            Background(colors.surface_container),
            Border{colors.outline, 1.0F},
            CornerRadius(8.0F),
            CrossAlign(CrossAxisAlignment::Stretch)
        )
    );
    actions.push_back(
        SecondaryAction(Button(installer_strings::cancel).OnClick([window] { window.Close(); }), theme)
    );
    actions.push_back(SecondaryAction(
        Button(installer_strings::uninstall).OnClick([installer] { installer.Uninstall(); }), theme
    ));
    actions.push_back(
        Button(installer_strings::repair)
            .OnClick([installer] { installer.Repair(); })
            .With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Ready &&
             (status.product == InstallerProductState::Absent || status.product == InstallerProductState::Present)) {
    const TextEditingValue destination_value = destination.Get().value_or(
        TextEditingValue::FromText(InstallPathText(status.default_destination))
    );
    const std::optional<std::filesystem::path> destination_path = ParseInstallPath(destination_value.text);
    const std::filesystem::path browse_initial = destination_path.value_or(status.default_destination);
    const bool create_desktop_shortcut =
        desktop_shortcut.Get().value_or(status.default_create_desktop_shortcut);
    eyebrow = installer_strings::installation;
    heading = installer_strings::ready_to_install;
    details.push_back(
        Text(installer_strings::installation_options).With(Foreground(colors.on_surface_variant))
    );
    details.push_back(Row{
        TextField(destination_value)
            .Label(installer_strings::installation_folder)
            .Variant(TextFieldVariant::Outlined)
            .Validation(
                destination_path ? ValidationResult::None()
                                 : ValidationResult::Invalid(installer_strings::invalid_installation_folder)
            )
            .OnChanged([destination](const TextEditingValue& value) { destination = value; })
            .With(Grow()),
        SecondaryAction(
            Button(installer_strings::browse).OnClick([installer, destination, browse_initial, tasks] {
              tasks.Launch([installer, destination, browse_initial]() -> Task<void> {
                const std::optional<std::filesystem::path> selected =
                    co_await installer.ChooseDestinationAsync(browse_initial);
                if (selected) {
                  destination = TextEditingValue::FromText(InstallPathText(*selected));
                }
              });
            }),
            theme
        ),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)));
    details.push_back(
        Checkbox(installer_strings::create_desktop_shortcut, create_desktop_shortcut)
            .OnChanged([desktop_shortcut](bool enabled) { desktop_shortcut = enabled; })
            .With(
                Padding(EdgeInsets::Symmetric(14.0F, 10.0F)),
                Background(colors.surface_container),
                Border{colors.outline, 1.0F},
                CornerRadius(8.0F)
            )
    );
    actions.push_back(
        SecondaryAction(Button(installer_strings::cancel).OnClick([window] { window.Close(); }), theme)
    );
    actions.push_back(
        Button(installer_strings::install)
            .OnClick([installer, destination_path, create_desktop_shortcut] {
              if (destination_path) {
                installer.Install({
                    .destination = *destination_path,
                    .create_desktop_shortcut = create_desktop_shortcut,
                });
              }
            })
            .With(Enabled(destination_path.has_value()), Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Ready) {
    eyebrow = installer_strings::setup;
    heading = installer_strings::setup_could_not_continue;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Text(installer_strings::installed_version_unknown).With(Foreground(colors.on_surface_variant))
    );
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Planning || status.phase == InstallerPhase::Applying ||
             status.phase == InstallerPhase::Canceling) {
    eyebrow = installer_strings::installing;
    heading = installer_strings::installing_product;
    panel_alignment = MainAxisAlignment::Center;
    if (status.phase == InstallerPhase::Canceling) {
      eyebrow = installer_strings::rollback;
      heading = installer_strings::canceling_and_rolling_back;
    } else if (status.action == InstallerAction::Repair) {
      eyebrow = installer_strings::repairing;
      heading = installer_strings::repairing_product;
    } else if (status.action == InstallerAction::Uninstall) {
      eyebrow = installer_strings::removing;
      heading = installer_strings::uninstalling_product;
    }
    const int percent = static_cast<int>(std::round(std::clamp(status.progress, 0.0F, 1.0F) * 100.0F));
    StringVariant progress_message = status.current_package.empty()
                                         ? StringVariant(installer_strings::operation_may_take_a_moment)
                                         : StringVariant(status.current_package);
    details.push_back(Row{
        Text(std::move(progress_message)).With(Foreground(colors.on_surface_variant), Grow()),
        Text(std::to_string(percent) + "%", TextRole::Label)
            .Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_primary_container}),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)));
    details.push_back(ProgressBar(status.progress));
    actions.push_back(
        SecondaryAction(
            Button(installer_strings::cancel)
                .OnClick([installer] { installer.Cancel(); })
                .With(Enabled(status.phase != InstallerPhase::Canceling)),
            theme
        )
    );
  } else if (status.phase == InstallerPhase::Completed) {
    eyebrow = installer_strings::complete;
    heading = status.action == InstallerAction::Uninstall ? installer_strings::application_removed
                                                          : installer_strings::installation_complete;
    panel_alignment = MainAxisAlignment::Center;
    StringVariant message;
    if (status.action == InstallerAction::Uninstall) {
      message = status.restart == InstallerRestart::Required ? installer_strings::application_removed_restart_message
                                                             : installer_strings::application_removed_message;
    } else {
      message = status.restart == InstallerRestart::Required
                    ? installer_strings::installation_complete_restart_message
                    : installer_strings::installation_complete_message;
    }
    details.push_back(Row{CompletionMark(theme)}.With(MainAlign(MainAxisAlignment::Center)));
    details.push_back(Text(std::move(message)).With(Foreground(colors.on_surface_variant)));
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.phase == InstallerPhase::Canceled) {
    eyebrow = installer_strings::canceled;
    heading = installer_strings::installation_canceled;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Text(installer_strings::no_further_changes).With(Foreground(colors.on_surface_variant))
    );
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  } else if (status.failure) {
    eyebrow = installer_strings::setup_error;
    heading = installer_strings::installation_failed;
    panel_alignment = MainAxisAlignment::Center;
    details.push_back(
        Column{
            Text(status.failure->message).With(Foreground(colors.error)),
        }.With(
            Padding(14.0F),
            Background(WithAlpha(colors.error, 0.08F)),
            Border{WithAlpha(colors.error, 0.35F), 1.0F},
            CornerRadius(8.0F),
            CrossAlign(CrossAxisAlignment::Stretch)
        )
    );
    actions.push_back(
        Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F})
    );
  }

  std::vector<View> panel{
      Text(std::move(eyebrow))
          .Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_primary_container}),
      Text(std::move(heading), TextRole::Title)
          .Style({Font::System(22.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
  };
  panel.insert(panel.end(), std::make_move_iterator(details.begin()), std::make_move_iterator(details.end()));

  std::vector<View> content;
  content.push_back(Column(std::move(panel)).With(
      Spacing(14.0F),
      MainAlign(panel_alignment),
      CrossAlign(CrossAxisAlignment::Stretch),
      Grow()
  ));
  if (!actions.empty()) {
    content.push_back(ProvideEnvironment(DividerStyle{colors.outline}, Divider()));
    content.push_back(Row(std::move(actions)).With(
        Spacing(10.0F),
        MainAlign(MainAxisAlignment::End),
        CrossAlign(CrossAxisAlignment::Center)
    ));
  }

  Dialog restart_dialog;
  if (restart_prompt) {
    restart_dialog = Dialog{
        .visible = true,
        .content = [installer, prompt = *status.prompt] { return RestartDialog(installer, prompt); },
        .dismiss_on_cancel = true,
        .on_dismiss_request = [installer, id = status.prompt->id] { installer.Respond(id, InstallerPromptChoice::No); },
    };
  }

  return Row{
      BrandPanel(theme, status.phase),
      ProvideEnvironment(DividerStyle{colors.outline}, Divider(Axis::Vertical)),
      Column{
          WindowTitleBar{}.With(Background(colors.surface)),
          Column(std::move(content)).With(
              Padding(EdgeInsets{.top = 6.0F, .right = 32.0F, .bottom = 28.0F, .left = 32.0F}),
              Spacing(20.0F),
              CrossAlign(CrossAxisAlignment::Stretch),
              Grow()
          ),
      }.With(CrossAlign(CrossAxisAlignment::Stretch), Grow()),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface), Grow(), std::move(restart_dialog));
}

} // namespace

View InstallerPage() {
  const ThemeSpec theme = InstallerThemeSpec(UseSystemThemeBrightness());
  return MaterialTheme(theme, InstallerControlsTheme(InstallerContent(), theme));
}

const Application application{
    InstallerPage,
    {
        .window = {
            .title = "GanWork",
            .initial_size = {800.0F, 520.0F},
            .minimum_size = Size{720.0F, 480.0F},
            .chrome_mode = WindowChromeMode::Custom,
            .title_bar_height = 40.0F,
        },
        .show_debug_overlay = false,
        .window_hooks = {InstallInstallerSession},
    },
};
