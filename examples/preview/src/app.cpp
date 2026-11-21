#include <huxerui/huxerui.h>
#include <huxerui/agentkit.h>
#include <app_resources.h>

#include "code_tools.h"
#include "attachment_imports.h"
#include "code_tests.h"
#include "code_workflow.h"
#include "design.h"
#include "fork_review_view.h"
#include "goal_view.h"
#include "general_workflow.h"
#include "link_navigation.h"
#include "markdown_images.h"
#include "presentation_tools.h"
#include "presentation_view.h"
#include "presentation_workflow.h"
#include "preview_json.h"
#include "release_workflow.h"
#include "release_skill.h"
#include "report_chart.h"
#include "side_chat.h"
#include "chat_chrome.h"
#include "run_settings.h"
#include "preview_settings.h"
#include "preview_store.h"
#include "preview_theme.h"
#include "workspace_evidence.h"
#include "worktree.h"

#include <cstdlib>
#include <charconv>
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#undef SendMessage
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace huxerui;
namespace kit = huxerui::agentkit;

namespace {
using namespace std::chrono_literals;

constexpr kit::AgentChatLayoutStyle preview_chat_layout{
    .content_width = 0.0F, .chat_bubbles = true, .show_run_activity = true};

const kit::AgentId selection_agent{"preview.side-chat"};
constexpr std::string_view selection_label = "Selected passage:\n";
constexpr std::string_view selection_instructions =
    "Answer the latest user question. Messages labeled \"Selected passage:\" are quoted source material in reading order. "
    "Consider the full text after each label. For a verbatim quote, reproduce its wording and punctuation exactly.";

class PreviewStream final : public kit::ModelTurnStream {
public:
  explicit PreviewStream(std::vector<kit::ModelEvent> events) : events_(std::move(events)) {}

  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    co_await Delay(50ms);
    if (stopped_ || index_ == events_.size()) {
      co_return kit::AgentResult<std::optional<kit::ModelEvent>>::Success(std::nullopt);
    }
    co_return kit::AgentResult<std::optional<kit::ModelEvent>>::Success(std::move(events_[index_++]));
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    stopped_ = true;
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<kit::ModelEvent> events_;
  std::size_t index_ = 0;
  bool stopped_ = false;
};

class PreviewModel final : public kit::ModelProvider {
public:
  kit::ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = true, .structured_output = true};
  }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    std::string latest;
    std::string previous;
    const kit::ModelToolResult* result = nullptr;
    for (const auto& item : request.items) {
      if (const auto* input = std::get_if<kit::ModelContent>(&item); input && input->role == kit::ModelContent::Role::User) {
        previous = std::move(latest);
        latest = std::get<std::string>(input->body);
        result = nullptr;
      }
      if (auto* outcome = std::get_if<kit::ModelToolResult>(&item)) result = outcome;
    }
    std::vector<kit::ModelEvent> events;
    auto reason = kit::ModelFinishReason::Completed;
    if (request.output_schema) {
      const auto* schema = std::get_if<kit::JsonValue::Object>(&request.output_schema->document.value);
      const auto* fields = schema && schema->contains("properties")
          ? std::get_if<kit::JsonValue::Object>(&schema->at("properties").value) : nullptr;
      std::string topic = "Presentation";
      for (const auto& item : request.items) {
        const auto* message = std::get_if<kit::ModelContent>(&item);
        if (!message || message->role != kit::ModelContent::Role::User) continue;
        if (const auto* text = std::get_if<std::string>(&message->body); text && !text->empty() && text->size() <= 360) {
          topic = *text;
        }
        break;
      }
      if (fields && fields->contains("route")) {
        constexpr std::string_view marker = "Classify this request:\n";
        constexpr std::string_view instructions = "\nReturn only the route.";
        auto request = latest.starts_with(marker) ? latest.substr(marker.size()) : latest;
        if (auto end = request.rfind(instructions); end != std::string::npos) request.resize(end);
        const auto route = request.starts_with("Outline this report:") ? "outline" :
            request.starts_with("Pie chart:") ? agentkit_preview::ReportChartStage :
            request.starts_with("Release deck:") ? std::string_view(agentkit_preview::ReleasePresentationSkill.value) :
            request.starts_with("Workspace release:") ? agentkit_preview::WorkspaceRelease :
            request.starts_with("Design briefing:") ? "design-briefing" : "answer";
        events = {kit::ModelStructuredOutput{kit::JsonValue{kit::JsonValue::Object{
            {"route", kit::JsonValue{std::string(route)}}}}}};
      } else if (fields && fields->contains("categories")) {
        constexpr std::string_view marker = "Pie chart:";
        auto start = latest.find(marker);
        const auto data = start == std::string::npos ? nlohmann::json{} :
            nlohmann::json::parse(latest.substr(start + marker.size()), nullptr, false);
        if (!data.is_object()) co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Failure({
            .code = kit::ErrorCode::InvalidArgument, .message = "Supply chart categories and values as JSON in local preview"});
        events = {kit::ModelStructuredOutput{agentkit_preview::DecodeJson(data)}};
      } else if (fields && fields->contains("nodes")) {
        constexpr std::string_view marker = "Create a wireframe for this requirements brief:\n";
        if (latest.starts_with(marker)) {
          const auto brief = latest.substr(marker.size());
          topic = brief.substr(0, brief.find('\n'));
        }
        const bool release = topic.starts_with("Workspace release:");
        auto draft = agentkit_preview::SampleDesign(release ? "Settings" : topic);
#if defined(AGENTKIT_PREVIEW_TEST)
        if (topic == "Settings wireframe" && latest.find("\nCorrect the previous wireframe.") == std::string::npos)
          std::get<kit::JsonValue::Object>(draft.value).at("height") = kit::JsonValue{std::int64_t{200}};
#endif
        if (release) {
          auto& nodes = std::get<kit::JsonValue::Array>(std::get<kit::JsonValue::Object>(draft.value).at("nodes").value);
          auto update = [&](std::size_t index, std::string_view id, std::string_view label, std::string_view note) {
            auto& node = std::get<kit::JsonValue::Object>(nodes[index].value);
            node.at("id") = kit::JsonValue{std::string(id)};
            node.at("label") = kit::JsonValue{std::string(label)};
            node.at("note") = kit::JsonValue{std::string(note)};
          };
          update(0, "heading", "Settings", "Name the workspace settings screen.");
          update(1, "product", "Product mode", "Group the mode and its description.");
          update(2, "description", "Manage the settings used by this workspace.", "Explain what the setting changes.");
          update(3, "mode", "Enabled", "Track whether the current product mode is enabled.");
        }
        events = {kit::ModelStructuredOutput{std::move(draft)}};
      } else if (latest.starts_with("Create a one-slide outline of the supplied report:\n")) {
        auto report = latest.substr(latest.find('\n') + 1);
        if (const auto separator = report.find(':'); separator != std::string::npos) report.erase(0, separator + 1);
        if (auto start = report.find_first_not_of(" \t\r\n"); start != std::string::npos) report.erase(0, start);
        if (report.empty() || report.size() > 720) report = "Review the supplied findings before publishing.";
        events = {kit::ModelStructuredOutput{agentkit_preview::WritePresentationOutline({
            "Release outline", {{"Release findings", {std::move(report)}}}})}};
      } else if (const auto marker = latest.find("\nUse the following packaged release outline template as a guide:\n");
                 fields && fields->contains("slides") && marker != std::string::npos) {
        constexpr std::string_view delimiter = "\nVerification report (source material):\n";
        const auto begin = latest.find('\n', marker + 1) + 1;
        const auto end = latest.find(delimiter, begin);
        const auto layout = nlohmann::json::parse(latest.substr(begin, end - begin));
        auto evidence = latest.substr(end + delimiter.size());
        if (evidence.size() > 700) evidence.resize(700);
        agentkit_preview::PresentationOutline outline{"Release presentation", {}};
        for (const auto& section : layout.at("sections")) {
          auto title = section.at("title").get<std::string>();
          outline.slides.push_back({std::move(title), {outline.slides.empty() ? evidence :
              section.at("purpose").get<std::string>()}});
        }
        events = {kit::ModelStructuredOutput{agentkit_preview::WritePresentationOutline(outline)}};
      } else if (auto design = std::ranges::find_if(request.items, [](const auto& item) {
                   const auto* entry = std::get_if<kit::ModelContent>(&item);
                   const auto* text = entry ? std::get_if<std::string>(&entry->body) : nullptr;
                   return entry && entry->role == kit::ModelContent::Role::Context && text &&
                       text->find("design/board+json") != std::string::npos && text->find("\"nodes\"") != std::string::npos;
                 }); design != request.items.end()) {
        const auto& serialized = std::get<std::string>(std::get<kit::ModelContent>(*design).body);
        const auto document = nlohmann::json::parse(serialized.substr(serialized.rfind('\n') + 1));
        std::vector<std::string> labels;
        for (const auto& node : document.at("nodes")) {
          labels.push_back(node.at("label").get<std::string>());
          if (labels.size() == 6) break;
        }
        agentkit_preview::PresentationOutline outline{"Design briefing", {{"Approved wireframe", std::move(labels)}}};
        for (const auto& item : request.items) {
          const auto* entry = std::get_if<kit::ModelContent>(&item);
          const auto* context = entry ? std::get_if<std::string>(&entry->body) : nullptr;
          if (!entry || entry->role != kit::ModelContent::Role::Context || !context ||
              context->find(agentkit_preview::VerificationEvidence) == std::string::npos) continue;
          const auto report = nlohmann::json::parse(context->substr(context->rfind('\n') + 1), nullptr, false);
          if (report.is_object() && report.value("exit_code", -1) == 0) {
            constexpr std::string_view marker = "Required slide count: ";
            auto count = std::size_t{2};
            if (const auto found = latest.rfind(marker); found != std::string::npos) {
              std::from_chars(latest.data() + found + marker.size(), latest.data() + latest.size(), count);
            }
            if (count == 1) {
              outline.slides.front().bullets.resize(3);
              outline.slides.front().bullets.push_back("Workspace tests passed");
            } else {
              for (std::size_t page = 0; page + 2 < count && page < 28; ++page) {
                const auto& elements = outline.slides.front().bullets;
                outline.slides.push_back({"Implementation review " + std::to_string(page + 1), {elements[page % elements.size()]}});
              }
              outline.slides.push_back({"Verification", {"Workspace tests passed"}});
            }
          }
          break;
        }
        events = {kit::ModelStructuredOutput{agentkit_preview::WritePresentationOutline(outline)}};
      } else events = {kit::ModelTextDelta{"A sample outline is ready to review."},
                kit::ModelStructuredOutput{agentkit_preview::WritePresentationOutline({
                    std::move(topic), {{"Overview", {"Describe the purpose and audience"}},
                                       {"Next steps", {"Summarize decisions and owners"}}}})}};
    } else if (latest == agentkit_preview::GeneralAnswerInstruction) {
      events = {kit::ModelTextDelta{"Received: "}, kit::ModelTextDelta{std::move(previous)}};
    } else if (request.instructions == selection_instructions) {
      for (const auto& item : request.items) {
        const auto* content = std::get_if<kit::ModelContent>(&item);
        if (content && content->role == kit::ModelContent::Role::Context) {
          const auto label = events.empty() ? "Selected text: " : "\nSelected text: ";
          const auto passage = std::string_view(std::get<std::string>(content->body));
          if (passage.starts_with(selection_label))
            events.emplace_back(kit::ModelTextDelta{label + std::string(passage.substr(selection_label.size()))});
        }
      }
    } else if (result) {
      if (result->error || result->provider_call_id != "release-skill")
        events = {kit::ModelTextDelta{result->error ? result->error->message : "Preview action completed."}};
    } else if (std::ranges::find_if(request.tools, [](const auto& tool) {
                 return tool.tool.id == kit::SkillActivationTool;
               }) != request.tools.end() && !request.available_skills.empty()) {
      const auto& skill = request.available_skills.front().ref;
      const auto args = nlohmann::json{{"source", skill.source.value}, {"id", skill.id.value}, {"version", std::to_string(skill.version)}};
      events = {kit::ModelToolStarted{"release-skill", kit::SkillActivationTool.value},
                kit::ModelToolArguments{"release-skill", args.dump()}, kit::ModelToolFinished{"release-skill"}};
      reason = kit::ModelFinishReason::ToolCalls;
    } else if (latest == "approval" && !request.tools.empty()) {
      events = {kit::ModelTextDelta{"An approval request is ready."},
                kit::ModelToolStarted{"preview-action", request.tools.front().name},
                kit::ModelToolArguments{"preview-action", "{}"}, kit::ModelToolFinished{"preview-action"}};
      reason = kit::ModelFinishReason::ToolCalls;
    } else {
      events = {kit::ModelTextDelta{"Received: "}, kit::ModelTextDelta{std::move(latest)},
                kit::ModelTextDelta{"\n\nEnter 'approval' to try a tool confirmation."}};
    }
    events.emplace_back(kit::ModelFinished{reason, {}});
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(
        std::make_unique<PreviewStream>(std::move(events)));
  }
};

struct NavigationFailure {
  bool loading;
  kit::AgentError error;
  bool operator==(const NavigationFailure&) const = default;
};

struct PreviewService {
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::shared_ptr<agentkit_preview::CodeWorkspaces> workspaces;
  std::shared_ptr<agentkit_preview::WorktreeForks> worktrees;
  std::optional<File> workspace_evidence;
#ifdef __EMSCRIPTEN__
  std::optional<File> exports;
#endif
  std::shared_ptr<agentkit_preview::RunSettings> run_settings;
  std::optional<File> model_profiles;
  std::optional<File> settings_file;
  State<agentkit_preview::PreviewSettings> settings{agentkit_preview::PreviewSettings{}};
  State<agentkit_preview::PreviewSettings> applied_settings{agentkit_preview::PreviewSettings{}};
  std::shared_ptr<agentkit_preview::AttachmentImports> attachments;
  std::shared_ptr<agentkit_preview::ComposerPreferences> preferences;
  std::shared_ptr<agentkit_preview::ConversationNavigationStore> navigation_store;
  State<agentkit_preview::NavigationPreferences> navigation{agentkit_preview::NavigationPreferences{}};
  State<std::optional<NavigationFailure>> navigation_failure{std::optional<NavigationFailure>{}};
  std::deque<agentkit_preview::NavigationEdit> navigation_edits;
  bool saving_navigation = false;
  bool reload_navigation = false;
  std::map<kit::SessionId, agentkit_preview::ComposerDraft> composers;
  State<std::string> preference_load_error{std::string{}};
  std::string model_configuration_error;
  std::optional<kit::WorkspaceId> startup_workspace;
  State<bool> code_available{false};
  using Started = kit::AgentResult<std::shared_ptr<kit::AgentRuntime>>;
  std::optional<Started> startup;
  std::vector<CallbackCompletion<Started>> listeners;
};

std::optional<View> RenderPreviewBlock([[maybe_unused]] const PreviewService& service, const kit::Message& message,
    const kit::ContentBlock& block, std::shared_ptr<kit::Conversation> conversation) {
  if (const auto* step = std::get_if<kit::ToolBlock>(&block.body)) {
    const auto current = conversation->Snapshot();
    if (!current) return {};
    const auto call = std::ranges::find(current->tools, step->call, &kit::ToolCallSnapshot::id);
    if (call == current->tools.end()) return {};
    if ((call->tool.id == agentkit_preview::RunCodeTests || call->tool.id == agentkit_preview::RunWorkspaceTests) &&
        call->output_message == message.ref) return agentkit_preview::CTestActivity(message);
    if (agentkit_preview::IsCodePatchTool(call->tool.id) && !call->effects.empty() && service.workspace_evidence) {
      auto workspace = current->summary.workspace ? service.workspaces->Find(*current->summary.workspace) : std::nullopt;
      return agentkit_preview::WorkspaceChangeActivity(std::move(conversation), service.runtime, *call,
          std::move(workspace), *service.workspace_evidence);
    }
    return {};
  }
  if (const auto* custom = std::get_if<kit::CustomBlock>(&block.body)) {
    if (custom->type == agentkit_preview::ReportChart)
      return agentkit_preview::RenderReportChart({message.ref, block.id}, *custom, std::move(conversation));
#ifdef __EMSCRIPTEN__
    if (custom->type == agentkit_preview::BrowserPresentationExport && service.exports)
      return agentkit_preview::RenderBrowserPresentationExport(*custom, *service.exports, service.runtime);
#endif
    return {};
  }
  return agentkit_preview::RenderCTestDiagnostics(message, block);
}

kit::Async<std::shared_ptr<kit::AgentRuntime>> WaitForRuntime(std::shared_ptr<PreviewService> service) {
  if (service->startup) co_return *service->startup;
  auto listen = [service](CallbackCompletion<PreviewService::Started> reply) {
    if (service->startup) reply.Complete(*service->startup);
    else service->listeners.push_back(std::move(reply));
    return std::function<void()>{};
  };
  co_return co_await AwaitCallback<PreviewService::Started>(std::move(listen));
}

void Require(kit::AgentResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

std::string Environment(const char* name) {
  const auto* value = std::getenv(name);
  return value ? std::string(value) : std::string{};
}

constexpr std::string_view workspace_project = "preview.workspace/";
const auto& preview_user = agentkit_preview::PreviewUser;

using agentkit_preview::ConversationGroup;
using agentkit_preview::NavigationPreferences;

void SaveNavigation(const std::shared_ptr<PreviewService>& service) {
  if (service->saving_navigation || (!service->reload_navigation && service->navigation_edits.empty())) return;
  service->saving_navigation = true;
  UseApplicationTaskScope().Launch([service]() -> Task<void> {
    while (service->reload_navigation || !service->navigation_edits.empty()) {
      if (std::exchange(service->reload_navigation, false)) {
        auto loaded = co_await service->navigation_store->LoadAsync(preview_user);
        if (!loaded.Succeeded()) {
          service->reload_navigation = true;
          service->navigation_failure = NavigationFailure{true, loaded.Error()};
          break;
        }
        service->navigation = std::move(loaded).Value();
        service->navigation_failure = std::nullopt;
        continue;
      }
      auto saved = co_await service->navigation_store->EditAsync(preview_user, service->navigation_edits.front());
      if (!saved.Succeeded()) {
        if (saved.Error().code == kit::ErrorCode::Conflict || saved.Error().code == kit::ErrorCode::NotFound) {
          auto latest = co_await service->navigation_store->LoadAsync(preview_user);
          if (latest.Succeeded()) service->navigation = std::move(latest).Value();
        }
        service->navigation_failure = NavigationFailure{false, saved.Error()};
        break;
      }
      service->navigation = std::move(saved).Value();
      service->navigation_edits.pop_front();
      service->navigation_failure = std::nullopt;
    }
    service->saving_navigation = false;
  });
}

void EditNavigation(const std::shared_ptr<PreviewService>& service, agentkit_preview::NavigationEdit edit) {
  service->navigation_edits.push_back(std::move(edit));
  if (!service->navigation_failure.Get()) SaveNavigation(service);
}

std::optional<std::string> GroupFor(const NavigationPreferences& navigation, const kit::SessionId& session) {
  auto found = navigation.memberships.find(session);
  return found == navigation.memberships.end() ? std::nullopt : std::optional{found->second};
}

void PlaceConversation(const std::shared_ptr<PreviewService>& service, kit::SessionId session,
                       std::optional<std::string> group, std::optional<std::string> expected = {}) {
  EditNavigation(service, [session = std::move(session), group = std::move(group),
                           expected = std::move(expected)](NavigationPreferences& data) {
    const auto current = GroupFor(data, session);
    if (current != expected && current != group)
      return kit::AgentResult<void>::Failure({.code = kit::ErrorCode::Conflict, .message = "This conversation moved to another group"});
    if (group) {
      if (std::ranges::find(data.groups, *group, &ConversationGroup::id) == data.groups.end())
        return kit::AgentResult<void>::Failure({.code = kit::ErrorCode::NotFound, .message = "The conversation group is no longer available"});
      data.memberships.insert_or_assign(session, *group);
    } else {
      data.memberships.erase(session);
    }
    return kit::AgentResult<void>::Success();
  });
}

void SaveComposerPreference(std::shared_ptr<PreviewService> service, kit::SessionId id) {
  auto& composer = service->composers.at(id);
  if (composer.saving_preference) return;
  composer.saving_preference = true;
  UseApplicationTaskScope().Launch([service, id = std::move(id)]() -> Task<void> {
    auto& draft = service->composers.at(id);
    for (;;) {
      const auto setting = draft.next.Get();
      auto saved = co_await service->preferences->SaveAsync(preview_user, id, setting);
      if (setting != draft.next.Get()) continue;
      draft.preference_error = saved.Succeeded() ? "" : "Could not save this conversation's run settings";
      draft.saving_preference = false;
      co_return;
    }
  });
}

kit::ProjectId ProjectFor(std::optional<kit::WorkspaceId> workspace) {
  return {workspace ? std::string(workspace_project) + workspace->value : std::string("preview.local")};
}

struct PreviewModelOptions {
  std::string model;
  std::string base_url;
  std::string provider;
  std::string protocol;
  std::string api_key;
  std::string anthropic_version;
  bool allow_server_state = false;
  bool persist_resume_state = false;
  bool image_input = false;
  bool file_input = false;
  std::string credential_environment;
  bool reasoning_summary = false;
};

std::optional<kit::ModelRegistration> ConfiguredModel(PreviewModelOptions options, std::string_view slot) {
  const auto identity_for = [slot, image = options.image_input, file = options.file_input](std::string base) {
    if (image || file) base += "/media/" + std::to_string(image) + "/" + std::to_string(file);
    if (!slot.empty()) base = "preview.profile/" + std::string(slot) + "/" + base;
    return kit::ModelProviderRef{{std::move(base)}, 1};
  };
  auto& model = options.model;
  auto& url = options.base_url;
  auto& provider = options.provider;
  const auto& protocol = options.protocol;
  if (model.empty() && url.empty() && provider.empty() && protocol.empty()) return {};
  if (model.empty() || url.empty()) throw std::invalid_argument("Set both AgentKit model and base URL");
  if (provider.empty()) provider = protocol == "messages" ? "anthropic" :
      protocol == "generate-content" || protocol == "interactions" ? "gemini" : "openai";
  if ((options.image_input || options.file_input) && protocol != "generate-content" && protocol != "interactions")
    throw std::invalid_argument("Attachments require Gemini GenerateContent or Interactions");
  if (options.reasoning_summary && (provider != "openai" || protocol != "responses"))
    throw std::invalid_argument("Reasoning summaries require a supporting OpenAI Responses model");
  const auto connect = [](auto registration) {
    if (!registration.Succeeded()) throw std::invalid_argument(registration.Error().message);
    return std::move(registration).Value();
  };
  std::function<kit::Async<std::string>()> credential;
  if (!options.credential_environment.empty()) {
    credential = [name = std::move(options.credential_environment)]() -> kit::Async<std::string> {
      auto token = Environment(name.c_str());
      if (token.empty()) co_return kit::AgentResult<std::string>::Failure({
          .code = kit::ErrorCode::ProviderFailure, .message = "Model API key environment variable is unset: " + name});
      co_return kit::AgentResult<std::string>::Success(std::move(token));
    };
  } else if (!options.api_key.empty()) {
    credential = [token = std::move(options.api_key)]() -> kit::Async<std::string> {
      co_return kit::AgentResult<std::string>::Success(token);
    };
  }
  if (protocol == "responses") {
    const std::map<std::string_view, kit::ResponsesDialect> dialects{
        {"openai", kit::ResponsesDialect::OpenAI}, {"deepseek", kit::ResponsesDialect::DeepSeek},
        {"glm", kit::ResponsesDialect::GlmCompatible}};
    const auto dialect = dialects.find(provider);
    if (dialect == dialects.end()) throw std::invalid_argument("Unknown AgentKit Responses provider");
    const bool store = dialect->second == kit::ResponsesDialect::GlmCompatible &&
        options.allow_server_state;
    const bool persist = options.persist_resume_state;
    auto provider_prefix = std::string("preview.responses/");
    if (provider != "openai" || persist) {
      provider_prefix += provider + "/" + std::to_string(store) + "/" + std::to_string(persist) + "/";
    }
    if (options.reasoning_summary) provider_prefix += "summary/";
    const auto identity = identity_for(provider_prefix + std::to_string(url.size()) + "/" + url + model);
    return connect(kit::MakeResponsesModel({.ref = identity, .model = std::move(model), .base_url = std::move(url),
        .bearer_token = std::move(credential), .allow_persist_resume_state = persist,
        .output_format = kit::ResponsesOutputFormat::JsonObject, .dialect = dialect->second, .allow_server_state = store,
        .reasoning_summary = options.reasoning_summary}));
  }
  if (protocol == "messages") {
    const std::map<std::string_view, kit::MessagesDialect> dialects{
        {"anthropic", kit::MessagesDialect::Anthropic}, {"deepseek", kit::MessagesDialect::DeepSeek},
        {"glm", kit::MessagesDialect::GlmCompatible}};
    const auto dialect = dialects.find(provider);
    if (dialect == dialects.end()) throw std::invalid_argument("Unknown AgentKit Messages provider");
    auto api_version = std::move(options.anthropic_version);
    if (api_version.empty()) api_version = "2023-06-01";
    const auto identity = identity_for("preview.messages/" + provider + "/" + api_version +
        "/" + std::to_string(url.size()) + "/" + url + model);
    const auto output = dialect->second == kit::MessagesDialect::Anthropic
        ? kit::MessagesOutputFormat::JsonSchema : kit::MessagesOutputFormat::PromptedJson;
    return connect(kit::MakeMessagesModel({.ref = identity, .model = std::move(model), .base_url = std::move(url),
        .dialect = dialect->second, .api_key = std::move(credential),
        .api_version = std::move(api_version), .output_format = output}));
  }
  if (protocol == "generate-content") {
    if (provider != "gemini") throw std::invalid_argument("Unknown AgentKit GenerateContent provider");
    const auto identity = identity_for("preview.generate-content/" + std::to_string(url.size()) + "/" + url + model);
    return connect(kit::MakeGeminiGenerateContentModel({.ref = identity, .model = std::move(model),
        .base_url = std::move(url), .api_key = std::move(credential),
        .image_input = options.image_input, .file_input = options.file_input}));
  }
  if (protocol == "interactions") {
    if (provider != "gemini") throw std::invalid_argument("Unknown AgentKit Interactions provider");
    const bool store = options.allow_server_state;
    const bool persist = options.persist_resume_state;
    const auto identity = identity_for("preview.interactions/" + std::to_string(store) + "/" +
        std::to_string(persist) + "/" + std::to_string(url.size()) + "/" + url + model);
    return connect(kit::MakeGeminiInteractionsModel({.ref = identity, .model = std::move(model),
        .base_url = std::move(url), .api_key = std::move(credential),
        .image_input = options.image_input, .file_input = options.file_input, .allow_server_state = store,
        .allow_persist_resume_state = persist}));
  }
  if (!protocol.empty() && protocol != "chat-completions") throw std::invalid_argument("Unknown AgentKit model protocol");
  const std::map<std::string_view, kit::ChatCompletionsDialect> dialects{
      {"openai", kit::ChatCompletionsDialect::OpenAI}, {"deepseek", kit::ChatCompletionsDialect::DeepSeek},
      {"gemini", kit::ChatCompletionsDialect::GeminiCompatible}, {"glm", kit::ChatCompletionsDialect::GlmCompatible},
      {"compatible", kit::ChatCompletionsDialect::Compatible}};
  auto dialect = dialects.find(provider);
  if (dialect == dialects.end()) throw std::invalid_argument("Unknown AgentKit Chat Completions provider");
  const auto identity = identity_for("preview.chat/" + provider + "/" + std::to_string(url.size()) + "/" + url + model);
  kit::ChatCompletionsOptions chat{
      .ref = identity, .model = std::move(model), .base_url = std::move(url), .dialect = dialect->second,
      .bearer_token = std::move(credential),
      .request_usage = provider == "openai",
      .developer_role = provider == "openai",
      .output_format = kit::ChatOutputFormat::JsonObject};
  if (provider == "deepseek" || provider == "glm") chat.thinking = false;
  return connect(kit::MakeChatCompletionsModel(std::move(chat)));
}

bool ValidModelProfileName(std::string_view profile) {
  return !profile.empty() && profile.size() <= 32 && std::ranges::all_of(profile, [](char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
  });
}

#ifndef __EMSCRIPTEN__
using agentkit_preview::ModelKeyEnvironment;
using agentkit_preview::TrimModelField;

constexpr std::size_t max_model_key_bytes = 4096;

#ifdef _WIN32
struct LocalCredentialData {
  DATA_BLOB blob{};

  ~LocalCredentialData() {
    if (!blob.pbData) return;
    SecureZeroMemory(blob.pbData, blob.cbData);
    LocalFree(blob.pbData);
  }
};

std::vector<std::uint8_t> ProtectModelKey(std::string& key) {
  DATA_BLOB input{static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(key.data())};
  LocalCredentialData output;
  if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
    throw std::runtime_error("Windows could not protect the model API key.");
  }
  return {output.blob.pbData, output.blob.pbData + output.blob.cbData};
}

std::string UnprotectModelKey(const nlohmann::json& value) {
  if (!value.is_array() || value.empty() || value.size() > max_model_key_bytes * 2) {
    throw std::runtime_error("The protected model API key is invalid.");
  }
  auto bytes = value.get<std::vector<std::uint8_t>>();
  DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()};
  LocalCredentialData output;
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output.blob) ||
      !output.blob.cbData || output.blob.cbData > max_model_key_bytes) {
    throw std::runtime_error("Windows could not read the protected model API key.");
  }
  return {reinterpret_cast<const char*>(output.blob.pbData), output.blob.cbData};
}
#endif

kit::ModelRegistration MakeProfileModel(const agentkit_preview::ModelProfileInput& profile, std::string environment) {
  const std::string_view url = profile.base_url;
  const auto scheme = url.starts_with("https://") ? 8 : url.starts_with("http://") ? 7 : 0;
  const auto authority = url.substr(scheme, url.find('/', scheme) - scheme);
  if (profile.model.empty() || profile.model.size() > 256 || url.size() > 2048 || !scheme || authority.empty() ||
      authority.find('@') != std::string_view::npos || url.find_first_of("?#\\ \t\r\n") != std::string_view::npos) {
    throw std::invalid_argument("Enter a model ID and an HTTP(S) API base URL without credentials, query, or fragment.");
  }
  if (profile.api_key.size() > max_model_key_bytes ||
      profile.api_key.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
    throw std::invalid_argument("Enter a single-line API key of at most 4096 bytes.");
  }
  auto model = ConfiguredModel({.model = profile.model, .base_url = profile.base_url, .provider = profile.provider,
      .protocol = profile.protocol, .api_key = environment.empty() ? profile.api_key : std::string{},
      .image_input = profile.image_input, .file_input = profile.file_input,
      .credential_environment = std::move(environment), .reasoning_summary = profile.reasoning_summary}, {});
  return std::move(model).value();
}

Task<void> LoadModelProfiles(const std::shared_ptr<PreviewService>& service, const File& directory,
                             std::vector<kit::ModelRegistration>& models) {
  if (!co_await directory.CreateDirectoriesAsync()) {
    service->model_configuration_error = "Could not open saved model profiles.";
    co_return;
  }
  auto files = co_await directory.ListChildrenAsync();
  if (!files.Succeeded()) {
    service->model_configuration_error = "Could not read saved model profiles.";
    co_return;
  }
  std::ranges::sort(files.Value(), {}, &File::Name);
  for (const auto& file : files.Value()) {
    if (!file.Name().ends_with(".json")) continue;
    try {
      auto raw = co_await file.ReadStringAsync();
      if (!raw.Succeeded()) throw std::runtime_error("Could not read model profile");
      const auto record = nlohmann::json::parse(raw.Value());
      const auto reference = record.value("api_key_reference", std::string{});
      auto environment = ModelKeyEnvironment(reference);
      if (!reference.empty() && environment.empty()) throw std::invalid_argument("Invalid model API key reference");
      std::string api_key;
      if (const auto encrypted = record.find("protected_api_key"); encrypted != record.end()) {
#ifdef _WIN32
        if (!environment.empty()) throw std::invalid_argument("Conflicting model API keys");
        api_key = UnprotectModelKey(*encrypted);
#else
        throw std::invalid_argument("The model API key was protected on Windows");
#endif
      }
      const auto limit = [&](const char* key) -> std::optional<std::uint64_t> {
        const auto value = record.find(key);
        if (value == record.end() || !value->is_number_unsigned()) return {};
        const auto tokens = value->get<std::uint64_t>();
        return tokens ? std::optional{tokens} : std::nullopt;
      };
      agentkit_preview::ModelLimits limits{limit("context_window"), limit("max_input_tokens"), limit("max_output_tokens")};
      auto model = MakeProfileModel({.model = record.at("model").get<std::string>(),
          .base_url = record.at("base_url").get<std::string>(), .protocol = record.at("protocol").get<std::string>(),
          .provider = record.at("provider").get<std::string>(), .api_key = std::move(api_key),
          .image_input = record.value("image_input", false), .file_input = record.value("file_input", false),
          .reasoning_summary = record.value("reasoning_summary", false)},
          std::move(environment));
      if (std::ranges::find(models, model.ref, &kit::ModelRegistration::ref) == models.end()) {
        service->run_settings->AddModel(model, {}, limits);
        models.push_back(std::move(model));
      }
    } catch (const std::exception&) {
      service->model_configuration_error = "Some saved model profiles could not be loaded.";
    }
  }
}

kit::Async<kit::ModelProviderRef> SaveModelProfile(std::shared_ptr<PreviewService> service,
                                                   agentkit_preview::ModelProfileInput profile) {
  using Result = kit::AgentResult<kit::ModelProviderRef>;
  profile.model = TrimModelField(std::move(profile.model));
  profile.base_url = TrimModelField(std::move(profile.base_url));
  while (profile.base_url.ends_with('/')) profile.base_url.pop_back();
  profile.api_key = TrimModelField(std::move(profile.api_key));
  kit::ModelRegistration model;
  std::string environment;
  try {
    environment = ModelKeyEnvironment(profile.api_key);
    model = MakeProfileModel(profile, environment);
  } catch (const std::invalid_argument& error) {
    co_return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = error.what()});
  }
  const auto existing = service->run_settings->Models(agentkit_preview::GeneralAgent);
  if (std::ranges::find(existing, model.ref, &agentkit_preview::ModelChoice::ref) != existing.end()) {
    co_return Result::Failure({.code = kit::ErrorCode::Conflict, .message = "This model and endpoint are already registered."});
  }
  const auto name = kit::NewCommandId().value;
  const auto target = service->model_profiles->Child(name + ".json");
  const auto pending = service->model_profiles->Child(name + ".pending");
  auto record = nlohmann::json{{"model", profile.model}, {"base_url", profile.base_url},
      {"protocol", profile.protocol}, {"provider", profile.provider},
      {"image_input", profile.image_input}, {"file_input", profile.file_input}, {"reasoning_summary", profile.reasoning_summary}};
  if (profile.limits.context_window) record["context_window"] = *profile.limits.context_window;
  if (profile.limits.max_input_tokens) record["max_input_tokens"] = *profile.limits.max_input_tokens;
  if (profile.limits.max_output_tokens) record["max_output_tokens"] = *profile.limits.max_output_tokens;
  const bool direct = !profile.api_key.empty() && environment.empty();
  if (!environment.empty()) record["api_key_reference"] = profile.api_key;
#ifdef _WIN32
  if (direct) {
    try {
      record["protected_api_key"] = ProtectModelKey(profile.api_key);
    } catch (const std::runtime_error& error) {
      co_return Result::Failure({.code = kit::ErrorCode::PersistenceFailure, .message = error.what()});
    }
  }
  const bool persist = true;
#else
  const bool persist = !direct;
#endif
  if (persist && (!co_await pending.WriteStringAsync(record.dump()) || !co_await pending.MoveToAsync(target))) {
    static_cast<void>(co_await pending.DeleteAsync());
    co_return Result::Failure({.code = kit::ErrorCode::PersistenceFailure, .message = "Could not save this model profile."});
  }
  auto registered = service->runtime->RegisterModel(model);
  if (!registered.Succeeded()) {
    if (persist) static_cast<void>(co_await target.DeleteAsync());
    co_return Result::Failure(registered.Error());
  }
  service->run_settings->AddModel(model, {}, profile.limits);
  if (!service->run_settings->DefaultModel(agentkit_preview::CodeAgent)) {
    service->run_settings->SetDefaults(*service->run_settings->DefaultModel(agentkit_preview::GeneralAgent), model.ref);
  }
  service->code_available = true;
  co_return Result::Success(std::move(model.ref));
}

std::optional<kit::ModelRegistration> ConfiguredEnvironmentModel(std::string_view slot = {}) {
  const std::string prefix = slot.empty() ? "HUXERUI_AGENTKIT_" : "HUXERUI_AGENTKIT_" + std::string(slot) + "_";
  const auto read = [&prefix](std::string_view name) { return Environment((prefix + std::string(name)).c_str()); };
  return ConfiguredModel({read("MODEL"), read("BASE_URL"), read("PROVIDER"), read("PROTOCOL"), read("API_KEY"),
      read("ANTHROPIC_VERSION"), read("ALLOW_SERVER_STATE") == "1", read("PERSIST_RESUME_STATE") == "1",
      read("IMAGE_INPUT") == "1", read("FILE_INPUT") == "1", {}, read("REASONING_SUMMARY") == "1"}, slot);
}

std::vector<kit::ModelRegistration> ConfiguredModels() {
  std::vector<kit::ModelRegistration> configured;
  if (auto model = ConfiguredEnvironmentModel()) configured.push_back(std::move(*model));
  const auto slots = Environment("HUXERUI_AGENTKIT_MODEL_PROFILES");
  if (slots.ends_with(',')) throw std::invalid_argument("Invalid AgentKit model profile name");
  std::string_view remaining(slots);
  std::set<std::string_view> names;
  while (!remaining.empty()) {
    const auto comma = remaining.find(',');
    const auto slot = remaining.substr(0, comma);
    if (!ValidModelProfileName(slot) || !names.insert(slot).second)
      throw std::invalid_argument("Invalid AgentKit model profile name");
    auto model = ConfiguredEnvironmentModel(slot);
    if (!model) throw std::invalid_argument("Configure a base URL and model for every AgentKit model profile");
    configured.push_back(std::move(*model));
    if (comma == std::string_view::npos) break;
    remaining.remove_prefix(comma + 1);
  }
  return configured;
}

std::optional<kit::AgentResult<kit::ModelRegistration>> ConfiguredReviewModel() {
  using Result = kit::AgentResult<kit::ModelRegistration>;
  try {
    if (auto model = ConfiguredEnvironmentModel("REVIEW")) return Result::Success(std::move(*model));
    if (Environment("HUXERUI_AGENTKIT_REVIEW_API_KEY").empty()) return {};
  } catch (const std::invalid_argument&) {
  }
  return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Review model configuration is incomplete or invalid"});
}
#else
EM_JS_DEPS(agentkit_model_configuration, "$stringToNewUTF8");
EM_JS(char*, ReadBrowserModelConfiguration, (), {
  return stringToNewUTF8(Module['agentkitModelConfiguration'] ?? '{}');
});

struct BrowserModelConfiguration {
  std::vector<kit::ModelRegistration> models;
  std::optional<kit::AgentResult<kit::ModelRegistration>> reviewer;
  std::string error;
};

kit::ModelRegistration BrowserModel(const nlohmann::json& profile, std::string_view slot, bool reviewer) {
  constexpr std::array<std::string_view, 9> keys{
      "model", "base_url", "provider", "protocol", "anthropic_version", "allow_server_state", "persist_resume_state",
      "image_input", "file_input"};
  if (!profile.is_object()) throw std::invalid_argument("Expected a model profile");
  for (const auto& field : profile.items()) {
    const auto& key = field.key();
    if ((!reviewer && key == "id") || std::ranges::find(keys, key) != keys.end()) continue;
    throw std::invalid_argument("Unknown model profile field");
  }
  auto model = ConfiguredModel({
      .model = profile.at("model").get<std::string>(),
      .base_url = profile.at("base_url").get<std::string>(),
      .provider = profile.value("provider", std::string{}),
      .protocol = profile.value("protocol", std::string{}),
      .anthropic_version = profile.value("anthropic_version", std::string{}),
      .allow_server_state = profile.value("allow_server_state", false),
      .persist_resume_state = profile.value("persist_resume_state", false),
      .image_input = profile.value("image_input", false), .file_input = profile.value("file_input", false)}, slot);
  if (!model) throw std::invalid_argument("Provide a model and base URL");
  return std::move(*model);
}

BrowserModelConfiguration ConfiguredBrowserModels() {
  using Review = kit::AgentResult<kit::ModelRegistration>;
  BrowserModelConfiguration configured;
  std::unique_ptr<char, decltype(&std::free)> raw(ReadBrowserModelConfiguration(), &std::free);
  try {
    const auto config = nlohmann::json::parse(raw ? raw.get() : "{}");
    if (config.is_object() && config.contains("error")) {
      configured.error = "Could not load agentkit-models.json.";
      return configured;
    }
    if (!config.is_object()) throw std::invalid_argument("Expected a model directory");
    for (const auto& field : config.items()) {
      if (field.key() != "profiles" && field.key() != "reviewer")
        throw std::invalid_argument("Unknown model directory field");
    }
    if (const auto profiles = config.find("profiles"); profiles != config.end()) {
      std::set<std::string> names;
      for (const auto& profile : profiles->get_ref<const nlohmann::json::array_t&>()) {
        auto name = profile.at("id").get<std::string>();
        if (!ValidModelProfileName(name) || !names.insert(name).second)
          throw std::invalid_argument("Invalid model profile ID");
        configured.models.push_back(BrowserModel(profile, name, false));
      }
    }
    if (const auto reviewer = config.find("reviewer"); reviewer != config.end()) {
      try {
        configured.reviewer = Review::Success(BrowserModel(*reviewer, "REVIEW", true));
      } catch (const std::exception&) {
        configured.reviewer = Review::Failure({
            .code = kit::ErrorCode::InvalidArgument, .message = "Review model configuration is incomplete or invalid"});
      }
    }
  } catch (const std::exception&) {
    configured.models.clear();
    configured.error = "Invalid Web model configuration. Check agentkit-models.json.";
  }
  return configured;
}
#endif

agentkit_preview::AddModelProfile AddModelAction(const std::shared_ptr<PreviewService>& service) {
#ifndef __EMSCRIPTEN__
  if (service->model_profiles) return [service](agentkit_preview::ModelProfileInput profile) {
    return SaveModelProfile(service, std::move(profile));
  };
#endif
  return {};
}

std::shared_ptr<kit::AgentRuntime> MakeRuntime(TaskScope tasks, std::shared_ptr<kit::SessionStore> store,
                                              std::vector<kit::ModelRegistration> configured,
                                              agentkit_preview::PresentationFiles files,
                                              std::shared_ptr<agentkit_preview::CodeWorkspaces> workspaces,
                                              std::shared_ptr<agentkit_preview::WorktreeForks> worktrees,
                                              std::optional<kit::ModelRegistration> code_model,
                                              std::optional<File> test_runner,
                                              File test_logs, std::shared_ptr<agentkit_preview::RunSettings> settings,
                                              std::shared_ptr<agentkit_preview::AttachmentImports> attachments,
                                              std::optional<kit::AgentResult<kit::ModelRegistration>> review_model,
                                              const agentkit_preview::PreviewSettings& preferences,
                                              bool review_user_comparisons, bool can_add_models) {
  const auto designs = files.staging.Parent()->Child("designs");
  const auto evidence = files.staging.Parent()->Child("workspace-evidence");
  const bool code_available = !configured.empty() || code_model.has_value();
  const bool release_available = code_available && test_runner.has_value();
  const auto model_for = [&configured](const kit::ModelProviderRef& ref) {
    return std::ranges::find(configured, ref, &kit::ModelRegistration::ref);
  };
  const auto daily = preferences.daily_model ? model_for(*preferences.daily_model) : configured.begin();
  const auto general = daily != configured.end() ? daily->ref : agentkit_preview::DemoModel;
  if (preferences.review_model) {
    const auto review = model_for(*preferences.review_model);
    using Registration = kit::AgentResult<kit::ModelRegistration>;
    review_model = review != configured.end() && review->provider->Capabilities().structured_output
        ? Registration::Success(*review)
        : Registration::Failure({.code = kit::ErrorCode::NotFound, .message = "The selected Auto Review model is unavailable"});
  }
  auto runtime_ref = std::make_shared<std::weak_ptr<kit::AgentRuntime>>();
  auto runtime = kit::AgentRuntime::Create({
      .tasks = tasks,
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success(preview_user); },
      .access = [settings, workspaces, review_user_comparisons](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        if (review_user_comparisons && request.capability == kit::Capability::CreateComparison &&
            request.operation != kit::JsonValue{}) {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::RequireConfirmation, {}});
        }
        if (request.capability == kit::Capability::ActivateSkill && request.skill &&
            request.skill->source == agentkit_preview::PreviewSkills && request.actor.kind == kit::ActionParty::Kind::Agent) {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::RequireConfirmation, {}});
        }
        const auto tool = request.tool ? request.tool->id : kit::ToolId{};
        const auto workspace = request.workspace ? workspaces->Find(*request.workspace) : std::nullopt;
        const bool workspace_available = workspace && workspace->NativeFolder();
        if (agentkit_preview::IsWorkspaceTool(tool) && !workspace_available &&
            (request.capability == kit::Capability::DiscoverTool || request.capability == kit::Capability::ExecuteTool)) {
          co_return kit::AgentResult<kit::PolicyDecision>::Failure({
              .code = kit::ErrorCode::PermissionDenied, .message = "Choose a native workspace for this conversation"});
        }
        const bool needs_confirmation = request.capability == kit::Capability::ExecuteTool &&
            request.tool && tool != agentkit_preview::GeneratePresentation && tool != agentkit_preview::ReadCodeFiles &&
            tool != agentkit_preview::ReadWorkspaceFiles && tool != agentkit_preview::ListWorkspaceFiles &&
            tool != agentkit_preview::SearchWorkspaceFiles &&
            tool != agentkit_preview::ReadDesign && tool != agentkit_preview::PublishDesign &&
            tool != agentkit_preview::ReadReport && tool != agentkit_preview::PrepareReleaseOutline &&
            tool != agentkit_preview::PublishReportChart;
        co_return settings->Decide(request, needs_confirmation, workspace_available);
      },
      .tool_reviewer = agentkit_preview::ChartExportReviewer(files.exports, std::move(review_model)),
      .tool_resources = [presentations = agentkit_preview::PresentationResources(files, runtime_ref),
                        code = agentkit_preview::CodeResources(workspaces),
                        design = agentkit_preview::DesignChangeResources(runtime_ref),
                        charts = agentkit_preview::ReportChartResources(files.exports)](
                            kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
        if (agentkit_preview::HasDesignTargets(invocation.prepared_targets))
          co_return co_await design(std::move(invocation));
        if (invocation.reverts || (invocation.candidate_artifact && agentkit_preview::HasCodeTargets(invocation.prepared_targets)) ||
            agentkit_preview::IsWorkspaceTool(invocation.tool.id)) {
          co_return co_await code(std::move(invocation));
        }
        if (invocation.tool.id == agentkit_preview::ExportReportChart) co_return co_await charts(std::move(invocation));
        co_return co_await presentations(std::move(invocation));
      },
      .workspace = [worktrees](kit::WorkspaceRequest request) { return worktrees->AllocateAsync(std::move(request)); },
      .project = [workspaces](kit::ProjectId project) -> kit::Async<kit::ProjectBinding> {
        using Result = kit::AgentResult<kit::ProjectBinding>;
        auto members = workspaces->ProjectForks(project);
        if (project == ProjectFor({})) co_return Result::Success({project, std::move(members)});
        if (project.value.starts_with(workspace_project)) {
          kit::WorkspaceId workspace{project.value.substr(workspace_project.size())};
          if (workspaces->Find(workspace)) {
            members.push_back(std::move(workspace));
            co_return Result::Success({project, std::move(members)});
          }
        }
        co_return Result::Failure({.code = kit::ErrorCode::NotFound, .message = "Project directory is unavailable"});
      },
      .verify = [runtime_ref, workspaces, files](kit::VerificationRequest request) {
        return agentkit_preview::VerifyWorkspaceRelease(runtime_ref->lock(), workspaces, files, std::move(request));
      },
      .artifact_store = agentkit_preview::WithAttachmentImports(agentkit_preview::WithWorkspaceEvidence(
          agentkit_preview::WithDesignArtifacts(agentkit_preview::PresentationArtifactStore(files), designs), evidence),
          std::move(attachments)),
      .changes = agentkit_preview::WithDesignChanges(
          agentkit_preview::WithCodeChanges(agentkit_preview::PresentationChanges(files, runtime_ref), evidence, workspaces, runtime_ref),
          designs, runtime_ref),
      .store = std::move(store)});
  if (!runtime.Succeeded()) throw std::runtime_error(runtime.Error().message);
  auto registered = std::move(runtime).Value();
  *runtime_ref = registered;
  using agentkit_preview::DemoModel;
  const auto code = code_model ? std::optional{code_model->ref}
      : configured.empty() ? std::nullopt : std::optional{general == DemoModel ? configured.front().ref : general};
  const auto selected = general.id;
  const auto instructions = [&preferences](std::string base) {
    return agentkit_preview::WithReplyPreferences(std::move(base), preferences);
  };
  kit::ModelRegistration demo{DemoModel, {"preview", "local", "script", {}}, std::make_shared<PreviewModel>()};
  settings->AddModel(demo, "Local demo model");
  Require(registered->RegisterModel(std::move(demo)));
  for (auto& model : configured) {
    settings->AddModel(model);
    Require(registered->RegisterModel(std::move(model)));
  }
  settings->SetDefaults(general, code);
  const bool workspace_tests = test_runner.has_value() && (code || can_add_models);
  const auto register_workspace_agent = [&](kit::AgentRegistration agent) {
    auto tools = agentkit_preview::WorkspaceModelTools(workspace_tests);
    agent.definition.tools.insert(agent.definition.tools.end(), tools.begin(), tools.end());
    Require(registered->RegisterAgent(std::move(agent)));
  };
  using SpecialistWorkflow = std::function<kit::Async<kit::RunCompletion>(
      kit::RunContext, kit::RunInput, std::optional<kit::RunCheckpoint>)>;
  const auto register_specialist = [&](kit::AgentDefinition definition, std::string deliverable, SpecialistWorkflow workflow) {
    auto start = [agent = definition.ref.id, deliverable = std::move(deliverable), workflow = std::move(workflow),
                  runtime_ref, workspaces, settings, workspace_tests](
                     kit::RunContext run, kit::RunInput input,
                     std::optional<kit::RunCheckpoint> checkpoint) -> kit::Async<kit::RunCompletion> {
      using Result = kit::AgentResult<kit::RunCompletion>;
      if (checkpoint && checkpoint->stage == agentkit_preview::WorkspaceAnswerStage && checkpoint->schema_version == 1)
        co_return co_await agentkit_preview::AnswerWithWorkspaceTools(run, checkpoint->state == kit::JsonValue{true}, true);
      const bool user_request = input.trigger == kit::RunTrigger::UserInput || input.trigger == kit::RunTrigger::Retry ||
          input.trigger == kit::RunTrigger::Regenerate;
      if (!checkpoint && user_request && !input.artifact_action) {
        auto allowed = co_await agentkit_preview::AllowsWorkspaceTools(run, agent, *runtime_ref, *workspaces, *settings);
        if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
        if (allowed.Value()) {
          auto route = co_await run.CompleteAsync({
              .instruction = "Classify the user's current request. Use the specialist workflow to " + deliverable +
                  " when explicitly requested. Use answer for questions, inspecting or editing the current project, "
                  "and other tasks. Return only the route.",
              .output_schema = kit::JsonSchema{agentkit_preview::DecodeJson(nlohmann::json{
                  {"type", "object"}, {"required", {"route"}}, {"additionalProperties", false},
                  {"properties", {{"route", {{"type", "string"}, {"enum", {"specialist", "answer"}}}}}}})}});
          if (!route.Succeeded()) co_return Result::Failure(route.Error());
          if (agentkit_preview::EncodeJson(*route.Value().structured).at("route") == "answer")
            co_return co_await agentkit_preview::AnswerWithWorkspaceTools(run, workspace_tests, false);
        }
      }
      co_return co_await workflow(std::move(run), std::move(input), std::move(checkpoint));
    };
    register_workspace_agent({.definition = std::move(definition),
        .run = [start](kit::RunContext run, kit::RunInput input) {
          return start(std::move(run), std::move(input), {});
        },
        .restore = [start](kit::RunContext run, kit::RunInput input, kit::RunResume resume) {
          return start(std::move(run), std::move(input), std::move(resume.checkpoint));
        }});
  };
  Require(registered->RegisterAgent({.definition = {
      .ref = {selection_agent, 1}, .display_name = "Side chat",
      .instructions = instructions(std::string(selection_instructions)), .model = selected, .requires_model = {.text_stream = true},
      .budget = {.tokens = 6000, .model_turns = 2, .context_bytes = 128 << 10, .deadline = std::chrono::seconds(90)}}}));
  Require(agentkit_preview::RegisterCodeTools(registered, evidence, workspaces));
  if (code || can_add_models) {
    auto code_provider = code ? code->id : selected;
    if (code_model) {
      settings->AddModel(*code_model);
      Require(registered->RegisterModel(std::move(*code_model)));
    }
    std::vector<kit::ToolId> tools{agentkit_preview::ReadCodeFiles, agentkit_preview::ApplyCodePatch};
    std::vector<kit::AgentId> targets;
    if (test_runner) {
      Require(agentkit_preview::RegisterCodeTests(registered, *test_runner, std::move(test_logs), evidence));
      tools.push_back(agentkit_preview::RunCodeTests);
      targets.push_back(agentkit_preview::VerificationAgent);
      Require(registered->RegisterAgent({
          .definition = {.ref = {agentkit_preview::VerificationAgent, 1}, .display_name = "Verification assistant",
              .model = selected, .tools = {agentkit_preview::RunCodeTests},
              .capabilities = {kit::Capability::ReadGoal, kit::Capability::Reply}},
          .run = [](kit::RunContext run, kit::RunInput input) {
            return agentkit_preview::RunVerification(std::move(run), std::move(input), {});
          },
          .restore = [](kit::RunContext run, kit::RunInput input, kit::RunResume resume) {
            return agentkit_preview::RunVerification(std::move(run), std::move(input), std::move(resume.checkpoint));
          }}));
    }
    const bool tests_configured = test_runner.has_value();
    register_workspace_agent({
        .definition = {.ref = {agentkit_preview::CodeAgent, 1}, .display_name = "Code assistant",
            .instructions = instructions("Work within the current workspace and respect its approvals."),
            .model = std::move(code_provider), .tools = std::move(tools), .context = {agentkit_preview::CodeEditorContext},
            .collaboration_targets = std::move(targets),
            .capabilities = {kit::Capability::ReadGoal, kit::Capability::ReadProject, kit::Capability::ManageTask,
                kit::Capability::DiscoverSession, kit::Capability::ReadSession, kit::Capability::CreateSession,
                kit::Capability::SendMessage, kit::Capability::Reply, kit::Capability::WaitForResult},
            .requires_model = {.text_stream = true, .structured_output = true}},
        .run = [tests_configured, runtime_ref, workspaces, settings](kit::RunContext run, kit::RunInput input) {
          return agentkit_preview::RunCode(std::move(run), std::move(input), {}, tests_configured, *runtime_ref, workspaces, settings);
        },
        .restore = [tests_configured, runtime_ref, workspaces, settings](kit::RunContext run, kit::RunInput input, kit::RunResume resume) {
          return agentkit_preview::RunCode(std::move(run), std::move(input), std::move(resume.checkpoint),
              tests_configured, *runtime_ref, workspaces, settings);
        }});
  }
  Require(registered->RegisterTool({
      .id = agentkit_preview::ConfirmPreview, .version = 1, .description = "Confirm a preview action",
      .input_schema = {kit::JsonValue{kit::JsonValue::Object{}}},
      .effect = kit::ToolEffect::Read,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({
            .preview = {.title = "Confirm preview action", .summary = "Return a confirmation to this conversation."},
            .undo_support = kit::UndoSupport::None});
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success(
            {.outcome = kit::ToolOutcome::Succeeded, .model_value = kit::JsonValue{true}});
      }}));
  Require(agentkit_preview::RegisterGeneralTools(registered));
  Require(agentkit_preview::RegisterReportChart(registered, files.exports));
  Require(registered->RegisterSkillSource(agentkit_preview::PreviewSkills, agentkit_preview::MakePreviewSkills()));
  std::vector<kit::ContentTypeRef> presentation_content;
#ifdef __EMSCRIPTEN__
  Require(agentkit_preview::RegisterBrowserPresentationExport(registered));
  presentation_content.push_back(agentkit_preview::BrowserPresentationExport);
#endif
  auto general_content = presentation_content;
  general_content.push_back(agentkit_preview::ReportChart);
  const auto export_directory = files.exports.Path();
  std::vector<kit::AgentId> general_targets{agentkit_preview::DesignAgent, agentkit_preview::PptAgent};
  if (release_available) general_targets.push_back(agentkit_preview::CodeAgent);
  register_workspace_agent({
      .definition = {.ref = {agentkit_preview::GeneralAgent, 1}, .display_name = "General assistant",
          .instructions = instructions({}), .model = selected,
          .tools = {agentkit_preview::ConfirmPreview, agentkit_preview::ReadReport, agentkit_preview::PrepareReleaseOutline,
              agentkit_preview::PublishReportChart, agentkit_preview::ExportReportChart, agentkit_preview::GeneratePresentation,
              agentkit_preview::ExportPresentation},
          .skill_sources = {agentkit_preview::PreviewSkills},
          .content_types = std::move(general_content),
          .collaboration_targets = std::move(general_targets),
          .capabilities = {kit::Capability::ReadGoal, kit::Capability::ManageGoal, kit::Capability::CompleteGoal,
              kit::Capability::ReadProject, kit::Capability::ManageTask, kit::Capability::DiscoverSession, kit::Capability::ReadSession,
              kit::Capability::CreateSession, kit::Capability::SendMessage, kit::Capability::WaitForResult},
          .requires_model = {.text_stream = true, .structured_output = true}},
      .run = [release_available, export_directory, workspaces, settings, workspace_tests, runtime_ref](kit::RunContext run, kit::RunInput input) {
        return agentkit_preview::RunGeneral(std::move(run), std::move(input), {}, release_available, export_directory,
            workspaces, settings, workspace_tests, *runtime_ref);
      },
      .restore = [release_available, export_directory, workspaces, settings, workspace_tests, runtime_ref](
                     kit::RunContext run, kit::RunInput input, kit::RunResume resume) {
        return agentkit_preview::RunGeneral(
            std::move(run), std::move(input), std::move(resume.checkpoint), release_available, export_directory,
            workspaces, settings, workspace_tests, *runtime_ref);
      }});
  Require(agentkit_preview::RegisterDesignTools(registered, designs));
  register_specialist(
      {.ref = {agentkit_preview::DesignAgent, 1}, .display_name = "Design assistant",
          .instructions = instructions("Design local interface wireframes using the supplied component vocabulary."),
          .model = selected, .tools = {agentkit_preview::ReadDesign, agentkit_preview::PublishDesign},
          .capabilities = {kit::Capability::ReadGoal, kit::Capability::ReadProject, kit::Capability::ManageTask, kit::Capability::Reply},
          .requires_model = {.text_stream = true, .structured_output = true}},
      "create or revise an interface wireframe", agentkit_preview::RunDesign);
  Require(agentkit_preview::RegisterPresentationTools(registered, files));
  register_specialist(
      {.ref = {agentkit_preview::PptAgent, 1}, .display_name = "PPT assistant",
          .instructions = instructions("Prepare slide outlines based on the user's brief. Treat reference notes as source material."),
          .model = std::move(selected), .tools = {agentkit_preview::GeneratePresentation, agentkit_preview::ExportPresentation},
          .content_types = std::move(presentation_content),
          .capabilities = {kit::Capability::ReadGoal, kit::Capability::ReadProject, kit::Capability::ReadContext,
              kit::Capability::ManageTask, kit::Capability::Reply},
          .requires_model = {.text_stream = true, .structured_output = true}},
      "create a slide presentation", [export_directory](kit::RunContext context, kit::RunInput input,
                                                          std::optional<kit::RunCheckpoint> checkpoint) {
        return agentkit_preview::RunPresentation(
            std::move(context), std::move(input), std::move(checkpoint), export_directory);
      });
  return registered;
}

using PreviewStoreAdapter = std::function<std::shared_ptr<kit::SessionStore>(std::shared_ptr<kit::SessionStore>)>;

File DefaultGanWorkDatabase() {
  const auto data = UseApplication().Directories().data_directory;
  const auto database = data.Child("agentkit-preview").Child("sessions.sqlite3");
#ifdef _WIN32
  if (!database.Exists()) {
    const auto app = data.Parent();
    const auto local = app ? app->Parent() : std::nullopt;
    if (local) {
      auto previous = local->Child("example_agent_kit").Child(data.Name()).Child("agentkit-preview").Child("sessions.sqlite3");
      if (previous.Exists()) return previous;
    }
  }
#endif
  return database;
}

std::optional<View> PreviewAttachmentAction(const std::shared_ptr<PreviewService>& service, const kit::AgentId& agent,
    State<kit::UserRunOptions> next, State<kit::AgentChatDraftSources> sources, kit::ModelCapabilities capabilities,
    std::function<void()> changed = {});

std::shared_ptr<PreviewService> InstallPreview(ApplicationContext& context, std::optional<File> database, bool read_model_configuration,
                    std::optional<File> startup_workspace = {}, std::optional<kit::ModelRegistration> code_model = {},
                    std::optional<File> test_runner = {}, PreviewStoreAdapter adapt_store = {},
                    std::vector<kit::ModelRegistration> additional_models = {},
                    bool review_user_comparisons = false) {
  auto service = std::make_shared<PreviewService>();
  service->run_settings = std::make_shared<agentkit_preview::RunSettings>();
  auto selection = std::make_shared<agentkit_preview::PreviewSideChats>(agentkit_preview::PreviewSideChats{nullptr, selection_agent});
  context.Provide(service);
  context.Provide(std::make_shared<kit::AgentChatModelAvatar>([settings = service->run_settings](
      const kit::ModelProviderRef& model, Color foreground) { return settings->ModelAvatar(model, foreground); }));
  context.Provide(std::make_shared<State<agentkit_preview::PreviewSettings>>(service->settings));
  context.Provide(selection);
  const auto tasks = UseApplicationTaskScope();
  auto file = database ? *database : DefaultGanWorkDatabase();
  std::optional<File> temporary;
#ifdef __EMSCRIPTEN__
  temporary = UseApplication().Directories().temporary_directory;
#endif
  tasks.Launch([service, selection, tasks, file, temporary = std::move(temporary),
                read_model_configuration, startup_workspace, code_model, test_runner,
                additional_models = std::move(additional_models),
                adapt_store = std::move(adapt_store), review_user_comparisons]() mutable -> Task<void> {
    try {
      auto store = co_await agentkit_preview::OpenPreviewStoresAsync(file);
      if (!store.Succeeded()) {
        service->startup = PreviewService::Started::Failure(store.Error());
      } else {
        service->preferences = std::move(store.Value().preferences);
        service->navigation_store = std::move(store.Value().navigation);
        auto navigation = co_await service->navigation_store->LoadAsync(preview_user);
        if (navigation.Succeeded()) service->navigation = std::move(navigation).Value();
        else {
          service->reload_navigation = true;
          service->navigation_failure = NavigationFailure{true, navigation.Error()};
        }
        auto saved_preferences = co_await service->preferences->LoadAsync(preview_user);
        if (saved_preferences.Succeeded()) {
          for (const auto& [id, options] : saved_preferences.Value())
            service->composers.try_emplace(id).first->second.next = options;
        } else {
          service->preference_load_error = "Could not read saved conversation settings";
        }
        const auto data = file.Parent().value();
        auto models = std::move(additional_models);
        std::optional<kit::AgentResult<kit::ModelRegistration>> reviewer;
        if (read_model_configuration) {
#ifdef __EMSCRIPTEN__
          auto configured = ConfiguredBrowserModels();
          models = std::move(configured.models);
          reviewer = std::move(configured.reviewer);
          service->model_configuration_error = std::move(configured.error);
#else
          models = ConfiguredModels();
          service->settings_file = data.Child("settings.json");
          auto preferences = co_await agentkit_preview::ReadPreviewSettings(*service->settings_file);
          if (preferences.Succeeded()) {
            service->settings = preferences.Value();
            service->applied_settings = std::move(preferences).Value();
          } else {
            service->model_configuration_error = preferences.Error().message;
          }
          auto profiles = data.Child("models");
          co_await LoadModelProfiles(service, profiles, models);
          service->model_profiles = std::move(profiles);
          reviewer = ConfiguredReviewModel();
          auto executable = Environment("HUXERUI_AGENTKIT_CTEST");
          if (!executable.empty()) {
            if (!std::filesystem::u8path(executable).is_absolute()) throw std::invalid_argument("Use an absolute CTest path");
            test_runner = File(executable);
          }
#endif
        }
        service->workspace_evidence = data.Child("workspace-evidence");
        agentkit_preview::PresentationFiles files{data.Child("presentations"), data.Child("exports"), temporary};
#ifdef __EMSCRIPTEN__
        service->exports = files.exports;
#endif
        if (!co_await files.exports.CreateDirectoriesAsync()) {
          throw std::runtime_error("Could not open the presentation export directory");
        }
#ifdef __EMSCRIPTEN__
        service->workspaces = std::make_shared<agentkit_preview::CodeWorkspaces>(data.Child("workspaces"), file);
#else
        service->workspaces = std::make_shared<agentkit_preview::CodeWorkspaces>(data.Child("workspaces"));
#endif
        Require(co_await service->workspaces->LoadAsync());
        service->worktrees = std::make_shared<agentkit_preview::WorktreeForks>(data.Child("worktree-plans"), service->workspaces);
        Require(co_await service->worktrees->LoadAsync());
        if (startup_workspace) {
          auto added = co_await service->workspaces->AddAsync(*startup_workspace, {});
          if (!added.Succeeded()) throw std::runtime_error(added.Error().message);
          service->startup_workspace = std::move(added).Value();
        }
        service->code_available = !models.empty() || code_model.has_value();
        auto persisted = std::move(store.Value().sessions);
        if (adapt_store) persisted = adapt_store(std::move(persisted));
        service->attachments = std::make_shared<agentkit_preview::AttachmentImports>(data.Child("attachments"));
        service->runtime = MakeRuntime(tasks, std::move(persisted), std::move(models), std::move(files),
                                      service->workspaces, service->worktrees, std::move(code_model),
                                      std::move(test_runner), data.Child("test-logs"), service->run_settings,
                                      service->attachments, std::move(reviewer), service->applied_settings.Get(), review_user_comparisons,
                                      service->model_profiles.has_value());
        selection->runtime = service->runtime;
        selection->composer_controls = [owner = std::weak_ptr{service}, settings = service->run_settings](
            State<kit::UserRunOptions> next, State<kit::AgentChatDraftSources> sources) -> kit::AgentChatRunControls {
          return {next, [owner, next, sources](const kit::SessionSummary&, std::optional<kit::RunStartBinding> current) -> View {
            const auto host = owner.lock();
            if (!host) return Spacer();
            auto caps = host->run_settings->Capabilities(next->model, selection_agent);
            if (current) {
              const auto running = host->run_settings->Capabilities(current->model, selection_agent);
              caps.image_input |= running.image_input;
              caps.file_input |= running.file_input;
            }
            auto attachments = PreviewAttachmentAction(host, selection_agent, next, sources, caps);
            return agentkit_preview::RunSettingsControls(host->run_settings, next, selection_agent, {}, {},
                std::move(current), {}, std::move(attachments), AddModelAction(host), false);
          }, [settings](const kit::RunStartBinding& binding) {
            return UseService<Resources>()->GetString(settings->ModelLabel(binding.model));
          }};
        };
        auto started = co_await service->runtime->StartAsync();
        service->startup = started.Succeeded() ? PreviewService::Started::Success(service->runtime)
                                               : PreviewService::Started::Failure(started.Error());
      }
    } catch (const std::exception& error) {
      service->startup = PreviewService::Started::Failure(
          {.code = kit::ErrorCode::PersistenceFailure, .message = error.what()});
    }
    for (const auto& listener : std::exchange(service->listeners, {})) listener.Complete(*service->startup);
  });
  return service;
}

struct Selection {
  kit::SessionId id;
  std::shared_ptr<kit::Conversation> conversation;
  std::optional<kit::AgentChatReviewRequest> review;
  std::optional<kit::AgentChatInteractionRequest> interaction;
  bool operator==(const Selection&) const = default;
};

const kit::UserRunOptions default_draft_start{.execution_profile = agentkit_preview::AskProfile};

struct NewConversationDraft {
  const std::string view_key = "new-draft/" + kit::NewCommandId().value;
  State<TextEditingValue> text{TextEditingValue::FromText({})};
  State<std::size_t> assistant{std::size_t{0}};
  State<std::optional<std::string>> group{std::optional<std::string>{}};
  State<kit::UserRunOptions> start{default_draft_start};
  State<std::vector<kit::SkillRef>> skills{std::vector<kit::SkillRef>{}};
  State<std::optional<agentkit_preview::ImportedPresentationReport>> report{
      std::optional<agentkit_preview::ImportedPresentationReport>{}};
  State<kit::AgentChatDraftSources> sources{kit::AgentChatDraftSources{}};
  State<bool> creating{false};
  State<std::string> error{std::string{}};
};

struct ComparisonNavigation {
  Selection origin;
  std::optional<kit::ForkReviewId> review;
  std::optional<kit::SessionId> branch;
  bool showing = false;
  bool operator==(const ComparisonNavigation&) const = default;
};

kit::Async<kit::HistoryBoundary> ForkBoundary(std::shared_ptr<kit::AgentRuntime> runtime, kit::SessionId session) {
  using Result = kit::AgentResult<kit::HistoryBoundary>;
  auto opened = co_await runtime->Sessions().OpenAsync(session);
  if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
  auto view = opened.Value()->Snapshot();
  if (view) {
    auto anchor = std::ranges::find_if(view->messages.rbegin(), view->messages.rend(),
        [](const kit::Message& message) { return message.state == kit::MessageState::Complete; });
    if (anchor != view->messages.rend()) co_return Result::Success({session, anchor->ref.message, anchor->sequence});
  }
  co_return Result::Failure({.code = kit::ErrorCode::InvalidState, .message = "No committed messages are available to copy"});
}

kit::Async<kit::SessionId> CompleteWorktree(std::shared_ptr<PreviewService> service, agentkit_preview::WorktreePreview prepared) {
  using Result = kit::AgentResult<kit::SessionId>;
  const auto confirm = [&](kit::SessionId session) -> kit::Async<kit::SessionId> {
    auto finished = co_await service->worktrees->ConfirmAsync(prepared.plan, prepared.command, session);
    if (!finished.Succeeded()) co_return Result::Failure(finished.Error());
    co_return Result::Success(std::move(session));
  };
  auto commands = service->runtime->Commands();
  auto status = co_await commands.ReadAsync(prepared.command);
  if (status.Succeeded()) {
    if (status.Value().state == kit::CommandState::Applied && status.Value().result) {
      if (const auto* found = std::get_if<kit::SessionCreated>(&*status.Value().result))
        co_return co_await confirm(found->session);
    }
    if (status.Value().state == kit::CommandState::Withdrawn) {
      co_return Result::Failure({
          .code = kit::ErrorCode::AlreadyClosed, .message = "The copy was abandoned. Use cleanup to finish"});
    }
    co_return Result::Failure(status.Value().failure.value_or(kit::AgentError{
        .code = kit::ErrorCode::Busy, .message = "The copy is awaiting a decision in the source conversation"}));
  }
  if (status.Error().code != kit::ErrorCode::NotFound) co_return Result::Failure(status.Error());
  auto receipt = co_await service->runtime->Sessions().ForkAsync(prepared.command, prepared.request);
  if (!receipt.Succeeded()) {
    if (receipt.Error().code != kit::ErrorCode::CommitUncertain) co_return Result::Failure(receipt.Error());
    auto reconciled = co_await commands.ReadAsync(prepared.command);
    if (reconciled.Succeeded() && reconciled.Value().state == kit::CommandState::Applied && reconciled.Value().result) {
      if (const auto* found = std::get_if<kit::SessionCreated>(&*reconciled.Value().result))
        co_return co_await confirm(found->session);
    }
    co_return Result::Failure(receipt.Error());
  }
  if (const auto* created = std::get_if<kit::SessionCreated>(&receipt.Value().outcome))
    co_return co_await confirm(created->session);
  co_return Result::Failure({.code = kit::ErrorCode::Busy, .message = "Approve the copy in the source conversation"});
}

[[huxerui::composable]]
View CopyWorktreeDialog(DialogContext dialog, std::shared_ptr<PreviewService> service, kit::SessionSummary source,
    std::shared_ptr<agentkit_preview::WorktreePreview> recovered, std::function<void(kit::SessionId)> opened,
    std::function<void()> refresh) {
  const auto tasks = UseTaskScope();
  const auto revision = UseState(TextEditingValue::FromText("HEAD"));
  const auto branch = UseState(TextEditingValue::FromText("agentkit/copy-" + kit::NewCommandId().value));
  const auto directory = UseState(TextEditingValue::FromText({}));
  const auto preview = UseState(recovered);
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const bool comparison_cleanup = recovered && recovered->comparison;
  const auto discard = UseState(comparison_cleanup || (recovered && recovered->cleanup_started));
  const auto prepare = [tasks, service, source, revision, branch, directory, preview, busy, error] {
    if (busy.Get() || !source.workspace) return;
    busy = true;
    error = "";
    tasks.Launch([service, source, base = revision->text, target = directory->text, name = branch->text,
                  preview, busy, error]() -> Task<void> {
      auto boundary = co_await ForkBoundary(service->runtime, source.id);
      if (!boundary.Succeeded()) {
        busy = false;
        error = boundary.Error().message;
        co_return;
      }
      auto planned = co_await service->worktrees->PrepareAsync(preview_user, std::move(boundary).Value(), *source.workspace,
          source.project, source.title + " (copy)", base, target, name);
      busy = false;
      if (!planned.Succeeded()) error = planned.Error().message;
      else preview = std::make_shared<agentkit_preview::WorktreePreview>(std::move(planned).Value());
    });
  };
  const auto confirm = [dialog, tasks, service, preview, opened, busy, error, refresh] {
    if (busy.Get() || !preview.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([dialog, service, prepared = *preview.Get(), opened, busy, error, refresh]() -> Task<void> {
      auto result = co_await CompleteWorktree(service, std::move(prepared));
      refresh();
      busy = false;
      if (!result.Succeeded()) { error = result.Error().message; co_return; }
      opened(result.Value());
      dialog.Dismiss();
    });
  };
  const auto remove = [dialog, tasks, service, preview, busy, error, refresh] {
    if (busy.Get() || !preview.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([dialog, service, prepared = preview.Get(), busy, error, refresh]() -> Task<void> {
      auto outcome = co_await service->worktrees->DiscardAsync(service->runtime, preview_user, prepared->plan,
          prepared->comparison.value_or(prepared->command));
      refresh();
      busy = false;
      if (!outcome.Succeeded()) { error = outcome.Error().message; co_return; }
      dialog.Dismiss();
    });
  };
  std::vector<View> content{Text(comparison_cleanup ? ::app::strings::ui_remove_unused_candidate_checkout :
      discard.Get() ? ::app::strings::ui_discard_unfinished_copy : ::app::strings::ui_copy_to_worktree, TextRole::Title)};
  if (preview.Get()) {
    const auto& plan = *preview.Get();
    content.push_back(Text(StringVariant::Format(::app::strings::ui_repository_n_0, plan.source)));
    content.push_back(Text(StringVariant::Format(::app::strings::ui_commit_n_0, plan.commit)));
    content.push_back(Text(StringVariant::Format(::app::strings::ui_new_branch_n_0, plan.branch)));
    content.push_back(Text(StringVariant::Format(::app::strings::ui_destination_n_0, plan.directory)));
    if (discard.Get()) {
      if (plan.recovery_error) content.push_back(Text(plan.recovery_error->message, TextRole::Label));
      content.push_back(Text(::app::strings::ui_remove_the_checkout_and_its_workspace_registration, TextRole::Label));
    } else {
      content.push_back(Text(::app::strings::ui_the_copy_starts_with_ask_permissions, TextRole::Label));
    }
  } else {
    content.push_back(TextField(revision).Label(::app::strings::ui_commit_or_revision).Key("worktree-revision")
        .OnChanged([revision](const auto& value) { revision = value; }));
    content.push_back(TextField(branch).Label(::app::strings::ui_new_branch).Key("worktree-branch")
        .OnChanged([branch](const auto& value) { branch = value; }));
    content.push_back(TextField(directory).Label(::app::strings::ui_absolute_destination_path).Key("worktree-directory")
        .OnChanged([directory](const auto& value) { directory = value; }));
  }
  if (!error.Get().empty()) content.push_back(Text(error.Get(), TextRole::Label));
  std::vector<View> buttons;
  if (preview.Get() && !recovered) {
    buttons.push_back(Button(::app::strings::ui_edit_details).OnClick([preview, error] {
      preview = std::shared_ptr<agentkit_preview::WorktreePreview>{};
      error = "";
    }).With(Enabled(!busy.Get())));
  }
  if (recovered && !recovered->cleanup_started && !comparison_cleanup) {
    buttons.push_back(Button(discard.Get() ? ::app::strings::ui_back : ::app::strings::ui_discard_copy).OnClick([discard, error] {
      discard = !discard.Get();
      error = "";
    }).With(Enabled(!busy.Get())));
  }
  buttons.push_back(Spacer());
  buttons.push_back(Button(::app::strings::ui_close).OnClick([dialog] { dialog.Dismiss(); }).With(Enabled(!busy.Get())));
  if (discard.Get()) {
    buttons.push_back(Button(::app::strings::ui_remove_checkout).OnClick(remove).With(Enabled(!busy.Get())));
  } else if (preview.Get()) {
    buttons.push_back(Button(::app::strings::ui_create_worktree_and_copy).OnClick(confirm).With(Enabled(!busy.Get())));
  } else {
    buttons.push_back(Button(::app::strings::ui_review_worktree).OnClick(prepare)
        .With(Enabled(!busy.Get() && !revision->text.empty() && !branch->text.empty() && !directory->text.empty())));
  }
  content.push_back(Row(std::move(buttons)).With(Spacing(8.0F)));
  return Column(std::move(content)).With(Frame{.width = 500.0F}, Spacing(14.0F), Padding(22.0F));
}

enum class NavigationSymbol { New, Folder, Inbox, Plus, Down, Right, Panel, More, Chat, Code, Slides, Design, Layers, Settings };

View NavigationIcon(NavigationSymbol symbol, Color ink) {
  using namespace ::app::images;
  static const std::array<const ImageResource*, 14> icons{
      &new_conversation, &folder, &inbox, &add, &chevron_down, &chevron_right,
      &sidebar, &more, &chat, &code, &slides, &design, &layers, &settings,
  };
  return Image(*icons.at(static_cast<std::size_t>(symbol))).Tint(ink).With(Frame{.width = 16.0F, .height = 16.0F});
}

[[huxerui::composable]]
View NavigationAction(StringVariant label, NavigationSymbol symbol, std::function<void()> action,
    bool enabled = true, bool selected = false, std::string count = {}, std::optional<bool> expanded = {}) {
  const auto colors = UseTheme().colors;
  const auto hovered = UseState(false);
  const bool new_conversation = symbol == NavigationSymbol::New;
  const bool folder_heading = expanded.has_value();
  const auto ink = selected ? colors.on_primary_container : colors.on_surface;
  std::vector<View> content{NavigationIcon(symbol, folder_heading ? colors.on_surface_variant : ink)};
  if (folder_heading) content.push_back(NavigationIcon(NavigationSymbol::Folder, colors.on_surface_variant));
  content.push_back(Text(label).Style({Font::System(13.0F).WithWeight(new_conversation ?
      FontWeight::SemiBold : folder_heading ? FontWeight::Medium : FontWeight::Regular), ink})
      .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()));
  if (!count.empty()) content.push_back(Text(count).Style({Font::System(12.0F), colors.on_surface_variant})
      .Key("pending-navigation-count"));
  return Row(std::move(content))
      .OnClick(action).On<ViewEvents::KeyDown>([action](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    action();
    return true;
  }).On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
      .With(Frame{.height = new_conversation ? 42.0F : folder_heading ? 34.0F : 36.0F},
            Padding(EdgeInsets::Symmetric(folder_heading ? 6.0F : 10.0F, 0.0F)), Spacing(folder_heading ? 6.0F : 9.0F),
            CrossAlign(CrossAxisAlignment::Center), CornerRadius(9.0F), Focusable{}, Enabled(enabled),
            Background(hovered.Get() && enabled ? colors.surface_container_highest : selected ? colors.primary_container :
                new_conversation ? colors.surface_container_high : Color::Transparent()),
            Border{new_conversation ? colors.outline : Color::Transparent()},
            Semantics{.role = SemanticRole::Button, .label = label, .value = count, .selected = selected, .expanded = expanded});
}

[[huxerui::composable]]
View DraftChoice(NavigationSymbol icon, StringVariant label, std::string value, std::string hint, std::string key,
    std::function<std::vector<MenuEntry>()> choices, bool enabled) {
  const auto colors = UseTheme().colors;
  const auto anchor = UseViewHandle();
  const auto menu = UseMenu();
  const auto hovered = UseState(false);
  const auto open = [anchor, menu, choices] { menu.Show(anchor, choices()); };
  return Row{
      NavigationIcon(icon, colors.on_surface_variant),
      Text(label).Style({Font::System(12.0F), colors.on_surface_variant}),
      Text(value).Style({Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface})
          .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Frame{.max_width = 135.0F}),
      NavigationIcon(NavigationSymbol::Down, colors.on_surface_variant),
  }.OnClick(open).On<ViewEvents::KeyDown>([open](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space && event.key != Key::ArrowDown) return false;
    open();
    return true;
  }).On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
      .With(anchor, Enabled(enabled), Focusable{}, Frame{.height = 34.0F}, Padding(EdgeInsets::Symmetric(10.0F, 0.0F)),
          Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center), CornerRadius(9.0F), Border{colors.outline},
          Background(hovered.Get() && enabled ? colors.surface_container_highest : colors.surface_container_low),
          Tooltip(hint.empty() ? value : hint),
          Semantics{.role = SemanticRole::Button, .label = label, .value = value, .hint = hint}).Key(key);
}

View PreviewWordmark(const ColorScheme& colors) {
  auto mark = Image(::app::images::agentkit_mark).Tint(colors.on_primary_container)
      .With(Frame{.width = 26.0F, .height = 26.0F}, Background(colors.primary_container), CornerRadius(7.0F));
  return Row{
      std::move(mark),
      Text(::app::strings::app_name).Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
  }.With(Spacing(9.0F), CrossAlign(CrossAxisAlignment::Center));
}

StringVariant PreviewAgentLabel(const kit::AgentRef& agent) {
  if (agent.id == agentkit_preview::CodeAgent) return ::app::strings::ui_code_assistant;
  if (agent.id == agentkit_preview::PptAgent) return ::app::strings::ui_ppt_assistant;
  if (agent.id == agentkit_preview::DesignAgent) return ::app::strings::ui_design_assistant;
  if (agent.id == agentkit_preview::GeneralAgent) return ::app::strings::ui_general_assistant;
  return {};
}

[[huxerui::composable]]
View SessionRow(kit::SessionSummary item, bool selected, bool grouped, std::function<void(kit::SessionId)> open,
    std::function<std::vector<MenuEntry>()> actions) {
  const auto theme = UseTheme().colors;
  const auto menu = UseMenu();
  const auto target = UseViewHandle();
  const auto hovered = UseState(false);
  const auto focused = UseState(false);
  auto agent = UseString(PreviewAgentLabel(item.agent));
  if (agent.empty()) agent = item.agent.id.value;
  const auto activate = [open, id = item.id] { open(id); };
  const auto ink = selected ? theme.on_primary_container : theme.on_surface;
  const auto status = item.pending_interactions ? "Pending" : item.active_run ? "Running" : "";
  const auto symbol = item.agent.id == agentkit_preview::CodeAgent ? NavigationSymbol::Code :
      item.agent.id == agentkit_preview::PptAgent ? NavigationSymbol::Slides :
      item.agent.id == agentkit_preview::DesignAgent ? NavigationSymbol::Design : NavigationSymbol::Chat;
  std::vector<View> label{NavigationIcon(symbol, selected ? ink : theme.on_surface_variant).With(Tooltip(agent)),
      Row{Text(item.title).Style({Font::System(13.0F), ink}).LineLimit(1).Overflow(TextOverflow::Ellipsis)
          .With(Grow()).Key(item.id.value)}.With(Grow())
          .Key(item.pending_interactions ? "pending-session-title/" + item.title : "session-title")};
  if (item.pending_interactions || item.active_run) {
    const auto dot = item.pending_interactions ? Color::Rgb(181, 113, 56) : theme.on_primary_container;
    label.push_back(Canvas([dot](PaintContext& context, Size) { context.DrawCircle({3, 3}, 2.5F, dot); })
        .With(Frame{.width = 6.0F, .height = 6.0F}));
  }
  auto item_button = Row(std::move(label)).OnClick(activate).On<ViewEvents::KeyDown>([activate](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    activate();
    return true;
  }).On<ViewEvents::FocusChanged>([focused](bool active) { focused = active; })
      .With(Grow(), Focusable{}, CrossAlign(CrossAxisAlignment::Center), Spacing(5.0F),
        Padding(EdgeInsets::Symmetric(grouped ? 10.0F : 8.0F, 7.0F)),
        Semantics{.role = SemanticRole::Button, .label = item.title, .hint = agent, .state_description = status, .selected = selected});
  const auto show_more = selected || hovered.Get() || focused.Get();
  auto more = Row{NavigationIcon(NavigationSymbol::More, show_more ? theme.on_surface_variant : Color::Transparent())
      .Key("session-actions-icon")}
      .OnClick([menu, target, actions] { menu.Show(target, actions()); })
      .On<ViewEvents::KeyDown>([menu, target, actions](const KeyEvent& event) {
        if (event.key != Key::Enter && event.key != Key::Space) return false;
        menu.Show(target, actions());
        return true;
      }).On<ViewEvents::FocusChanged>([focused](bool active) { focused = active; })
      .With(target, Focusable{}, Frame{.width = 30.0F, .height = 32.0F}, MainAlign(MainAxisAlignment::Center),
          CrossAlign(CrossAxisAlignment::Center), Semantics{.role = SemanticRole::Button, .label = ::app::strings::ui_conversation_actions})
      .Key("session-actions/" + item.id.value);
  return Row{std::move(item_button), std::move(more)}
      .On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
      .On<ViewEvents::ContextMenuRequested>([menu, actions](Point location) { menu.ShowAt(location, actions()); })
      .On<ViewEvents::KeyDown>([menu, target, actions](const KeyEvent& event) {
        if (event.key != Key::ContextMenu && (event.key != Key::F10 || !event.modifiers.shift)) return false;
        menu.Show(target, actions());
        return true;
      })
      .With(Frame{.height = 34.0F}, CrossAlign(CrossAxisAlignment::Center), CornerRadius(6.0F),
            Background(selected ? theme.primary_container : hovered.Get() ? theme.surface_container_highest : Color::Transparent()));
}

[[huxerui::composable]]
View NavigationNameDialog(DialogContext dialog, StringVariant title, std::string initial, std::function<void(std::string)> apply) {
  const auto name = UseState(TextEditingValue::FromText(std::move(initial)));
  const auto colors = UseTheme().colors;
  const auto buttons = UseEnvironment<ButtonStyle>();
  auto fields = UseEnvironment<TextFieldStyle>();
  fields.show_label = false;
  const auto save = [dialog, name, apply] {
    const auto first = name->text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return;
    apply(name->text.substr(first, name->text.find_last_not_of(" \t\r\n") - first + 1));
    dialog.Dismiss();
  };
  const auto label = ::app::strings::ui_name;
  auto field = TextField(name).Label(label).Variant(TextFieldVariant::Outlined)
      .OnChanged([name](const TextEditingValue& value) { name = value; }).OnSubmitted(save).Key("navigation-name");
  return agentkit_preview::PreviewFormDialog(title, Column {
    ProvideEnvironment(fields, agentkit_preview::PreviewLabeledField(label, std::move(field), colors)),
  }.With(Padding(20.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  Row {
    Spacer(),
    ProvideEnvironment(agentkit_preview::PreviewSecondaryButton(buttons, colors),
        Button(::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); })),
    ProvideEnvironment(agentkit_preview::PreviewPrimaryButton(buttons, colors),
        Button(::app::strings::ui_save).OnClick(save).With(Enabled(name->text.find_first_not_of(" \t\r\n") != std::string::npos))),
  }.With(Spacing(8.0F)), colors, 380.0F);
}

void EditGroup(const std::shared_ptr<PreviewService>& service, std::string id,
               std::function<kit::AgentResult<void>(ConversationGroup&)> change) {
  EditNavigation(service, [id = std::move(id), change = std::move(change)](NavigationPreferences& data) {
    auto group = std::ranges::find(data.groups, id, &ConversationGroup::id);
    if (group == data.groups.end())
      return kit::AgentResult<void>::Failure({.code = kit::ErrorCode::NotFound, .message = "The conversation group is no longer available"});
    return change(*group);
  });
}

void MoveGroup(const std::shared_ptr<PreviewService>& service, std::string id, std::string anchor, bool after) {
  EditNavigation(service, [id = std::move(id), anchor = std::move(anchor), after](NavigationPreferences& data) {
    auto group = std::ranges::find(data.groups, id, &ConversationGroup::id);
    auto peer = std::ranges::find(data.groups, anchor, &ConversationGroup::id);
    if (group == data.groups.end() || peer == data.groups.end() || group == peer)
      return kit::AgentResult<void>::Failure({.code = kit::ErrorCode::NotFound, .message = "The conversation group order changed"});
    auto place = after ? std::next(peer) : peer;
    if (group < place) std::rotate(group, std::next(group), place);
    else std::rotate(place, group, std::next(group));
    return kit::AgentResult<void>::Success();
  });
}

[[huxerui::composable]]
View ConversationGroupRow(ConversationGroup group, std::function<void()> toggle, std::function<std::vector<MenuEntry>()> actions) {
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto colors = UseTheme().colors;
  const auto actions_id = "group-actions/" + group.id;
  auto style = UseEnvironment<ButtonStyle>();
  style.background = Color::Transparent();
  style.label_style = TextStyle{Font::System(13.0F).WithWeight(FontWeight::SemiBold), colors.on_surface};
  style.padding = EdgeInsets::Symmetric(3.0F, 4.0F);
  auto label = Row{
      NavigationIcon(group.collapsed ? NavigationSymbol::Right : NavigationSymbol::Down, colors.on_surface_variant),
      Text(group.name).Style(style.label_style).LineLimit(1).Overflow(TextOverflow::Ellipsis)
          .With(Grow()).Key("group/" + group.id),
  }.OnClick(toggle).On<ViewEvents::KeyDown>([toggle](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    toggle();
    return true;
  }).With(Grow(), Focusable{}, Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center),
      Semantics{.role = SemanticRole::Button, .label = group.name, .expanded = !group.collapsed});
  return Row{std::move(label),
      IconButton(::app::images::more, ::app::strings::ui_group_actions).With(anchor, Semantics{.identifier = actions_id}).Key(actions_id)
          .OnClick([menu, anchor, actions] { menu.Show(anchor, actions()); }),
  }.On<ViewEvents::ContextMenuRequested>([menu, actions](Point location) { menu.ShowAt(location, actions()); })
      .With(Frame{.height = 32.0F}, Padding(EdgeInsets::Symmetric(8.0F, 0.0F)), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View PendingRequests(std::shared_ptr<kit::AgentRuntime> runtime, State<std::shared_ptr<const kit::PendingInteractionPage>> page,
                     std::string load_error, std::function<void()> retry, std::function<void(kit::PendingInteraction)> open) {
  const auto tasks = UseTaskScope();
  const auto page_error = UseState(std::string{});
  const auto loading_more = UseState(false);
  std::vector<View> rows;
  const auto current = page.Get();
  if (current) {
    for (const auto& item : current->items) {
      const auto label = std::visit([](const auto& body) {
        if constexpr (std::is_same_v<std::decay_t<decltype(body)>, kit::FormRequest>) return body.title;
        else return body.preview.title;
      }, item.interaction.body);
      rows.push_back(Row{
          Column{Text(label), Text(item.session.title, TextRole::Label)}.With(Grow(), Spacing(4.0F)),
          Button(::app::strings::ui_open_request).OnClick([open, item] { open(item); }).Key("pending/" + item.interaction.id.value),
      }.With(Spacing(12.0F), Padding(12.0F), CrossAlign(CrossAxisAlignment::Center)).Key(item.interaction.id.value));
    }
    if (current->items.empty()) rows.push_back(Text(::app::strings::ui_no_pending_requests, TextRole::Label));
    if (current->next) rows.push_back(Button(::app::strings::ui_load_more_requests).OnClick([runtime, tasks, page, page_error, loading_more, current] {
      if (loading_more.Get()) return;
      loading_more = true;
      page_error = "";
      tasks.Launch([runtime, page, page_error, loading_more, current]() -> Task<void> {
        auto next = co_await runtime->Interactions().ListPendingAsync({}, current->next);
        const bool expired = !next.Succeeded() && next.Error().code == kit::ErrorCode::CursorExpired;
        if (expired && page.Get() == current) next = co_await runtime->Interactions().ListPendingAsync({}, {});
        loading_more = false;
        if (page.Get() != current) co_return;
        if (!next.Succeeded()) { page_error = next.Error().message; co_return; }
        if (expired) { page = std::make_shared<const kit::PendingInteractionPage>(std::move(next).Value()); co_return; }
        auto merged = std::make_shared<kit::PendingInteractionPage>(*current);
        merged->items.insert(merged->items.end(), next.Value().items.begin(), next.Value().items.end());
        merged->next = next.Value().next;
        page = std::move(merged);
      });
    }).With(Enabled(!loading_more.Get())));
  } else if (load_error.empty()) {
    rows.push_back(Text(::app::strings::ui_loading_pending_requests, TextRole::Label));
  }
  if (!load_error.empty()) {
    rows.push_back(Text(std::move(load_error), TextRole::Label));
    rows.push_back(Button(::app::strings::ui_retry_loading_requests).OnClick(std::move(retry)));
  }
  if (!page_error.Get().empty()) rows.push_back(Text(page_error.Get(), TextRole::Label));
  return Column{Text(::app::strings::ui_pending_requests, TextRole::Title),
      ScrollView(Column(std::move(rows)).With(Spacing(6.0F))).With(Grow(), ScrollBar())}
      .With(Grow(), Padding(24.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View PreviewConversation(kit::AgentId agent, std::shared_ptr<kit::Conversation> conversation,
                         std::shared_ptr<kit::AgentRuntime> runtime, kit::AgentChatOptions base_options,
                         std::optional<agentkit_preview::CodeWorkspace> workspace = {}, std::optional<File> evidence = {}) {
  const auto selection = UseState(std::shared_ptr<const kit::AgentChatTextSelection>{});
  const auto local_draft = UseState(kit::AgentChatDraftSources{});
  const auto draft = base_options.draft_sources.value_or(local_draft);
  const auto local_focus = UseViewHandle();
  const auto focus = base_options.composer_focus.value_or(local_focus);
  auto options = base_options;
  options.selection = selection;
  options.draft_sources = draft;
  options.composer_focus = focus;
  auto chat = [&]() -> View {
    if (agent == agentkit_preview::DesignAgent)
      return agentkit_preview::DesignChat(conversation, runtime, std::move(options), {}, std::move(evidence));
    if (agent == agentkit_preview::CodeAgent)
      return agentkit_preview::CodeChat(conversation, runtime, std::move(options), std::move(workspace), std::move(evidence));
    if (agent == agentkit_preview::GeneralAgent)
      return agentkit_preview::GeneralChat(conversation, runtime, std::move(options));
    if (agent == agentkit_preview::PptAgent)
      return agentkit_preview::PresentationChat(conversation, runtime, std::move(options));
    return agentkit_preview::PreviewChat(conversation, std::move(options));
  }();
  return agentkit_preview::PreviewSideChatWorkspace(std::move(chat), std::move(conversation), selection, draft, focus);
}

std::optional<View> PreviewAttachmentAction(const std::shared_ptr<PreviewService>& service, const kit::AgentId& agent,
    State<kit::UserRunOptions> next, State<kit::AgentChatDraftSources> sources, kit::ModelCapabilities capabilities,
    std::function<void()> changed) {
  const auto image_model = service->applied_settings->image_model;
  if (image_model && sources->attachments.empty() &&
      service->run_settings->Capabilities(image_model, agent).image_input) {
    auto images = agentkit_preview::AttachmentPicker(service->attachments, sources, {.image_input = true},
        [next, image_model, changed] {
          auto selected = next.Get();
          selected.model = image_model;
          next = std::move(selected);
          if (changed) changed();
        });
    if (!capabilities.file_input) return images;
    capabilities.image_input = false;
    return Row{std::move(images), agentkit_preview::AttachmentPicker(service->attachments, sources, capabilities)}
        .With(Spacing(6.0F));
  }
  if (capabilities.image_input || capabilities.file_input) {
    return agentkit_preview::AttachmentPicker(service->attachments, sources, capabilities);
  }
  return {};
}

} // namespace

[[huxerui::composable]]
View DetachedArtifactReview(std::shared_ptr<kit::Conversation> conversation, std::shared_ptr<kit::AgentRuntime> runtime,
                            kit::AgentId agent, kit::AgentChatReviewRequest request,
                            agentkit_preview::PresentationReviewPages initial_pages,
                            std::optional<State<kit::AgentChatDraftSources>> draft_sources) {
  const auto window = UseWindow();
  const auto pages = UseState(std::move(initial_pages));
  kit::AgentChatOptions options{
      .draft_sources = draft_sources, .review_request = std::move(request), .review_only = true};
  agentkit_preview::ConfigurePreviewArtifactReview(options, runtime, pages);
  const auto service = UseService<PreviewService>();
  const auto snapshot = conversation->Snapshot();
  auto workspace = service && service->workspaces && snapshot && snapshot->summary.workspace
      ? service->workspaces->Find(*snapshot->summary.workspace) : std::nullopt;
  const auto theme = agentkit_preview::PreviewTheme(agentkit_preview::PreviewBrightness(
      service ? service->settings->appearance : agentkit_preview::PreviewAppearance::Dark, UseSystemThemeBrightness()));
  auto chat = PreviewConversation(std::move(agent), std::move(conversation), std::move(runtime), std::move(options),
      std::move(workspace), service ? service->workspace_evidence : std::nullopt);
  auto chrome = std::make_shared<const agentkit_preview::ChatHeader>(
      [window](const kit::AgentChatPage&, std::optional<View>) -> std::optional<View> {
        return Row{Button(::app::strings::ui_close_review).OnClick([window] { window.Close(); }), Spacer()}
            .With(Padding(14.0F));
      });
  return MaterialTheme(theme, agentkit_preview::PreviewControlsTheme(ProvideEnvironment(preview_chat_layout,
      ProvideEnvironment(agentkit_preview::ChatChrome{std::move(chrome)}, std::move(chat).With(Background(theme.colors.surface))))));
}

WindowHandle OpenPreviewArtifactReview(const ApplicationHandle& application, std::shared_ptr<kit::Conversation> conversation,
    std::shared_ptr<kit::AgentRuntime> runtime, kit::AgentId agent, kit::AgentChatReviewRequest request,
    agentkit_preview::PresentationReviewPages initial_pages, std::optional<State<kit::AgentChatDraftSources>> draft_sources) {
  const auto current = conversation->Snapshot();
  const auto title = current ? current->summary.title : "Artifact review";
  return application.OpenWindow([runtime = std::move(runtime), conversation = std::move(conversation), agent = std::move(agent),
      request = std::move(request), initial_pages = std::move(initial_pages), draft_sources] {
    return DetachedArtifactReview(conversation, runtime, agent, request, initial_pages, draft_sources);
  }, {.title = title + " - Review", .initial_size = {900.0F, 760.0F}});
}

namespace {

constexpr bool custom_preview_chrome =
#ifdef _WIN32
    true;
#else
    false;
#endif

bool CanPickPreviewFolder(const std::shared_ptr<FilePicker>& picker) {
#ifdef __EMSCRIPTEN__
  return true;
#else
  return picker && picker->CanOpenDirectories(true);
#endif
}

std::function<void()> PickPreviewFolder(const std::shared_ptr<PreviewService>& service,
    const std::shared_ptr<FilePicker>& picker, TaskScope tasks, State<bool> choosing_folder, State<std::string> error,
    std::function<void(std::optional<kit::WorkspaceId>)> use_workspace) {
#ifdef __EMSCRIPTEN__
  return [service, tasks, choosing_folder, error, use_workspace] {
    if (choosing_folder.Get()) return;
    choosing_folder = true;
    tasks.Launch([service, choosing_folder, error, use_workspace]() -> Task<void> {
      auto picked = co_await service->workspaces->PickBrowserAsync();
      choosing_folder = false;
      if (!picked.Succeeded()) error = picked.Error().message;
      else if (picked.Value()) use_workspace(std::move(*picked.Value()));
    });
  };
#else
  return [service, picker, tasks, choosing_folder, error, use_workspace] {
    if (!picker || !picker->CanOpenDirectories(true) || choosing_folder.Get()) return;
    choosing_folder = true;
    tasks.Launch([service, picker, choosing_folder, error, use_workspace]() -> Task<void> {
      auto picked = co_await picker->OpenDirectoryAsync(true);
      if (picked) {
        auto path = picked->AsFile();
        if (!path) error = "Choose a local folder for code tools.";
        else {
          auto added = co_await service->workspaces->AddAsync(*path, std::move(picked));
          if (!added.Succeeded()) error = added.Error().message;
          else use_workspace(std::move(added).Value());
        }
      }
      choosing_folder = false;
    });
  };
#endif
}

std::optional<View> BrowserDraftReport(const std::shared_ptr<PreviewService>& service, const kit::ProjectId& project,
    const std::optional<agentkit_preview::CodeWorkspace>& workspace, const std::optional<File>& native_workspace,
    State<std::optional<agentkit_preview::ImportedPresentationReport>> report) {
#ifdef __EMSCRIPTEN__
  if (workspace && !native_workspace) {
    return agentkit_preview::BrowserWorkspaceReportControl(service->runtime, project,
        service->workspaces->BrowserStore(), workspace->id, workspace->browser_name,
        [report](agentkit_preview::ImportedPresentationReport selected) { report = std::move(selected); })
        .Key("folder-report/" + project.value);
  }
#endif
  return {};
}

} // namespace

[[huxerui::composable]]
View PreviewShell() {
  const auto theme = UseTheme();
  const auto resources = UseService<Resources>();
  const auto service = UseService<PreviewService>();
  const auto application = UseApplication();
  const auto picker = UseService<FilePicker>();
  const auto dialogs = UseDialog();
  const auto tasks = UseTaskScope();
  const auto sessions = UseState(std::shared_ptr<const kit::SessionPage>{});
  const auto loading_sessions = UseState(false);
  const auto pending_page = UseState(std::shared_ptr<const kit::PendingInteractionPage>{});
  const auto pending_error = UseState(std::string{});
  const auto review_window_error = UseState(std::string{});
  const auto pending_retry = UseState(std::uint64_t{0});
  const auto navigation_open = UseState(false);
  const auto workspace_expansion = UseState(std::map<kit::WorkspaceId, bool>{});
  const auto showing_settings = UseState(false);
  const auto viewport_class = UseViewportClass();
  const bool compact = viewport_class == ViewportClass::Compact;
  const bool medium = viewport_class == ViewportClass::Medium;
  const float sidebar_width = medium ? 188.0F : 224.0F;
  const float page_gutter = compact ? 14.0F : medium ? 21.0F : 36.0F;
  using PaneSizing = std::pair<Frame, Grow>;
  const auto sidebar_split = UseState(std::optional<std::array<PaneSizing, 2>>{});
  auto conversation_action_style = UseEnvironment<IconButtonStyle>();
  conversation_action_style.icon_size = 18.0F;
  conversation_action_style.minimum_interactive_size = 34.0F;
  conversation_action_style.state_layer_size = 32.0F;
  conversation_action_style.foreground = theme.colors.on_surface_variant;
  const auto showing_pending = UseState(false);
  const auto selected = UseState(Selection{});
  const auto goal_session = UseState(kit::SessionId{});
  const auto comparison = UseState(ComparisonNavigation{});
  const auto comparison_entry = UseViewHandle();
  const auto focus_comparison_entry = UseState(false);
  const auto selecting = UseState(std::uint64_t{0});
  const auto presentation_pages = UseState(agentkit_preview::PresentationReviewPages{});
  const auto draft_focused = UseState(false);
  const auto draft_input = UseViewHandle();
  const auto focus_draft = UseState(false);
  using WorkspaceDrafts = std::map<std::optional<kit::WorkspaceId>, NewConversationDraft>;
  const auto drafts = UseState(std::make_shared<WorkspaceDrafts>());
  const auto error = UseState(std::string{});
  const auto ready = UseState(false);
  const auto folder = UseState(std::optional<kit::WorkspaceId>{});
  const auto pending_copies = UseState(std::vector<std::shared_ptr<agentkit_preview::WorktreePreview>>{});
  const auto workspace_id = folder.Get();
  const auto& current_draft = drafts.Get()->try_emplace(workspace_id).first->second;
  const auto draft = current_draft.text;
  const auto assistant = current_draft.assistant;
  const auto draft_group = current_draft.group;
  const auto new_start = current_draft.start;
  const auto new_skills = current_draft.skills;
  const auto new_report = current_draft.report;
  const auto new_sources = current_draft.sources;
  const auto creating = current_draft.creating;
  const auto draft_error = current_draft.error;
  std::optional<agentkit_preview::CodeWorkspace> workspace;
  if (workspace_id && ready.Get()) workspace = service->workspaces->Find(*workspace_id);
  const auto new_agent = assistant.Get() == 2 ? agentkit_preview::CodeAgent : assistant.Get() == 3 ? agentkit_preview::DesignAgent :
      assistant.Get() == 1 ? agentkit_preview::PptAgent : agentkit_preview::GeneralAgent;
  const bool can_focus_draft = ready.Get() && !selected->conversation && !showing_pending.Get() && !navigation_open.Get();
  Lifecycle([focus_draft, draft_input, draft_focused, can_focus_draft] {
    if (!can_focus_draft || !focus_draft.Get()) return;
    // The drawer can restore focus to its opener on the frame after RequestFocus succeeds.
    if (draft_focused.Get()) focus_draft = false;
    else draft_input.RequestFocus(true);
  }, can_focus_draft, focus_draft.Get(), draft_focused);

  const auto refresh_pending = [service, tasks, pending_copies, error] {
    tasks.Launch([service, pending_copies, error]() -> Task<void> {
      auto pending = co_await service->worktrees->PendingAsync(service->runtime, preview_user);
      if (!pending.Succeeded()) { error = pending.Error().message; co_return; }
      std::vector<std::shared_ptr<agentkit_preview::WorktreePreview>> recovered;
      for (auto& copy : pending.Value())
        recovered.push_back(std::make_shared<agentkit_preview::WorktreePreview>(std::move(copy)));
      pending_copies = std::move(recovered);
    });
  };
  Lifecycle([service, tasks, sessions, error, ready, folder, drafts, refresh_pending] {
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto task = tasks.Launch([service, sessions, error, ready, folder, drafts, refresh_pending, subscription]() -> Task<void> {
      auto started = co_await WaitForRuntime(service);
      if (!started.Succeeded()) { error = started.Error().message; co_return; }
      auto observed = co_await started.Value()->Sessions().ObserveListAsync({},
          [sessions, error](auto result) {
            if (result.Succeeded()) sessions = std::move(result).Value();
            else { sessions = std::shared_ptr<const kit::SessionPage>{}; error = result.Error().message; }
          });
      if (!observed.Succeeded()) { error = observed.Error().message; co_return; }
      *subscription = std::move(observed).Value();
      if (service->startup_workspace && !drafts.Get()->contains(service->startup_workspace)) {
        auto initial_draft = drafts.Get()->extract(std::nullopt);
        if (initial_draft) {
          initial_draft.key() = service->startup_workspace;
          drafts.Get()->insert(std::move(initial_draft));
        }
      }
      folder = service->startup_workspace;
      ready = true;
      refresh_pending();
    });
    return [task, subscription] { task.Cancel(); subscription->reset(); };
  });
  Lifecycle([service, tasks, ready, pending_page, pending_error]() -> std::function<void()> {
    if (!ready.Get()) return [] {};
    pending_error = "";
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto task = tasks.Launch([runtime = service->runtime, pending_page, pending_error, subscription]() -> Task<void> {
      auto pending = co_await runtime->Interactions().ObservePendingAsync({}, [pending_page, pending_error](auto result) {
        if (result.Succeeded()) {
          pending_page = std::move(result).Value();
          pending_error = "";
        } else {
          pending_page = std::shared_ptr<const kit::PendingInteractionPage>{};
          pending_error = result.Error().message;
        }
      });
      if (pending.Succeeded()) *subscription = std::move(pending).Value();
      else pending_error = pending.Error().message;
    });
    return [task, subscription] { task.Cancel(); subscription->reset(); };
  }, ready.Get(), pending_retry.Get());

  UseWindow().OnLifecycleChanged([service, ready](WindowLifecycleState state) {
    if (state != WindowLifecycleState::Active || !ready.Get() || service->navigation_failure.Get()) return;
    service->reload_navigation = true;
    SaveNavigation(service);
  });

  Lifecycle([comparison_entry, focus_comparison_entry] {
    if (focus_comparison_entry.Get()) {
      focus_comparison_entry = false;
      comparison_entry.RequestFocus(true);
    }
  }, focus_comparison_entry.Get());

  const auto select_session = [service, tasks, selected, selecting, presentation_pages, error, comparison, showing_settings,
                               showing_pending, navigation_open, goal_session, focus_comparison_entry](
      kit::SessionId id, std::optional<agentkit_preview::PresentationLocation> target,
      std::optional<kit::InteractionId> focus = {}, bool returning_to_origin = false) {
    const auto ticket = selecting.Get() + 1;
    selecting = ticket;
    showing_settings = false;
    tasks.Launch([service, selected, selecting, presentation_pages, error, comparison, showing_pending, navigation_open,
                  goal_session, focus_comparison_entry, ticket, id = std::move(id), target = std::move(target),
                  focus = std::move(focus), returning_to_origin]() -> Task<void> {
      auto request = target ? agentkit_preview::OpenPresentationReviewAsync(service->runtime, *target)
                            : service->runtime->Sessions().OpenAsync(id);
      auto opened = co_await std::move(request);
      if (ticket != selecting.Get()) co_return;
      if (!opened.Succeeded()) { error = opened.Error().message; co_return; }
      if (target && target->part) {
        auto pages = presentation_pages.Get();
        pages.insert_or_assign(std::pair{target->deck.id, target->deck.version}, *target->part);
        presentation_pages = std::move(pages);
      }
      auto route = comparison.Get();
      auto next = Selection{id, std::move(opened).Value(),
          target ? std::optional{kit::AgentChatReviewRequest{target->deck, ticket}} : std::nullopt,
          focus ? std::optional{kit::AgentChatInteractionRequest{*focus, ticket}} : std::nullopt};
      if (route.showing && route.origin.id == id) {
        if (returning_to_origin) next = route.origin;
        else next.conversation = route.origin.conversation;
      }
      route.showing = false;
      comparison = std::move(route);
      goal_session = kit::SessionId{};
      selected = std::move(next);
      if (returning_to_origin) focus_comparison_entry = true;
      showing_pending = false;
      navigation_open = false;
      error = "";
    });
  };
  const auto open_session = [select_session](kit::SessionId id) { select_session(std::move(id), std::nullopt); };
  const auto open_presentation = [select_session](agentkit_preview::PresentationLocation target) {
    auto id = target.session;
    select_session(std::move(id), std::move(target));
  };
  const auto create = [service, tasks, ready, entry = current_draft, new_agent, open_session, selecting, workspace_id] {
    if (!ready.Get() || entry.creating.Get() || entry.text->composition ||
        (entry.text->text.empty() && entry.sources->attachments.empty())) return;
    const auto code = new_agent == agentkit_preview::CodeAgent;
    if (code) {
      const auto workspace = workspace_id ? service->workspaces->Find(*workspace_id) : std::nullopt;
      if (!service->code_available.Get() || !workspace || !workspace->NativeFolder()) return;
    }
    const auto draft_start = entry.start.Get();
    auto start = draft_start;
    if (code && !start.model) start.model = service->run_settings->DefaultModel(new_agent);
    entry.creating = true;
    entry.error = "";
    tasks.Launch([service, entry, input = entry.text->text, group = entry.group.Get(), role = entry.assistant.Get(),
                  agent = new_agent, start = std::move(start), draft_start, open_session, selecting, route = selecting.Get(),
                  workspace = workspace_id, skills = entry.skills.Get(), report = entry.report.Get(),
                  staged = entry.sources->attachments]() -> Task<void> {
      kit::SendInput first{.text = input, .skills = skills};
      const auto project = ProjectFor(workspace);
      if (report && agent == agentkit_preview::PptAgent && report->ref.project == project) first.context.push_back({report->ref});
      for (const auto& attachment : staged) first.staged_attachments.push_back(attachment->id);
      kit::CreateSessionRequest request{
          .options = {.agent = agent, .project = project, .workspace = workspace,
                      .retention = kit::SessionRetention::Stored},
          .initial = std::move(first), .user_start = start};
      auto result = co_await service->runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request));
      entry.creating = false;
      if (!result.Succeeded()) { entry.error = result.Error().message; co_return; }
      if (entry.text->text == input && !entry.text->composition) entry.text = TextEditingValue::FromText({});
      if (entry.skills.Get() == skills) entry.skills = std::vector<kit::SkillRef>{};
      if (entry.report.Get() == report) entry.report = std::nullopt;
      auto remaining = entry.sources.Get();
      std::erase_if(remaining.attachments, [&](const auto& item) {
        return std::ranges::find(staged, item) != staged.end();
      });
      const bool draft_consumed = entry.text->text.empty() && !entry.text->composition &&
          remaining.attachments.empty() && entry.skills->empty() && !entry.report.Get();
      entry.sources = std::move(remaining);
      if (draft_consumed && entry.assistant.Get() == role && entry.start.Get() == draft_start && entry.group.Get() == group) {
        entry.assistant = std::size_t{0};
        entry.start = default_draft_start;
        entry.group = std::nullopt;
      }
      const auto id = std::get<kit::SessionCreated>(result.Value().outcome).session;
      service->composers.try_emplace(id).first->second.next = start;
      SaveComposerPreference(service, id);
      if (group) PlaceConversation(service, id, group);
      if (selecting.Get() == route) open_session(id);
    });
  };
  const auto colors = theme.colors;
  const auto nav = service->navigation.Get();
  const auto show_draft = [selected, selecting, error, showing_settings, showing_pending, navigation_open, focus_draft] {
    selecting = selecting.Get() + 1;
    selected = Selection{};
    showing_settings = false;
    showing_pending = false;
    navigation_open = false;
    focus_draft = true;
    error = "";
  };
  const auto start_draft = [show_draft, drafts, folder](std::optional<kit::WorkspaceId> scope, std::optional<std::string> group) {
    if (group) drafts.Get()->try_emplace(scope).first->second.group = std::move(group);
    folder = std::move(scope);
    show_draft();
  };
  const auto begin_draft = [start_draft, workspace_id](std::optional<std::string> group) {
    start_draft(workspace_id, std::move(group));
  };
  const auto use_workspace = [start_draft, workspace_expansion](std::optional<kit::WorkspaceId> id) {
    if (id) {
      auto expansion = workspace_expansion.Get();
      expansion[*id] = true;
      workspace_expansion = std::move(expansion);
    }
    start_draft(std::move(id), {});
  };
  std::vector<View> directory;
  const auto show_worktree = [dialogs, service, open_session, refresh_pending](kit::SessionSummary item,
      std::shared_ptr<agentkit_preview::WorktreePreview> recovered) {
    const auto source = recovered ? recovered->request.from.session : item.id;
    const auto open_copy = [service, source, open_session](kit::SessionId id) {
      if (auto group = GroupFor(service->navigation.Get(), source)) PlaceConversation(service, id, *group);
      open_session(std::move(id));
    };
    agentkit_preview::ShowPreviewDialog(dialogs, DialogFactory{[service, item, recovered, open_copy, refresh_pending](DialogContext context) {
      return CopyWorktreeDialog(context, service, item, recovered, open_copy, refresh_pending);
    }}, {.dismiss_on_outside_press = false, .dismiss_on_cancel = false});
  };
  const auto copy_conversation = [service, tasks, error, open_session](kit::SessionSummary item) {
    tasks.Launch([service, error, open_session, item]() -> Task<void> {
      auto boundary = co_await ForkBoundary(service->runtime, item.id);
      if (!boundary.Succeeded()) { error = boundary.Error().message; co_return; }
      kit::ForkSessionRequest request{.from = std::move(boundary).Value(),
          .options = {.agent = item.agent.id, .project = item.project, .title = item.title + " (copy)"}};
      if (item.workspace) request.workspace = kit::WorkspaceChoice{kit::WorkspaceChoice::Kind::Existing, *item.workspace};
      auto result = co_await service->runtime->Sessions().ForkAsync(kit::NewCommandId(), std::move(request));
      if (!result.Succeeded()) { error = result.Error().message; co_return; }
      if (const auto* created = std::get_if<kit::SessionCreated>(&result.Value().outcome)) {
        if (auto group = GroupFor(service->navigation.Get(), item.id)) PlaceConversation(service, created->session, *group);
        open_session(created->session);
      } else error = "Approve the copy in the source conversation";
    });
  };
  const auto change_metadata = [service, selected, selecting, error](kit::SessionSummary item, kit::SessionMetadataChange change) {
    UseApplicationTaskScope().Launch([service, selected, selecting, error, item, change = std::move(change)]() -> Task<void> {
      const auto command = kit::NewCommandId();
      auto updated = co_await service->runtime->Sessions().UpdateMetadataAsync(command, item.id, item.metadata_revision, change);
      if (!updated.Succeeded()) { error = updated.Error().message; co_return; }
      error = "";
      if (std::holds_alternative<kit::DeleteSession>(change)) {
        EditNavigation(service, [id = item.id](NavigationPreferences& navigation) {
          navigation.memberships.erase(id);
          return kit::AgentResult<void>::Success();
        });
        if (selected->id == item.id) { selecting = selecting.Get() + 1; selected = Selection{}; }
      }
    });
  };
  const auto transfer = [service, tasks, select_session, selected, error](kit::SessionSummary item, kit::AgentId assistant) {
    tasks.Launch([service, select_session, selected, error, item, assistant = std::move(assistant)]() -> Task<void> {
      auto session = co_await service->runtime->Sessions().OpenAsync(item.id);
      if (!session.Succeeded()) { error = session.Error().message; co_return; }
      auto receipt = co_await session.Value()->TransferAsync(kit::NewCommandId(), {assistant, item.binding_revision});
      if (!receipt.Succeeded()) { error = receipt.Error().message; co_return; }
      if (const auto* approval = std::get_if<kit::PendingApproval>(&receipt.Value().outcome))
        select_session(approval->session, std::nullopt, approval->interaction);
      else if (selected->id != item.id) select_session(item.id, std::nullopt);
      else error = "";
    });
  };
  const auto render_session = [&](const kit::SessionSummary& item) {
    return SessionRow(item, selected->id == item.id && !showing_pending.Get(), GroupFor(nav, item.id).has_value(), open_session,
        [item, copy_conversation, show_worktree, service, dialogs, change_metadata, transfer] {
      std::vector<MenuEntry> entries{MenuItem(::app::strings::ui_rename_conversation, [dialogs, item, change_metadata] {
        agentkit_preview::ShowPreviewDialog(dialogs, DialogFactory{[item, change_metadata](DialogContext context) {
          return NavigationNameDialog(context, ::app::strings::ui_rename_conversation, item.title,
              [item, change_metadata](std::string name) { change_metadata(item, kit::RenameSession{std::move(name)}); });
        }});
      })};
      const auto current = GroupFor(service->navigation.Get(), item.id);
      std::vector<MenuEntry> destinations{MenuItem(::app::strings::ui_ungrouped, [item, service, current] {
        PlaceConversation(service, item.id, {}, current);
      }).Checked(!current)};
      for (const auto& group : service->navigation.Get().groups) {
        destinations.emplace_back(MenuItem(group.name, [item, service, current, id = group.id] {
          PlaceConversation(service, item.id, id, current);
        })
            .Checked(current == group.id));
      }
      entries.emplace_back(MenuItem(::app::strings::ui_move_to_group, std::move(destinations)));
      const auto workspace = item.workspace ? service->workspaces->Find(*item.workspace) : std::nullopt;
      const bool native_workspace = workspace && workspace->NativeFolder().has_value();
      std::vector<MenuEntry> assistants;
      for (const auto& [label, id] : {std::pair{::app::strings::ui_general_assistant, agentkit_preview::GeneralAgent},
              {::app::strings::ui_code_assistant, agentkit_preview::CodeAgent}, {::app::strings::ui_ppt_assistant, agentkit_preview::PptAgent},
              {::app::strings::ui_design_assistant, agentkit_preview::DesignAgent}}) {
        if (id == item.agent.id || (id == agentkit_preview::CodeAgent && (!native_workspace || !service->code_available.Get()))) continue;
        assistants.emplace_back(MenuItem(label, [item, transfer, id] { transfer(item, id); }));
      }
      entries.emplace_back(MenuItem(::app::strings::ui_transfer_to_assistant, std::move(assistants)).Enabled(!item.active_run));
      if (item.project) entries.emplace_back(MenuItem(::app::strings::ui_shared_reports, [dialogs, runtime = service->runtime, project = *item.project] {
        agentkit_preview::ShowPreviewDialog(dialogs, DialogFactory{[runtime, project](DialogContext dialog) {
          return agentkit_preview::SharedReportsDialog(dialog, runtime, project);
        }});
      }));
      entries.emplace_back(MenuItem(::app::strings::ui_copy_conversation, [item, copy_conversation] { copy_conversation(item); }));
      if (native_workspace && item.agent.id == agentkit_preview::CodeAgent)
        entries.emplace_back(MenuItem(::app::strings::ui_copy_to_worktree, [item, show_worktree] { show_worktree(item, {}); }));
      entries.emplace_back(MenuItem(::app::strings::ui_delete_conversation, [item, dialogs, change_metadata] {
        agentkit_preview::ShowPreviewDialog(dialogs, DialogFactory{[item, change_metadata](DialogContext context) {
          return Column{Text(::app::strings::ui_delete_conversation, TextRole::Title), Text(item.title, TextRole::Label),
              Text(::app::strings::ui_this_removes_the_conversation_and_its_group),
              Row{Spacer(), Button(::app::strings::ui_cancel).OnClick([context] { context.Dismiss(); }),
                  Button(::app::strings::ui_delete).OnClick([context, item, change_metadata] {
                    change_metadata(item, kit::DeleteSession{});
                    context.Dismiss();
                  })}.With(Spacing(8.0F))}.With(Frame{.width = 400.0F}, Padding(24.0F), Spacing(14.0F));
        }});
      }).Enabled(!item.active_run));
      return entries;
    }).Key("session-row/" + item.id.value);
  };
  const auto listing = sessions.Get();
  const auto folders = ready.Get() ? service->workspaces->Folders() : std::vector<agentkit_preview::CodeWorkspace>{};
  const auto has_folder = [&folders](const std::optional<kit::WorkspaceId>& id) {
    return id && std::ranges::find(folders, *id, &agentkit_preview::CodeWorkspace::id) != folders.end();
  };
  const auto workspace_directory = [&](const std::optional<kit::WorkspaceId>& scope) {
    const auto in_scope = [&scope, &has_folder](const kit::SessionSummary& item) {
      return scope ? item.workspace == scope : !has_folder(item.workspace);
    };
    std::vector<const ConversationGroup*> groups;
    for (const auto& group : nav.groups) {
      const auto member = [&nav, &group](const kit::SessionSummary& item) { return GroupFor(nav, item.id) == group.id; };
      const bool has_any = listing && std::ranges::any_of(listing->sessions, member);
      const bool has_local = listing && std::ranges::any_of(listing->sessions, [&](const auto& item) {
        return in_scope(item) && member(item);
      });
      if (!has_local && (has_any || scope != (has_folder(group.workspace) ? group.workspace : std::nullopt))) continue;
      groups.push_back(&group);
    }
    std::vector<View> rows;
    for (std::size_t i = 0; i < groups.size(); ++i) {
      const auto& group = *groups[i];
      std::vector<View> section{ConversationGroupRow(group,
          [service, group] {
            EditGroup(service, group.id, [collapsed = !group.collapsed](auto& stored) {
              stored.collapsed = collapsed;
              return kit::AgentResult<void>::Success();
            });
          },
          [service, group, dialogs, scope, start_draft, before = i ? std::optional{groups[i - 1]->id} : std::nullopt,
              after = i + 1 < groups.size() ? std::optional{groups[i + 1]->id} : std::nullopt] {
            return std::vector<MenuEntry>{
                MenuItem(::app::strings::ui_new_conversation_in_group, [start_draft, scope, id = group.id] { start_draft(scope, id); }),
                MenuItem(::app::strings::ui_rename_group, [service, group, dialogs] {
                  agentkit_preview::ShowPreviewDialog(dialogs, DialogFactory{[service, group](DialogContext context) {
                    return NavigationNameDialog(context, ::app::strings::ui_rename_group, group.name, [service, group](std::string name) {
                      EditGroup(service, group.id, [expected = group.name, name = std::move(name)](auto& stored) {
                        if (stored.name != expected && stored.name != name)
                          return kit::AgentResult<void>::Failure({.code = kit::ErrorCode::Conflict, .message = "This group was renamed in another window"});
                        stored.name = name;
                        return kit::AgentResult<void>::Success();
                      });
                    });
                  }});
                }),
                MenuItem(::app::strings::ui_move_up, [service, id = group.id, before] { if (before) MoveGroup(service, id, *before, false); }).Enabled(before.has_value()),
                MenuItem(::app::strings::ui_move_down, [service, id = group.id, after] { if (after) MoveGroup(service, id, *after, true); }).Enabled(after.has_value()),
                MenuItem(::app::strings::ui_remove_group, [service, id = group.id] {
                  EditNavigation(service, [id](auto& data) {
                    std::erase_if(data.groups, [&](const auto& stored) { return stored.id == id; });
                    std::erase_if(data.memberships, [&](const auto& item) { return item.second == id; });
                    return kit::AgentResult<void>::Success();
                  });
                }),
            };
          }).Key("category/" + group.id)};
      if (!group.collapsed && listing) {
        for (const auto& item : listing->sessions) {
          if (in_scope(item) && GroupFor(nav, item.id) == group.id) section.push_back(render_session(item));
        }
      }
      rows.push_back(Column(std::move(section)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key(group.id));
    }
    if (listing) for (const auto& item : listing->sessions) {
      if (in_scope(item) && !GroupFor(nav, item.id)) rows.push_back(render_session(item));
    }
    return rows;
  };
  if (listing) {
    if (listing->next) directory.push_back(Button(::app::strings::ui_load_more_conversations).OnClick([service, tasks, sessions, listing, loading_sessions, error] {
      if (loading_sessions.Get()) return;
      loading_sessions = true;
      tasks.Launch([service, sessions, listing, loading_sessions, error]() -> Task<void> {
        auto next = co_await service->runtime->Sessions().ListAsync({}, listing->next);
        const bool expired = !next.Succeeded() && next.Error().code == kit::ErrorCode::CursorExpired;
        if (expired && sessions.Get() == listing) next = co_await service->runtime->Sessions().ListAsync({}, {});
        loading_sessions = false;
        if (sessions.Get() != listing) co_return;
        if (!next.Succeeded()) { error = next.Error().message; co_return; }
        if (expired) { sessions = std::make_shared<const kit::SessionPage>(std::move(next).Value()); co_return; }
        auto merged = std::make_shared<kit::SessionPage>(*listing);
        merged->sessions.insert(merged->sessions.end(), next.Value().sessions.begin(), next.Value().sessions.end());
        merged->next = next.Value().next;
        sessions = std::move(merged);
      });
    }).With(Enabled(!loading_sessions.Get())));
  }
  for (const auto& pending : pending_copies.Get()) {
    const auto label = pending->comparison ? "Review candidate checkout \xc2\xb7 " + pending->request.options.title :
        std::string(pending->cleanup_started ? "Finish cleanup \xc2\xb7 " : "Resume copy \xc2\xb7 ") + pending->branch;
    directory.push_back(Button(label)
        .OnClick([show_worktree, pending] { show_worktree({}, pending); }));
  }
  const auto choosing_folder = UseState(false);
  const bool can_open_folder = ready.Get() && CanPickPreviewFolder(picker);
  const auto pick_workspace = PickPreviewFolder(service, picker, tasks, choosing_folder, error, use_workspace);
  const auto open_pending = [showing_settings, showing_pending, selecting, error, navigation_open] {
    showing_pending = showing_settings.Get() || !showing_pending.Get();
    showing_settings = false;
    selecting = selecting.Get() + 1;
    navigation_open = false;
    error = "";
  };
  const auto section_label = [colors](StringVariant label) {
    return Text(std::move(label)).Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface_variant})
        .With(Padding(EdgeInsets{.left = 8.0F}));
  };
  const auto active_snapshot = selected->conversation ? selected->conversation->Snapshot() : nullptr;
  const auto heading_snapshot = selected->conversation && !showing_pending.Get() && comparison->showing && comparison->origin.conversation
      ? comparison->origin.conversation->Snapshot() : active_snapshot;
  const auto current_folder = heading_snapshot ? heading_snapshot->summary.workspace : workspace_id;
  const auto folder_name = [](const agentkit_preview::CodeWorkspace& entry) {
    if (entry.root.empty()) return entry.browser_name;
    auto name = entry.root.filename().u8string();
    if (name.empty()) name = entry.root.u8string();
    return std::string(name.begin(), name.end());
  };
  const auto new_group = [dialogs, service, current_folder, folder_name, workspace_expansion] {
    const auto workspace = current_folder ? service->workspaces->Find(*current_folder) : std::nullopt;
    const auto scope = workspace ? current_folder : std::nullopt;
    const auto title = workspace ? StringVariant::Format(::app::strings::ui_new_group_in_0, folder_name(*workspace)) : StringVariant{::app::strings::ui_new_standalone_group};
    agentkit_preview::ShowPreviewDialog(dialogs, DialogFactory{[service, scope, title, workspace_expansion](DialogContext context) {
      return NavigationNameDialog(context, title, {}, [service, scope, workspace_expansion](std::string name) {
        if (scope) {
          auto expansion = workspace_expansion.Get();
          expansion[*scope] = true;
          workspace_expansion = std::move(expansion);
        }
        EditNavigation(service, [group = ConversationGroup{kit::NewCommandId().value, std::move(name), false, scope}](auto& stored) {
          if (std::ranges::find(stored.groups, group.id, &ConversationGroup::id) == stored.groups.end()) stored.groups.push_back(group);
          return kit::AgentResult<void>::Success();
        });
      });
    }});
  };
  std::vector<View> sections;
  if (!folders.empty()) {
    std::vector<View> items{section_label(::app::strings::ui_workspaces)};
    for (const auto& entry : folders) {
      auto title = folder_name(entry);
      auto tooltip = title;
      if (const auto root = entry.NativeFolder()) {
        tooltip = root->Path();
        if (std::ranges::count_if(folders, [&](const auto& folder) { return folder_name(folder) == title; }) > 1) {
          const auto parent = entry.root.parent_path().filename().u8string();
          if (!parent.empty()) title += " \xc2\xb7 " + std::string(parent.begin(), parent.end());
        }
      }
      const auto expanded = workspace_expansion->find(entry.id);
      auto children = workspace_directory(entry.id);
      const bool open = expanded == workspace_expansion->end() ? entry.id == current_folder || !children.empty() : expanded->second;
      const auto toggle = [workspace_expansion, id = entry.id, open] {
        auto expansion = workspace_expansion.Get();
        expansion[id] = !open;
        workspace_expansion = std::move(expansion);
      };
      const auto create = [use_workspace, id = entry.id] { use_workspace(id); };
      const auto new_label = StringVariant::Format(::app::strings::ui_new_conversation_in_0, title);
      auto folder_row = NavigationAction(std::move(title), open ? NavigationSymbol::Down : NavigationSymbol::Right,
          toggle, true, false, {}, open)
          .With(Tooltip(std::move(tooltip)), Grow()).Key("workspace/" + entry.id.value);
      auto add = ProvideEnvironment(conversation_action_style, IconButton(::app::images::add, new_label)
          .OnClick(create).With(Tooltip(new_label)).Key("workspace-new/" + entry.id.value));
      std::vector<View> branch{Row{std::move(folder_row), std::move(add)}.With(CrossAlign(CrossAxisAlignment::Center))};
      if (open && !children.empty()) branch.push_back(Row{
          Divider(Axis::Vertical),
          Column(std::move(children)).With(Grow(), Spacing(2.0F), CrossAlign(CrossAxisAlignment::Stretch)),
      }.With(Padding(EdgeInsets{.left = 13.0F}), Spacing(7.0F), CrossAlign(CrossAxisAlignment::Stretch)));
      items.push_back(Column(std::move(branch)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch))
          .Key("workspace-tree/" + entry.id.value));
    }
    sections.push_back(Column(std::move(items)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  }
  sections.push_back(section_label(::app::strings::ui_standalone));
  sections.push_back(Column(workspace_directory(std::nullopt)).With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  if (!directory.empty()) sections.push_back(Column(std::move(directory)).With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  const auto pending = pending_page.Get();
  const auto pending_count = pending && !pending->items.empty() ?
      std::to_string(pending->items.size()) + (pending->next ? "+" : "") : std::string{};
  const auto settings_available = ready.Get() && service->settings_file.has_value();
  const auto show_settings = [showing_settings, selecting, navigation_open] {
    selecting = selecting.Get() + 1;
    navigation_open = false;
    showing_settings = true;
  };
  std::vector<View> sidebar_items;
  if (!custom_preview_chrome) sidebar_items.push_back(PreviewWordmark(colors));
  sidebar_items.push_back(NavigationAction(::app::strings::ui_new_conversation, NavigationSymbol::New,
      [begin_draft] { begin_draft(std::nullopt); }, true,
      !selected->conversation && !showing_pending.Get() && !showing_settings.Get()).Key("new-conversation"));
  sidebar_items.push_back(Column{
      NavigationAction(::app::strings::ui_open_folder, NavigationSymbol::Folder, pick_workspace, can_open_folder && !choosing_folder.Get())
          .Key("open-folder"),
      NavigationAction(::app::strings::ui_new_group, NavigationSymbol::Plus, new_group, ready.Get()).Key("new-group"),
      NavigationAction(::app::strings::ui_pending, NavigationSymbol::Inbox, open_pending, ready.Get(), showing_pending.Get(), pending_count)
          .Key("pending-navigation"),
  }.With(Spacing(2.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  sidebar_items.push_back(ProvideEnvironment(DividerStyle{colors.outline}, Divider()));
  sidebar_items.push_back(ScrollView(Column(std::move(sections)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .Key("session-directory").With(Grow()));
  if (settings_available) sidebar_items.push_back(Column{
      ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
      NavigationAction(::app::strings::ui_settings, NavigationSymbol::Settings, show_settings, true, showing_settings.Get()).Key("open-settings"),
  }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  auto sidebar = Column(std::move(sidebar_items)).With(
      Padding(EdgeInsets{.top = 18.0F, .right = 14.0F, .bottom = 16.0F, .left = 14.0F}), Spacing(14.0F),
      CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low)).Key("workbench-navigation");

  View main;
  if (showing_pending.Get() && ready.Get()) {
    main = PendingRequests(service->runtime, pending_page, pending_error.Get(), [pending_retry] {
      pending_retry = pending_retry.Get() + 1;
    }, [select_session](kit::PendingInteraction item) {
      select_session(std::move(item.session.id), std::nullopt, item.interaction.id);
    });
    if (!error.Get().empty()) main = Column{std::move(main).With(Grow()), Text(error.Get(), TextRole::Label)}.With(Grow());
  } else if (selected->conversation) {
    const auto render_conversation = [&](const Selection& selection, const std::shared_ptr<const kit::SessionSnapshot>& snapshot) {
      const auto agent = snapshot ? snapshot->summary.agent.id : kit::AgentId{};
      const auto& composer = service->composers.try_emplace(selection.id).first->second;
      const bool returning = comparison->origin.conversation && comparison->review && comparison->branch == selection.id;
      std::vector<View> actions;
      if (returning || (snapshot && snapshot->summary.project)) {
        const auto& label = returning ? ::app::strings::ui_back_to_comparison : ::app::strings::ui_compare_alternatives;
        auto compare = IconButton(::app::images::compare, label).OnClick([comparison, selection, returning] {
          auto route = comparison.Get();
          if (!returning) {
            if (route.origin.id != selection.id) route = ComparisonNavigation{};
            route.origin = selection;
          }
          route.showing = true;
          comparison = std::move(route);
        });
        if (!returning) compare = std::move(compare).With(comparison_entry);
        actions.push_back(std::move(compare).With(Tooltip(returning ? label : ::app::strings::ui_comparison_explainer))
            .Key(returning ? "comparison-return" : "comparison-entry"));
      }
      kit::AgentChatOptions options{
          .draft_sources = composer.sources,
          .draft = composer.text,
          .run_controls = kit::AgentChatRunControls{composer.next,
              [service, goal_session, conversation_action_style, next = composer.next, skills = composer.skills,
               error = composer.preference_error, sources = composer.sources](
                  const kit::SessionSummary& summary, std::optional<kit::RunStartBinding> current) {
                const auto folder = summary.workspace ? service->workspaces->Find(*summary.workspace) : std::nullopt;
                std::vector<View> actions{
                    ProvideEnvironment(conversation_action_style,
                        IconButton(::app::images::goal, ::app::strings::ui_goal)
                            .OnClick([goal_session, id = summary.id] { goal_session = id; })
                            .With(Tooltip(::app::strings::ui_goal)).Key("conversation-goal")),
                };
                if (summary.agent.id == agentkit_preview::GeneralAgent)
                  actions.push_back(agentkit_preview::SkillPicker(service->runtime, {summary.id}, skills));
                auto caps = service->run_settings->Capabilities(next->model, summary.agent.id);
                if (current) {
                  const auto active = service->run_settings->Capabilities(current->model, summary.agent.id);
                  caps.image_input |= active.image_input;
                  caps.file_input |= active.file_input;
                }
                auto save_preference = [service, id = summary.id] { SaveComposerPreference(service, id); };
                if (auto picker = PreviewAttachmentAction(service, summary.agent.id, next, sources, caps, save_preference))
                  actions.push_back(std::move(*picker));
                auto controls = agentkit_preview::RunSettingsControls(service->run_settings, next, summary.agent.id, summary.workspace,
                    folder ? folder->NativeFolder() : std::nullopt, std::move(current),
                    std::move(save_preference), Flow(std::move(actions)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)),
                    AddModelAction(service));
                if (error.Get().empty()) return controls;
                return View(Column{std::move(controls), Text(error.Get(), TextRole::Label)}.With(Spacing(4.0F)));
              },
              [settings = service->run_settings, resources](const kit::RunStartBinding& binding) { return settings->Describe(binding, *resources); }},
          .recovered_references = composer.recovered,
          .draft_skills = composer.skills,
          .review_request = selection.review, .interaction_request = selection.interaction,
          .agent_label = [resources](const kit::AgentRef& ref) { return resources->GetString(PreviewAgentLabel(ref)); }};
      options.render_block = [service](const kit::Message& message, const kit::ContentBlock& block,
          std::shared_ptr<kit::Conversation> conversation) {
        return RenderPreviewBlock(*service, message, block, std::move(conversation));
      };
      options.review_attachment_label = agentkit_preview::CodeReviewAttachmentLabel;
      options.tool_label = [resources](const kit::ToolRef& tool) -> std::string {
        using namespace agentkit_preview;
        if (tool.id == ReadCodeFiles || tool.id == ReadWorkspaceFiles) return resources->GetString(::app::strings::ui_activity_read_files);
        if (tool.id == ListWorkspaceFiles) return resources->GetString(::app::strings::ui_activity_list_files);
        if (tool.id == SearchWorkspaceFiles) return resources->GetString(::app::strings::ui_activity_search_files);
        if (IsCodePatchTool(tool.id)) return resources->GetString(::app::strings::ui_activity_apply_changes);
        if (tool.id == RunCodeTests || tool.id == RunWorkspaceTests) return resources->GetString(::app::strings::ui_activity_workspace_tests);
        return {};
      };
      agentkit_preview::ConfigurePreviewArtifactReview(options, service->runtime, presentation_pages, open_presentation, service->workspaces);
      std::function<void(kit::AgentChatReviewRequest)> open_review;
      if (application.SupportsMultipleWindows()) {
        open_review = [application, runtime = service->runtime, conversation = selection.conversation,
            agent, presentation_pages, draft_sources = composer.sources](kit::AgentChatReviewRequest request) {
          agentkit_preview::PresentationReviewPages initial_pages;
          const auto key = std::pair{request.artifact.id, request.artifact.version};
          const auto& positions = presentation_pages.Get();
          if (auto page = positions.find(key); page != positions.end()) initial_pages.emplace(key, page->second);
          OpenPreviewArtifactReview(application, conversation, runtime, agent, std::move(request), std::move(initial_pages), draft_sources);
        };
      }
      std::string location;
      if (snapshot && snapshot->summary.workspace) {
        const auto folder = service->workspaces->Find(*snapshot->summary.workspace);
        location = folder ? folder_name(*folder) : UseString(::app::strings::ui_workspace_unavailable);
      }
      if (const auto group = GroupFor(nav, selection.id)) {
        const auto found = std::ranges::find(nav.groups, *group, &ConversationGroup::id);
        if (found != nav.groups.end()) {
          if (!location.empty()) location += " · ";
          location += found->name;
        }
      }
      auto chrome = std::make_shared<const agentkit_preview::ChatHeader>(
          [actions = std::move(actions), snapshot, location = std::move(location), conversation_action_style,
           open_review = std::move(open_review), review_window_error](
              const kit::AgentChatPage& page, std::optional<View> workspace_actions) -> std::optional<View> {
            if (const auto* review = std::get_if<kit::AgentChatReviewRequest>(&page)) {
              if (!open_review) return {};
              std::vector<View> toolbar{Spacer(), Button(::app::strings::ui_open_in_new_window).OnClick(
                  [open_review, review = *review, review_window_error] {
                    try {
                      open_review(review);
                      review_window_error = "";
                    } catch (const std::exception& failure) {
                      review_window_error = failure.what();
                    }
                  })};
              if (!review_window_error->empty())
                toolbar.insert(toolbar.begin(), Text(review_window_error.Get(), TextRole::Label));
              return Row(std::move(toolbar)).With(Padding(EdgeInsets::Symmetric(24.0F, 4.0F)));
            }
            if (!snapshot || !std::holds_alternative<std::monostate>(page)) return {};
            const auto viewport = UseViewportClass();
            const auto compact = viewport == ViewportClass::Compact;
            const auto colors = UseTheme().colors;
            const float gutter = compact ? 14.0F : viewport == ViewportClass::Medium ? 21.0F : 36.0F;
            auto title = Text(snapshot->summary.title, TextRole::Title)
                .Style({Font::System(17.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
                .LineLimit(1).Overflow(TextOverflow::Ellipsis).Key("agent-chat-title");
            auto buttons = actions;
            if (workspace_actions) buttons.insert(buttons.begin(), std::move(*workspace_actions));
            auto controls = ProvideEnvironment(conversation_action_style,
                Row(std::move(buttons)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Center)))
                .Key("agent-chat-header-actions");
            std::vector<View> row;
            if (!location.empty()) {
              row.push_back(Row{
                  NavigationIcon(NavigationSymbol::Folder, colors.on_surface_variant),
                  Text(location).Style({Font::System(12.0F), colors.on_surface_variant})
                      .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Frame{.max_width = compact ? 72.0F : 152.0F}),
                  NavigationIcon(NavigationSymbol::Right, colors.on_surface_variant),
              }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center), Tooltip(location)).Key("conversation-location"));
            }
            row.push_back(std::move(title).With(Grow()));
            row.push_back(std::move(controls));
            auto heading = Row(std::move(row)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
            return Column{std::move(heading).Key("agent-chat-heading")}
                .With(Padding(EdgeInsets{.top = 10.0F, .right = gutter, .left = gutter}),
                    CrossAlign(CrossAxisAlignment::Stretch));
          });
      auto code_workspace = snapshot && snapshot->summary.workspace ? service->workspaces->Find(*snapshot->summary.workspace) : std::nullopt;
      auto chat = PreviewConversation(agent, selection.conversation, service->runtime, std::move(options),
          std::move(code_workspace), service->workspace_evidence);
      const auto run = snapshot && snapshot->summary.active_run ? snapshot->summary.run_state : std::nullopt;
      return agentkit_preview::SessionWorkspace(selection.conversation,
          ProvideEnvironment(preview_chat_layout,
              ProvideEnvironment(agentkit_preview::ChatChrome{std::move(chrome)}, std::move(chat))),
          service->runtime, run,
          goal_session.Get() == selection.id, [goal_session] { goal_session = kit::SessionId{}; },
          [select_session](kit::PendingApproval approval) {
            select_session(std::move(approval.session), std::nullopt, std::move(approval.interaction));
          }).Key(selection.id.value);
    };
    const bool comparing = comparison->showing && comparison->origin.conversation;
    const bool returning = comparison->origin.conversation && comparison->review && comparison->branch == selected->id;
    const bool retain_origin = (comparing || returning) && comparison->origin.id != selected->id;
    auto origin = retain_origin ? render_conversation(comparison->origin, comparison->origin.conversation->Snapshot())
                                : render_conversation(selected.Get(), active_snapshot);
    View comparison_page = Spacer();
    if (comparing) {
      comparison_page = agentkit_preview::ComparisonWorkspace(service->runtime, comparison->origin.conversation, comparison->review,
          {service->worktrees, service->workspaces, *service->workspace_evidence, preview_user, service->code_available.Get(), refresh_pending},
          [comparison](std::optional<kit::ForkReviewId> id) {
            auto route = comparison.Get();
            route.review = std::move(id);
            route.branch.reset();
            comparison = std::move(route);
          },
          [comparison, selected, selecting, select_session, focus_comparison_entry] {
            auto route = comparison.Get();
            if (selected->id != route.origin.id) {
              select_session(route.origin.id, std::nullopt, std::nullopt, true);
              return;
            }
            selecting = selecting.Get() + 1;
            route.showing = false;
            comparison = std::move(route);
            focus_comparison_entry = true;
          },
          [comparison, open_session](kit::SessionId branch) {
            auto route = comparison.Get();
            route.branch = branch;
            comparison = std::move(route);
            open_session(std::move(branch));
          },
          open_session).Key("comparison/" + comparison->origin.id.value);
    }
    const bool showing_branch = retain_origin && !comparing;
    auto branch = showing_branch ? render_conversation(selected.Get(), active_snapshot) : View(Spacer());
    main = IndexedPages({std::move(origin), std::move(comparison_page), std::move(branch)},
        comparing ? 1U : showing_branch ? 2U : 0U).With(Grow());
    if (!error.Get().empty())
      main = Column{std::move(main).With(Grow()), Text(error.Get(), TextRole::Label).With(Padding(10.0F))}.With(Grow());
  } else {
    const bool code = assistant.Get() == 2;
    const auto native_workspace = workspace ? workspace->NativeFolder() : std::nullopt;
    std::string status = draft_error->empty() ? error.Get() : draft_error.Get();
    if (code && status.empty()) {
      if (!service->code_available.Get()) status = UseString(::app::strings::ui_add_a_model_to_use_the_code);
      else if (!workspace) status = UseString(::app::strings::ui_open_a_local_folder_to_start);
      else if (!native_workspace) status = UseString(::app::strings::ui_code_changes_and_tests_require_a_desktop);
    }
    const std::array assistants{::app::strings::ui_general, ::app::strings::ui_ppt, ::app::strings::ui_code, ::app::strings::ui_design};
    const auto chosen_group = draft_group.Get();
    const auto group = chosen_group ? std::ranges::find(nav.groups, *chosen_group, &ConversationGroup::id) : nav.groups.end();
    const auto workspace_path = native_workspace ? native_workspace->Path() : std::string{};
    const auto choose_assistant = [assistants, assistant, new_start, new_skills, focus_draft] {
      std::vector<MenuEntry> choices;
      for (std::size_t index = 0; index < assistants.size(); ++index) {
        choices.emplace_back(MenuItem(assistants[index], [assistant, new_start, new_skills, focus_draft, index] {
          focus_draft = true;
          if (assistant.Get() == index) return;
          assistant = index;
          new_start = default_draft_start;
          new_skills = std::vector<kit::SkillRef>{};
        }).Checked(assistant.Get() == index));
      }
      return choices;
    };
    std::vector<View> selectors{
        DraftChoice(NavigationSymbol::Chat, ::app::strings::ui_assistant, UseString(assistants[assistant.Get()]),
            {}, "draft-assistant", choose_assistant, ready.Get()),
        DraftChoice(NavigationSymbol::Folder, ::app::strings::ui_workspace, workspace ? folder_name(*workspace) :
            UseString(workspace_id ? ::app::strings::ui_workspace_unavailable : ::app::strings::ui_no_workspace),
            workspace_path, "draft-workspace", [use_workspace, workspace_id, folders, folder_name] {
              std::vector<MenuEntry> choices{MenuItem(::app::strings::ui_no_workspace, [use_workspace, workspace_id] {
                if (workspace_id) use_workspace(std::nullopt);
              }).Checked(!workspace_id)};
              for (const auto& entry : folders) {
                const auto path = entry.NativeFolder();
                choices.emplace_back(MenuItem(path ? path->Path() : folder_name(entry), [use_workspace, workspace_id, id = entry.id] {
                  if (workspace_id != id) use_workspace(id);
                }).Checked(workspace_id == entry.id));
              }
              return choices;
            }, ready.Get()),
        DraftChoice(NavigationSymbol::Layers, ::app::strings::ui_group, group == nav.groups.end() ? UseString(::app::strings::ui_ungrouped) : group->name, {}, "draft-group", [draft_group, service] {
          std::vector<MenuEntry> choices{MenuItem(::app::strings::ui_ungrouped, [draft_group] { draft_group = std::nullopt; }).Checked(!draft_group.Get())};
          for (const auto& entry : service->navigation.Get().groups)
            choices.emplace_back(MenuItem(entry.name, [draft_group, id = entry.id] { draft_group = id; })
                .Checked(draft_group.Get() == entry.id));
          return choices;
        }, ready.Get()),
    };
    std::vector<View> compose;
    std::optional<View> controls;
    if (ready.Get()) {
      std::optional<View> draft_action;
      if (new_agent == agentkit_preview::GeneralAgent) {
        kit::SkillCatalogQuery query{new_agent, ProjectFor(workspace_id), workspace_id};
        draft_action = agentkit_preview::SkillPicker(service->runtime, std::move(query), new_skills);
      } else if (new_agent == agentkit_preview::PptAgent) {
        const auto project = ProjectFor(workspace_id);
        if (const auto report = new_report.Get(); report && report->ref.project == project) {
          auto report_style = UseEnvironment<ButtonStyle>();
          report_style.background = Color::Transparent();
          report_style.label_style = {Font::System(12.0F), colors.on_surface_variant};
          report_style.padding = EdgeInsets::Symmetric(7.0F, 4.0F);
          report_style.minimum_height = 32.0F;
          compose.push_back(Row{Text(report->name, TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()),
              ProvideEnvironment(report_style, Button(::app::strings::ui_remove_report).OnClick([new_report] { new_report = std::nullopt; }))}
              .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center), Padding(EdgeInsets::Symmetric(8.0F, 0.0F)),
                    Background(colors.surface_container_high), CornerRadius(6.0F)));
        } else {
          if (auto control = BrowserDraftReport(service, project, workspace, native_workspace, new_report)) {
            compose.push_back(std::move(*control));
          }
          draft_action = agentkit_preview::ImportReportControl(service->runtime, project,
              [new_report](agentkit_preview::ImportedPresentationReport report) { new_report = std::move(report); }).Key(project.value);
        }
      }
      if (!new_sources->attachments.empty()) compose.push_back(kit::AgentChatAttachmentChips(new_sources));
      const auto caps = service->run_settings->Capabilities(new_start->model, new_agent);
      if (auto picker = PreviewAttachmentAction(service, new_agent, new_start, new_sources, caps)) {
        if (draft_action) draft_action = Row{std::move(*draft_action), std::move(*picker)}.With(Spacing(8.0F));
        else draft_action = std::move(*picker);
      }
      controls = agentkit_preview::RunSettingsControls(service->run_settings, new_start, new_agent, workspace_id,
          native_workspace, {}, {}, std::move(draft_action), AddModelAction(service));
    }
    const bool can_send = ready.Get() && !creating.Get() && (!draft->text.empty() || !new_sources->attachments.empty()) &&
        !draft->composition && (!code || (service->code_available.Get() && native_workspace));
    const auto placeholder = code ? ::app::strings::ui_describe_the_change : assistant.Get() == 1 ?
        ::app::strings::ui_describe_your_presentation : assistant.Get() == 3 ?
        ::app::strings::ui_describe_the_screen_or_interaction : ::app::strings::ui_send_a_message;
    std::vector<View> context{
        Flow(std::move(selectors)).With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center)),
    };
    std::vector<View> start_page{
        Text(::app::strings::ui_new_conversation, TextRole::Title)
            .Style({Font::System(compact ? 26.0F : 31.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
            .Key("new-conversation-heading"),
        Column(std::move(context)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key("draft-choices"),
    };
    if (listing) {
      const auto recent = std::ranges::find_if(listing->sessions, [workspace_id](const auto& item) {
        return item.workspace == workspace_id && item.lifecycle == kit::SessionLifecycle::Open;
      });
      if (recent != listing->sessions.end()) {
        const auto style = agentkit_preview::PreviewSecondaryButton(UseEnvironment<ButtonStyle>(), colors);
        start_page.push_back(Row{
            NavigationIcon(NavigationSymbol::Chat, colors.on_primary_container),
            Column{Text(::app::strings::ui_continue_last_conversation, TextRole::Label).With(Foreground(colors.on_surface_variant)),
                Text(recent->title).LineLimit(1).Overflow(TextOverflow::Ellipsis)}.With(Grow(), Spacing(4.0F)),
            ProvideEnvironment(style, Button(::app::strings::ui_continue).OnClick([open_session, id = recent->id] { open_session(id); }))
                .Key("resume-conversation"),
        }.With(Padding(11.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center),
            CornerRadius(12.0F), Background(colors.surface_container_low), Border{colors.outline}));
      }
    }
    start_page.push_back(Text(::app::strings::ui_start_with_a_task, TextRole::Label).With(Foreground(colors.on_surface_variant)));
    std::vector<View> starters;
    const auto prefill = [draft, focus_draft](std::string_view text) {
      if (draft->composition) return;
      auto message = draft->text;
      if (!message.empty()) message += "\n\n";
      message += text;
      draft = TextEditingValue::FromText(std::move(message));
      focus_draft = true;
    };
    const auto add_starter = [&](const char* key, StringResource label, NavigationSymbol icon, StringResource instruction) {
      auto prompt = UseString(instruction);
      starters.push_back(ProvideEnvironment(DividerStyle{colors.outline}, Divider()));
      starters.push_back(NavigationAction(label, icon, [prefill, prompt = std::move(prompt)] { prefill(prompt); }).Key(key));
    };
    add_starter("Review acceptance gaps", ::app::strings::ui_review_acceptance_gaps, NavigationSymbol::Inbox,
        ::app::strings::ui_compare_the_design_and_acceptance_criteria_list);
    add_starter("Check Markdown references", ::app::strings::ui_check_markdown_references, NavigationSymbol::Chat,
        ::app::strings::ui_check_whether_markdown_references_preserve_the_original);
    add_starter("Walk through the workspace", ::app::strings::ui_walk_through_the_workspace, NavigationSymbol::Design,
        ::app::strings::ui_walk_through_the_new_conversation_chat_and);
    start_page.push_back(Column(std::move(starters)).With(CrossAlign(CrossAxisAlignment::Stretch)).Key("draft-task-starters"));
    std::vector<View> lane{
        ScrollView(Column(std::move(start_page)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)))
            .With(Grow(), ScrollBar()),
    };
    if (!status.empty()) lane.push_back(Text(status, TextRole::Label)
        .With(Foreground(colors.on_surface_variant)).Key("new-draft-feedback"));
    if (creating.Get()) lane.push_back(Text(::app::strings::ui_sending_message, TextRole::Label)
        .With(Foreground(colors.on_surface_variant)).Key("chat-sending-message"));
    lane.push_back(agentkit_preview::PreviewDraftComposer(draft, draft_input, draft_focused, placeholder,
        "new-conversation-composer", std::move(compose), std::move(controls), !creating.Get(), can_send, create).Key("new-draft-composer"));
    main = Column(std::move(lane)).With(Grow(), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch), Padding(EdgeInsets{
        .top = compact ? 20.0F : 28.0F, .right = page_gutter, .bottom = compact ? 14.0F : 25.0F, .left = page_gutter}))
        .Key(current_draft.view_key);
  }
  if (!service->preference_load_error.Get().empty()) {
    main = Column{std::move(main).With(Grow()), Text(service->preference_load_error.Get(), TextRole::Label)}
        .With(Grow(), Spacing(5.0F));
  }
  if (const auto failure = service->navigation_failure.Get()) {
    std::vector<View> controls{Text(failure->error.message, TextRole::Label).With(Grow()),
        Button(failure->loading ? ::app::strings::ui_retry_loading_groups : ::app::strings::ui_retry_saving_groups).OnClick([service] { SaveNavigation(service); })};
    if (!failure->loading) {
      controls.push_back(Button(::app::strings::ui_discard_unsaved_change).OnClick([service] {
        if (service->saving_navigation || service->navigation_edits.empty()) return;
        service->navigation_edits.pop_front();
        service->reload_navigation = true;
        SaveNavigation(service);
      }));
    }
    main = Column{std::move(main).With(Grow()), Row(std::move(controls)).With(Spacing(8.0F), Padding(10.0F))}.With(Grow());
  }
  std::vector<View> content;
  std::vector<View> header;
  const auto navigation_button = [&] {
    const auto open_navigation = [navigation_open] { navigation_open = true; };
    return Row{NavigationIcon(NavigationSymbol::Panel, colors.on_surface_variant)}.OnClick(open_navigation)
        .On<ViewEvents::KeyDown>([open_navigation](const KeyEvent& event) {
          if (event.key != Key::Enter && event.key != Key::Space) return false;
          open_navigation();
          return true;
        }).With(Frame{.width = 32.0F, .height = 34.0F}, MainAlign(MainAxisAlignment::Center),
                CrossAlign(CrossAxisAlignment::Center), Focusable{}, Semantics{.role = SemanticRole::Button, .label = ::app::strings::ui_conversations},
                Tooltip(::app::strings::ui_open_conversation_list)).Key("open-navigation");
  };
  if (compact && !custom_preview_chrome) header.push_back(navigation_button());
  if (!header.empty()) {
    content.push_back(Column{
        Row(std::move(header)).With(Frame{.height = 42.0F},
            Padding(EdgeInsets::Symmetric(compact ? 10.0F : 20.0F, 0.0F)), Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center)),
        Canvas([line = colors.outline](PaintContext& context, Size size) { context.DrawRect({0, 0, size.width, size.height}, line); })
            .With(Frame{.height = 1.0F}),
    }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface_container_low)).Key("conversation-navigation"));
  }
  if (ready.Get() && !service->model_configuration_error.empty()) {
    content.push_back(Text(service->model_configuration_error, TextRole::Label)
        .With(Padding(EdgeInsets::Symmetric(12.0F, 6.0F))));
  }
  content.push_back(Column{std::move(main).With(Grow())}
      .With(Grow(), CrossAlign(CrossAxisAlignment::Stretch)).Key("preview-content"));
  auto drawer_style = UseEnvironment<DrawerStyle>();
  drawer_style.background = colors.surface_container_low;
  drawer_style.preferred_width = sidebar_width;
  drawer_style.minimum_width = sidebar_width;
  drawer_style.minimum_content_width = 370.0F;
  // Draft navigation needs the drawer to release its focus trap in the same frame.
  if (compact && focus_draft.Get() && drawer_style.motion) drawer_style.motion->close = SnapSpec{};
  View settings = settings_available && showing_settings.Get()
      ? agentkit_preview::PreviewSettingsPage(service->settings, service->applied_settings, service->run_settings,
          *service->settings_file, AddModelAction(service), [showing_settings] { showing_settings = false; })
      : View(Spacer());
  auto pages = IndexedPages({Column(std::move(content)).With(CrossAlign(CrossAxisAlignment::Stretch)),
      std::move(settings)}, showing_settings.Get() ? 1U : 0U).Key("workbench-main");
  auto sizing = sidebar_split.Get().value_or(std::array{
      PaneSizing{Frame{.width = sidebar_width, .min_width = 160.0F, .max_width = 340.0F}, Grow(0.0F)},
      PaneSizing{Frame{}, Grow()},
  });
  sizing[1].first.min_width = compact ? std::nullopt : std::optional{medium ? 360.0F : 600.0F};
  View columns = SplitLayout({
      compact ? View{} : View(std::move(sidebar).With(sizing[0].first, sizing[0].second)),
      std::move(pages).With(sizing[1].first, sizing[1].second),
  }).OnChanged([sidebar_split, sizing](const std::vector<SplitChange>& changes) mutable {
    for (const auto& change : changes) sizing[change.index] = {change.frame, change.grow};
    sidebar_split = sizing;
  });
  auto workbench = ProvideEnvironment(drawer_style, (compact
      ? DrawerLayout(std::move(columns), StartDrawer(std::move(sidebar)).Open(navigation_open.Get())
          .OnOpenChanged([navigation_open](bool open) { navigation_open = open; }))
      : DrawerLayout(std::move(columns))).With(Grow(), Background(colors.surface)));
  if (!custom_preview_chrome) return workbench;
  std::vector<View> chrome;
  if (compact) chrome.push_back(navigation_button());
  chrome.push_back(PreviewWordmark(colors));
  chrome.push_back(Spacer().With(Grow()));
  return Column{
      WindowTitleBar(std::move(chrome)).With(Padding(EdgeInsets::Symmetric(14.0F, 0.0F)), Spacing(7.0F),
          Frame{.min_height = 46.0F}, Background(colors.surface_container_low)).Key("preview-title-bar"),
      ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
      std::move(workbench),
  }.With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface));
}

View App() {
  const auto service = UseService<PreviewService>();
  const auto brightness = agentkit_preview::PreviewBrightness(service->settings->appearance, UseSystemThemeBrightness());
  return MaterialTheme(agentkit_preview::PreviewTheme(brightness), agentkit_preview::PreviewControlsTheme(PreviewShell()));
}

ApplicationHook MakePreviewInstaller(std::optional<File> database, bool read_model_configuration) {
  return [database = std::move(database), read_model_configuration](ApplicationContext& context) {
    if (auto links = agentkit_preview::MakePreviewLinkHandler()) context.Provide(std::move(links));
    context.Provide(agentkit_preview::MakePreviewImageLoader());
    InstallPreview(context, database, read_model_configuration);
  };
}

#if defined(AGENTKIT_PREVIEW_TEST)
using ConfiguredPreviewDriver = std::function<Task<void>(
    kit::AgentResult<std::shared_ptr<kit::AgentRuntime>>, std::optional<kit::WorkspaceId>, kit::ProjectId)>;

ApplicationHook MakeRunSettingsTestInstaller(File database, File workspace, std::vector<kit::ModelRegistration> models,
                                            ConfiguredPreviewDriver drive,
                                            bool review_user_comparisons, PreviewStoreAdapter adapt_store) {
  return [database = std::move(database), workspace = std::move(workspace), models = std::move(models),
          drive = std::move(drive), review_user_comparisons,
          adapt_store = std::move(adapt_store)](ApplicationContext& context) {
    auto service = InstallPreview(context, database, false, workspace, {}, {}, adapt_store, models, review_user_comparisons);
    UseApplicationTaskScope().Launch([service, drive]() -> Task<void> {
      auto started = co_await WaitForRuntime(service);
      co_await drive(std::move(started), service->startup_workspace, ProjectFor(service->startup_workspace));
    });
  };
}

ApplicationHook MakeConfiguredPreviewTestInstaller(File database, File workspace, File ctest, ConfiguredPreviewDriver drive) {
  return [database = std::move(database), workspace = std::move(workspace), ctest = std::move(ctest), drive = std::move(drive)](
             ApplicationContext& context) {
    auto service = InstallPreview(context, database, true, workspace, {}, ctest);
    if (drive) UseApplicationTaskScope().Launch([service, drive]() -> Task<void> {
      auto started = co_await WaitForRuntime(service);
      co_await drive(std::move(started), service->startup_workspace, ProjectFor(service->startup_workspace));
    });
  };
}

ApplicationHook MakeCodeTestInstaller(File database, File workspace, kit::ModelRegistration model, File ctest,
    PreviewStoreAdapter adapt_store, ConfiguredPreviewDriver drive) {
  return [database = std::move(database), workspace = std::move(workspace), model = std::move(model),
          ctest = std::move(ctest), adapt_store = std::move(adapt_store), drive = std::move(drive)](
             ApplicationContext& context) {
    auto service = InstallPreview(context, database, false, workspace, model, ctest, adapt_store);
    if (drive) UseApplicationTaskScope().Launch([service, drive]() -> Task<void> {
      auto started = co_await WaitForRuntime(service);
      co_await drive(std::move(started), service->startup_workspace, ProjectFor(service->startup_workspace));
    });
  };
}
#endif

#if !defined(AGENTKIT_PREVIEW_TEST)
const Application application{
    App,
    {.window = {.title = "GanWork", .initial_size = {1080.0F, 760.0F},
#ifdef _WIN32
        .minimum_size = Size{460.0F, 560.0F}, .chrome_mode = WindowChromeMode::Custom, .title_bar_height = 46.0F
#endif
     },
     .show_debug_overlay = false,
     .application_hooks = {MakePreviewInstaller({}, true)}},
};
#endif
