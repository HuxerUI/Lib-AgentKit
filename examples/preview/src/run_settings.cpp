#include <app_resources.h>

#include "run_settings.h"

#include "code_tools.h"
#include "preview_json.h"
#include "preview_theme.h"
#include "report_chart.h"

#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using namespace std::chrono_literals;
constexpr std::string_view full_access_prefix = "preview.full/";
constexpr std::string_view model_slot_prefix = "preview.profile/";
constexpr auto separator = "  \xc2\xb7  ";

View RunOptionGlyph(const ImageResource& icon, Color ink) {
  return Image(icon).Tint(ink).With(Frame{.width = 14.0F, .height = 14.0F});
}

StringVariant ProfileLabel(const std::optional<kit::ExecutionProfileRef>& profile) {
  if (!profile || *profile == AskProfile) return ::app::strings::ui_ask;
  if (*profile == ReviewProfile) return ::app::strings::ui_auto_review;
  if (profile->id.value.starts_with(full_access_prefix)) return ::app::strings::ui_full_access;
  return ::app::strings::ui_unavailable_permissions;
}

kit::Revision ReviewModelRevision(const kit::ModelProviderRef& model) {
  // Persist a stable configuration revision without publishing the endpoint in the review record.
  auto revision = 14695981039346656037ULL;
  for (const unsigned char byte : model.id.value) {
    revision = (revision ^ byte) * 1099511628211ULL;
  }
  revision = (revision ^ model.version) * 1099511628211ULL;
  return revision == 0 ? 1 : revision;
}

kit::ToolReviewResult ReviewDecision(const kit::JsonValue& output) {
  const auto* fields = std::get_if<kit::JsonValue::Object>(&output.value);
  if (fields && fields->size() == 1 && fields->begin()->first == "decision") {
    const auto* decision = std::get_if<std::string>(&fields->begin()->second.value);
    if (decision && *decision == "approve") return {kit::ToolReviewDecision::Approve, "Review model approved this new chart CSV"};
    if (decision && *decision == "deny") return {kit::ToolReviewDecision::Deny, "Review model denied this chart export"};
    if (decision && *decision == "defer") return {kit::ToolReviewDecision::DeferToUser, "Review model requested manual confirmation"};
  }
  return {kit::ToolReviewDecision::DeferToUser, "Review model returned an invalid decision"};
}

kit::Async<kit::ToolReviewResult> ReviewChartWithModel(const kit::ModelRegistration& model,
                                                     const kit::ToolReviewRequest& request) {
  using Result = kit::AgentResult<kit::ToolReviewResult>;
  using Json = nlohmann::json;
  const auto fallback = Result::Success({kit::ToolReviewDecision::DeferToUser, "Review model did not complete a decision"});
  if (request.stop.stop_requested() || !model.provider->Capabilities().structured_output) co_return fallback;
  const auto facts = DecodeJson(Json{{"operation", "create_chart_csv"}, {"target_scope", "local_export_folder"},
                                     {"target_count", 1}, {"overwrite", false}});
  const kit::JsonSchema schema{DecodeJson(Json::parse(R"({
    "type": "object",
    "additionalProperties": false,
    "required": ["decision"],
    "properties": {"decision": {"type": "string", "enum": ["approve", "deny", "defer"]}}
  })"))};
  kit::ModelTurnRequest turn{.turn = {kit::NewCommandId().value},
      .run = request.invocation.run, .provider = model.ref, .allow_server_state = false,
      .instructions = "Review this prepared export. Approve only the creation of a single new chart CSV in the local export folder. "
                      "Deny a plan outside this scope and defer if the facts are insufficient. Return only the structured decision.",
      .items = {kit::ModelContent{kit::ModelContent::Role::User, facts}}, .output_schema = schema,
      .deadline = request.deadline, .stop = request.stop};
  auto started = co_await model.provider->StartTurnAsync(std::move(turn));
  if (!started.Succeeded() || !started.Value()) co_return fallback;
  auto stream = std::move(started).Value();
  std::optional<kit::JsonValue> output;
  bool completed = false;
  for (int events = 0; events < 256 && !request.stop.stop_requested(); ++events) {
    auto read = co_await stream->ReadAsync();
    if (!read.Succeeded() || request.stop.stop_requested()) break;
    if (!read.Value()) {
      if (completed && output) co_return Result::Success(ReviewDecision(*output));
      co_return fallback;
    }
    if (completed) break;
    auto& event = *read.Value();
    if (const auto* finished = std::get_if<kit::ModelFinished>(&event)) {
      if (finished->reason != kit::ModelFinishReason::Completed) break;
      completed = true;
    } else if (auto* structured = std::get_if<kit::ModelStructuredOutput>(&event)) {
      if (output) break;
      output = std::move(structured->value);
    } else if (!std::holds_alternative<kit::ModelTextDelta>(event) && !std::holds_alternative<kit::ModelUsage>(event)) {
      break;
    }
  }
  (void)co_await stream->StopAsync();
  co_return fallback;
}

} // namespace

RunSettings::RunSettings(std::function<kit::TimePoint()> clock)
    : clock_(clock ? std::move(clock) : [] { return std::chrono::time_point_cast<kit::Duration>(std::chrono::system_clock::now()); }) {}

void RunSettings::AddModel(const kit::ModelRegistration& registration, std::string label) {
  if (std::ranges::find(models_, registration.ref, &ModelChoice::ref) != models_.end()) return;
  const auto& endpoint = registration.endpoint;
  auto name = label.empty() ? endpoint.model : label;
  if (label.empty()) label = name + separator + endpoint.provider + " / " + endpoint.protocol;
  if (std::string_view identity = registration.ref.id.value; identity.starts_with(model_slot_prefix)) {
    identity.remove_prefix(model_slot_prefix.size());
    auto profile = std::string(separator) + std::string(identity.substr(0, identity.find('/')));
    label += profile;
    name += profile;
  }
  const auto caps = registration.provider->Capabilities();
  models_.push_back({registration.ref, std::move(label), std::move(name), registration.ref != DemoModel,
                     caps.text_stream && caps.structured_output, caps});
}

void RunSettings::SetDefaults(kit::ModelProviderRef general, std::optional<kit::ModelProviderRef> code) {
  general_ = std::move(general);
  code_ = std::move(code);
}

std::optional<kit::ModelProviderRef> RunSettings::DefaultModel(const kit::AgentId& agent) const {
  return agent == CodeAgent ? code_ : std::optional{general_};
}

std::vector<ModelChoice> RunSettings::Models(const kit::AgentId& agent) const {
  std::vector<ModelChoice> result;
  std::ranges::copy_if(models_, std::back_inserter(result), [&](const ModelChoice& choice) {
    return choice.suitable && (agent != CodeAgent || choice.code);
  });
  return result;
}

StringVariant RunSettings::ModelLabel(const std::optional<kit::ModelProviderRef>& ref) const {
  if (ref == DemoModel) return ::app::strings::ui_local_demo_model;
  const auto found = ref ? std::ranges::find(models_, *ref, &ModelChoice::ref) : models_.end();
  return found == models_.end() ? StringVariant{::app::strings::ui_unavailable_model} : StringVariant{found->label};
}

kit::ModelCapabilities RunSettings::Capabilities(const std::optional<kit::ModelProviderRef>& ref,
    const kit::AgentId& agent) const {
  const auto selected = ref ? ref : DefaultModel(agent);
  const auto found = selected ? std::ranges::find(models_, *selected, &ModelChoice::ref) : models_.end();
  return found == models_.end() ? kit::ModelCapabilities{} : found->capabilities;
}

std::string RunSettings::Describe(const kit::RunStartBinding& binding, const Resources& resources) const {
  return resources.GetString(ModelLabel(binding.model)) + separator + resources.GetString(ProfileLabel(binding.execution_profile));
}

bool RunSettings::NeedsRenewal(const kit::ExecutionProfileRef& profile, const std::optional<kit::WorkspaceId>& workspace) const {
  if (!profile.id.value.starts_with(full_access_prefix)) return false;
  const auto grant = full_access_.find(profile.id);
  return grant == full_access_.end() || profile.version != 1 || workspace != grant->second.workspace || clock_() >= grant->second.expires;
}

kit::ExecutionProfileRef RunSettings::GrantFullAccess(kit::WorkspaceId workspace) {
  kit::ExecutionProfileId id{std::string(full_access_prefix) + kit::NewCommandId().value};
  full_access_.emplace(id, FullAccessGrant{std::move(workspace), clock_() + 30min});
  return {std::move(id), 1};
}

kit::AgentResult<kit::PolicyDecision> RunSettings::Decide(const kit::PolicyRequest& request, bool needs_confirmation,
                                                        bool workspace_available) const {
  using Result = kit::AgentResult<kit::PolicyDecision>;
  const auto profile = request.run_configuration ? request.run_configuration->execution_profile : std::nullopt;
  auto decision = needs_confirmation ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow;
  if (request.capability == kit::Capability::UseRunConfiguration && request.agent && request.agent->id == CodeAgent &&
      request.run_configuration && request.run_configuration->model == DemoModel) {
    return Result::Failure({.code = kit::ErrorCode::Unsupported, .message = "Choose a configured model for the code assistant"});
  }
  if (!profile || *profile == AskProfile) return Result::Success({decision, {}});
  if (request.actor.principal != PreviewUser) {
    return Result::Failure({.code = kit::ErrorCode::PermissionDenied, .message = "Permissions belong to another user"});
  }
  if (*profile == ReviewProfile) {
    if (needs_confirmation && request.tool->id == ExportReportChart) decision = kit::AccessDecision::AllowReview;
    return Result::Success({decision, {}});
  }
  if (!profile->id.value.starts_with(full_access_prefix)) {
    return Result::Failure({.code = kit::ErrorCode::Conflict, .message = "Permission preset changed. Choose permissions again."});
  }
  const auto grant = full_access_.find(profile->id);
  if (grant == full_access_.end() || profile->version != 1 || request.workspace != grant->second.workspace ||
      !request.agent || request.agent->id != CodeAgent) {
    return Result::Failure({.code = kit::ErrorCode::Conflict, .message = "Full Access is unavailable for this task. Choose permissions again."});
  }
  if (clock_() >= grant->second.expires) {
    return Result::Failure({.code = kit::ErrorCode::DeadlineExceeded, .message = "Full Access expired. Choose permissions again."});
  }
  if (!workspace_available) {
    return Result::Failure({.code = kit::ErrorCode::PermissionDenied, .message = "Full Access workspace is unavailable"});
  }
  if (needs_confirmation && request.tool->id == ApplyCodePatch && std::ranges::none_of(request.sources, [](const auto& slice) {
        const auto* resource = std::get_if<kit::ResourceRef>(&slice.source);
        return resource && resource->version.starts_with(CodeEditorVersionPrefix);
      })) decision = kit::AccessDecision::Allow;
  return Result::Success({decision, {}});
}

kit::ToolReviewer ChartExportReviewer(File export_folder, std::optional<kit::AgentResult<kit::ModelRegistration>> model) {
  const auto id = model ? "preview.report-chart.model/v1" : "preview.report-chart";
  const auto version = model && model->Succeeded() ? ReviewModelRevision(model->Value().ref) : 1;
  const auto timeout = model ? 15s : 3s;
  return {id, version, timeout, [export_folder = std::move(export_folder), model = std::move(model)](kit::ToolReviewRequest request)
      -> kit::Async<kit::ToolReviewResult> {
    const auto& call = request.invocation;
    if (request.stop.stop_requested() || call.tool.id != ExportReportChart || call.tool.version != 1 ||
        call.effect != kit::ToolEffect::WriteResource || call.prepared_targets.size() != 1 || call.prepared_targets.front().expected) {
      co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::DeferToUser, "Check the export plan"});
    }
    const File target(call.prepared_targets.front().target.value);
    if (target.ParentPath() != export_folder.Path()) {
      co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::Deny, "Export target is outside the local export folder"});
    }
    if (model) {
      if (!model->Succeeded()) {
        co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::DeferToUser, model->Error().message});
      }
      co_return co_await ReviewChartWithModel(model->Value(), request);
    }
    co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::Approve, "New chart CSV in the local export folder"});
  }};
}

namespace {

struct ModelApiFormat {
  const char* label;
  const char* protocol;
  const char* provider;
};

constexpr std::array model_api_formats{
    ModelApiFormat{"OpenAI-compatible Chat Completions", "chat-completions", "compatible"},
    ModelApiFormat{"OpenAI Responses", "responses", "openai"},
    ModelApiFormat{"Claude Messages", "messages", "anthropic"},
    ModelApiFormat{"Gemini GenerateContent", "generate-content", "gemini"},
    ModelApiFormat{"Gemini Interactions", "interactions", "gemini"},
};

#ifdef _WIN32
constexpr bool saves_direct_api_keys = true;
#else
constexpr bool saves_direct_api_keys = false;
#endif

[[huxerui::composable]]
View AddModelDialog(DialogContext dialog, AddModelProfile add_model, std::function<void(kit::ModelProviderRef)> select) {
  const auto format = UseState(std::size_t{0});
  const auto model = UseState(TextEditingValue::FromText({}));
  const auto url = UseState(TextEditingValue::FromText({}));
  const auto api_key = UseState(TextEditingValue::FromText({}));
  const auto image_input = UseState(false);
  const auto file_input = UseState(false);
  const auto reasoning_summary = UseState(false);
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto tasks = UseApplicationTaskScope();
  const auto theme = UseTheme();
  const auto color = theme.colors;
  const auto save = [dialog, add_model, select, format, model, url, api_key, image_input, file_input, reasoning_summary, busy, error, tasks] {
    if (busy.Get() || model->text.empty() || url->text.empty()) return;
    const auto api = model_api_formats.at(format.Get());
    const bool gemini = std::string_view(api.provider) == "gemini";
    ModelProfileInput input{.model = model->text, .base_url = url->text, .protocol = api.protocol, .provider = api.provider,
        .api_key = api_key->text, .image_input = gemini && image_input.Get(), .file_input = gemini && file_input.Get(),
        .reasoning_summary = std::string_view(api.provider) == "openai" && std::string_view(api.protocol) == "responses" &&
            reasoning_summary.Get()};
    busy = true;
    error = "";
    tasks.Launch([dialog, add_model, select, api_key, busy, error, input = std::move(input)]() mutable -> Task<void> {
      auto result = co_await add_model(std::move(input));
      busy = false;
      if (!result.Succeeded()) { error = result.Error().message; co_return; }
      api_key = TextEditingValue::FromText({});
      select(std::move(result).Value());
      dialog.Dismiss();
    });
  };
  const auto button_style = UseEnvironment<ButtonStyle>();
  const auto secondary = PreviewSecondaryButton(button_style, color);
  const auto save_style = PreviewPrimaryButton(button_style, color);
  const auto fields = PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), color);
  std::vector<View> content{
      Text(::app::strings::ui_add_model, TextRole::Title)
          .Style({Font::System(23.0F).WithWeight(FontWeight::SemiBold), color.on_surface}),
      Select(model_api_formats, format, [](const ModelApiFormat& item) { return Text(item.label); })
          .Label(::app::strings::ui_api_format).OnChanged([format](std::size_t index) { format = index; }).Key("add-model-api"),
      TextField(url).Label(::app::strings::ui_api_base_url).Placeholder("https://provider.example/v1").Variant(TextFieldVariant::Outlined)
          .OnChanged([url](const TextEditingValue& value) { url = value; }).Key("add-model-url"),
      TextField(model).Label(::app::strings::ui_model_id).Variant(TextFieldVariant::Outlined)
          .OnChanged([model](const TextEditingValue& value) { model = value; }).Key("add-model-id"),
      TextField(api_key).Label(::app::strings::ui_api_key).Placeholder(::app::strings::ui_optional_paste_a_key_or_enter_env)
          .Variant(TextFieldVariant::Outlined).Secure()
          .OnChanged([api_key](const TextEditingValue& value) { api_key = value; }).Key("add-model-key"),
  };
  if (!saves_direct_api_keys && !api_key->text.empty() && !api_key->text.starts_with("${")) {
    content.push_back(Text(::app::strings::ui_direct_keys_are_available_until_preview_closes, TextRole::Label));
  }
  if (std::string_view(model_api_formats.at(format.Get()).provider) == "gemini") {
    content.push_back(Row{
        Checkbox(::app::strings::ui_image_input, image_input.Get()).OnChanged([image_input](bool enabled) { image_input = enabled; }),
        Checkbox(::app::strings::ui_pdf_input, file_input.Get()).OnChanged([file_input](bool enabled) { file_input = enabled; }),
    }.With(Spacing(12.0F)));
  }
  const auto api = model_api_formats.at(format.Get());
  if (std::string_view(api.provider) == "openai" && std::string_view(api.protocol) == "responses") {
    content.push_back(Checkbox(::app::strings::ui_request_reasoning_summary, reasoning_summary.Get())
        .OnChanged([reasoning_summary](bool value) { reasoning_summary = value; }));
  }
  if (!error->empty()) {
    content.push_back(Text(error.Get(), TextRole::Label).Style({Font::System(13.0F), color.error}).Key("add-model-error"));
  }
  content.push_back(Row{Spacer(), ProvideEnvironment(secondary,
      Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); }).With(Enabled(!busy.Get()))),
      ProvideEnvironment(save_style, Button(busy.Get() ? ::app::strings::ui_saving : ::app::strings::ui_save_model).OnClick(save)
          .With(Enabled(!busy.Get() && !url->text.empty() && !model->text.empty())).Key("save-model"))}
      .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)));
  return ProvideEnvironment(fields, Column(std::move(content)).With(Frame{.width = 460.0F},
      Padding(22.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .Key("add-model-dialog");
}

} // namespace

void ShowAddModel(const huxerui::DialogHandle& dialog, AddModelProfile add_model,
    std::function<void(kit::ModelProviderRef)> added) {
  ShowPreviewDialog(dialog, DialogFactory{[add_model = std::move(add_model), added = std::move(added)](DialogContext context) {
    return AddModelDialog(std::move(context), add_model, added);
  }});
}

namespace {

View RunOptionRow(StringVariant name, View control) {
  return Row{Text(name, TextRole::Label).With(Grow()),
      Column{std::move(control)}.With(Frame{.width = 212.0F}, CrossAlign(CrossAxisAlignment::Stretch))}
      .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View RunOptionPicker(std::string text, StringVariant label, ViewHandle anchor, std::function<void()> open,
    Color ink, bool enabled = true) {
  const auto colors = UseTheme().colors;
  const auto hovered = UseState(false);
  return Row{
      Text(text).Style({Font::System(13.0F).WithWeight(FontWeight::Medium), ink})
          .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()),
      RunOptionGlyph(::app::images::chevron_down, ink),
  }.OnClick(open).On<ViewEvents::KeyDown>([open](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    if (!event.repeat) open();
    return true;
  }).On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
      .With(anchor, Focusable{}, Enabled(enabled), Frame{.height = 34.0F}, Padding(EdgeInsets::Symmetric(10.0F, 0.0F)),
          Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center), CornerRadius(8.0F), Tooltip(text),
          Background(hovered.Get() && enabled ? colors.surface_container_highest : colors.surface_container_high),
          Semantics{.role = SemanticRole::Button, .label = std::move(label), .value = std::move(text)});
}

[[huxerui::composable]]
View RunOptionsPanel(PopupContext popup, std::shared_ptr<RunSettings> settings, State<kit::UserRunOptions> next, kit::AgentId agent,
    std::optional<kit::WorkspaceId> workspace, std::optional<File> workspace_root, std::optional<kit::RunStartBinding> current,
    std::function<void()> changed, AddModelProfile add_model, std::string assistant,
    std::function<std::vector<MenuEntry>(PopupContext)> draft_assistant) {
  const auto menu = UseMenu();
  const auto dialogs = UseDialog();
  const auto assistant_anchor = UseViewHandle();
  const auto model_anchor = UseViewHandle();
  const auto permission_anchor = UseViewHandle();
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  auto choices = settings->Models(agent);
  const auto effective_model = next->model ? next->model : settings->DefaultModel(agent);
  const auto selected = effective_model ? std::ranges::find(choices, *effective_model, &ModelChoice::ref) : choices.end();
  const auto model_name = effective_model == DemoModel ? UseString(::app::strings::ui_local_demo_model) : selected == choices.end() ?
      (effective_model ? UseString(settings->ModelLabel(effective_model)) : UseString(::app::strings::ui_select_model)) : selected->name;
  const auto effective_profile = next->execution_profile.value_or(AskProfile);
  const auto next_model = current && effective_model != current->model;
  const auto next_permission = current && effective_profile != current->execution_profile.value_or(AskProfile);
  const bool full_access = agent == CodeAgent && workspace && workspace_root;
  const auto select_model = [next, changed, popup](kit::ModelProviderRef ref) {
    auto selected = next.Get();
    selected.model = std::move(ref);
    next = std::move(selected);
    if (changed) changed();
    popup.Dismiss();
  };
  auto choose_model = [menu, dialogs, popup, model_anchor, choices, effective_model, select_model, add_model] {
    std::vector<MenuEntry> items;
    for (const auto& choice : choices) {
      auto label = choice.ref == DemoModel ? StringVariant{::app::strings::ui_local_demo_model} : StringVariant{choice.label};
      items.emplace_back(MenuItem(std::move(label), [select_model, ref = choice.ref] {
        select_model(ref);
      }).Checked(effective_model == choice.ref));
    }
    if (add_model) {
      if (!items.empty()) items.emplace_back(MenuSection{});
      items.emplace_back(MenuItem(::app::strings::ui_add_model_ellipsis, [dialogs, popup, add_model, select_model] {
        popup.Dismiss();
        ShowAddModel(dialogs, add_model, select_model);
      }));
    }
    if (!items.empty()) menu.Show(model_anchor, std::move(items));
  };
  auto choose_permission = [menu, popup, permission_anchor, dialogs, settings, next, effective_profile, full_access, workspace,
                            workspace_root, changed] {
    const auto set = [next, changed, popup](kit::ExecutionProfileRef profile) {
      auto selected = next.Get();
      selected.execution_profile = std::move(profile);
      next = std::move(selected);
      if (changed) changed();
      popup.Dismiss();
    };
    menu.Show(permission_anchor, {
        MenuItem(::app::strings::ui_ask, [set] { set(AskProfile); }).Checked(effective_profile == AskProfile),
        MenuItem(::app::strings::ui_auto_review, [set] { set(ReviewProfile); }).Checked(effective_profile == ReviewProfile),
        MenuItem(::app::strings::ui_full_access, [dialogs, popup, settings, workspace, workspace_root, set] {
          if (!workspace || !workspace_root) return;
          popup.Dismiss();
          ShowPreviewDialog(dialogs, DialogFactory{[settings, workspace = *workspace, root = *workspace_root, set](DialogContext dialog) {
            return Column{
                Text(::app::strings::ui_allow_patches_in_this_workspace, TextRole::Title),
                Text(root.Path()),
                Text(::app::strings::ui_for_30_minutes_this_assistant_can_apply),
                Row{Spacer(), Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); }),
                    Button(::app::strings::ui_allow_for_30_minutes).OnClick([settings, workspace, set, dialog] {
                      set(settings->GrantFullAccess(workspace));
                      dialog.Dismiss();
                    })}.With(Spacing(8.0F)),
            }.With(Frame{.max_width = 470.0F}, Padding(20.0F), Spacing(12.0F));
          }});
        }).Checked(effective_profile.id.value.starts_with(full_access_prefix)).Enabled(full_access),
    });
  };
  std::vector<View> views{Text(::app::strings::ui_conversation_options, TextRole::Title)};
  if (next_model || next_permission) views.push_back(Text(UseString(::app::strings::ui_this_run) + std::string(separator) + settings->Describe(*current, *UseService<Resources>()),
      TextRole::Label));
  const auto permission = std::string(settings->NeedsRenewal(effective_profile, workspace)
      ? UseString(::app::strings::ui_renew_full_access) : UseString(ProfileLabel(effective_profile)));
  const auto menu_label = [](std::string value, bool next_run) {
    if (next_run) value += " \xc2\xb7 Next";
    return value;
  };
  if (draft_assistant) {
    views.push_back(RunOptionRow(::app::strings::ui_assistant, RunOptionPicker(assistant, ::app::strings::ui_assistant, assistant_anchor,
        [menu, assistant_anchor, draft_assistant, popup] { menu.Show(assistant_anchor, draft_assistant(popup)); },
        colors.on_surface).Key("draft-assistant")));
  } else if (!assistant.empty()) {
    views.push_back(RunOptionRow(::app::strings::ui_assistant, Text(assistant).With(Padding(EdgeInsets{.left = 10.0F}))));
  }
  const auto permission_ink = effective_profile.id.value.starts_with(full_access_prefix)
      ? theme.brightness == ThemeBrightness::Dark ? Color::Rgb(240, 189, 130) : Color::Rgb(136, 82, 35) : colors.on_surface;
  views.push_back(RunOptionRow(::app::strings::ui_model, RunOptionPicker(menu_label(model_name, next_model),
      next_model ? "Model for next task" : "Model", model_anchor, choose_model, colors.on_surface,
      !choices.empty() || bool(add_model)).Key("run-model")));
  views.push_back(RunOptionRow(::app::strings::ui_permissions, RunOptionPicker(menu_label(permission, next_permission),
      next_permission ? "Permissions for next task" : "Permissions", permission_anchor, choose_permission,
      permission_ink).Key("run-permissions")));
  return Column(std::move(views)).With(Frame{.width = 360.0F}, Padding(14.0F), Spacing(9.0F),
      CrossAlign(CrossAxisAlignment::Stretch), CornerRadius(12.0F), Background(colors.surface_container_low),
      Border{colors.outline}).Key("conversation-options-panel");
}

} // namespace

[[huxerui::composable]]
View RunSettingsControls(std::shared_ptr<RunSettings> settings, State<kit::UserRunOptions> next, kit::AgentId agent,
    std::optional<kit::WorkspaceId> workspace, std::optional<File> workspace_root, std::optional<kit::RunStartBinding> current,
    std::function<void()> changed, std::optional<View> extra_action, AddModelProfile add_model, std::string assistant,
    std::function<std::vector<MenuEntry>(PopupContext)> draft_assistant) {
  const auto trigger = UseViewHandle();
  const auto anchor = UseEnvironment<kit::AgentChatComposerAnchor>().view.value_or(trigger);
  const auto popup = UsePopup();
  const auto hovered = UseState(false);
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto colors = UseTheme().colors;
  auto style = UseEnvironment<ButtonStyle>();
  style.background = Color::Transparent();
  style.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface_variant};
  style.padding = EdgeInsets::Symmetric(7.0F, 5.0F);
  style.minimum_height = 32.0F;
  const auto choices = settings->Models(agent);
  const auto model = next->model ? next->model : settings->DefaultModel(agent);
  const auto selected = model ? std::ranges::find(choices, *model, &ModelChoice::ref) : choices.end();
  const auto name = model == DemoModel ? UseString(::app::strings::ui_local_demo_model) : selected != choices.end() ? selected->name :
      model ? UseString(::app::strings::ui_unavailable_model) : UseString(::app::strings::ui_select_model);
  const auto effective_profile = next->execution_profile.value_or(AskProfile);
  const bool next_model = current && model != current->model;
  const bool next_permission = current && effective_profile != current->execution_profile.value_or(AskProfile);
  auto profile = settings->NeedsRenewal(effective_profile, workspace) ? UseString(::app::strings::ui_renew_full_access) : UseString(ProfileLabel(effective_profile));
  if (next_permission) profile += std::string(separator) + UseString(::app::strings::ui_next);
  const auto open = [popup, anchor, settings, next, agent, workspace, workspace_root, current, changed, add_model, assistant, draft_assistant] {
    popup.Show(anchor, PopupFactory{[settings, next, agent, workspace, workspace_root, current, changed, add_model,
                                     assistant, draft_assistant](PopupContext context) {
      return RunOptionsPanel(context, settings, next, agent, workspace, workspace_root, current, changed,
          add_model, assistant, draft_assistant);
    }}, {.placement = {AnchorSide::Above, AnchorAlignment::Start}, .gap = 8.0F});
  };
  auto label = assistant.empty() ? name : assistant + separator + name;
  if (next_model) label += std::string(separator) + UseString(::app::strings::ui_next);
  std::vector<View> controls{
      Row{
          Text(label).Style(style.label_style).LineLimit(1).Overflow(TextOverflow::Ellipsis)
              .With(Frame{.max_width = compact ? 156.0F : 280.0F}).Key("conversation-options-summary"),
          RunOptionGlyph(::app::images::chevron_down, colors.on_surface_variant),
      }.OnClick(open).On<ViewEvents::KeyDown>([open](const KeyEvent& event) {
        if (event.key != Key::Enter && event.key != Key::Space) return false;
        if (!event.repeat) open();
        return true;
      }).On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
          .With(trigger, Focusable{}, Frame{.height = 32.0F}, Padding(EdgeInsets::Symmetric(7.0F, 0.0F)), Spacing(5.0F),
              CornerRadius(7.0F), CrossAlign(CrossAxisAlignment::Center), Tooltip(label),
              Background(hovered.Get() ? colors.surface_container_high : Color::Transparent()),
              Semantics{.role = SemanticRole::Button, .label = ::app::strings::ui_conversation_options, .value = name}).Key("conversation-options"),
      Row{RunOptionGlyph(::app::images::shield, colors.on_surface_variant),
          Text(profile, TextRole::Label).With(Foreground(colors.on_surface_variant)).Key("run-permission-summary")}
          .With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center), Frame{.height = 32.0F}),
  };
  if (extra_action) controls.push_back(ProvideEnvironment(style, std::move(*extra_action)));
  return Flow(std::move(controls)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center));
}

} // namespace agentkit_preview
