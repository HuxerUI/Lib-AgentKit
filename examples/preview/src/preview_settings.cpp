#include <app_resources.h>

#include "preview_settings.h"

#include "general_workflow.h"
#include "preview_theme.h"

#include <huxerui/huxerui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;

constexpr std::size_t instruction_limit = 4096;
const std::array appearance_labels{::app::strings::ui_dark, ::app::strings::ui_light, ::app::strings::ui_follow_system};
const std::array tone_labels{::app::strings::ui_default, ::app::strings::ui_concise, ::app::strings::ui_friendly, ::app::strings::ui_formal};

struct PaletteChoice {
  PreviewPalette palette;
  std::string_view id;
  StringResource label;
};

const std::array palettes{
    PaletteChoice{PreviewPalette::Graphite, "graphite", ::app::strings::ui_palette_graphite},
    PaletteChoice{PreviewPalette::Indigo, "indigo", ::app::strings::ui_palette_indigo},
    PaletteChoice{PreviewPalette::Forest, "forest", ::app::strings::ui_palette_forest},
    PaletteChoice{PreviewPalette::Sandstone, "sandstone", ::app::strings::ui_palette_sandstone},
};

enum class SettingsSection { Models, Personalization, Appearance, Permissions, Skills, Mcp, Storage };

struct SettingsCategory {
  SettingsSection section;
  StringResource title;
  const char* key;
};

const std::array categories{
    SettingsCategory{SettingsSection::Models, ::app::strings::ui_models, "settings-section-models"},
    SettingsCategory{SettingsSection::Personalization, ::app::strings::ui_personalization, "settings-section-personalization"},
    SettingsCategory{SettingsSection::Appearance, ::app::strings::ui_appearance, "settings-section-appearance"},
    SettingsCategory{SettingsSection::Permissions, ::app::strings::ui_permissions, "settings-section-permissions"},
    SettingsCategory{SettingsSection::Skills, ::app::strings::ui_skills, "settings-section-skills"},
    SettingsCategory{SettingsSection::Mcp, ::app::strings::ui_mcp, "settings-section-mcp"},
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

std::vector<McpServer> ReadMcpServers(const Json& records) {
  if (!records.is_array() || records.size() > 12) throw std::invalid_argument("Invalid MCP server list");
  std::vector<McpServer> servers;
  std::set<std::string> ids;
  for (const auto& record : records) {
    McpServer server{record.at("id").get<std::string>(), record.at("name").get<std::string>(),
        record.at("url").get<std::string>(), record.value("token_reference", std::string{}),
        record.value("enabled", true), record.value("legacy", false)};
    ValidateMcpServer(server);
    if (!ids.insert(server.id).second) throw std::invalid_argument("Duplicate MCP server identity");
    servers.push_back(std::move(server));
  }
  return servers;
}

View ModelRoleField(StringVariant label, std::vector<ModelChoice> choices, StringVariant fallback,
    State<PreviewSettings> draft, ModelRole role) {
  const auto selected = draft.Get().*role;
  std::vector<std::pair<std::optional<kit::ModelProviderRef>, StringVariant>> options{{std::nullopt, std::move(fallback)}};
  for (auto& item : choices) options.emplace_back(std::move(item.ref), std::move(item.label));
  auto found = std::ranges::find_if(options, [&](const auto& option) {
    return selected ? option.first && option.first->id == selected->id : !option.first;
  });
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

class SettingsFieldRow final : public Layout<SettingsFieldRow> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    auto& label = node.ChildAt(0);
    auto& control = node.ChildAt(1);
    const float lane = std::min(constraints.max_width, 820.0F);
    const float gap = std::min(20.0F, lane * 0.05F);
    const float control_width = std::min(282.0F, lane * 0.45F);
    const float label_width = lane - control_width - gap;
    const auto label_size = context.Measure(label, constraints.Loose().TightWidth(label_width));
    const auto control_size = context.Measure(control, constraints.Loose().TightWidth(control_width));
    const auto size = constraints.Constrain({lane, std::max(label_size.height, control_size.height)});
    return LayoutResult{}.SetSize(size)
        .Place(label, {0.0F, (size.height - label_size.height) / 2.0F})
        .Place(control, {label_width + gap, (size.height - control_size.height) / 2.0F});
  }
};

View SettingsRow(StringVariant label, StringVariant help, View control, bool stacked, const ColorScheme& colors) {
  auto copy = Column{
      Text(label).Style({Font::System(14.0F).WithWeight(FontWeight::Medium), colors.on_surface}),
      Text(help).Style({Font::System(13.0F), colors.on_surface_variant}),
  }.With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
  if (stacked) return Column{std::move(copy), std::move(control)}.With(Padding(EdgeInsets::Symmetric(0.0F, 14.0F)), Spacing(9.0F),
      CrossAlign(CrossAxisAlignment::Stretch));
  return SettingsFieldRow{std::move(copy), std::move(control)}.With(Padding(EdgeInsets::Symmetric(0.0F, 15.0F)));
}

View SettingsPanel(std::vector<View> rows, const ColorScheme& colors) {
  std::vector<View> content;
  for (auto& row : rows) {
    content.push_back(ProvideEnvironment(DividerStyle{colors.outline}, Divider()));
    content.push_back(std::move(row));
  }
  return Column(std::move(content)).With(CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View McpServerDialog(DialogContext dialog, McpServer initial, State<PreviewSettings> draft) {
  const auto name = UseState(TextEditingValue::FromText(initial.name));
  const auto endpoint = UseState(TextEditingValue::FromText(initial.url));
  const auto token = UseState(TextEditingValue::FromText(initial.token_reference));
  const auto legacy = UseState(initial.legacy);
  const auto error = UseState(std::string{});
  const auto& colors = UseTheme().colors;
  const auto style = UseEnvironment<ButtonStyle>();
  auto fields = UseEnvironment<TextFieldStyle>();
  fields.show_label = false;
  const auto save = [initial, name, endpoint, token, legacy, draft, dialog, error] {
    auto server = initial;
    server.name = TrimModelField(name->text);
    server.url = TrimModelField(endpoint->text);
    server.token_reference = TrimModelField(token->text);
    server.legacy = legacy.Get();
    try { ValidateMcpServer(server); }
    catch (const std::exception& invalid) { error = invalid.what(); return; }
    auto settings = draft.Get();
    auto existing = std::ranges::find(settings.mcp_servers, server.id, &McpServer::id);
    if (existing == settings.mcp_servers.end()) settings.mcp_servers.push_back(std::move(server));
    else *existing = std::move(server);
    draft = std::move(settings);
    dialog.Dismiss();
  };
  const auto field = [&colors](StringResource label, State<TextEditingValue> value, std::string key) {
    return PreviewLabeledField(label, TextField(value).Label(label).Variant(TextFieldVariant::Outlined)
        .OnChanged([value](const TextEditingValue& next) { value = next; }).Key(std::move(key)), colors);
  };
  auto body = Column{
      field(::app::strings::ui_mcp_name, name, "mcp-server-name"),
      field(::app::strings::ui_mcp_endpoint, endpoint, "mcp-server-url"),
      field(::app::strings::ui_mcp_token, token, "mcp-server-token"),
      Checkbox(::app::strings::ui_mcp_legacy, legacy.Get()).OnChanged([legacy](bool value) { legacy = value; }).Key("mcp-server-legacy"),
      error->empty() ? View{} : Text(error.Get()).With(Foreground(colors.error)),
  }.With(Spacing(10.0F), Padding(EdgeInsets::Symmetric(20.0F, 15.0F)), CrossAlign(CrossAxisAlignment::Stretch));
  auto actions = Row{Spacer(),
      ProvideEnvironment(PreviewSecondaryButton(style, colors), Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); })),
      ProvideEnvironment(PreviewPrimaryButton(style, colors), Button(::app::strings::ui_mcp_save).OnClick(save)
          .With(Enabled(!name->text.empty() && !endpoint->text.empty())).Key("mcp-server-save")),
  }.With(Spacing(8.0F));
  return ProvideEnvironment(fields, PreviewFormDialog(initial.name.empty() ? ::app::strings::ui_mcp_add : ::app::strings::ui_mcp_edit,
      std::move(body), std::move(actions), colors, 500.0F)).Key("mcp-server-dialog");
}

[[huxerui::composable]]
View SettingsCategoryLink(SettingsCategory category, State<SettingsSection> active) {
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto select = [active, section = category.section] { active = section; };
  const bool selected = active.Get() == category.section;
  return Column{
      Text(category.title).Style({Font::System(13.0F).WithWeight(selected ? FontWeight::SemiBold : FontWeight::Medium),
          selected ? colors.on_surface : colors.on_surface_variant})
          .With(Padding(EdgeInsets::Symmetric(9.0F, 8.0F))),
      Column{}.With(Frame{.height = 2.0F}, CornerRadius(1.0F),
          Background(selected ? colors.primary : Color::Transparent())),
  }
      .OnClick(select).On<ViewEvents::KeyDown>([select](const KeyEvent& event) {
        if (event.key != Key::Enter && event.key != Key::Space) return false;
        select();
        return true;
      })
      .With(Focusable{}, CrossAlign(CrossAxisAlignment::Stretch),
          Indication{.focus = theme.interactions.indication.focus},
          PointerCursor(PointerCursorKind::Hand),
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
    const auto palette = doc.value("palette", std::string("graphite"));
    const auto choice = std::ranges::find(palettes, palette, &PaletteChoice::id);
    const auto tone = doc.value("tone", std::size_t{});
    auto instructions = doc.value("instructions", std::string{});
    if (appearance >= appearance_labels.size() || tone >= tone_labels.size() || instructions.size() > instruction_limit) {
      throw std::invalid_argument("Invalid Preview settings");
    }
    const auto generation = doc.find("image_generation_model");
    const auto image_generation_model = generation != doc.end() && generation->is_string()
        ? std::nullopt : ReadModel(doc, "image_generation_model");
    LocalFilePermissions file_access;
    if (const auto access = doc.find("file_access"); access != doc.end()) {
      file_access.full_access = access->at("full_access").get<bool>();
      file_access.directories = access->at("directories").get<std::vector<std::string>>();
      file_access.revision = access->at("revision").get<std::string>();
      if (file_access.revision.empty() || file_access.directories.size() > 64)
        throw std::invalid_argument("Invalid file access settings");
    }
    co_return Result::Success({.appearance = static_cast<PreviewAppearance>(appearance),
        .palette = choice == palettes.end() ? PreviewPalette::Graphite : choice->palette, .tone = static_cast<ReplyTone>(tone),
        .instructions = std::move(instructions), .daily_model = ReadModel(doc, "daily_model"),
        .image_model = ReadModel(doc, "image_model"), .review_model = ReadModel(doc, "review_model"),
        .image_generation_model = image_generation_model, .file_access = std::move(file_access),
        .mcp_servers = ReadMcpServers(doc.value("mcp_servers", Json::array()))});
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
  const auto palette = std::ranges::find(palettes, settings.palette, &PaletteChoice::palette);
  auto mcp_servers = Json::array();
  for (const auto& server : settings.mcp_servers)
    mcp_servers.push_back({{"id", server.id}, {"name", server.name}, {"url", server.url},
        {"token_reference", server.token_reference}, {"enabled", server.enabled}, {"legacy", server.legacy}});
  auto contents = Json{{"appearance", static_cast<std::size_t>(settings.appearance)},
      {"palette", palette == palettes.end() ? palettes.front().id : palette->id},
      {"tone", static_cast<std::size_t>(settings.tone)}, {"instructions", std::move(settings.instructions)},
      {"daily_model", ModelRecord(settings.daily_model)}, {"image_model", ModelRecord(settings.image_model)},
      {"review_model", ModelRecord(settings.review_model)},
      {"image_generation_model", ModelRecord(settings.image_generation_model)}, {"mcp_servers", std::move(mcp_servers)}, {"file_access", {
        {"full_access", settings.file_access.full_access}, {"directories", std::move(settings.file_access.directories)},
        {"revision", std::move(settings.file_access.revision)}}}}.dump();
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

ThemeSpec PreviewThemeFor(const PreviewSettings& settings, ThemeBrightness system) {
  const auto brightness = settings.appearance == PreviewAppearance::System ? system :
      settings.appearance == PreviewAppearance::Light ? ThemeBrightness::Light : ThemeBrightness::Dark;
  return PreviewTheme(settings.palette, brightness);
}

[[huxerui::composable]]
View PreviewSettingsPage(State<PreviewSettings> current, State<PreviewSettings> applied,
    std::shared_ptr<RunSettings> models, File file, AddModelProfile add_model, EditModelProfile edit_model, std::function<void()> close,
    std::function<kit::Async<void>(LocalFilePermissions)> apply_file_access, std::shared_ptr<ImportedSkillSource> imported_skills,
    std::shared_ptr<McpConnections> mcp, std::shared_ptr<kit::AgentRuntime> runtime, State<bool> busy) {
  const auto baseline = current.Get();
  const auto draft = UseState(baseline);
  const auto draft_baseline = UseState(baseline);
  const auto instructions = UseState(TextEditingValue::FromText(baseline.instructions));
  const auto error = UseState(std::string{});
  const auto extensions_changed = UseState(std::size_t{0});
  const auto section = UseState(SettingsSection::Models);
  const auto dialog = UseDialog();
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseApplicationTaskScope();
  Lifecycle([baseline, draft_baseline, draft, instructions] {
    if (draft.Get() == draft_baseline.Get()) {
      draft = baseline;
      instructions = TextEditingValue::FromText(baseline.instructions);
    }
    draft_baseline = baseline;
    return [] {};
  }, baseline);
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto viewport = UseViewportClass();
  const bool compact = viewport == ViewportClass::Compact;
  const bool stack_fields = viewport != ViewportClass::Expanded;
  auto fields = UseEnvironment<TextFieldStyle>();
  fields.show_label = false;
  const auto secondary = PreviewSecondaryButton(UseEnvironment<ButtonStyle>(), colors);
  const auto save = [current, draft, file, tasks, error, busy, apply_file_access, mcp] {
    if (busy.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([current, draft, file, value = draft.Get(), busy, error, apply_file_access, mcp]() mutable -> Task<void> {
      const bool access_changed = value.file_access != current->file_access;
      if (access_changed) value.file_access.revision = kit::NewCommandId().value;
      auto saved = co_await SavePreviewSettings(file, value);
      if (saved.Succeeded()) {
        if (mcp && value.mcp_servers != current->mcp_servers) {
          auto updated = co_await mcp->Apply(value.mcp_servers);
          if (!updated.Succeeded()) error = updated.Error().message;
        }
        if (access_changed) {
          auto updated = co_await apply_file_access(value.file_access);
          if (!updated.Succeeded()) error = updated.Error().message;
          auto current_draft = draft.Get();
          current_draft.file_access.revision = value.file_access.revision;
          draft = std::move(current_draft);
        }
        current = std::move(value);
      } else error = saved.Error().message;
      busy = false;
    });
  };
  const auto pending_restart = current->daily_model != applied->daily_model ||
      current->review_model != applied->review_model || current->tone != applied->tone ||
      current->instructions != applied->instructions;
  std::vector<View> content;
  std::vector<View> links;
  for (const auto& category : categories) {
    links.push_back(SettingsCategoryLink(category, section).Key(category.key));
    if (category.section != section.Get()) continue;
    auto title = Text(category.title, TextRole::Title)
        .Style({Font::System(22.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}).Key("settings-section-title");
    if (category.section == SettingsSection::Models && add_model) {
      auto action_style = UseEnvironment<ChipStyle>();
      action_style.background = Color::Transparent();
      action_style.border = Border{Color::Transparent()};
      action_style.label_style = {Font::System(13.0F).WithWeight(FontWeight::Medium), colors.primary};
      action_style.corner_radii = CornerRadii{7.0F};
      action_style.minimum_height = 34.0F;
      action_style.icon_size = 16.0F;
      action_style.icon_spacing = 5.0F;
      action_style.indication = theme.interactions.indication;
      action_style.indication->ripple.reset();
      content.push_back(Row{std::move(title).With(Grow()), ProvideEnvironment(action_style,
          Chip(::app::images::add, ::app::strings::ui_add_model).OnClick([dialog, add_model] {
            ShowAddModel(dialog, add_model, {});
          }).Key("settings-add-model"))}.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)));
    } else {
      content.push_back(std::move(title));
    }
  }
  switch (section.Get()) {
  case SettingsSection::Models: {
    auto choices = models->Models(GeneralAgent);
    std::erase_if(choices, [](const auto& item) { return item.ref == DemoModel; });
    auto vision = choices;
    std::erase_if(vision, [](const auto& item) { return !item.capabilities.image_input; });
    auto generation = choices;
    std::erase_if(generation, [](const auto& item) { return !item.editable; });
    std::vector<View> catalog;
    if (choices.empty()) catalog.push_back(Text(::app::strings::ui_no_models_configured)
        .Style({Font::System(13.0F), colors.on_surface_variant}).With(Padding(EdgeInsets::Symmetric(0.0F, 13.0F))));
    const auto token_display = [unknown = UseString(::app::strings::ui_model_limit_unknown)](auto value) {
      return value ? std::to_string(*value) : unknown;
    };
    for (const auto& model : choices) {
      const auto& limits = model.limits;
      auto details = Column {
        Text(model.label).Style({Font::System(13.0F), colors.on_surface})
            .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Tooltip(model.endpoint + " / " + model.connection.protocol)),
        Text(StringVariant::Format(::app::strings::ui_model_limit_details, token_display(limits.context_window),
            token_display(limits.max_input_tokens), token_display(limits.max_output_tokens)), TextRole::Label)
            .With(Foreground(colors.on_surface_variant)),
      }.With(Spacing(3.0F), Grow());
      std::vector<View> row{std::move(details)};
      if (edit_model && model.editable) row.push_back(ProvideEnvironment(secondary, Button(::app::strings::ui_edit_model)
          .OnClick([dialog, model, edit_model] { ShowEditModel(dialog, model, edit_model); }).Key("settings-edit-model/" + model.ref.id.value)));
      catalog.push_back(Row(std::move(row)).With(Padding(EdgeInsets::Symmetric(0.0F, 10.0F)),
          Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)).Key(model.ref.id.value));
    }
    content.push_back(SettingsPanel(std::move(catalog), colors).Key("settings-model-list"));
    content.push_back(Text(::app::strings::ui_model_roles).Style({Font::System(15.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}));
    content.push_back(SettingsPanel({
        SettingsRow(::app::strings::ui_daily_model, ::app::strings::ui_default_for_new_conversations,
            ModelRoleField(::app::strings::ui_daily_model, choices, ::app::strings::ui_automatic, draft, &PreviewSettings::daily_model).Key("settings-daily-model"),
            stack_fields, colors),
        SettingsRow(::app::strings::ui_image_understanding, ::app::strings::ui_choose_a_model_that_accepts_images,
            ModelRoleField(::app::strings::ui_image_understanding, vision, ::app::strings::ui_use_the_selected_chat_model, draft, &PreviewSettings::image_model)
                .Key("settings-image-model"), stack_fields, colors),
        SettingsRow(::app::strings::ui_image_generation, ::app::strings::ui_image_generation_help,
            ModelRoleField(::app::strings::ui_image_generation, generation, ::app::strings::ui_image_generation_off,
                draft, &PreviewSettings::image_generation_model).Key("settings-image-generation-model"), stack_fields, colors),
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
                .LineLimits(TextFieldLineLimits::MultiLine(6, 12)).OnChanged([draft, instructions](const TextEditingValue& text) {
                  instructions = text;
                  auto value = draft.Get();
                  value.instructions = text.text;
                  draft = std::move(value);
                }).Key("settings-instructions")),
        }.With(Padding(EdgeInsets::Symmetric(0.0F, 15.0F)), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }, colors));
    break;
  case SettingsSection::Appearance:
    content.push_back(SettingsPanel({
        SettingsRow(::app::strings::ui_theme_brightness, ::app::strings::ui_applies_as_soon_as_you_save,
            Select(appearance_labels, static_cast<std::size_t>(draft->appearance), [](const StringResource& label) { return Text(label); })
              .Label(::app::strings::ui_theme_brightness).OnChanged([draft](std::size_t index) {
                auto value = draft.Get();
                value.appearance = static_cast<PreviewAppearance>(index);
                draft = std::move(value);
              }).Key("settings-theme"), stack_fields, colors),
        SettingsRow(::app::strings::ui_theme_palette, ::app::strings::ui_theme_palette_help,
            Select(palettes, static_cast<std::size_t>(draft->palette), [](const PaletteChoice& choice) {
                const auto light = PreviewTheme(choice.palette, ThemeBrightness::Light).colors;
                const auto dark = PreviewTheme(choice.palette, ThemeBrightness::Dark).colors;
                return Row{
                    Column{}.With(Frame{.width = 11.0F, .height = 11.0F}, CornerRadius(3.0F), Background(light.primary)),
                    Column{}.With(Frame{.width = 11.0F, .height = 11.0F}, CornerRadius(3.0F), Background(dark.primary)),
                    Text(choice.label),
                }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center), Semantics{.label = choice.label});
              }).Label(::app::strings::ui_theme_palette).OnChanged([draft](std::size_t index) {
                auto value = draft.Get();
                value.palette = palettes.at(index).palette;
                draft = std::move(value);
              }).Key("settings-palette"), stack_fields, colors),
    }, colors));
    break;
  case SettingsSection::Permissions: {
    const auto revise = [draft](bool full_access) {
      auto value = draft.Get();
      value.file_access.full_access = full_access;
      draft = std::move(value);
    };
    auto access = Checkbox(::app::strings::ui_local_full_access, draft->file_access.full_access)
        .OnChanged([dialog, revise](bool enabled) {
          if (!enabled) { revise(false); return; }
          ShowPreviewDialog(dialog, DialogFactory{[revise](DialogContext dialog) {
            const auto color = UseTheme().colors;
            const auto style = UseEnvironment<ButtonStyle>();
            return PreviewFormDialog(::app::strings::ui_local_full_access,
                Text(::app::strings::ui_local_full_access_confirmation).With(Padding(EdgeInsets::Symmetric(20.0F, 14.0F))),
                Row{Spacer(), ProvideEnvironment(PreviewSecondaryButton(style, color),
                    Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); })),
                    ProvideEnvironment(PreviewPrimaryButton(style, color),
                    Button(::app::strings::ui_allow_full_access).OnClick([dialog, revise] { revise(true); dialog.Dismiss(); }))}
                    .With(Spacing(8.0F)), color, 510.0F);
          }});
        }).With(Enabled(!busy.Get())).Key("settings-local-full-access");
    const auto add_directory = [draft, busy, error, tasks, picker] {
      if (busy.Get() || draft->file_access.directories.size() >= 64) return;
      busy = true;
      tasks.Launch([draft, busy, error, picker]() -> Task<void> {
        const auto picked = co_await picker->OpenDirectoryAsync(true);
        busy = false;
        if (!picked) co_return;
        const auto file = picked->AsFile();
        if (!file) co_return;
        auto directory = LocalDirectory(*file);
        if (!directory.Succeeded()) { error = directory.Error().message; co_return; }
        auto value = draft.Get();
        if (std::ranges::find(value.file_access.directories, directory.Value()) == value.file_access.directories.end())
          value.file_access.directories.push_back(std::move(directory).Value());
        draft = std::move(value);
      });
    };
    std::vector<View> rows{SettingsRow(::app::strings::ui_local_full_access, ::app::strings::ui_local_full_access_help,
        std::move(access), stack_fields, colors)};
    rows.push_back(SettingsRow(::app::strings::ui_authorized_directories, ::app::strings::ui_authorized_directories_help,
        Row{ProvideEnvironment(secondary, Button(::app::strings::ui_add_directory).OnClick(add_directory)
            .With(Enabled(!busy.Get() && draft->file_access.directories.size() < 64)).Key("settings-add-authorized-directory"))},
        stack_fields, colors));
    for (const auto& directory : draft->file_access.directories) {
      rows.push_back(Row{Text(directory).LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow(), Tooltip(directory)),
          ProvideEnvironment(secondary, Button(::app::strings::ui_revoke_directory).OnClick([draft, directory] {
            auto value = draft.Get();
            std::erase(value.file_access.directories, directory);
            draft = std::move(value);
          }).With(Enabled(!busy.Get())))}
          .With(Padding(EdgeInsets::Symmetric(0.0F, 8.0F)), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)).Key(directory));
    }
    content.push_back(SettingsPanel(std::move(rows), colors).Key("settings-file-access"));
    break;
  }
  case SettingsSection::Skills: {
    static_cast<void>(extensions_changed.Get());
    const auto import = [imported_skills, runtime, picker, tasks, busy, error, extensions_changed] {
      if (!imported_skills || !picker || busy.Get()) return;
      busy = true;
      error = "";
      tasks.Launch([imported_skills, runtime, picker, busy, error, extensions_changed]() -> Task<void> {
        const auto picked = co_await picker->OpenDirectoryAsync();
        if (picked) {
          if (const auto folder = picked->AsFile()) {
            auto result = co_await imported_skills->Import(*folder);
            if (result.Succeeded()) result = co_await runtime->RefreshAccessAsync();
            if (!result.Succeeded()) error = result.Error().message;
            extensions_changed = extensions_changed.Get() + 1;
          }
        }
        busy = false;
      });
    };
    content.push_back(SettingsPanel({
        SettingsRow(::app::strings::ui_skills, ::app::strings::ui_skill_import_help,
            ProvideEnvironment(secondary, Button(::app::strings::ui_skill_import).OnClick(import)
                .With(Enabled(imported_skills && picker && picker->CanOpenDirectories() && !busy.Get()))).Key("settings-import-skill"),
            stack_fields, colors),
        Column{Text(::app::strings::ui_release_presentation), Text(::app::strings::ui_skill_builtin, TextRole::Label)
            .With(Foreground(colors.on_surface_variant))}
            .With(Padding(EdgeInsets::Symmetric(0.0F, 12.0F)), Spacing(4.0F)),
    }, colors));
    if (!imported_skills) break;
    std::vector<View> rows;
    for (const auto& entry : imported_skills->Entries()) {
      rows.push_back(SettingsRow(entry.summary.name, entry.summary.description,
          Checkbox(::app::strings::ui_extension_enabled, entry.enabled)
              .OnChanged([imported_skills, runtime, id = entry.summary.ref.id, tasks, busy, error, extensions_changed](bool on) {
                if (busy.Get()) return;
                busy = true;
                error = "";
                tasks.Launch([imported_skills, runtime, id, busy, error, extensions_changed, on]() -> Task<void> {
                  auto result = co_await imported_skills->SetEnabled(id, on);
                  if (result.Succeeded()) result = co_await runtime->RefreshAccessAsync();
                  if (!result.Succeeded()) error = result.Error().message;
                  extensions_changed = extensions_changed.Get() + 1;
                  busy = false;
                });
              }).With(Enabled(!busy.Get())), stack_fields, colors).Key(entry.summary.ref.id.value));
    }
    if (!rows.empty()) content.push_back(SettingsPanel(std::move(rows), colors).Key("settings-imported-skills"));
    break;
  }
  case SettingsSection::Mcp: {
    static_cast<void>(extensions_changed.Get());
    const auto open = [dialog, draft](McpServer server) {
      ShowPreviewDialog(dialog, DialogFactory{[server = std::move(server), draft](DialogContext context) {
        return McpServerDialog(std::move(context), server, draft);
      }});
    };
    std::vector<View> rows{SettingsRow(::app::strings::ui_mcp, ::app::strings::ui_mcp_help,
        ProvideEnvironment(secondary, Button(::app::strings::ui_mcp_add).OnClick([open] {
          open({.id = kit::NewCommandId().value});
        }).With(Enabled(mcp && !busy.Get() && draft->mcp_servers.size() < 12))).Key("settings-add-mcp"),
        stack_fields, colors)};
    for (const auto& server : draft->mcp_servers) {
      const auto status = mcp ? mcp->ConnectionStatus(server) : std::nullopt;
      const auto status_text = !server.enabled ? StringVariant{::app::strings::ui_extension_disabled} :
          !status ? StringVariant{::app::strings::ui_mcp_pending_restart} :
          status->error.empty() ? StringVariant::Format(::app::strings::ui_mcp_tools_available, std::to_string(status->tools)) :
          StringVariant{status->error};
      const auto change = [draft, id = server.id](std::optional<bool> enabled) {
        auto value = draft.Get();
        const auto found = std::ranges::find(value.mcp_servers, id, &McpServer::id);
        if (found == value.mcp_servers.end()) return;
        if (enabled) found->enabled = *enabled;
        else value.mcp_servers.erase(found);
        draft = std::move(value);
      };
      auto buttons = Flow{
          Checkbox(::app::strings::ui_extension_enabled, server.enabled).OnChanged([change](bool enabled) { change(enabled); }),
          Button(::app::strings::ui_mcp_edit).OnClick([open, server] { open(server); }),
          Button(::app::strings::ui_mcp_refresh).OnClick([mcp, id = server.id, tasks, busy, error, extensions_changed] {
            if (!mcp || busy.Get()) return;
            busy = true;
            error = "";
            tasks.Launch([mcp, id, busy, error, extensions_changed]() -> Task<void> {
              auto refreshed = co_await mcp->Refresh(id);
              if (!refreshed.Succeeded()) error = refreshed.Error().message;
              extensions_changed = extensions_changed.Get() + 1;
              busy = false;
            });
          }).With(Enabled(status.has_value() && server.enabled)),
          Button(::app::strings::ui_mcp_remove).OnClick([change] { change({}); }),
      }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center), Enabled(!busy.Get()));
      rows.push_back(Column{
          Text(server.name).Style({Font::System(14.0F).WithWeight(FontWeight::Medium), colors.on_surface}),
          Text(server.url, TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis),
          Text(status_text, TextRole::Label).With(Foreground(status && !status->error.empty() ? colors.error : colors.on_surface_variant)),
          ProvideEnvironment(secondary, std::move(buttons)),
      }.With(Spacing(4.0F), Padding(EdgeInsets::Symmetric(0.0F, 12.0F)), CrossAlign(CrossAxisAlignment::Stretch)).Key(server.id));
    }
    content.push_back(SettingsPanel(std::move(rows), colors).Key("settings-mcp-servers"));
    break;
  }
  case SettingsSection::Storage:
    content.push_back(SettingsPanel({
        Column{
            Text(::app::strings::ui_preview_data_folder).Style({Font::System(14.0F).WithWeight(FontWeight::Medium), colors.on_surface}),
            Text(file.ParentPath().value_or(std::string{})).With(Foreground(colors.on_surface_variant)),
        }.With(Padding(EdgeInsets::Symmetric(0.0F, 15.0F)), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }, colors));
    break;
  }

  const auto gutter = compact ? 14.0F : 24.0F;
  const auto settings_lane = [gutter](View view) {
    return Row{Column{std::move(view)}.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch))}
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
          Row{PreviewBackAction(::app::strings::ui_back_to_conversations, std::move(close), theme).Key("close-settings"),
              Text(::app::strings::ui_settings, TextRole::Title)
                  .Style({Font::System(20.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}).With(Grow()).Key("settings-title"),
          }.With(Padding(EdgeInsets::Symmetric(0.0F, 10.0F)), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
          Flow(std::move(links)).With(Spacing(9.0F)),
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
