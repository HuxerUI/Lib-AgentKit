#include <app_resources.h>

#include "preview_settings.h"

#include "general_workflow.h"
#include "preview_theme.h"

#include <huxerui/huxerui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;

constexpr std::size_t instruction_limit = 4096;
const std::array appearance_labels{::app::strings::ui_dark, ::app::strings::ui_light, ::app::strings::ui_follow_system};
const std::array tone_labels{::app::strings::ui_default, ::app::strings::ui_concise, ::app::strings::ui_friendly, ::app::strings::ui_formal};

enum class SettingsSection { Models, Personalization, Appearance, Storage };

struct SettingsCategory {
  SettingsSection section;
  StringResource title;
  const char* key;
};

const std::array categories{
    SettingsCategory{SettingsSection::Models, ::app::strings::ui_models, "settings-section-models"},
    SettingsCategory{SettingsSection::Personalization, ::app::strings::ui_personalization, "settings-section-personalization"},
    SettingsCategory{SettingsSection::Appearance, ::app::strings::ui_appearance, "settings-section-appearance"},
    SettingsCategory{SettingsSection::Storage, ::app::strings::ui_storage, "settings-section-storage"},
};

Json ModelRecord(const std::optional<kit::ModelProviderRef>& model) {
  if (!model) return nullptr;
  return {{"id", model->id.value}, {"version", model->version}};
}

std::optional<kit::ModelProviderRef> ReadModel(const Json& document, const char* key) {
  const auto value = document.find(key);
  if (value == document.end() || value->is_null()) return {};
  kit::ModelProviderRef model{{value->at("id").get<std::string>()}, value->at("version").get<kit::Revision>()};
  if (model.id.value.empty() || !model.version) throw std::invalid_argument("Invalid model role");
  return model;
}

using ModelRole = std::optional<kit::ModelProviderRef> PreviewSettings::*;

View ModelRoleField(StringVariant label, std::vector<ModelChoice> choices, StringVariant fallback,
    State<PreviewSettings> draft, ModelRole role) {
  const auto selected = draft.Get().*role;
  std::vector<std::pair<std::optional<kit::ModelProviderRef>, StringVariant>> options{{std::nullopt, std::move(fallback)}};
  for (auto& item : choices) options.emplace_back(std::move(item.ref), std::move(item.label));
  auto found = std::ranges::find(options, selected, &decltype(options)::value_type::first);
  if (found == options.end()) {
    options.emplace_back(selected, ::app::strings::ui_saved_model_unavailable);
    found = std::prev(options.end());
  }
  const auto index = static_cast<std::size_t>(found - options.begin());
  return Select(options, index, [](const auto& option) { return Text(option.second).LineLimit(1).Overflow(TextOverflow::Ellipsis); })
      .Label(label).OnChanged([draft, role, options = std::move(options)](std::size_t selected_index) {
        auto settings = draft.Get();
        settings.*role = options.at(selected_index).first;
        draft = std::move(settings);
      });
}

View SettingsRow(StringVariant label, StringVariant help, View control, bool stacked, const ColorScheme& colors) {
  auto copy = Column{
      Text(label).Style({Font::System(14.0F).WithWeight(FontWeight::Medium), colors.on_surface}),
      Text(help).Style({Font::System(12.0F), colors.on_surface_variant}),
  }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
  if (stacked) return Column{std::move(copy), std::move(control)}.With(Padding(14.0F), Spacing(9.0F),
      CrossAlign(CrossAxisAlignment::Stretch));
  return Row{std::move(copy).With(Grow()), std::move(control).With(Frame{.width = 282.0F})}
      .With(Padding(16.0F), Spacing(20.0F), CrossAlign(CrossAxisAlignment::Center));
}

View SettingsPanel(std::vector<View> rows, const ColorScheme& colors) {
  std::vector<View> content;
  for (auto& row : rows) {
    if (!content.empty()) content.push_back(
        ProvideEnvironment(DividerStyle{colors.outline}, Divider().With(Padding(EdgeInsets::Symmetric(18.0F, 0.0F)))));
    content.push_back(std::move(row));
  }
  return Column(std::move(content)).With(CrossAlign(CrossAxisAlignment::Stretch),
      CornerRadius(10.0F), Background(colors.surface_container_low), Border{colors.outline});
}

[[huxerui::composable]]
View SettingsCategoryLink(SettingsCategory category, State<SettingsSection> active) {
  const auto colors = UseTheme().colors;
  const auto hovered = UseState(false);
  const auto select = [active, section = category.section] { active = section; };
  const bool selected = active.Get() == category.section;
  return Row{
      Canvas([ink = selected ? colors.primary : Color::Transparent()](PaintContext& context, Size size) {
        context.DrawRect({0.0F, 0.0F, size.width, size.height}, ink, CornerRadii{1.0F});
      }).With(Frame{.width = 2.0F, .height = 14.0F}),
      Text(category.title).Style({Font::System(13.0F).WithWeight(selected ? FontWeight::SemiBold : FontWeight::Medium),
          selected ? colors.on_primary_container : colors.on_surface_variant}),
  }
      .OnClick(select).On<ViewEvents::KeyDown>([select](const KeyEvent& event) {
        if (event.key != Key::Enter && event.key != Key::Space) return false;
        select();
        return true;
      }).On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
      .With(Focusable{}, Padding(EdgeInsets::Symmetric(10.0F, 0.0F)), Frame{.height = 38.0F}, Spacing(7.0F),
          CrossAlign(CrossAxisAlignment::Center), CornerRadius(9.0F),
          Background(selected ? colors.primary_container : hovered.Get() ? colors.surface_container_high : Color::Transparent()),
          Semantics{.role = SemanticRole::Button, .label = category.title, .selected = selected});
}

} // namespace

kit::Async<PreviewSettings> ReadPreviewSettings(File file) {
  using Result = kit::AgentResult<PreviewSettings>;
  if (!file.Exists()) co_return Result::Success({});
  const auto raw = co_await file.ReadStringAsync();
  if (!raw.Succeeded()) co_return Result::Failure({
      .code = kit::ErrorCode::PersistenceFailure, .message = "Could not read Preview settings."});
  try {
    const auto doc = Json::parse(raw.Value());
    const auto appearance = doc.value("appearance", std::size_t{});
    const auto tone = doc.value("tone", std::size_t{});
    auto instructions = doc.value("instructions", std::string{});
    if (appearance >= appearance_labels.size() || tone >= tone_labels.size() || instructions.size() > instruction_limit) {
      throw std::invalid_argument("Invalid Preview settings");
    }
    co_return Result::Success({.appearance = static_cast<PreviewAppearance>(appearance), .tone = static_cast<ReplyTone>(tone),
        .instructions = std::move(instructions), .daily_model = ReadModel(doc, "daily_model"),
        .image_model = ReadModel(doc, "image_model"), .review_model = ReadModel(doc, "review_model")});
  } catch (const std::exception&) {
    co_return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "The saved Preview settings are invalid."});
  }
}

kit::Async<void> SavePreviewSettings(File file, PreviewSettings settings) {
  using Result = kit::AgentResult<void>;
  if (settings.instructions.size() > instruction_limit) {
    co_return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Personal instructions may use up to 4 KiB."});
  }
  auto temporary = file.Parent()->Child(kit::NewCommandId().value + ".settings-pending");
  auto contents = Json{{"appearance", static_cast<std::size_t>(settings.appearance)},
      {"tone", static_cast<std::size_t>(settings.tone)}, {"instructions", std::move(settings.instructions)},
      {"daily_model", ModelRecord(settings.daily_model)}, {"image_model", ModelRecord(settings.image_model)},
      {"review_model", ModelRecord(settings.review_model)}}.dump();
  if (!co_await temporary.WriteStringAsync(std::move(contents)) || !co_await temporary.MoveToAsync(file, true)) {
    static_cast<void>(co_await temporary.DeleteAsync());
    co_return Result::Failure({.code = kit::ErrorCode::PersistenceFailure, .message = "Could not save Preview settings."});
  }
  co_return Result::Success();
}

std::string WithReplyPreferences(std::string instructions, const PreviewSettings& settings) {
  if (settings.tone == ReplyTone::Default && settings.instructions.empty()) return instructions;
  instructions += "\nApply these preferences to user-facing replies while preserving the requested output format.";
  switch (settings.tone) {
  case ReplyTone::Default: break;
  case ReplyTone::Concise: instructions += "\nKeep replies concise and direct."; break;
  case ReplyTone::Friendly: instructions += "\nUse a friendly, conversational tone."; break;
  case ReplyTone::Formal: instructions += "\nUse a formal, professional tone."; break;
  }
  if (!settings.instructions.empty()) instructions += "\nUser preferences for replies:\n" + settings.instructions;
  return instructions;
}

ThemeBrightness PreviewBrightness(PreviewAppearance appearance, ThemeBrightness system) {
  switch (appearance) {
  case PreviewAppearance::Light: return ThemeBrightness::Light;
  case PreviewAppearance::System: return system;
  default: return ThemeBrightness::Dark;
  }
}

[[huxerui::composable]]
View PreviewSettingsPage(State<PreviewSettings> current, State<PreviewSettings> applied,
    std::shared_ptr<RunSettings> models, File file, AddModelProfile add_model, std::function<void()> close) {
  const auto draft = UseState(current.Get());
  const auto instructions = UseState(TextEditingValue::FromText(current->instructions));
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto refresh_models = UseState(std::size_t{0});
  const auto section = UseState(SettingsSection::Models);
  const auto dialog = UseDialog();
  const auto tasks = UseTaskScope();
  const auto colors = UseTheme().colors;
  const auto viewport = UseViewportClass();
  const bool compact = viewport == ViewportClass::Compact;
  const bool stack_fields = viewport != ViewportClass::Expanded;
  auto fields = PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), colors);
  fields.show_label = false;
  const auto secondary = PreviewSecondaryButton(UseEnvironment<ButtonStyle>(), colors);
  const auto save = [current, draft, file, tasks, error, busy] {
    if (busy.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([current, file, value = draft.Get(), busy, error]() -> Task<void> {
      auto saved = co_await SavePreviewSettings(file, value);
      busy = false;
      if (saved.Succeeded()) current = std::move(value);
      else error = saved.Error().message;
    });
  };
  const auto pending_restart = current->daily_model != applied->daily_model || current->image_model != applied->image_model ||
      current->review_model != applied->review_model || current->tone != applied->tone ||
      current->instructions != applied->instructions;
  std::vector<View> content;
  std::vector<View> links;
  for (const auto& category : categories) {
    links.push_back(SettingsCategoryLink(category, section).Key(category.key));
    if (category.section == section.Get()) content.push_back(Text(category.title, TextRole::Title)
        .Style({Font::System(22.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}));
  }
  switch (section.Get()) {
  case SettingsSection::Models: {
    static_cast<void>(refresh_models.Get());
    auto choices = models->Models(GeneralAgent);
    std::erase_if(choices, [](const auto& item) { return item.ref == DemoModel; });
    auto vision = choices;
    std::erase_if(vision, [](const auto& item) { return !item.capabilities.image_input; });
    std::vector<View> catalog_heading{
        Text(::app::strings::ui_configured_models).Style({Font::System(15.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}).With(Grow()),
    };
    if (add_model) catalog_heading.push_back(ProvideEnvironment(secondary,
        Button(::app::strings::ui_add_model_dots).OnClick([dialog, add_model, refresh_models] {
          ShowAddModel(dialog, add_model, [refresh_models](kit::ModelProviderRef) { refresh_models = refresh_models.Get() + 1; });
        })).Key("settings-add-model"));
    std::vector<View> catalog{
        Row(std::move(catalog_heading)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
    };
    if (choices.empty()) catalog.push_back(Text(::app::strings::ui_no_models_configured).Style({Font::System(13.0F), colors.on_surface_variant}));
    for (const auto& model : choices) {
      catalog.push_back(Text(model.name).Style({Font::System(13.0F), colors.on_surface_variant})
          .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Tooltip(model.label)));
    }
    content.push_back(SettingsPanel({Column(std::move(catalog)).With(Padding(16.0F), Spacing(9.0F),
        CrossAlign(CrossAxisAlignment::Stretch))}, colors));
    content.push_back(Text(::app::strings::ui_model_roles).Style({Font::System(15.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}));
    content.push_back(SettingsPanel({
        SettingsRow(::app::strings::ui_daily_model, ::app::strings::ui_default_for_new_conversations,
            ModelRoleField(::app::strings::ui_daily_model, choices, ::app::strings::ui_automatic, draft, &PreviewSettings::daily_model).Key("settings-daily-model"),
            stack_fields, colors),
        SettingsRow(::app::strings::ui_image_understanding, ::app::strings::ui_choose_a_model_that_accepts_images,
            ModelRoleField(::app::strings::ui_image_understanding, vision, ::app::strings::ui_use_the_selected_chat_model, draft, &PreviewSettings::image_model)
                .Key("settings-image-model"), stack_fields, colors),
        SettingsRow(::app::strings::ui_auto_review_model, ::app::strings::ui_use_for_automatic_approval_review,
            ModelRoleField(::app::strings::ui_auto_review_model, choices, ::app::strings::ui_use_the_host_review_policy, draft, &PreviewSettings::review_model)
                .Key("settings-review-model"), stack_fields, colors),
    }, colors));
    break;
  }
  case SettingsSection::Personalization:
    content.push_back(SettingsPanel({
        SettingsRow(::app::strings::ui_reply_tone, ::app::strings::ui_choose_how_the_assistant_writes_its_replies,
            Select(tone_labels, static_cast<std::size_t>(draft->tone), [](const StringResource& label) { return Text(label); })
              .Label(::app::strings::ui_reply_tone).OnChanged([draft](std::size_t index) {
                auto value = draft.Get();
                value.tone = static_cast<ReplyTone>(index);
                draft = std::move(value);
              }).Key("settings-reply-tone"), stack_fields, colors),
        Column{
            Text(::app::strings::ui_personal_instructions).Style({Font::System(14.0F).WithWeight(FontWeight::Medium), colors.on_surface}),
            ProvideEnvironment(fields, TextField(instructions).Label(::app::strings::ui_personal_instructions)
                .Variant(TextFieldVariant::Outlined)
                .Placeholder(::app::strings::ui_add_preferences_the_assistant_should_follow_when)
                .LineLimits(TextFieldLineLimits::MultiLine(3, 8)).OnChanged([draft, instructions](const TextEditingValue& text) {
                  instructions = text;
                  auto value = draft.Get();
                  value.instructions = text.text;
                  draft = std::move(value);
                }).Key("settings-instructions")),
        }.With(Padding(16.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }, colors));
    break;
  case SettingsSection::Appearance:
    content.push_back(SettingsPanel({
        SettingsRow(::app::strings::ui_theme, ::app::strings::ui_applies_as_soon_as_you_save,
            Select(appearance_labels, static_cast<std::size_t>(draft->appearance), [](const StringResource& label) { return Text(label); })
              .Label(::app::strings::ui_theme).OnChanged([draft](std::size_t index) {
                auto value = draft.Get();
                value.appearance = static_cast<PreviewAppearance>(index);
                draft = std::move(value);
              }).Key("settings-theme"), stack_fields, colors),
    }, colors));
    break;
  case SettingsSection::Storage:
    content.push_back(SettingsPanel({
        Column{
            Text(::app::strings::ui_preview_data_folder).Style({Font::System(14.0F).WithWeight(FontWeight::Medium), colors.on_surface}),
            Text(file.ParentPath().value_or(std::string{})).With(Foreground(colors.on_surface_variant)),
        }.With(Padding(16.0F), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }, colors));
    break;
  }

  const auto gutter = compact ? 14.0F : 24.0F;
  const auto settings_lane = [gutter](View view) {
    return Row{Column{std::move(view)}.With(Frame{.width = 760.0F}, CrossAlign(CrossAxisAlignment::Stretch)), Spacer()}
        .With(Padding(EdgeInsets::Symmetric(gutter, 0.0F)), CrossAlign(CrossAxisAlignment::Stretch));
  };
  auto content_area = ScrollView(settings_lane(Column(std::move(content)).With(Spacing(14.0F),
      CrossAlign(CrossAxisAlignment::Stretch), Padding(EdgeInsets{.top = 18.0F, .bottom = 20.0F})).Key("settings-content")))
      .With(Grow(), ScrollBar()).Key("settings-scroll");
  const auto primary = PreviewPrimaryButton(UseEnvironment<ButtonStyle>(), colors);
  auto status = !error->empty() ? Text(error.Get()).With(Foreground(colors.error)).With(Grow())
      : pending_restart ? Text(::app::strings::ui_restart_preview_to_apply_model_roles_and, TextRole::Label)
          .With(Foreground(colors.on_surface_variant)).With(Grow()) : View(Spacer().With(Grow()));
  return Column{
      settings_lane(Column{
          Row{Text(::app::strings::ui_settings, TextRole::Title)
                  .Style({Font::System(20.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}).With(Grow()),
              ProvideEnvironment(secondary, Button(::app::strings::ui_back_to_conversations).OnClick(std::move(close))).Key("close-settings"),
          }.With(Padding(EdgeInsets::Symmetric(0.0F, 10.0F)), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
          Flow(std::move(links)).With(Spacing(5.0F), Padding(EdgeInsets{.bottom = 8.0F})),
      }.With(CrossAlign(CrossAxisAlignment::Stretch))),
      ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
      std::move(content_area),
      ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
      settings_lane(Row{std::move(status), ProvideEnvironment(primary, Button(busy.Get() ? ::app::strings::ui_saving : ::app::strings::ui_save_settings)
              .OnClick(save).With(Enabled(!busy.Get() && draft.Get() != current.Get()))).Key("settings-save"),
      }.With(Padding(EdgeInsets::Symmetric(0.0F, 10.0F)), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)))
          .With(Background(colors.surface_container_low)),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch)).Key("preview-settings");
}

} // namespace agentkit_preview
