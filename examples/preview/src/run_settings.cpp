#include <app_resources.h>

#include "run_settings.h"

#include "code_tools.h"
#include "general_workflow.h"
#include "preview_json.h"
#include "preview_theme.h"
#include "report_chart.h"

#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <initializer_list>
#include <span>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using namespace std::chrono_literals;
constexpr std::string_view full_access_prefix = "preview.full/";
constexpr std::string_view model_slot_prefix = "preview.profile/";
constexpr auto separator = "  \xc2\xb7  ";

struct ModelBrand {
  std::initializer_list<std::string_view> families;
  ImageResource image;
  bool tint = false;

  Image Icon(Color foreground) const {
    Image icon(image);
    return tint ? std::move(icon).Tint(foreground) : std::move(icon);
  }
};

const ModelBrand model_brands[]{
    {{"gpt", "chatgpt", "codex", "o1", "o3", "o4"}, ::app::images::model_openai, true},
    {{"claude"}, ::app::images::model_claude},
    {{"gemini"}, ::app::images::model_gemini},
    {{"gemma"}, ::app::images::model_gemma},
    {{"deepseek"}, ::app::images::model_deepseek},
    {{"qwen", "qwq", "qvq"}, ::app::images::model_qwen},
    {{"grok"}, ::app::images::model_grok, true},
    {{"llama", "code-llama", "codellama"}, ::app::images::model_llama},
    {{"mistral", "mixtral", "ministral", "codestral", "devstral", "pixtral", "magistral"}, ::app::images::model_mistral},
    {{"kimi", "moonshot"}, ::app::images::model_kimi, true},
    {{"glm", "chatglm"}, ::app::images::model_glm},
    {{"doubao", "seed-oss"}, ::app::images::model_doubao},
    {{"hunyuan"}, ::app::images::model_hunyuan},
    {{"minimax", "abab"}, ::app::images::model_minimax},
    {{"command", "aya"}, ::app::images::model_cohere},
    {{"sonar"}, ::app::images::model_perplexity},
    {{"nova"}, ::app::images::model_nova, true},
    {{"nemotron"}, ::app::images::model_nvidia},
    {{"ernie", "wenxin"}, ::app::images::model_wenxin},
    {{"spark"}, ::app::images::model_spark},
    {{"step", "stepfun"}, ::app::images::model_stepfun},
    {{"longcat"}, ::app::images::model_longcat, true},
    {{"mimo"}, ::app::images::model_mimo, true},
    {{"baichuan"}, ::app::images::model_baichuan},
    {{"yi"}, ::app::images::model_yi, true},
};

std::string SearchableModelName(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

const ModelBrand* FindModelBrand(const std::string& model) {
  const auto name = SearchableModelName(model);
  std::string_view id = name;
  if (const auto slash = id.find_last_of('/'); slash != std::string_view::npos) id.remove_prefix(slash + 1);
  constexpr std::string_view regions[]{"us.", "eu.", "apac.", "global.", "jp.", "au."};
  constexpr std::string_view publishers[]{
      "amazon.", "anthropic.", "cohere.", "deepseek.", "google.", "meta.", "mistral.", "openai.", "qwen."};
  for (const auto prefixes : {std::span<const std::string_view>{regions}, std::span<const std::string_view>{publishers}}) {
    const auto prefix = std::ranges::find_if(prefixes, [id](auto part) { return id.starts_with(part); });
    if (prefix != prefixes.end()) id.remove_prefix(prefix->size());
  }
  const auto brand = std::ranges::find_if(model_brands, [id](const auto& entry) {
    return std::ranges::any_of(entry.families, [id](std::string_view family) {
      if (!id.starts_with(family)) return false;
      if (id.size() == family.size()) return true;
      const char suffix = id[family.size()];
      return suffix == '-' || suffix == '_' || suffix == '.' || suffix == ':' || (suffix >= '0' && suffix <= '9');
    });
  });
  return brand == std::end(model_brands) ? nullptr : brand;
}

StringVariant ProfileLabel(const std::optional<kit::ExecutionProfileRef>& profile) {
  if (!profile || *profile == AskProfile) return ::app::strings::ui_ask;
  if (*profile == ReviewProfile) return ::app::strings::ui_auto_review;
  if (profile->id.value.starts_with(full_access_prefix)) return ::app::strings::ui_approve_project_edits;
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

void RunSettings::AddModel(const kit::ModelRegistration& registration, std::string label, ModelLimits limits, bool editable) {
  if (std::ranges::find(models_, registration.ref, &ModelChoice::ref) != models_.end()) return;
  const auto& endpoint = registration.endpoint;
  auto name = label.empty() ? endpoint.model : std::move(label);
  if (std::string_view identity = registration.ref.id.value; identity.starts_with(model_slot_prefix)) {
    identity.remove_prefix(model_slot_prefix.size());
    name += std::string(separator) + std::string(identity.substr(0, identity.find('/')));
  }
  auto location = endpoint.provider;
  if (endpoint.base_url) {
    const auto uri = Uri::Parse(*endpoint.base_url);
    if (uri && uri->Authority()) location = std::string(*uri->Authority()) + std::string(uri->Path());
  }
  const auto caps = registration.provider->Capabilities();
  models_.push_back({registration.ref, name, std::move(name), registration.ref != DemoModel,
      caps.text_stream && caps.structured_output, caps, std::move(location), endpoint, limits, editable});
  auto& version = model_versions_[registration.ref.id];
  version = std::max(version, registration.ref.version);
  model_revision_ = model_revision_.Get() + 1;
}

void RunSettings::SetDefaults(kit::ModelProviderRef general, std::optional<kit::ModelProviderRef> code) {
  general_ = std::move(general);
  code_ = std::move(code);
}

std::optional<kit::ModelProviderRef> RunSettings::DefaultModel(const kit::AgentId& agent) const {
  return CurrentModel(agent == CodeAgent ? code_ : std::optional{general_});
}

std::optional<kit::ModelProviderRef> RunSettings::CurrentModel(std::optional<kit::ModelProviderRef> model) const {
  static_cast<void>(model_revision_.Get());
  if (model) {
    const auto version = model_versions_.find(model->id);
    if (version != model_versions_.end()) model->version = version->second;
  }
  return model;
}

std::vector<ModelChoice> RunSettings::Models(const kit::AgentId& agent) const {
  static_cast<void>(model_revision_.Get());
  std::vector<ModelChoice> result;
  std::ranges::copy_if(models_, std::back_inserter(result), [&](const ModelChoice& choice) {
    return choice.ref.version == model_versions_.at(choice.ref.id) && choice.suitable && (agent != CodeAgent || choice.code);
  });
  for (auto& choice : result) {
    if (std::ranges::count(result, choice.name, &ModelChoice::name) > 1) choice.label += separator + choice.endpoint;
  }
  return result;
}

StringVariant RunSettings::ModelLabel(const std::optional<kit::ModelProviderRef>& ref) const {
  if (ref == DemoModel) return ::app::strings::ui_local_demo_model;
  const auto found = ref ? std::ranges::find(models_, *ref, &ModelChoice::ref) : models_.end();
  if (found == models_.end()) return ::app::strings::ui_unavailable_model;
  return std::ranges::any_of(models_, [&](const auto& other) {
    return other.ref.id != found->ref.id && other.name == found->name && other.ref.version == model_versions_.at(other.ref.id);
  })
      ? found->name + separator + found->endpoint : found->name;
}

std::optional<Image> RunSettings::ModelAvatar(const kit::ModelProviderRef& ref, Color foreground) const {
  const auto found = std::ranges::find(models_, ref, &ModelChoice::ref);
  if (found == models_.end()) return {};
  const auto* brand = FindModelBrand(found->connection.model);
  if (!brand) return {};
  return brand->Icon(foreground);
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

bool RunSettings::HasFullAccess(const kit::ExecutionProfileRef& profile, const std::optional<kit::WorkspaceId>& workspace) const {
  const auto grant = full_access_.find(profile.id);
  return grant != full_access_.end() && profile.version == 1 && workspace == grant->second;
}

kit::ExecutionProfileRef RunSettings::GrantFullAccess(kit::WorkspaceId workspace) {
  kit::ExecutionProfileId id{std::string(full_access_prefix) + kit::NewCommandId().value};
  full_access_.emplace(id, std::move(workspace));
  return {std::move(id), 1};
}

kit::AgentResult<kit::PolicyDecision> RunSettings::Decide(const kit::PolicyRequest& request, bool needs_confirmation,
                                                        bool workspace_available, bool full_local_access) const {
  using Result = kit::AgentResult<kit::PolicyDecision>;
  const auto profile = request.run_configuration ? request.run_configuration->execution_profile : std::nullopt;
  auto decision = needs_confirmation ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow;
  if (request.capability == kit::Capability::UseRunConfiguration && request.agent && request.agent->id == CodeAgent &&
      request.run_configuration && request.run_configuration->model == DemoModel) {
    return Result::Failure({.code = kit::ErrorCode::Unsupported, .message = "Choose a configured model for the code assistant"});
  }
  if (full_local_access && request.actor.principal == PreviewUser && workspace_available &&
      request.capability == kit::Capability::ExecuteTool && request.tool && IsCodePatchTool(request.tool->id))
    decision = kit::AccessDecision::Allow;
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
  if (!HasFullAccess(*profile, request.workspace) || !request.agent) {
    if (full_local_access) return Result::Success({decision, {}});
    return Result::Failure({.code = kit::ErrorCode::Conflict, .message = "Full Access is unavailable for this task. Choose permissions again."});
  }
  if (!workspace_available) {
    return Result::Failure({.code = kit::ErrorCode::PermissionDenied, .message = "Full Access workspace is unavailable"});
  }
  if (needs_confirmation && IsCodePatchTool(request.tool->id) && std::ranges::none_of(request.sources, [](const auto& slice) {
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

using LimitFields = std::array<TextEditingValue, 3>;
using CatalogSelection = std::map<std::string, LimitFields, std::less<>>;

LimitFields EditLimits(const ModelLimits& limits = {}) {
  const auto field = [](auto value) { return TextEditingValue::FromText(value ? std::to_string(*value) : ""); };
  return {field(limits.context_window), field(limits.max_input_tokens), field(limits.max_output_tokens)};
}

ModelLimits ReadLimits(const LimitFields& fields) {
  const auto read = [](const TextEditingValue& field) -> std::optional<std::uint64_t> {
    if (field.text.empty()) return {};
    const auto* end = field.text.data() + field.text.size();
    std::uint64_t amount = 0;
    const auto parsed = std::from_chars(field.text.data(), end, amount);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !amount)
      throw std::invalid_argument("Token limits must be positive whole numbers.");
    return amount;
  };
  return {read(fields[0]), read(fields[1]), read(fields[2])};
}

View ModelLimitFields(const LimitFields& fields, bool enabled, const ColorScheme& colors,
                      std::function<void(std::size_t, TextEditingValue)> change) {
  const auto& strings = std::array{::app::strings::ui_model_context_window, ::app::strings::ui_model_max_input,
      ::app::strings::ui_model_max_output};
  std::vector<View> row;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    auto input = TextField(fields[i]).Label(strings[i]).Placeholder(::app::strings::ui_model_limit_unknown)
        .Variant(TextFieldVariant::Outlined).InputConfiguration({.type = TextInputType::Number})
        .OnChanged([change, i](const TextEditingValue& value) { change(i, value); })
        .With(Enabled(enabled)).Key("model-limit-" + std::to_string(i));
    row.push_back(PreviewLabeledField(strings[i], std::move(input), colors).With(Grow()));
  }
  return Row(std::move(row)).With(Spacing(6.0F));
}

[[huxerui::composable]]
View AddModelDialog(DialogContext dialog, AddModelProfile add_model, std::function<void(kit::ModelProviderRef)> select) {
  const auto format = UseState(std::size_t{0});
  const auto model = UseState(TextEditingValue::FromText({}));
  const auto url = UseState(TextEditingValue::FromText({}));
  const auto api_key = UseState(TextEditingValue::FromText({}));
  const auto search = UseState(TextEditingValue::FromText({}));
  const auto limits = UseState(EditLimits());
  const auto catalog = UseState(std::shared_ptr<const std::vector<DiscoveredModel>>{});
  const auto selected = UseState(CatalogSelection{});
  const auto show_catalog = UseState(false);
  const auto show_connection = UseState(false);
  const auto show_limits = UseState(false);
  const auto image_input = UseState(false);
  const auto file_input = UseState(false);
  const auto reasoning_summary = UseState(false);
  const auto fetching = UseState(false);
  const auto saving = UseState(false);
  const auto error = UseState(std::string{});
  const auto tasks = UseTaskScope();
  const auto application_tasks = UseApplicationTaskScope();
  const auto client = UseService<HttpClient>();
  const auto color = UseTheme().colors;
  const auto busy = fetching.Get() || saving.Get();
  const auto invalid_limits = UseString(::app::strings::ui_model_token_limit_invalid);
  const auto profile = [format, url, api_key, image_input, file_input, reasoning_summary] {
    const auto api = model_api_formats.at(format.Get());
    const bool gemini = std::string_view(api.provider) == "gemini";
    return ModelProfileInput{.base_url = url->text, .protocol = api.protocol, .provider = api.provider,
        .api_key = api_key->text, .image_input = gemini && image_input.Get(), .file_input = gemini && file_input.Get(),
        .reasoning_summary = std::string_view(api.protocol) == "responses" && reasoning_summary.Get()};
  };
  const auto clear_catalog = [catalog, selected, show_catalog, error] {
    catalog = std::shared_ptr<const std::vector<DiscoveredModel>>{};
    selected = CatalogSelection{};
    show_catalog = false;
    error = "";
  };
  const auto fetch = [profile, client, tasks, fetching, saving, catalog, show_catalog, show_connection, search, error] {
    if (!client || fetching.Get() || saving.Get()) return;
    fetching = true;
    error = "";
    tasks.Launch([request = profile(), client, fetching, catalog, show_catalog, show_connection, search, error]() mutable -> Task<void> {
      auto result = co_await DiscoverModels(std::move(request), client);
      fetching = false;
      if (!result.Succeeded()) { error = result.Error().message; co_return; }
      catalog = std::make_shared<const std::vector<DiscoveredModel>>(std::move(result).Value());
      show_catalog = true;
      show_connection = false;
      search = TextEditingValue::FromText({});
    });
  };
  const auto save = [dialog, profile, add_model, select, model, limits, selected, show_catalog, api_key, saving, fetching,
                     error, invalid_limits, show_limits, application_tasks] {
    if (saving.Get() || fetching.Get()) return;
    std::vector<ModelProfileInput> entries;
    const auto make = [&entries, &profile](std::string name, const LimitFields& fields) {
      auto value = profile();
      value.model = std::move(name);
      value.limits = ReadLimits(fields);
      entries.push_back(std::move(value));
    };
    try {
      if (show_catalog.Get()) {
        for (const auto& [id, fields] : selected.Get()) make(id, fields);
      } else if (!TrimModelField(model->text).empty()) {
        make(model->text, limits.Get());
      }
    } catch (const std::invalid_argument&) {
      if (!show_catalog.Get()) show_limits = true;
      error = invalid_limits;
      return;
    }
    if (entries.empty() || TrimModelField(entries.front().base_url).empty()) return;
    saving = true;
    error = "";
    application_tasks.Launch([dialog, add_model, select, api_key, selected, saving, error,
                              entries = std::move(entries)]() mutable -> Task<void> {
      std::optional<kit::ModelProviderRef> added;
      std::string failures;
      for (auto& entry : entries) {
        const auto name = entry.model;
        auto result = co_await add_model(std::move(entry));
        if (result.Succeeded()) {
          if (!added) added = std::move(result).Value();
          auto pending = selected.Get();
          pending.erase(name);
          selected = std::move(pending);
        } else {
          if (!failures.empty()) failures += '\n';
          failures += name + separator + result.Error().message;
        }
      }
      saving = false;
      if (added && select) select(*added);
      if (!failures.empty()) { error = std::move(failures); co_return; }
      api_key = TextEditingValue::FromText({});
      dialog.Dismiss();
    });
  };
  const auto button_style = UseEnvironment<ButtonStyle>();
  const auto secondary = PreviewSecondaryButton(button_style, color);
  const auto primary = PreviewPrimaryButton(button_style, color);
  auto fields = UseEnvironment<TextFieldStyle>();
  fields.show_label = false;
  const auto api = model_api_formats.at(format.Get());
  const auto placeholder = std::string_view(api.provider) == "gemini" ? "https://generativelanguage.googleapis.com/v1beta" :
      std::string_view(api.provider) == "anthropic" ? "https://api.anthropic.com/v1" : "https://provider.example/v1";
  const auto heading = [color](StringResource label) {
    return Text(label, TextRole::Label).Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), color.on_surface});
  };
  std::vector<View> connection;
  if (show_catalog.Get() && !show_connection.Get()) {
    connection.push_back(Row {
      Text(url->text, TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis)
          .With(Grow(), Tooltip(url->text), Foreground(color.on_surface_variant)),
      ProvideEnvironment(secondary, Button(::app::strings::ui_model_connection).OnClick([show_connection] {
        show_connection = true;
      }).With(Enabled(!busy))),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)));
  } else {
    const auto format_label = ::app::strings::ui_api_format;
    const auto endpoint_label = ::app::strings::ui_api_base_url;
    const auto key_label = ::app::strings::ui_api_key;
    connection = {
      heading(::app::strings::ui_model_connection_heading),
      PreviewLabeledField(format_label, Select(model_api_formats, format, [](const ModelApiFormat& item) {
        return Text(item.label).LineLimit(1).Overflow(TextOverflow::Ellipsis);
      }).Label(format_label).OnChanged([format, clear_catalog](std::size_t index) {
        format = index;
        clear_catalog();
      }).With(Enabled(!busy)).Key("add-model-api"), color),
      PreviewLabeledField(endpoint_label, TextField(url).Label(endpoint_label).Placeholder(placeholder).Variant(TextFieldVariant::Outlined)
          .OnChanged([url, clear_catalog](const TextEditingValue& value) { url = value; clear_catalog(); })
          .With(Enabled(!busy)).Key("add-model-url"), color),
      PreviewLabeledField(key_label, TextField(api_key).Label(key_label).Placeholder(::app::strings::ui_optional_paste_a_key_or_enter_env)
          .Variant(TextFieldVariant::Outlined).Secure()
          .OnChanged([api_key, clear_catalog](const TextEditingValue& value) { api_key = value; clear_catalog(); })
          .With(Enabled(!busy)).Key("add-model-key"), color),
    };
  }
  if (!saves_direct_api_keys && !api_key->text.empty() && !api_key->text.starts_with("${")) {
    connection.push_back(Text(::app::strings::ui_direct_keys_are_available_until_preview_closes, TextRole::Label));
  }
  std::vector<View> content {
    Column(std::move(connection)).With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    ProvideEnvironment(DividerStyle{color.outline}, Divider()),
  };
  std::vector<View> discovery{Button(fetching.Get() ? ::app::strings::ui_fetching_models : ::app::strings::ui_fetch_models)
      .OnClick(fetch).With(Enabled(!busy && bool(client) && !TrimModelField(url->text).empty())).Key("fetch-models")};
  if (catalog.Get()) {
    discovery.push_back(Button(show_catalog.Get() ? ::app::strings::ui_enter_model_manually : ::app::strings::ui_show_model_catalog)
        .OnClick([show_catalog] { show_catalog = !show_catalog.Get(); }).With(Enabled(!busy)));
  }
  content.push_back(Row {
    heading(::app::strings::ui_models).With(Grow()),
    ProvideEnvironment(secondary, Flow(std::move(discovery)).With(Spacing(6.0F))),
  }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)));
  if (show_catalog.Get() && catalog.Get()) {
    const auto query = SearchableModelName(search->text);
    std::vector<DiscoveredModel> results;
    for (const auto& item : *catalog.Get()) {
      if (query.empty() || SearchableModelName(item.id + " " + item.name).find(query) != std::string::npos)
        results.push_back(item);
    }
    content.push_back(TextField(search).Label(::app::strings::ui_search_models).Placeholder(::app::strings::ui_search_models)
        .Variant(TextFieldVariant::Outlined).OnChanged([search](const TextEditingValue& value) { search = value; }).Key("search-models"));
    content.push_back(Text(StringVariant::Format(::app::strings::ui_0_models_selected, selected->size()), TextRole::Label));
    if (results.empty()) {
      content.push_back(Text(::app::strings::ui_no_catalog_models, TextRole::Label));
    } else {
      content.push_back(VirtualList(std::move(results), [selected, busy, color](const DiscoveredModel& item) {
        const auto pending = selected.Get();
        const auto entry = pending.find(item.id);
        const bool checked = entry != pending.end();
        const auto label = item.name == item.id ? item.id : item.name + separator + item.id;
        std::vector<View> row{Checkbox(label, checked).OnChanged([selected, item](bool on) {
          auto next = selected.Get();
          if (on) next.emplace(item.id, EditLimits(item.limits));
          else next.erase(item.id);
          selected = std::move(next);
        }).With(Enabled(!busy), Tooltip(label)).Key("catalog-model/" + item.id)};
        if (checked) row.push_back(ModelLimitFields(entry->second, !busy, color, [selected, id = item.id](std::size_t i, TextEditingValue value) {
          auto next = selected.Get();
          if (auto entry = next.find(id); entry != next.end()) {
            entry->second[i] = std::move(value);
            selected = std::move(next);
          }
        }));
        return Column(std::move(row)).With(Padding(EdgeInsets::Symmetric(4.0F, 3.0F)), Spacing(4.0F),
            CornerRadius(6.0F), Background(checked ? color.surface_container : Color::Transparent()),
            CrossAlign(CrossAxisAlignment::Stretch)).Key(item.id);
      }).EstimatedItemExtent(39.0F).With(Frame{.height = show_connection.Get() ? 185.0F : 231.0F}, ScrollBar()).Key("model-catalog"));
    }
  } else {
    const auto label = ::app::strings::ui_model_id;
    content.push_back(PreviewLabeledField(label, TextField(model).Label(label).Variant(TextFieldVariant::Outlined)
        .OnChanged([model](const TextEditingValue& value) { model = value; }).With(Enabled(!busy)).Key("add-model-id"), color));
    auto disclosure = UseEnvironment<ChipStyle>();
    disclosure.background = color.surface_container_low;
    disclosure.border = Border{Color::Transparent()};
    disclosure.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), color.on_surface_variant};
    disclosure.minimum_height = 28.0F;
    disclosure.icon_size = 13.0F;
    content.push_back(Row {ProvideEnvironment(disclosure, Chip(show_limits.Get() ? ::app::images::chevron_down : ::app::images::chevron_right,
        ::app::strings::ui_model_token_limits).OnClick([show_limits] { show_limits = !show_limits.Get(); })
            .With(Semantics{.expanded = show_limits.Get()}).Key("advanced-model-limits")), Spacer()});
    if (show_limits.Get()) {
      content.push_back(ModelLimitFields(limits.Get(), !busy, color, [limits](std::size_t i, TextEditingValue value) {
        auto next = limits.Get();
        next[i] = std::move(value);
        limits = std::move(next);
      }).Key("manual-model-limits"));
    }
  }
  if (std::string_view(api.provider) == "gemini") {
    content.push_back(Flow {
      Checkbox(::app::strings::ui_image_input, image_input.Get()).OnChanged([image_input](bool on) { image_input = on; }),
      Checkbox(::app::strings::ui_pdf_input, file_input.Get()).OnChanged([file_input](bool on) { file_input = on; }),
    }.With(Spacing(12.0F), Enabled(!busy)));
  }
  if (std::string_view(api.protocol) == "responses") {
    content.push_back(Checkbox(::app::strings::ui_request_reasoning_summary, reasoning_summary.Get())
        .OnChanged([reasoning_summary](bool on) { reasoning_summary = on; }).With(Enabled(!busy)));
  }
  if (!error->empty()) content.push_back(Text(error.Get()).With(Foreground(color.error)).Key("add-model-error"));
  const bool can_save = !busy && !TrimModelField(url->text).empty() &&
      (show_catalog.Get() ? !selected->empty() : !TrimModelField(model->text).empty());
  const auto action = saving.Get() ? ::app::strings::ui_saving :
      show_catalog.Get() ? ::app::strings::ui_save_models : ::app::strings::ui_save_model;
  auto body = ScrollView(Column(std::move(content)).With(Spacing(12.0F), Padding(EdgeInsets::Symmetric(20.0F, 15.0F)),
      CrossAlign(CrossAxisAlignment::Stretch))).With(Frame{.max_height = 520.0F}, ScrollBar());
  auto actions = Row {
    Spacer(),
    ProvideEnvironment(secondary, Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); }).With(Enabled(!saving.Get()))),
    ProvideEnvironment(primary, Button(action).OnClick(save).With(Enabled(can_save)).Key("save-model")),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
  return ProvideEnvironment(fields, PreviewFormDialog(::app::strings::ui_add_model, std::move(body), std::move(actions), color, 512.0F))
      .Key("add-model-dialog");
}

[[huxerui::composable]]
View EditModelDialog(DialogContext dialog, ModelChoice current, EditModelProfile edit_model) {
  const auto endpoint = UseState(TextEditingValue::FromText(current.connection.base_url.value_or(std::string{})));
  const auto key = UseState(TextEditingValue::FromText({}));
  const auto clear_key = UseState(false);
  const auto limits = UseState(EditLimits(current.limits));
  const auto saving = UseState(false);
  const auto error = UseState(std::string{});
  const auto tasks = UseApplicationTaskScope();
  const auto color = UseTheme().colors;
  const auto buttons = UseEnvironment<ButtonStyle>();
  auto fields = UseEnvironment<TextFieldStyle>();
  fields.show_label = false;
  const auto invalid_limits = UseString(::app::strings::ui_model_token_limit_invalid);
  const auto save = [dialog, current, edit_model, endpoint, key, clear_key, limits, saving, error, tasks, invalid_limits] {
    if (saving.Get() || TrimModelField(endpoint->text).empty()) return;
    ModelLimits updated_limits;
    try { updated_limits = ReadLimits(limits.Get()); }
    catch (const std::invalid_argument&) { error = invalid_limits; return; }
    auto text = TrimModelField(key->text);
    std::optional<std::string> updated_key;
    if (clear_key.Get()) updated_key = std::string{};
    else if (!text.empty()) updated_key = std::move(text);
    saving = true;
    error = "";
    tasks.Launch([dialog, edit_model, ref = current.ref, url = endpoint->text, updated_key = std::move(updated_key),
                  updated_limits, key, saving, error]() mutable -> Task<void> {
      auto result = co_await edit_model(std::move(ref), std::move(url), std::move(updated_key), updated_limits);
      saving = false;
      if (!result.Succeeded()) { error = result.Error().message; co_return; }
      key = TextEditingValue::FromText({});
      dialog.Dismiss();
    });
  };
  const auto endpoint_label = ::app::strings::ui_api_base_url;
  const auto key_label = ::app::strings::ui_api_key;
  const bool ready = !saving.Get();
  std::vector<View> content{
      Text(current.label).Style({Font::System(15.0F).WithWeight(FontWeight::SemiBold), color.on_surface}),
      PreviewLabeledField(endpoint_label, TextField(endpoint).Label(endpoint_label).Variant(TextFieldVariant::Outlined)
          .OnChanged([endpoint](const TextEditingValue& value) { endpoint = value; })
          .With(Enabled(ready)).Key("edit-model-url"), color),
      PreviewLabeledField(key_label, TextField(key).Label(key_label).Placeholder(::app::strings::ui_keep_saved_model_key)
          .Variant(TextFieldVariant::Outlined).Secure().OnChanged([key](const TextEditingValue& value) { key = value; })
          .With(Enabled(ready && !clear_key.Get())).Key("edit-model-key"), color),
      Checkbox(::app::strings::ui_clear_saved_model_key, clear_key.Get())
          .OnChanged([clear_key](bool checked) { clear_key = checked; }).With(Enabled(ready && key->text.empty())),
      ModelLimitFields(limits.Get(), ready, color, [limits](std::size_t i, TextEditingValue value) {
        auto next = limits.Get();
        next[i] = std::move(value);
        limits = std::move(next);
      }),
  };
  if (!saves_direct_api_keys && !key->text.empty() && !key->text.starts_with("${"))
    content.push_back(Text(::app::strings::ui_direct_keys_are_available_until_preview_closes, TextRole::Label));
  if (!error->empty()) content.push_back(Text(error.Get()).With(Foreground(color.error)).Key("edit-model-error"));
  auto body = ScrollView(Column(std::move(content)).With(Spacing(12.0F), Padding(EdgeInsets::Symmetric(20.0F, 15.0F)),
      CrossAlign(CrossAxisAlignment::Stretch))).With(Frame{.max_height = 520.0F}, ScrollBar());
  auto actions = Row{
      Spacer(),
      ProvideEnvironment(PreviewSecondaryButton(buttons, color), Button(::app::strings::ui_cancel)
          .OnClick([dialog] { dialog.Dismiss(); }).With(Enabled(ready))),
      ProvideEnvironment(PreviewPrimaryButton(buttons, color), Button(ready ? ::app::strings::ui_save_model : ::app::strings::ui_saving)
          .OnClick(save).With(Enabled(ready && !TrimModelField(endpoint->text).empty())).Key("update-model")),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
  return ProvideEnvironment(fields, PreviewFormDialog(::app::strings::ui_edit_model, std::move(body), std::move(actions), color, 512.0F))
      .Key("edit-model-dialog");
}

} // namespace

void ShowAddModel(const huxerui::DialogHandle& dialog, AddModelProfile add_model,
    std::function<void(kit::ModelProviderRef)> added) {
  ShowPreviewDialog(dialog, DialogFactory{[add_model = std::move(add_model), added = std::move(added)](DialogContext context) {
    return AddModelDialog(std::move(context), add_model, added);
  }});
}

void ShowEditModel(const DialogHandle& dialog, ModelChoice current, EditModelProfile edit_model) {
  ShowPreviewDialog(dialog, DialogFactory{[current = std::move(current), edit_model = std::move(edit_model)](DialogContext context) {
    return EditModelDialog(std::move(context), current, edit_model);
  }});
}

namespace {

[[huxerui::composable]]
View RunOptionTrigger(std::string value, StringResource label, std::string key, ViewHandle anchor,
                      std::function<void()> open, Color ink, bool enabled, const ModelBrand* brand = nullptr,
                      bool permission = false) {
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto compact = UseViewportClass() == ViewportClass::Compact;
  auto style = UseEnvironment<ChipStyle>();
  style.background = colors.surface_container_low;
  style.border = Border{Color::Transparent()};
  style.label_style = {Font::FromFamily(theme.typography.family, theme.typography.body_small).WithWeight(FontWeight::Medium), ink};
  style.padding = EdgeInsets::Symmetric(8.0F, 3.0F);
  style.minimum_height = 32.0F;
  style.icon_size = 14.0F;
  style.icon_spacing = 5.0F;
  style.corner_radii = CornerRadii{7.0F};
  const float max_width = permission ? 170.0F : compact ? 156.0F : 240.0F;
  const auto semantics = Semantics{.role = SemanticRole::Button, .label = label, .value = value};
  if (brand) {
    const Frame icon_size{.width = style.icon_size, .height = style.icon_size};
    return Row{
        brand->Icon(ink).With(icon_size, LayoutPriority(1.0F)).Key("run-model-icon"),
        Text(std::move(value)).Style(style.label_style).LineLimit(1).Overflow(TextOverflow::Ellipsis),
        Image(::app::images::chevron_down).Tint(ink).With(icon_size, LayoutPriority(1.0F)),
    }.OnClick(open).On<ViewEvents::KeyDown>([open](const KeyEvent& event) {
      if (event.key != Key::Enter && event.key != Key::Space && event.key != Key::ArrowDown) return false;
      open();
      return true;
    }).With(anchor, Focusable{}, Enabled(enabled), Frame{.max_width = max_width, .min_height = style.minimum_height},
        Padding(style.padding), Spacing(style.icon_spacing), CrossAlign(CrossAxisAlignment::Center),
        CornerRadius(style.corner_radii), Background(style.background), semantics).Key(std::move(key));
  }
  const auto& icon = permission ? ::app::images::shield : ::app::images::chevron_down;
  return ProvideEnvironment(style, Chip(icon, std::move(value)).OnClick(std::move(open))
      .With(anchor, Enabled(enabled), Frame{.max_width = max_width}, semantics).Key(std::move(key)));
}

} // namespace

[[huxerui::composable]]
View RunSettingsControls(std::shared_ptr<RunSettings> settings, State<kit::UserRunOptions> next, kit::AgentId agent,
    std::optional<kit::WorkspaceId> workspace, std::optional<File> workspace_root,
    std::function<void()> changed, std::optional<View> extra_action, AddModelProfile add_model, bool show_permissions,
    bool full_local_access, kit::ExecutionProfileRef default_profile) {
  const auto model_menu = UseMenu();
  const auto permission_menu = UseMenu();
  const auto dialogs = UseDialog();
  const auto model_anchor = UseViewHandle();
  const auto permission_anchor = UseViewHandle();
  const auto colors = UseTheme().colors;
  const auto choices = settings->Models(agent);
  const auto chosen_model = settings->CurrentModel(next->model);
  Lifecycle([next, chosen_model, changed] {
    if (next->model == chosen_model) return;
    auto value = next.Get();
    if (!value.model || !chosen_model || value.model->id != chosen_model->id) return;
    value.model = chosen_model;
    next = std::move(value);
    if (changed) changed();
  }, chosen_model, next->model);
  const auto effective_model = chosen_model ? chosen_model : settings->DefaultModel(agent);
  const auto selected = effective_model ? std::ranges::find(choices, *effective_model, &ModelChoice::ref) : choices.end();
  const auto* selected_brand = selected == choices.end() ? nullptr : FindModelBrand(selected->connection.model);
  auto model = effective_model == DemoModel ? UseString(::app::strings::ui_local_demo_model) :
      selected != choices.end() ? selected->name : effective_model ? UseString(::app::strings::ui_unavailable_model) :
      UseString(::app::strings::ui_select_model);
  const auto effective_profile = next->execution_profile.value_or(default_profile);
  const bool full_access = workspace && workspace_root;
  const bool full_access_selected = full_access && settings->HasFullAccess(effective_profile, workspace);
  const auto select_model = [next, changed](kit::ModelProviderRef ref) {
    auto selected = next.Get();
    selected.model = std::move(ref);
    next = std::move(selected);
    if (changed) changed();
  };
  const auto choose_model = [model_menu, dialogs, model_anchor, choices, effective_model, select_model, add_model] {
    std::vector<MenuEntry> items;
    for (const auto& choice : choices) {
      auto label = choice.ref == DemoModel ? StringVariant{::app::strings::ui_local_demo_model} : StringVariant{choice.label};
      const auto activate = [select_model, ref = choice.ref] { select_model(ref); };
      const auto* brand = FindModelBrand(choice.connection.model);
      auto item = brand ? MenuItem(brand->image, std::move(label), activate) : MenuItem(std::move(label), activate);
      items.emplace_back(std::move(item).Checked(effective_model == choice.ref));
    }
    if (add_model) {
      if (!items.empty()) items.emplace_back(MenuSection{});
      items.emplace_back(MenuItem(::app::strings::ui_add_model_ellipsis, [dialogs, add_model, select_model] {
        ShowAddModel(dialogs, add_model, select_model);
      }));
    }
    if (!items.empty()) model_menu.Show(model_anchor, std::move(items), {.placement = {AnchorSide::Above, AnchorAlignment::Start}});
  };
  const auto choose_permission = [permission_menu, permission_anchor, dialogs, settings, next, effective_profile, full_access, full_access_selected,
                                  workspace, workspace_root, changed] {
    const auto set = [next, changed](kit::ExecutionProfileRef profile) {
      auto selected = next.Get();
      selected.execution_profile = std::move(profile);
      next = std::move(selected);
      if (changed) changed();
    };
    permission_menu.Show(permission_anchor, {
        MenuItem(::app::strings::ui_ask, [set] { set(AskProfile); }).Checked(effective_profile == AskProfile),
        MenuItem(::app::strings::ui_auto_review, [set] { set(ReviewProfile); }).Checked(effective_profile == ReviewProfile),
        MenuItem(::app::strings::ui_approve_project_edits, [dialogs, settings, workspace, workspace_root, set, full_access_selected] {
          if (!workspace || !workspace_root || full_access_selected) return;
          ShowPreviewDialog(dialogs, DialogFactory{[settings, workspace = *workspace, root = *workspace_root, set](DialogContext dialog) {
            const auto colors = UseTheme().colors;
            const auto style = UseEnvironment<ButtonStyle>();
            return Column{
                Text(::app::strings::ui_allow_patches_in_this_workspace, TextRole::Title),
                Text(root.Path()),
                Text(::app::strings::ui_full_access_description),
                Row{Spacer(),
                    ProvideEnvironment(PreviewSecondaryButton(style, colors),
                        Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); })),
                    ProvideEnvironment(PreviewPrimaryButton(style, colors),
                        Button(::app::strings::ui_allow_full_access).OnClick([settings, workspace, set, dialog] {
                          set(settings->GrantFullAccess(workspace));
                          dialog.Dismiss();
                        }))}.With(Spacing(8.0F)),
            }.With(Frame{.max_width = 470.0F}, Padding(20.0F), Spacing(12.0F));
          }});
        }).Checked(full_access_selected).Enabled(full_access),
    }, {.placement = {AnchorSide::Above, AnchorAlignment::Start}});
  };
  auto permission = effective_profile.id.value.starts_with(full_access_prefix) && !full_access_selected
      ? UseString(::app::strings::ui_confirm_project_edits) : UseString(ProfileLabel(effective_profile));
  const auto permission_ink = full_access_selected ? colors.on_primary_container : colors.on_surface_variant;
  std::vector<View> controls{
      RunOptionTrigger(std::move(model), ::app::strings::ui_model, "run-model", model_anchor, choose_model,
          colors.on_surface_variant, !choices.empty() || bool(add_model), selected_brand),
  };
  if (show_permissions && full_local_access) {
    controls.push_back(Row{
        Image(::app::images::shield).Tint(colors.on_primary_container).With(Frame{.width = 14.0F, .height = 14.0F}),
        Text(::app::strings::ui_full_access_mode).Style({Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_primary_container}),
    }.With(Frame{.min_height = 32.0F}, Padding(EdgeInsets::Symmetric(8.0F, 3.0F)), Spacing(5.0F),
        CrossAlign(CrossAxisAlignment::Center), Tooltip(::app::strings::ui_local_full_access_help)).Key("run-global-full-access"));
  } else if (show_permissions) {
    controls.push_back(RunOptionTrigger(std::move(permission), ::app::strings::ui_permissions,
        "run-permissions", permission_anchor, choose_permission, permission_ink, true, nullptr, true));
  }
  if (extra_action) {
    auto style = UseEnvironment<ButtonStyle>();
    style.background = Color::Transparent();
    style.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface_variant};
    style.padding = EdgeInsets::Symmetric(7.0F, 5.0F);
    style.minimum_height = 32.0F;
    controls.push_back(ProvideEnvironment(style, std::move(*extra_action)));
  }
  return Flow(std::move(controls)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center));
}

} // namespace agentkit_preview
