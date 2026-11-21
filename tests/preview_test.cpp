#include <huxerui/agentkit.h>
#include <app_resources.h>
#include <huxerui_agentkit_resources.h>
#include <huxerui/archive.h>
#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>
#include <huxerui/testing/ui_test.h>
#include <nlohmann/json.hpp>

#include "sqlite_session_store.h"
#include "test_resources.h"

#include "../examples/preview/src/code_workspace.h"
#include "../examples/preview/src/code_tests.h"
#include "../examples/preview/src/code_tools.h"
#include "../examples/preview/src/code_workflow.h"
#include "../examples/preview/src/design.h"
#include "../examples/preview/src/git_process.h"
#include "../examples/preview/src/general_workflow.h"
#include "../examples/preview/src/link_navigation.h"
#include "../examples/preview/src/markdown_images.h"
#include "../examples/preview/src/preview_json.h"
#include "../examples/preview/src/presentation.h"
#include "../examples/preview/src/presentation_tools.h"
#include "../examples/preview/src/presentation_view.h"
#include "../examples/preview/src/presentation_workflow.h"
#include "../examples/preview/src/release_workflow.h"
#include "../examples/preview/src/report_chart.h"
#include "../examples/preview/src/release_skill.h"
#include "../examples/preview/src/run_settings.h"
#include "../examples/preview/src/preview_store.h"
#include "../examples/preview/src/workspace_evidence.h"

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <set>
#include <thread>

huxerui::View App();
huxerui::View DetachedArtifactReview(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::AgentId agent,
    huxerui::agentkit::AgentChatReviewRequest request, agentkit_preview::PresentationReviewPages initial_pages,
    std::optional<huxerui::State<huxerui::agentkit::AgentChatDraftSources>> draft_sources);
huxerui::WindowHandle OpenPreviewArtifactReview(const huxerui::ApplicationHandle& application,
    std::shared_ptr<huxerui::agentkit::Conversation> conversation, std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    huxerui::agentkit::AgentId agent, huxerui::agentkit::AgentChatReviewRequest request,
    agentkit_preview::PresentationReviewPages initial_pages,
    std::optional<huxerui::State<huxerui::agentkit::AgentChatDraftSources>> draft_sources = {});
huxerui::ApplicationHook MakePreviewInstaller(std::optional<huxerui::File> database, bool read_model_configuration);
using ConfiguredPreviewDriver = std::function<huxerui::Task<void>(
    huxerui::agentkit::AgentResult<std::shared_ptr<huxerui::agentkit::AgentRuntime>>,
    std::optional<huxerui::agentkit::WorkspaceId>, huxerui::agentkit::ProjectId)>;
huxerui::ApplicationHook MakeConfiguredPreviewTestInstaller(
    huxerui::File database, huxerui::File workspace, huxerui::File ctest, ConfiguredPreviewDriver drive);
using PreviewStoreAdapter = std::function<std::shared_ptr<huxerui::agentkit::SessionStore>(std::shared_ptr<huxerui::agentkit::SessionStore>)>;
huxerui::ApplicationHook MakeRunSettingsTestInstaller(huxerui::File database, huxerui::File workspace,
    std::vector<huxerui::agentkit::ModelRegistration> models,
    ConfiguredPreviewDriver drive, bool review_user_comparisons = false,
    PreviewStoreAdapter adapt_store = {});
huxerui::ApplicationHook MakeCodeTestInstaller(
    huxerui::File database, huxerui::File workspace, huxerui::agentkit::ModelRegistration model, huxerui::File ctest,
    PreviewStoreAdapter adapt_store = {}, ConfiguredPreviewDriver drive = {});

namespace {

constexpr auto edited_during_adoption = "User edited the second file during adoption";
struct ConcurrentCodeEdit {
  huxerui::agentkit::ToolCallId call;
  huxerui::File file;
  std::string text;
};
std::atomic<std::shared_ptr<const ConcurrentCodeEdit>> concurrent_code_edit;

} // namespace

namespace agentkit_preview {

void AfterCodeReplacementForTest(const huxerui::agentkit::ToolCallId& call, std::size_t index) {
  auto edit = concurrent_code_edit.load();
  if (index == 0 && edit && edit->call == call && concurrent_code_edit.compare_exchange_strong(edit, {}) &&
      !edit->file.WriteString(edit->text)) {
    throw std::runtime_error("Could not simulate a concurrent edit in the patch fixture");
  }
}

} // namespace agentkit_preview

namespace {
using namespace huxerui;
using namespace huxerui::testing;
using namespace std::chrono_literals;
namespace kit = huxerui::agentkit;

using PreviewTestResources = agentkit_test::PackagedResources;

class UiTest final : public huxerui::testing::UiTest {
 public:
  explicit UiTest(const Application& application, UiTestOptions options = {})
      : huxerui::testing::UiTest(application, WithResources(std::move(options))) {}

 private:
  static UiTestOptions WithResources(UiTestOptions options) {
    if (!options.resource_provider) options.resource_provider = std::make_shared<PreviewTestResources>();
    return options;
  }
};

void Wait(UiTest& preview, const std::function<bool()>& established, const char* message, std::chrono::seconds timeout = 8s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!established() && std::chrono::steady_clock::now() < deadline) {
    preview.Pump(12ms);
    std::this_thread::sleep_for(2ms);
  }
  if (!established()) throw std::runtime_error(message);
}

std::optional<UiNodeQuery> MessageText(UiTest& preview, const std::string& text) {
  auto message = preview.Find(UiSelector::AllOf(
      UiSelector::Type<Text>(), UiSelector::Key("chat-selectable-text"), UiSelector::Text(text)));
  if (message.Count() == 1 && message.One().in_viewport) return message;
  return {};
}

bool ShowsText(UiTest& preview, const std::string& text) {
  return preview.Find(UiSelector::Text(text)).Exists();
}

void CreateAndApprove(const Application& application) {
  UiTest preview(application, {.viewport = {1080.0F, 760.0F}});
  auto draft = preview.Find(UiSelector::Type<TextField>());
  auto send = preview.Find(UiSelector::Key("chat-send/Send"));
  auto approval = preview.Find(UiSelector::Text("Confirm preview action"));

  Wait(preview, [&] { return draft.Exists(); }, "conversation composer did not mount");
  draft.Tap();
  draft.ReplaceText("approval");
  Wait(preview, [&] { return send.One().enabled; }, "SQLite-backed preview did not start");
  send.Tap();
  Wait(preview, [&] { return approval.Exists(); }, "tool approval did not become visible");
  preview.Find(UiSelector::AllOf(UiSelector::Type<Button>(), UiSelector::Text("Approve"))).Tap();
  Wait(preview, [&] { return ShowsText(preview, "Preview action completed."); }, "tool did not return to its original conversation");
  if (approval.Exists()) throw std::runtime_error("Approval is still visible after completion");
  Wait(preview, [&] {
    return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists();
  }, "approved run did not settle before restart");
}

void ReopenConversation(const Application& application) {
  UiTest preview(application, {.viewport = {1080.0F, 760.0F}});
  auto conversation = preview.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text("approval"));
  Wait(preview, [&] { return conversation.Exists(); }, "directory did not restore the conversation");
  conversation.Tap();
  Wait(preview, [&] { return ShowsText(preview, "Preview action completed."); }, "restored conversation did not contain the committed result");
  if (preview.Find(UiSelector::Text("Confirm preview action")).Exists()) {
    throw std::runtime_error("Restart repeated a consumed approval");
  }
}

UiSelector ButtonText(const char* label) {
  if (label == std::string_view("Send") || label == std::string_view("Queue message") || label == std::string_view("Send as new task"))
    return UiSelector::Key(std::string("chat-send/") + label);
  if (label == std::string_view("New group")) return UiSelector::Key("new-group");
  if (label == std::string_view("Goal")) return UiSelector::Key("conversation-goal");
  if (label == std::string_view("Compare alternatives")) return UiSelector::Key("comparison-entry");
  if (label == std::string_view("Back to comparison")) return UiSelector::Key("comparison-return");
  if (label == std::string_view("Jump to latest")) return UiSelector::Key("jump-to-latest");
  return UiSelector::AllOf(UiSelector::Type<Button>(), UiSelector::Text(label));
}

UiSelector ReviewCard(std::string_view label) {
  return UiSelector::Key("artifact-review-action/" + std::string(label));
}

UiNodeQuery PendingConversation(UiTest& ui, std::string title) {
  return ui.Find(UiSelector::Key("pending-session-title/" + title));
}

constexpr auto ReportChartFilename = "\xe5\x9b\xa2\xe9\x98\x9f\xe6\x9e\x84\xe6\x88\x90.csv";

void VerifyReportChart(const File& root) {
  const auto database = root.Child("sessions.sqlite3");
  const auto output = root.Child("exports").Child(ReportChartFilename);
  Application application{App, {.application_hooks = {MakePreviewInstaller(database, false)}}};
  const auto approval = UiSelector::Text("Export report chart");
  const UiTestOptions options{.viewport = {1080.0F, 920.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()};
  {
    UiTest preview(application, options);
    auto composer = preview.Find(UiSelector::Type<TextField>());
    Wait(preview, [&] { return composer.Exists(); }, "Chart composer did not mount");
    composer.Tap();
    composer.ReplaceText("Pie chart:\n"
        R"({"title":"Team allocation","categories":[{"label":"Operations","value":6},{"label":"Support","value":2},{"label":"Product","value":2}]})");
    Wait(preview, [&] { return preview.Find(ButtonText("Send")).One().enabled; }, "Chart request cannot be submitted");
    preview.Find(ButtonText("Send")).Tap();
    auto chart = preview.Find(UiSelector::Key("chart-pie"));
    Wait(preview, [&] {
      return chart.Exists() && chart.One().in_viewport && ShowsText(preview, "Team allocation") && ShowsText(preview, "6  60%");
    }, "Model chart did not render with its category proportions");
    preview.Find(UiSelector::Key("chart-category-1")).Tap();
    Wait(preview, [&] { return ShowsText(preview, "6  75%"); }, "Chart filter did not update its proportions");
    auto name = preview.Find(UiSelector::Key("chart-file-name"));
    name.Tap();
    name.ReplaceText("../Chart.csv");
    preview.Find(ButtonText("Export CSV")).Tap();
    Wait(preview, [&] { return ShowsText(preview, "Enter a .csv file name in the export folder"); },
        "Chart export accepted a file name outside the export folder");
    if (root.Child("Chart.csv").Exists() || preview.Find(approval).Exists())
      throw std::runtime_error("An invalid chart submission staged a write");
    name.Tap();
    name.ReplaceText(ReportChartFilename);
    preview.Find(ButtonText("Export CSV")).Tap();
    Wait(preview, [&] { return preview.Find(approval).Exists() && ShowsText(preview, "Operations  6\nProduct  2"); },
        "Selected chart categories were not staged for approval");
    if (output.Exists()) throw std::runtime_error("Chart was written before approval");
  }
  {
    UiTest preview(application, options);
    auto conversation = PendingConversation(preview, "Pie chart:");
    Wait(preview, [&] { return conversation.Exists(); }, "Chart conversation did not restore");
    conversation.Tap();
    Wait(preview, [&] { return preview.Find(approval).Exists(); }, "Chart export approval was lost on restart");
    if (output.Exists() || preview.Find(ButtonText("Export CSV")).Exists())
      throw std::runtime_error("Restored chart reopened the submitted form or wrote before approval");
    preview.Find(ButtonText("Approve")).Tap();
    Wait(preview, [&] {
      return output.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists() && ShowsText(preview, "Saved " + output.Path());
    }, "Approved chart export did not finish");
    const auto bytes = output.ReadBytes();
    if (!bytes.Succeeded()) throw std::runtime_error(bytes.Error().message);
    const std::string_view csv(reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size());
    if (csv != "Category,Value\r\n\"Operations\",6\r\n\"Product\",2\r\n")
      throw std::runtime_error("CSV did not match the selection that was approved");
  }
}

void ClickPreviewMouse(UiTest& preview, Rect bounds) {
  const Point position{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
  preview.SendPointer({.type = PointerEventType::Move, .pointer_id = 1, .position = position,
      .device_kind = PointerDeviceKind::Mouse});
  preview.Pump(16ms);
  preview.TapAt(position, {.device_kind = PointerDeviceKind::Mouse});
}

void TapControl(UiTest& preview, UiSelector selector, const char* label, const char* scroll_key = "ppt-form-scroll",
                float scroll_step = 95.0F) {
  auto control = preview.Find(selector);
  Wait(preview, [&] { return control.Exists() && control.One().enabled; }, label);
  if (!control.One().in_viewport) {
    const auto scroll = preview.Find(UiSelector::Key(scroll_key));
    if (!scroll.Exists()) throw std::runtime_error(std::string(label) + " is outside the viewport");
    control = scroll.ScrollUntil(selector, {.step = {0.0F, scroll_step}});
  }
  control.Tap();
}

void TapFormButton(UiTest& preview, const char* label) {
  TapControl(preview, ButtonText(label), label);
}

void TapWorkspaceAction(UiTest& preview, const char* label) {
  auto trigger = preview.FindSemantics(UiSemanticSelector::AllOf(
      UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Workspace actions")));
  Wait(preview, [&] { return trigger.Count() == 1 && trigger.One().enabled && !trigger.One().offscreen; },
      "Workspace actions were not available");
  trigger.PerformSemanticAction({SemanticActionKind::Activate, {}});
  const auto menu_items = UiSemanticSelector::Role(SemanticRole::MenuItem);
  auto item = preview.FindSemantics(UiSemanticSelector::AllOf(menu_items, UiSemanticSelector::Label(label)));
  Wait(preview, [&] { return item.Count() == 1 && item.One().enabled && !item.One().offscreen; }, label);
  item.PerformSemanticAction({SemanticActionKind::Activate, {}});
  Wait(preview, [&] { return !preview.FindSemantics(menu_items).Exists(); }, "The workspace actions menu did not close");
}

UiNodeQuery ConversationRow(UiTest& preview, const std::string& title) {
  return preview.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text(title));
}

void FocusPreviewEditor(UiTest& preview, const UiNodeQuery& editor, const char* failure) {
  Wait(preview, [&] {
    if (!editor.Exists() || !editor.One().in_viewport) return false;
    if (!editor.One().focused || !preview.ActiveTextInput()) ClickPreviewMouse(preview, editor.One().visible_bounds);
    return editor.One().focused && preview.ActiveTextInput().has_value();
  }, failure);
}

void SelectHistoryText(UiTest& preview, const UiNodeQuery& message, TextRange range) {
  Wait(preview, [&] { return message.Exists() && message.One().in_viewport; }, "Historical text was not visible");
  const auto node = message.One();
  const auto bounds = node.visible_bounds;
  preview.TapAt({bounds.x + 1.0F, bounds.y + std::min(bounds.height / 2, 8.0F)}, {.device_kind = PointerDeviceKind::Mouse});
  if (node.value || preview.ActiveTextInput()) throw std::runtime_error("Historical text opened an edit session");
  preview.PressKey(Key::Home);
  const AttributedText text(*node.text);
  const auto scalar_count = [&](TextRange span) {
    return std::ranges::count_if(text.TextInRange(span), [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
  };
  for (auto count = scalar_count({0, range.start}); count > 0; --count) preview.PressKey(Key::ArrowRight);
  for (auto count = scalar_count(range); count > 0; --count) preview.PressKey(Key::ArrowRight, {.shift = true});
}

void CheckCTestDisclosure(UiTest& preview, std::string_view message) {
  auto output = preview.Find(UiSelector::Key("ctest-output"));
  if (output.Exists()) throw std::runtime_error("CTest output expanded before it was requested");
  const auto label = ShowsText(preview, "Run workspace tests · Completed") ?
      "Run workspace tests · Completed" : "Run workspace tests · Failed";
  const auto step = UiSelector::Text(label);
  const auto toggle = [&] {
    TapControl(preview, step, "CTest activity was not visible", "agent-chat-history", -96.0F);
  };
  toggle();
  Wait(preview, [&] {
    return output.Exists() && std::ranges::any_of(output.Find(UiSelector::Type<Text>()).All(), [&](const auto& entry) {
      return entry.text && entry.text->find(message) != std::string::npos && entry.text->find('\x1b') == std::string::npos &&
          entry.text->find("[HANDLER_OUTPUT]") == std::string::npos && entry.text->find("[ERROR_MESSAGE]") == std::string::npos;
    });
  }, "CTest output did not display the readable diagnostic");
  toggle();
  Wait(preview, [&] { return !output.Exists(); }, "CTest output did not collapse");
}

void TapReviewCard(UiTest& preview, const char* label) {
  TapControl(preview, ReviewCard(label), label);
}

void ChooseDraftAssistant(UiTest& preview, const char* label) {
  const auto selector = preview.Find(UiSelector::Key("draft-assistant"));
  Wait(preview, [&] { return selector.Exists() && selector.One().enabled && selector.One().in_viewport; },
      "The draft assistant selector did not mount");
  const auto chosen = selector.Find(UiSelector::Text(label));
  if (chosen.Exists()) return;
  ClickPreviewMouse(preview, selector.One().visible_bounds);
  const auto item = preview.FindSemantics(UiSemanticSelector::AllOf(
      UiSemanticSelector::Role(SemanticRole::MenuItem), UiSemanticSelector::Label(label)));
  Wait(preview, [&] { return item.Exists() && !item.One().offscreen && item.One().enabled; },
      "The draft assistant option did not open");
  ClickPreviewMouse(preview, item.One().bounds);
  Wait(preview, [&] { return chosen.Exists() && !item.Exists(); }, "The draft assistant did not change");
  preview.Pump(250ms);
}

std::string ReviewedDiff(UiTest& preview) {
  const auto reviews = preview.FindSemantics(UiSemanticSelector::Identifier("code-diff")).All();
  if (reviews.empty()) return {};
  const auto fields = preview.FindSemantics(UiSemanticSelector::Role(SemanticRole::TextField)).All();
  const auto patch = std::ranges::find_if(fields, [&](const SemanticNode& field) { return field.parent == reviews.front().id; });
  return patch == fields.end() ? std::string{} : patch->value;
}

std::size_t SelectDiffExcerpt(UiTest& preview, const UiNodeQuery& diff, std::string_view excerpt) {
  const auto text = ReviewedDiff(preview);
  const auto offset = text.find(excerpt);
  if (offset == text.npos) throw std::runtime_error("The review opened the wrong file preview");
  diff.Tap();
  preview.PressKey(Key::A, {.control = true});
  preview.PressKey(Key::ArrowLeft);
  for (std::size_t i = 0; i < offset; ++i) preview.PressKey(Key::ArrowRight);
  for (std::size_t i = 0; i < excerpt.size(); ++i) preview.PressKey(Key::ArrowRight, {.shift = true});
  return offset;
}

void AskAboutSelection(UiTest& preview, const std::string& excerpt) {
  const auto side = preview.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
      UiSemanticSelector::Label("Side chat"), UiSemanticSelector::Enabled(true)));
  Wait(preview, [&] { return side.Count() == 1 && !side.One().offscreen; },
      "The selected artifact did not offer side chat");
  side.PerformSemanticAction({SemanticActionKind::Activate, {}});
  auto question = preview.Find(UiSelector::Key("side-chat-question"));
  auto failure = preview.Find(UiSelector::Key("source-selection-error"));
  const auto waiting = "The artifact side chat did not open for " + excerpt;
  Wait(preview, [&] { return question.Exists() || failure.Exists(); }, waiting.c_str());
  if (failure.Exists()) throw std::runtime_error(failure.One().text.value_or("The artifact selection was rejected"));
  FocusPreviewEditor(preview, question, "The artifact side chat question did not accept focus");
  question.ReplaceText("Explain this selected artifact fragment");
  preview.Find(UiSelector::Key("side-chat-pane")).Find(UiSelector::Key("chat-send/Send")).Tap();
  const auto reply = "Selected text: " + excerpt;
  Wait(preview, [&] { return MessageText(preview, reply).has_value(); }, "The model received a different artifact fragment");
  auto editor = MessageText(preview, reply).value();
  SelectHistoryText(preview, editor, {15, TextEditingValue::FromText(reply).selection.active});
  auto bring_back = preview.Find(UiSelector::Key("side-chat-return-selection"));
  Wait(preview, [&] { return bring_back.Exists(); }, "The artifact reply could not be brought back");
  bring_back.Tap();
  const auto composer = preview.Find(UiSelector::Key("composer"));
  Wait(preview, [&] { return !preview.Find(UiSelector::Key("side-chat-pane")).Exists() &&
      composer.Exists() && composer.One().in_viewport && composer.One().focused &&
      preview.Find(ButtonText("Remove conclusion")).Exists(); }, "The side chat did not return to the focused conversation draft");
}

void CheckReturnedConclusion(UiTest& preview, const std::string& text) {
  auto chip = preview.Find(ButtonText("Remove conclusion"));
  Wait(preview, [&] { return chip.Count() == 1 && chip.One().in_viewport; }, "The artifact conclusion did not reach the main draft");
  if (!preview.Find(UiSelector::Text(text)).Exists()) throw std::runtime_error("The draft received a different conclusion");
  chip.Tap();
}

void DraftPresentationOutline(UiTest& preview) {
  ChooseDraftAssistant(preview, "PPT");
  auto prompt = preview.Find(UiSelector::Key("new-conversation-composer"));
  FocusPreviewEditor(preview, prompt, "The presentation draft did not receive focus");
  prompt.ReplaceText("Product rollout");
  Wait(preview, [&] { return preview.Find(ButtonText("Send")).One().enabled; }, "PPT conversation is not ready");
  preview.Find(ButtonText("Send")).Tap();
  auto audience = preview.Find(UiSelector::Key("ppt-audience"));
  Wait(preview, [&] { return audience.Exists(); }, "PPT brief was not requested");
  audience.Tap();
  audience.ReplaceText("Operations leads");
  TapFormButton(preview, "Draft outline");

  auto title = preview.Find(UiSelector::Key("slide-title-0"));
  Wait(preview, [&] { return title.Exists(); }, "Editable model outline was not shown");
}

void PreparePresentation(const Application& application, const File& output) {
  UiTest preview(application, {.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  DraftPresentationOutline(preview);
  auto title = preview.Find(UiSelector::Key("slide-title-0"));
  const auto outline_index = preview.Find(UiSelector::Key("outline-slide-index"));
  const auto outline_editor = preview.Find(UiSelector::Key("outline-slide-editor"));
  Wait(preview, [&] { return outline_index.Exists() && outline_editor.Exists() &&
      outline_index.One().bounds.x + outline_index.One().bounds.width <= outline_editor.One().bounds.x; },
      "The outline index did not sit next to the editor");
  title.Tap();
  title.ReplaceText("Approved rollout");
  preview.SetWindowMetrics({.viewport = {430.0F, 760.0F}});
  Wait(preview, [&] { return outline_index.One().bounds.y + outline_index.One().bounds.height <= outline_editor.One().bounds.y &&
      title.One().value == "Approved rollout"; }, "The outline did not stack its index and preserve its draft at narrow widths");
  preview.SetWindowMetrics({.viewport = {1080.0F, 880.0F}});
  auto next_outline = outline_index.Find(UiSelector::Key("outline-slide/2"));
  Wait(preview, [&] { return outline_index.One().bounds.x + outline_index.One().bounds.width <= outline_editor.One().bounds.x &&
      next_outline.Exists() && next_outline.One().in_viewport; }, "The second outline entry was not visible in the wide editor");
  next_outline.Tap({.device_kind = PointerDeviceKind::Mouse});
  auto second = preview.Find(UiSelector::Key("slide-title-1"));
  Wait(preview, [&] { return second.Exists(); }, "Second outline slide did not mount");
  second.Tap();
  second.ReplaceText("Validated operations");
  TapFormButton(preview, "Add slide");
  auto third = preview.Find(UiSelector::Key("slide-title-2"));
  Wait(preview, [&] { return third.Exists(); }, "The third slide did not mount");
  third.Tap();
  third.ReplaceText("Review limits");
  auto bullets = preview.Find(UiSelector::Key("slide-bullets-2"));
  bullets.Tap();
  bullets.ReplaceText("Brief & revise <option>");
  TapFormButton(preview, "Confirm outline");
  auto first_preview = preview.Find(UiSelector::Text("Approved rollout"));
  const auto stage = preview.Find(UiSelector::Key("presentation-slide"));
  const auto export_footer = preview.Find(UiSelector::Key("presentation-form-actions"));
  Wait(preview, [&] { return first_preview.Exists() && stage.Exists() && export_footer.Exists() &&
      stage.One().bounds.y + stage.One().bounds.height <= export_footer.One().bounds.y &&
      stage.One().visible_bounds.height >= stage.One().bounds.height - 1.0F; },
      "The first slide was clipped or overlapped by the export controls");
  const auto page_picker = preview.Find(UiSelector::Key("presentation-slide-picker"));
  page_picker.Tap();
  const auto second_preview = preview.Find(UiSelector::Text("2. Validated operations"));
  Wait(preview, [&] { return second_preview.Exists() && second_preview.One().in_viewport; }, "The preview did not offer the slide by title");
  second_preview.Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Validated operations")).Exists(); },
       "Staged presentation did not preview the edited second slide");
  TapFormButton(preview, "Review export");
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Export PowerPoint")).Exists(); }, "Missing export approval");
  if (output.Exists()) throw std::runtime_error("Preview exported a PPTX before user approval");
}

std::string ReadPart(archive::ArchiveReader& source, std::string_view path) {
  auto bytes = source.ReadEntry(path);
  if (!bytes.Succeeded()) throw std::runtime_error(bytes.Error().message);
  return {reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size()};
}

void RestartAndExport(const Application& application, const File& output, const std::filesystem::path& staging) {
  UiTest preview(application, {.viewport = {1080.0F, 880.0F}});
  auto conversation = PendingConversation(preview, "Product rollout");
  Wait(preview, [&] { return conversation.Exists(); }, "PPT conversation did not restore with a pending request");
  conversation.Tap();
  auto approval = preview.Find(UiSelector::Text("Export PowerPoint"));
  Wait(preview, [&] { return approval.Exists(); }, "PPT export approval did not survive restart");
  if (output.Exists()) throw std::runtime_error("Restart bypassed the export approval");
  preview.Find(ButtonText("Approve")).Tap();
  Wait(preview, [&] { return output.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
       "Approved PPTX export did not settle");

  auto exported = archive::ArchiveReader::Open(output);
  if (!exported.Succeeded()) throw std::runtime_error(exported.Error().message);
  if (ReadPart(exported.Value(), "ppt/slides/slide1.xml").find("Approved rollout") == std::string::npos ||
      ReadPart(exported.Value(), "ppt/slides/slide2.xml").find("Validated operations") == std::string::npos ||
      ReadPart(exported.Value(), "ppt/slides/slide3.xml").find("Brief &amp; revise &lt;option&gt;") == std::string::npos) {
    throw std::runtime_error("Exported PPTX does not match the approved outline");
  }
  auto exported_bytes = output.ReadBytes();
  if (!exported_bytes.Succeeded()) throw std::runtime_error(exported_bytes.Error().message);
  std::size_t staged_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(staging)) {
    if (entry.path().extension() != ".pptx") continue;
    ++staged_count;
    auto bytes = File(entry.path().u8string()).ReadBytes();
    if (!bytes.Succeeded() || bytes.Value() != exported_bytes.Value()) {
      throw std::runtime_error("Exported bytes do not match the staged version");
    }
  }
  if (staged_count != 1) throw std::runtime_error("Recovery regenerated the staged artifact");
}

void VerifyPresentationFailures(const File& root) {
  const auto generation = root.Child("generation");
  if (!generation.CreateDirectories() || !generation.Child("presentations").WriteString("Staging is blocked"))
    throw std::runtime_error("Could not create the blocked staging fixture");
  const UiTestOptions options{.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  {
    const Application application{App, {.application_hooks = {MakePreviewInstaller(generation.Child("sessions.sqlite3"), false)}}};
    UiTest preview(application, options);
    DraftPresentationOutline(preview);
    TapFormButton(preview, "Confirm outline");
    Wait(preview, [&] {
      return ShowsText(preview, "Could not create presentation staging directory") && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists();
    }, "PPTX generation failure did not settle in the original conversation");
    if (preview.Find(ReviewCard("Review artifact v1")).Exists() || preview.Find(ButtonText("Review export")).Exists() ||
        generation.Child("exports").Child("Presentation.pptx").Exists())
      throw std::runtime_error("Failed generation exposed an artifact or offered an export");
  }

  const auto exporting = root.Child("export");
  const auto output = exporting.Child("exports").Child("Presentation.pptx");
  const Application application{App, {.application_hooks = {MakePreviewInstaller(exporting.Child("sessions.sqlite3"), false)}}};
  PreparePresentation(application, output);
  if (!output.WriteString("Existing file")) throw std::runtime_error("Could not create the competing export destination");
  {
    UiTest preview(application, options);
    auto conversation = PendingConversation(preview, "Product rollout");
    Wait(preview, [&] { return conversation.Exists(); }, "The export approval did not recover before the failure case");
    conversation.Tap();
    Wait(preview, [&] { return preview.Find(UiSelector::Text("Export PowerPoint")).Exists(); }, "Missing recovered export plan");
    TapFormButton(preview, "Approve");
    Wait(preview, [&] {
      return ShowsText(preview, "Export target already exists") && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists();
    }, "The export conflict did not settle in the original conversation");
    const auto contents = output.ReadString();
    if (!contents.Succeeded() || contents.Value() != "Existing file")
      throw std::runtime_error("The failed export altered the existing destination");
  }
  {
    UiTest preview(application, options);
    auto conversation = ConversationRow(preview, "Product rollout");
    Wait(preview, [&] { return conversation.Exists(); }, "The failed export conversation did not reopen");
    conversation.Tap();
    auto review = preview.Find(ReviewCard("Review artifact v1"));
    Wait(preview, [&] { return review.Exists(); }, "The failed export lost the staged presentation version");
    if (!review.One().in_viewport) {
      review = preview.Find(UiSelector::Key("agent-chat-history")).ScrollUntil(ReviewCard("Review artifact v1"),
          {.step = {0.0F, -95.0F}});
    }
    review.Tap();
    Wait(preview, [&] { return ShowsText(preview, "Approved rollout"); }, "The original staged slide content could not be reviewed");
  }
}

void StageDesign(const Application& application, const std::filesystem::path& storage) {
  UiTest preview(application, {.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  ChooseDraftAssistant(preview, "Design");
  auto draft = preview.Find(UiSelector::Key("new-conversation-composer"));
  FocusPreviewEditor(preview, draft, "The design draft did not receive focus");
  draft.ReplaceText("Settings wireframe");
  auto send = preview.Find(ButtonText("Send"));
  Wait(preview, [&] { return send.One().enabled; }, "design input is not ready");
  send.Tap();
  auto review = preview.Find(ButtonText("Review design"));
  Wait(preview, [&] { return review.Exists() && review.One().in_viewport; }, "design draft was not offered for review", 25s);
  review.Tap();
  auto theme = preview.Find(ButtonText("Theme   \xc2\xb7   Use system setting"));
  Wait(preview, [&] { return theme.Exists() && theme.One().in_viewport; }, "design nodes were not available for review");
  theme.Tap();
  Wait(preview, [&] {
    return preview.Find(UiSelector::Text("Default to the system setting and expose available choices.")).Exists();
  }, "selecting a node did not show its annotation");
  if (std::filesystem::exists(storage)) throw std::runtime_error("Unapproved design was written");
}

void ReviewCopiedPresentation(const Application& application) {
  UiTest preview(application, {.viewport = {1080.0F, 920.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  auto source = preview.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text("Product rollout"));
  Wait(preview, [&] { return source.Count() == 1 && source.One().key.has_value(); }, "The source presentation is unavailable");
  const auto key = std::get<std::string>(*source.One().key);
  auto more = preview.Find(UiSelector::Key("session-actions/" + key));
  Wait(preview, [&] { return more.Count() == 1 && more.One().in_viewport; }, "The presentation copy action is unavailable");
  more.Tap();
  auto copy = preview.Find(UiSelector::Text("Copy conversation"));
  Wait(preview, [&] { return copy.Count() == 1 && copy.One().in_viewport; }, "The presentation copy menu did not open");
  copy.Tap();
  auto branch = preview.Find(UiSelector::Text("Product rollout (copy)"));
  auto artifacts = preview.Find(ReviewCard("Review artifact v1"));
  Wait(preview, [&] {
    return branch.Count() >= 2 && std::ranges::any_of(artifacts.All(), &UiNodeInfo::in_viewport);
  }, "The copied history did not show an inherited PowerPoint reference");
  const auto open_review = [&] {
    const auto review = preview.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
        UiSemanticSelector::Label("Review Product rollout.pptx v1")));
    Wait(preview, [&] { return review.Count() == 1; }, "The inherited presentation action did not become available");
    review.PerformSemanticAction({SemanticActionKind::Activate, {}});
  };
  open_review();
  auto summary = preview.Find(UiSelector::Key("presentation-slide"));
  Wait(preview, [&] { return summary.Exists() && summary.One().in_viewport; }, "The inherited PowerPoint preview did not open");
  const auto heading = preview.Find(UiSelector::Key("artifact-review-heading"));
  Wait(preview, [&] { return heading.Find(UiSelector::Text("Product rollout.pptx")).Exists() &&
      heading.Find(UiSelector::Text("Version 1")).Exists(); }, "The review did not show the inherited artifact name and exact version together");
  for (const auto* page : {"2 / 3", "3 / 3"}) {
    const auto next = preview.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
        UiSemanticSelector::Label("Next slide"), UiSemanticSelector::Enabled(true)));
    Wait(preview, [&] { return next.Count() == 1; }, "The copied presentation could not advance");
    next.PerformSemanticAction({SemanticActionKind::Activate, {}});
    Wait(preview, [&] { return ShowsText(preview, page); }, "The copied presentation did not change slides");
  }
  constexpr auto bullet = "Brief & revise <option>";
  const auto excerpt_action = preview.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
      UiSemanticSelector::Label(bullet)));
  Wait(preview, [&] { return excerpt_action.Count() == 1; }, "The copied presentation did not show the expected excerpt");
  excerpt_action.PerformSemanticAction({SemanticActionKind::Activate, {}});
  AskAboutSelection(preview, bullet);
  Wait(preview, [&] { return !summary.Exists() && artifacts.Exists(); }, "Closing preview lost the conversation history");
  CheckReturnedConclusion(preview, bullet);
  open_review();
  Wait(preview, [&] { return summary.Exists() && preview.Find(UiSelector::Text("3 / 3")).Exists(); },
      "Reopening the presentation review lost its last page");
}

void ApproveAndReviseDesign(const Application& application, const std::filesystem::path& storage) {
  UiTest preview(application, {.viewport = {1080.0F, 880.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  const auto visible = [&](const char* text) {
    return std::ranges::any_of(preview.Find(UiSelector::Text(text)).All(), &UiNodeInfo::in_viewport);
  };
  auto row = PendingConversation(preview, "Settings wireframe");
  Wait(preview, [&] { return row.Exists(); }, "the pending design session was not recovered");
  row.Tap();
  auto review = preview.Find(ButtonText("Review design"));
  auto approve = preview.Find(ButtonText("Approve design"));
  const auto original = preview.Find(ReviewCard("Review artifact v1"));
  Wait(preview, [&] { return review.Exists() && review.One().in_viewport; }, "restoring the run lost its original design review");
  review.Tap();
  Wait(preview, [&] { return approve.Exists() && approve.One().in_viewport; }, "the restored design could not be approved");
  approve.Tap();
  Wait(preview, [&] { return original.Exists() && original.One().in_viewport && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
      "confirmed design was not published");
  if (preview.Find(ButtonText("Open latest design")).Exists())
    throw std::runtime_error("The design card and the latest design shortcut were both shown");
  auto composer = preview.Find(UiSelector::Key("composer"));
  composer.Tap();
  composer.ReplaceText("Compact settings");
  preview.Find(ButtonText("Send")).Tap();
  Wait(preview, [&] { return review.Exists() && review.One().in_viewport; }, "revision draft did not reach review");
  review.Tap();
  Wait(preview, [&] { return visible("Compact settings"); },
      "revision review did not read the latest request");
  approve.Tap();
  const auto revised = preview.Find(ReviewCard("Review artifact v2"));
  Wait(preview, [&] { return revised.Exists() && revised.One().in_viewport && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
      "the design revision was not committed");
  revised.Tap();
  Wait(preview, [&] { return visible("Version 2"); },
      "the revision became a separate artifact");
  auto older = preview.Find(ButtonText("Older design"));
  older.Tap();
  Wait(preview, [&] { return visible("Version 1"); },
      "the original design could not be reopened");
  std::size_t documents = 0;
  for (const auto& entry : std::filesystem::directory_iterator(storage)) {
    if (!entry.is_regular_file()) continue;
    auto data = File(entry.path().u8string()).ReadString();
    if (!data.Succeeded() || data.Value().find("\"id\":\"theme\"") == std::string::npos ||
        data.Value().find("\"id\":\"save\"") == std::string::npos) throw std::runtime_error("Saved design lost stable node IDs");
    ++documents;
  }
  if (documents != 2) throw std::runtime_error("Design approvals did not publish exactly two versioned documents");
  TapFormButton(preview, "Increase contrast");
  const std::string node = "Increase contrast\nThe preference can be changed without leaving this screen.";
  AskAboutSelection(preview, node);
  CheckReturnedConclusion(preview, node);
  TapReviewCard(preview, "Review artifact v2");
  Wait(preview, [&] { return visible("Version 2"); }, "Reopening the design review lost its latest version");
  TapFormButton(preview, "Save changes");
  TapFormButton(preview, "Older design");
  Wait(preview, [&] { return visible("Version 1") && visible("The preference can be changed without leaving this screen."); },
      "Reopening the original design lost its selected node");
  TapFormButton(preview, "Back to conversation");
  const auto click = [&](const char* label) {
    auto control = preview.Find(ButtonText(label));
    Wait(preview, [&] {
      return std::ranges::any_of(control.All(), [](const auto& item) { return item.in_viewport && item.enabled; });
    }, label);
    control.Tap({.device_kind = PointerDeviceKind::Mouse});
  };
  click("Goal");
  const auto goal_panel_view = preview.Find(UiSelector::Key("goal-panel"));
  Wait(preview, [&] { return goal_panel_view.Exists() && goal_panel_view.One().bounds.width >= 304.0F &&
      composer.Exists() && composer.One().in_viewport; }, "The desktop goal did not open beside the conversation");
  click("Set goal");
  auto objective = preview.Find(UiSelector::Key("goal-objective"));
  objective.Tap();
  objective.ReplaceText("Review compact preferences");
  auto conditions = preview.Find(UiSelector::Key("goal-conditions"));
  conditions.Tap();
  conditions.ReplaceText("Check focus order in the settings screen.");
  click("Close goal");
  click("Goal");
  Wait(preview, [&] { return conditions.Exists() && conditions.One().value == "Check focus order in the settings screen."; },
      "Reopening the goal panel did not preserve its draft");
  click("Save goal");
  Wait(preview, [&] { return visible("Ready to enable   \xc2\xb7   Version 1") &&
      visible("Check focus order in the settings screen."); }, "the proposed goal lost its completion conditions");
  click("Edit goal");
  objective.Tap();
  objective.ReplaceText("Review accessible preferences");
  conditions.Tap();
  conditions.ReplaceText("Confirm that keyboard focus remains visible.");
  click("Review goal change");
  click("Apply goal change");
  Wait(preview, [&] { return visible("Ready to enable   \xc2\xb7   Version 2") &&
      visible("Confirm that keyboard focus remains visible."); }, "goal edit did not persist the new conditions");
  click("Enable");
  click("Close goal");
  Wait(preview, [&] { return review.Exists() && review.One().in_viewport; },
      "the enabled goal did not start an independent design review");
  const auto goal_panel = preview.FindSemantics(UiSemanticSelector::Label("Close goal"));
  for (int attempt = 0; attempt < 2 && !goal_panel.Exists(); ++attempt) {
    click("Goal");
    preview.Pump(250ms);
  }
  try {
    click("Pause");
  } catch (...) {
    static_cast<void>(File((storage.parent_path() / "goal-actions.snapshot").u8string())
        .WriteString(preview.CaptureSnapshot().ToString()));
    throw;
  }
  Wait(preview, [&] { return visible("Paused   \xc2\xb7   Version 2"); }, "pausing the goal did not affect the panel");
  click("Resume");
  Wait(preview, [&] { return visible("Active   \xc2\xb7   Version 2"); }, "resuming the goal did not affect the panel");
  click("Cancel goal");
  Wait(preview, [&] { return visible("Cancelled"); }, "cancelled goal did not settle");
}

void PlanBriefing(const Application& application) {
  UiTest preview(application, {.viewport = {1080.0F, 880.0F}});
  auto general = preview.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text("approval"));
  Wait(preview, [&] { return general.Exists(); }, "General conversation did not reopen");
  general.Tap();
  auto composer = preview.Find(UiSelector::Key("composer"));
  Wait(preview, [&] { return composer.Exists() && ShowsText(preview, "Preview action completed."); },
      "General conversation and composer did not mount");
  composer.Tap();
  composer.ReplaceText("Outline this report: Accessibility checks passed in the settings screen.");
  preview.Find(ButtonText("Send")).Tap();
  Wait(preview, [&] {
    return ShowsText(preview, "Release findings") &&
        ShowsText(preview, "Accessibility checks passed in the settings screen.") &&
        !preview.Find(UiSelector::Key("agent-chat-stop")).Exists();
  },
      "the General assistant did not return the tools' one-slide outline", 25s);

  composer.Tap();
  composer.ReplaceText("Design briefing: Create a preferences wireframe and a presentation of the design.");
  preview.Find(ButtonText("Send")).Tap();
  auto start = preview.Find(ButtonText("Start design briefing"));
  Wait(preview, [&] { return start.Exists() && start.One().in_viewport; }, "the General assistant did not prepare a handoff");
  if (!preview.Find(UiSelector::Text("The approved design will be sent to Product rollout.")).Exists()) {
    throw std::runtime_error("The existing PPT conversation was not identified in the confirmation");
  }
  start.Tap();
  Wait(preview, [&] {
    return PendingConversation(preview, "Design for Product rollout").Exists();
  }, "the General assistant did not create an independent design review");
}

void DeliverBriefing(const Application& application, const File& output) {
  UiTest preview(application, {.viewport = {1080.0F, 920.0F}});
  auto design = PendingConversation(preview, "Design for Product rollout");
  Wait(preview, [&] { return design.Exists(); }, "the design review did not recover across app restart");
  design.Tap();
  auto open = preview.Find(ButtonText("Review design"));
  Wait(preview, [&] { return open.Exists() && open.One().in_viewport; }, "the recovered design is unavailable");
  open.Tap();
  auto accept = preview.Find(ButtonText("Approve design"));
  Wait(preview, [&] { return accept.Exists() && accept.One().in_viewport; }, "the design approval did not reopen");
  accept.Tap();

  auto ppt = PendingConversation(preview, "Product rollout");
  Wait(preview, [&] { return ppt.Exists(); }, "the recovered coordinator did not contact the existing PPT session");
  ppt.Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Key("ppt-audience")).Exists(); }, "the PPT work request did not start");
  TapFormButton(preview, "Draft outline");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("slide-title-0")).Exists(); }, "the PPT assistant did not prepare the outline");
  TapFormButton(preview, "Confirm outline");
  auto export_name = preview.Find(UiSelector::Key("ppt-file_name"));
  Wait(preview, [&] { return export_name.Exists(); }, "the downstream tool did not stage a PPTX");
  if (!export_name.One().in_viewport) {
    export_name = preview.Find(UiSelector::Key("ppt-form-scroll")).ScrollUntil(UiSelector::Key("ppt-file_name"), {.step = {0.0F, 94.0F}});
  }
  export_name.Tap();
  export_name.ReplaceText("DesignBriefing.pptx");
  TapFormButton(preview, "Review export");
  auto approve = preview.Find(ButtonText("Approve"));
  Wait(preview, [&] { return approve.Exists() && approve.One().in_viewport; }, "the downstream export approval is missing");
  if (output.Exists()) throw std::runtime_error("The cross-session workflow bypassed export approval");
  approve.Tap();
  Wait(preview, [&] { return output.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "the briefing did not export");
  auto file = archive::ArchiveReader::Open(output);
  if (!file.Succeeded()) throw std::runtime_error(file.Error().message);
  const auto slide = ReadPart(file.Value(), "ppt/slides/slide1.xml");
  if (slide.find("Approved wireframe") == std::string::npos || slide.find("Increase contrast") == std::string::npos) {
    throw std::runtime_error("The exported slide did not use the approved design node data");
  }
  preview.Find(UiSelector::Text("approval")).Tap();
  Wait(preview, [&] {
    return ShowsText(preview, "Approved design and exported briefing received") &&
        !preview.Find(UiSelector::Key("agent-chat-stop")).Exists();
  }, "the original General run did not receive the exported PPTX reply");
}

std::string SettingsPage(bool enabled) {
  return std::string(R"html(<!doctype html>
<html lang="en"><meta charset="utf-8"><title>Product settings</title>
<style>
body { margin: 0; background: #edf0ef; color: #1e2b2f; font: 18px system-ui, sans-serif; }
main { box-sizing: border-box; width: min(680px, 90%); margin: 60px auto; }
h1 { margin: 0 0 24px; font-size: 34px; font-weight: 600; }
form { background: white; padding: 30px; border: 1px solid #dbe2dd; border-radius: 12px; }
label { display: block; margin: 14px 0; }
button { padding: 10px 16px; color: white; background: #23584f; border: 0; border-radius: 6px; font: inherit; }
output { margin-left: 18px; color: #305b49; }
</style>
<main><h1>Settings</h1><form onsubmit="event.preventDefault(); localStorage.mode = this.mode.checked ? 'enabled' : 'disabled'; this.status.value = 'Saved';">
<strong>Product mode</strong><p>Manage the settings used by this workspace.</p>
<label><input name="mode" type="checkbox")html") + (enabled ? " checked> Enabled" : "> Disabled") +
      R"html(</label>
<button type="submit">Save changes</button><output name="status"></output></form></main>
</html>)html";
}

class PatchStream final : public kit::ModelTurnStream {
public:
  explicit PatchStream(bool release, bool staged = false, std::string trailing_config = {}, bool reasoning_summary = false)
      : release_(release), staged_(staged), trailing_config_(std::move(trailing_config)), reasoning_summary_(reasoning_summary) {}

  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    using kit::JsonValue;
    using Result = kit::AgentResult<std::optional<kit::ModelEvent>>;
    if (std::exchange(reasoning_summary_, false))
      co_return Result::Success(kit::ModelReasoningSummary{"Check the two approved files before proposing the edit."});
    if (index_++ == 0) {
      JsonValue::Array files;
      JsonValue::Object config{{"path", JsonValue{std::string("config.txt")}},
          {"content", JsonValue{std::string(staged_ ? "mode=staged\n" : "mode=enabled\n") + trailing_config_}}};
      JsonValue::Object page{{"path", JsonValue{std::string("settings.html")}}, {"content", JsonValue{SettingsPage(true)}}};
      if (release_) {
        config.emplace("design_nodes", JsonValue{JsonValue::Array{JsonValue{std::string("mode")}}});
        page.emplace("design_nodes", JsonValue{JsonValue::Array{JsonValue{std::string("mode")}, JsonValue{std::string("save")}}});
      }
      files.push_back(JsonValue{std::move(config)});
      files.push_back(JsonValue{std::move(page)});
      co_return Result::Success(kit::ModelStructuredOutput{JsonValue{JsonValue::Object{{"files", JsonValue{std::move(files)}}}}});
    }
    if (index_ == 2) co_return Result::Success(kit::ModelFinished{kit::ModelFinishReason::Completed, {}});
    co_return Result::Success(std::nullopt);
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  bool release_;
  bool staged_;
  std::string trailing_config_;
  bool reasoning_summary_;
  std::size_t index_ = 0;
};

class PatchModel final : public kit::ModelProvider {
public:
  explicit PatchModel(bool fail_first = false, bool reasoning_summary = false)
      : fail_first_(fail_first), reasoning_summary_(reasoning_summary) {}

  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .structured_output = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    using Result = kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>;
    const auto* input = std::get_if<kit::ModelContent>(&request.items.back());
    const auto* prompt = input ? std::get_if<std::string>(&input->body) : nullptr;
    if (!request.output_schema || !prompt || prompt->find("mode=disabled") == std::string::npos ||
        prompt->find("Disabled") == std::string::npos) {
      co_return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Expected both scoped workspace files"});
    }
    if (std::exchange(fail_first_, false))
      co_return Result::Failure({.code = kit::ErrorCode::ProviderFailure, .message = "Fixture model failed before applying the patch"});
    const bool release = prompt->find("Implement the approved design for this release") != std::string::npos;
    if (release && std::ranges::none_of(request.items, [](const auto& item) {
          const auto* content = std::get_if<kit::ModelContent>(&item);
          const auto* body = content ? std::get_if<std::string>(&content->body) : nullptr;
          return content && content->role == kit::ModelContent::Role::Context && body &&
              body->find("design/board+json") != std::string::npos && body->find("Save changes") != std::string::npos &&
              body->find("\"id\":\"mode\"") != std::string::npos;
        })) {
      co_return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Expected the approved design contents"});
    }
    co_return Result::Success(std::make_unique<PatchStream>(release, prompt->find("Keep mode staged") != std::string::npos,
        std::string{}, reasoning_summary_));
  }

private:
  bool fail_first_;
  bool reasoning_summary_;
};

class EditorPatchModel final : public kit::ModelProvider {
public:
  explicit EditorPatchModel(bool restoring) : restoring_(restoring) {}

  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .structured_output = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    using Result = kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>;
    if (restoring_) co_return Result::Failure({.code = kit::ErrorCode::InvalidState, .message = "The pending patch must restore without a model turn"});
    const bool editor = std::ranges::any_of(request.items, [](const auto& item) {
      const auto* content = std::get_if<kit::ModelContent>(&item);
      const auto* text = content ? std::get_if<std::string>(&content->body) : nullptr;
      return text && content->role == kit::ModelContent::Role::Context &&
          *text == "Unsaved workspace file config.txt\nmode=disabled\nowner=preview\n";
    });
    if (!request.output_schema || !editor)
      co_return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Expected the exact unsaved workspace draft"});
    co_return Result::Success(std::make_unique<PatchStream>(false, false, "owner=preview\n"));
  }

private:
  bool restoring_;
};

void CheckText(const File& file, std::string_view expected) {
  auto contents = file.ReadString();
  if (!contents.Succeeded() || contents.Value() != expected) {
    throw std::runtime_error("Unexpected contents in " + file.Path());
  }
}

void WriteFixture(const File& file, std::string_view contents) {
  if (!file.WriteString(contents)) throw std::runtime_error("Could not write " + file.Path());
}

std::string WorkspaceTestScript(std::string_view enabled_pattern, bool capture_screen) {
  std::string test = R"(
file(READ "config.txt" configuration)
file(READ "settings.html" settings)
if (NOT configuration STREQUAL "mode=enabled\n" OR NOT settings MATCHES ")" + std::string(enabled_pattern) + R"(")
    message(FATAL_ERROR "Workspace contents do not match the approved patch")
endif ()
file(WRITE "tested.txt" "verified")
)";
#if defined(AGENTKIT_PREVIEW_BROWSER)
  if (capture_screen) test += R"(
set(capture "$ENV{HUXERUI_AGENTKIT_VERIFICATION_PNG}")
if (NOT capture STREQUAL "")
  set(profile "${capture}.profile")
  if (EXISTS "${profile}")
    message(FATAL_ERROR "Browser test profile already exists")
  endif ()
  string(REPLACE " " "%20" page "${CMAKE_CURRENT_BINARY_DIR}/settings.html")
  if (WIN32)
    set(page "/${page}")
  endif ()
  execute_process(
    COMMAND ")" AGENTKIT_PREVIEW_BROWSER R"(" --headless --disable-gpu --disable-background-networking --disable-extensions
         --no-first-run --dump-dom "--user-data-dir=${profile}" --force-device-scale-factor=1
         --window-size=900,570 "--screenshot=${capture}" "file://${page}"
    RESULT_VARIABLE browser_exit OUTPUT_VARIABLE browser_output ERROR_VARIABLE browser_error TIMEOUT 25)
  file(REMOVE_RECURSE "${profile}")
  if (NOT browser_exit EQUAL 0 OR NOT EXISTS "${capture}" OR NOT browser_output MATCHES "<h1>Settings</h1>")
    message(FATAL_ERROR "Browser screenshot failed: ${browser_exit} ${browser_output} ${browser_error}")
  endif ()
endif ()
)";
#else
  static_cast<void>(capture_screen);
#endif
  return test;
}

constexpr std::string_view live_settings_before =
    "<!doctype html><html lang=\"en\"><title>Settings</title><h1>Settings</h1><p id=\"status\">Disabled</p></html>\n";
constexpr std::string_view live_settings_after =
    "<!doctype html><html lang=\"en\"><title>Settings</title><h1>Settings</h1><p id=\"status\">Enabled</p></html>\n";

void SeedLiveReleaseWorkspace(const File& workspace) {
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the isolated workspace");
  WriteFixture(workspace.Child("config.txt"), "mode=disabled\n");
  WriteFixture(workspace.Child("settings.html"), live_settings_before);
  WriteFixture(workspace.Child("verify.cmake"), WorkspaceTestScript(">Enabled</p>", true));
  WriteFixture(workspace.Child("CTestTestfile.cmake"),
      "add_test(verify \"" AGENTKIT_PREVIEW_CMAKE "\" \"-P\" \"verify.cmake\")\n");
}

template<class T>
T LiveTake(kit::AgentResult<T> value) {
  if (!value.Succeeded()) throw std::runtime_error(value.Error().message);
  return std::move(value).Value();
}

void LiveTake(kit::AgentResult<void> value) {
  if (!value.Succeeded()) throw std::runtime_error(value.Error().message);
}

Task<kit::InteractionSnapshot> LiveInteraction(const std::shared_ptr<kit::Conversation>& session, std::string title,
    std::optional<kit::GoalRef> goal = {}, std::chrono::seconds wait = 60s) {
  const auto end = std::chrono::steady_clock::now() + wait;
  while (std::chrono::steady_clock::now() < end) {
    if (auto snapshot = session->Snapshot()) {
      for (const auto& item : snapshot->interactions) {
        if (item.state != kit::InteractionState::Pending) continue;
        if (goal) {
          const auto run = item.run ? std::ranges::find(snapshot->runs, *item.run, &kit::RunSnapshot::id) : snapshot->runs.end();
          if (run == snapshot->runs.end() || run->goal != goal) continue;
        }
        if (const auto* form = std::get_if<kit::FormRequest>(&item.body); form && form->title == title) co_return item;
        if (const auto* approval = std::get_if<kit::ToolApproval>(&item.body); approval && approval->preview.title == title)
          co_return item;
        if (const auto* approval = std::get_if<kit::OperationApproval>(&item.body); approval && approval->preview.title == title)
          co_return item;
      }
      if (!snapshot->summary.active_run && !snapshot->runs.empty()) {
        const auto& run = snapshot->runs.back();
        if ((!goal || run.goal == goal) && title != "Complete goal") {
          throw std::runtime_error(run.failure ? run.failure->message : "Run settled before " + title);
        }
      }
    }
    co_await Delay(25ms);
  }
  throw std::runtime_error("Timed out waiting for " + title);
}

Task<void> LiveApprove(const std::shared_ptr<kit::Conversation>& session, const kit::InteractionSnapshot& item) {
  LiveTake(co_await session->SubmitAsync(kit::NewCommandId(), {item.id, item.version, kit::ApprovalDecision::Approve}));
}

Task<kit::Subscription> LiveObserve(const std::shared_ptr<kit::Conversation>& session) {
  co_return LiveTake(co_await session->ObserveAsync([](auto) {}));
}

const kit::JsonValue::Object& LiveForm(const kit::InteractionSnapshot& item) {
  return std::get<kit::JsonValue::Object>(std::get<kit::FormRequest>(item.body).presentation.value);
}

Task<void> LiveRunCompleted(const std::shared_ptr<kit::Conversation>& session, std::chrono::seconds timeout = 30s) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (auto snapshot = session->Snapshot(); snapshot && !snapshot->summary.active_run && !snapshot->runs.empty()) {
      const auto& run = snapshot->runs.back();
      if (run.state != kit::RunState::Succeeded) {
        throw std::runtime_error(run.failure ? run.failure->message : "The live Preview run did not succeed");
      }
      co_return;
    }
    co_await Delay(25ms);
  }
  throw std::runtime_error("Timed out waiting for the live Preview run");
}

Task<void> RunConfiguredChartReview(const std::shared_ptr<kit::AgentRuntime>& runtime, const kit::ProjectId& project,
                                    const File& data, std::string_view phase) {
  using namespace agentkit_preview;
  const bool ask = phase == "review-ask";
  const bool manual = ask || phase == "review-manual";
  const bool denied = phase == "review-deny";
  if (!manual && !denied && phase != "review-approve") throw std::runtime_error("Unknown chart review acceptance phase");
  const auto output = data.Child("exports").Child("Review acceptance.csv");
  if (output.Exists()) throw std::runtime_error("Choose a fresh chart review acceptance directory");

  kit::CreateSessionRequest request{
      .options = {.agent = GeneralAgent, .project = project, .title = "Chart review acceptance"},
      .initial = kit::SendInput{.text = "Pie chart:\n"
          R"({"title":"Internal distribution","categories":[{"label":"Private Division A","value":6},{"label":"Private Division B","value":2}]})"},
      .user_start = {.execution_profile = ask ? AskProfile : ReviewProfile}};
  auto created = LiveTake(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request)));
  auto session = LiveTake(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(created.outcome).session));
  auto observed = co_await LiveObserve(session);
  auto form = co_await LiveInteraction(session, "Chart export");
  using Value = kit::JsonValue;
  Value answer{Value::Object{{"file_name", Value{std::string("Review acceptance.csv")}},
      {"indices", Value{Value::Array{Value{std::int64_t{0}}}}}}};
  LiveTake(co_await session->SubmitAsync(kit::NewCommandId(), {form.id, form.version, std::move(answer)}));

  const auto export_call = [](const kit::SessionSnapshot& snapshot) {
    return std::ranges::find_if(snapshot.tools, [](const auto& item) { return item.tool.id == ExportReportChart; });
  };
  if (manual) {
    auto item = co_await LiveInteraction(session, "Export report chart", {}, 25s);
    auto snapshot = session->Snapshot();
    const auto call = export_call(*snapshot);
    const auto& approval = std::get<kit::ToolApproval>(item.body);
    if (output.Exists() || call == snapshot->tools.end() || call->id != approval.call || call->plan != approval.plan ||
        (ask ? call->review.has_value() : (!call->review || call->review->outcome != kit::ToolReviewOutcome::Escalated ||
            call->review->plan != approval.plan))) {
      throw std::runtime_error("Manual confirmation lost the original chart plan or wrote before approval");
    }
    co_await LiveApprove(session, item);
  }

  const auto end = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < end) {
    auto snapshot = session->Snapshot();
    if (snapshot && !snapshot->summary.active_run && !snapshot->runs.empty()) {
      const auto call = export_call(*snapshot);
      if (call == snapshot->tools.end() || (ask ? call->review.has_value() : (!call->review || call->review->plan != call->plan ||
          call->review->reviewer != "preview.report-chart.model/v1"))) {
        throw std::runtime_error("Chart export did not record the configured approval path");
      }
      const auto has_approval = std::ranges::any_of(snapshot->interactions, [](const auto& item) {
        return std::holds_alternative<kit::ToolApproval>(item.body);
      });
      if (has_approval != manual) throw std::runtime_error("Chart export showed an unexpected manual approval state");
      if (denied) {
        if (output.Exists() || call->state != kit::ToolCallState::Denied || call->review->outcome != kit::ToolReviewOutcome::Denied)
          throw std::runtime_error("Rejected model review did not prevent the write");
      } else {
        if (snapshot->runs.back().state != kit::RunState::Succeeded || call->state != kit::ToolCallState::Succeeded ||
            (!manual && call->review->outcome != kit::ToolReviewOutcome::Approved))
          throw std::runtime_error("Chart export did not finish through the selected approval path");
        CheckText(output, "Category,Value\r\n\"Private Division A\",6\r\n");
      }
      if (call->review) std::cout << "Review plan " << call->review->plan.value << ": " << call->review->reason << '\n';
      co_return;
    }
    co_await Delay(25ms);
  }
  throw std::runtime_error("Timed out waiting for chart review acceptance");
}

Task<std::shared_ptr<kit::Conversation>> LiveConversation(const std::shared_ptr<kit::AgentRuntime>& runtime,
    const kit::ProjectId& project, const kit::WorkspaceId& workspace, kit::AgentId agent, std::string title,
    std::optional<std::string> initial = {}, bool wait_for_creation = false) {
  if (initial) {
    kit::CreateSessionRequest request{
        .options = {.agent = agent, .project = project, .workspace = workspace, .title = std::move(title)},
        .initial = kit::SendInput{.text = std::move(*initial)}};
    auto created = LiveTake(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request)));
    co_return LiveTake(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(created.outcome).session));
  }
  const auto end = std::chrono::steady_clock::now() + 60s;
  do {
    auto page = LiveTake(co_await runtime->Sessions().ListAsync({.project = project, .agent = agent}, {}));
    auto row = std::ranges::find(page.sessions, title, &kit::SessionSummary::title);
    if (row != page.sessions.end()) co_return LiveTake(co_await runtime->Sessions().OpenAsync(row->id));
    if (!wait_for_creation) break;
    co_await Delay(25ms);
  } while (std::chrono::steady_clock::now() < end);
  throw std::runtime_error("Could not find the native Preview conversation " + title);
}

Task<void> RunConfiguredReleaseSkill(const std::shared_ptr<kit::AgentRuntime>& runtime, const kit::ProjectId& project,
                                     const kit::WorkspaceId& workspace, const File& data, std::string_view phase) {
  using namespace agentkit_preview;
  using Value = kit::JsonValue;
  constexpr auto title = "Live release Skill";
  const auto output = data.Child("exports").Child("LiveReleaseSkill.pptx");
  if (output.Exists()) throw std::runtime_error("Use a fresh release Skill acceptance directory");
  if (phase != "skill-prepare" && phase != "skill-finish")
    throw std::runtime_error("Use skill-prepare or skill-finish");
  std::optional<std::string> prompt;
  if (phase == "skill-prepare") {
    prompt = "Create a release presentation as a PPTX using the release-presentation Skill and this verification report. "
        "Seven workspace settings tests ran and all seven passed. No layout regressions were found. "
        "The release decision may proceed after the final accessibility review.";
  }
  const auto chat = co_await LiveConversation(runtime, project, workspace, GeneralAgent, title, std::move(prompt));
  auto watching = co_await LiveObserve(chat);
  auto activation = co_await LiveInteraction(chat, "Activate Skill");
  std::cout << "Skill activation " << activation.id.value << '\n';
  if (phase == "skill-finish") {
    co_await LiveApprove(chat, activation);
    auto brief = co_await LiveInteraction(chat, "Presentation brief");
    Value answers{Value::Object{{"audience", Value{std::string("Release team")}}, {"style", Value{std::string("Concise")}},
        {"length", Value{std::string("Exactly 2 slides")}},
        {"reference_notes", Value{std::string("Use the section titles from the template verbatim and in their original order.")}}}};
    LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {brief.id, brief.version, std::move(answers)}));
    auto form = co_await LiveInteraction(chat, "Review and edit outline");
    auto outline = LiveTake(ReadPresentationOutline(LiveForm(form).at("outline")));
    if (outline.slides.size() != 2 || outline.slides[0].title != "Verification evidence" || outline.slides[1].title != "Release decision")
      throw std::runtime_error("The live model did not use the actual release template");
    outline.slides[0].title = "Verified release evidence";
    LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {form.id, form.version, WritePresentationOutline(outline)}));
    auto preview = co_await LiveInteraction(chat, "Preview slides and choose a file name");
    if (LiveForm(preview).at("outline") != WritePresentationOutline(outline))
      throw std::runtime_error("The staged presentation did not reflect the edited outline");
    Value destination{Value::Object{{"file_name", Value{std::string("LiveReleaseSkill.pptx")}}}};
    LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {preview.id, preview.version, std::move(destination)}));
    auto approval = co_await LiveInteraction(chat, "Export PowerPoint");
    if (output.Exists()) throw std::runtime_error("The live Skill wrote the final deck before export approval");
    co_await LiveApprove(chat, approval);
    co_await LiveRunCompleted(chat);
    const auto view = chat->Snapshot();
    if (view->runs.back().active_skills.size() != 1 ||
        view->runs.back().active_skills.front().trigger != kit::SkillActivationTrigger::Model)
      throw std::runtime_error("The live Skill did not retain the model activation");
    auto deck = archive::ArchiveReader::Open(output);
    if (!deck.Succeeded() || ReadPart(deck.Value(), "ppt/slides/slide1.xml").find("Verified release evidence") == std::string::npos ||
        ReadPart(deck.Value(), "ppt/slides/slide2.xml").find("Release decision") == std::string::npos)
      throw std::runtime_error("The live PPTX did not match the approved release outline");
  }
  LiveTake(co_await runtime->CloseAsync());
}

Task<kit::InteractionSnapshot> LiveWorkspacePatch(const std::shared_ptr<kit::Conversation>& code) {
  using kit::JsonValue;
  auto scope = co_await LiveInteraction(code, "Choose files to share and edit");
  JsonValue input{JsonValue::Array{JsonValue{std::string("config.txt")}, JsonValue{std::string("settings.html")}}};
  LiveTake(co_await code->SubmitAsync(kit::NewCommandId(), {scope.id, scope.version, std::move(input)}));
  auto patch = co_await LiveInteraction(code, "Review workspace changes");
  const auto& approval = std::get<kit::ToolApproval>(patch.body);
  if (approval.preview.targets.size() != 2 || approval.preview.summary.find("+mode=enabled") == std::string::npos ||
      approval.preview.summary.find(">Enabled</p>") == std::string::npos)
    throw std::runtime_error("The live model did not propose the requested two-file patch");
  co_return patch;
}

Task<void> RunConfiguredPreview(std::shared_ptr<kit::AgentRuntime> runtime, kit::WorkspaceId workspace,
    kit::ProjectId project, File data, std::string phase) {
  using kit::JsonValue;
  const auto files = data.Child("code");
  const auto config = files.Child("config.txt");
  const auto settings = files.Child("settings.html");
  const auto tested = files.Child("tested.txt");
  const auto output = data.Child("exports").Child("LiveAcceptance.pptx");
  constexpr auto code_title = "Live acceptance code";
  constexpr auto ppt_title = "Live acceptance presentation";
  if (phase == "prepare-code") {
    CheckText(config, "mode=disabled\n");
    CheckText(settings, live_settings_before);
    const auto code = co_await LiveConversation(runtime, project, workspace, agentkit_preview::CodeAgent, code_title,
        "Enable the sample workspace. In config.txt change mode=disabled to mode=enabled, keeping the final newline. "
        "In settings.html change only the status paragraph text from Disabled to Enabled. Preserve all other markup and the final newline.");
    auto watching = co_await LiveObserve(code);
    auto patch = co_await LiveWorkspacePatch(code);
    CheckText(config, "mode=disabled\n");
    CheckText(settings, live_settings_before);
    if (tested.Exists()) throw std::runtime_error("The test ran before approval");
    std::cout << "Pending code patch " << patch.id.value << '\n';
  } else if (phase == "prepare-ppt") {
    const auto code = co_await LiveConversation(runtime, project, workspace, agentkit_preview::CodeAgent, code_title);
    auto watching_code = co_await LiveObserve(code);
    auto patch = co_await LiveInteraction(code, "Review workspace changes");
    std::cout << "Recovered code patch " << patch.id.value << '\n';
    CheckText(config, "mode=disabled\n");
    CheckText(settings, live_settings_before);
    co_await LiveApprove(code, patch);
    auto test = co_await LiveInteraction(code, "Run workspace tests");
    CheckText(config, "mode=enabled\n");
    CheckText(settings, live_settings_after);
    if (tested.Exists()) throw std::runtime_error("CTest ran before its separate approval");
    co_await LiveApprove(code, test);
    co_await LiveRunCompleted(code);
    CheckText(tested, "verified");

    const auto ppt = co_await LiveConversation(runtime, project, workspace, agentkit_preview::PptAgent, ppt_title,
        "Create exactly two slides for a fictional workspace settings release. Summarize the two edited files and the passing validation.");
    auto watching_ppt = co_await LiveObserve(ppt);
    auto brief = co_await LiveInteraction(ppt, "Presentation brief");
    JsonValue answers{JsonValue::Object{{"audience", JsonValue{std::string("Engineering leads")}},
        {"style", JsonValue{std::string("Direct technical summary")}}, {"length", JsonValue{std::string("Exactly 2 slides")}},
        {"reference_notes", JsonValue{std::string("config.txt enables sample mode. settings.html shows Enabled. CTest passed.")}}}};
    LiveTake(co_await ppt->SubmitAsync(kit::NewCommandId(), {brief.id, brief.version, std::move(answers)}));
    auto edit = co_await LiveInteraction(ppt, "Review and edit outline");
    auto edited = LiveTake(agentkit_preview::ReadPresentationOutline(LiveForm(edit).at("outline")));
    if (edited.slides.size() != 2 || edited.slides[0].bullets.empty() || edited.slides[1].bullets.empty())
      throw std::runtime_error("The live model did not draft two substantive slides");
    edited.slides[0].title = "Approved Settings Changes";
    edited.slides[1].title = "Approved Verification Results";
    LiveTake(co_await ppt->SubmitAsync(kit::NewCommandId(), {edit.id, edit.version, agentkit_preview::WritePresentationOutline(edited)}));
    auto preview = co_await LiveInteraction(ppt, "Preview slides and choose a file name");
    if (LiveForm(preview).at("outline") != agentkit_preview::WritePresentationOutline(edited))
      throw std::runtime_error("The presentation preview did not preserve the edited outline");
    JsonValue destination{JsonValue::Object{{"file_name", JsonValue{std::string("LiveAcceptance.pptx")}}}};
    LiveTake(co_await ppt->SubmitAsync(kit::NewCommandId(), {preview.id, preview.version, std::move(destination)}));
    auto save = co_await LiveInteraction(ppt, "Export PowerPoint");
    if (output.Exists()) throw std::runtime_error("The presentation was exported before approval");
    std::cout << "Pending PowerPoint export " << save.id.value << '\n';
  } else if (phase == "finish") {
    const auto ppt = co_await LiveConversation(runtime, project, workspace, agentkit_preview::PptAgent, ppt_title);
    auto watching = co_await LiveObserve(ppt);
    auto save = co_await LiveInteraction(ppt, "Export PowerPoint");
    if (output.Exists()) throw std::runtime_error("The presentation was exported before approval");
    std::cout << "Recovered PowerPoint export " << save.id.value << '\n';
    co_await LiveApprove(ppt, save);
    co_await LiveRunCompleted(ppt);
    auto deck = archive::ArchiveReader::Open(output);
    if (!deck.Succeeded()) throw std::runtime_error(deck.Error().message);
    if (ReadPart(deck.Value(), "ppt/slides/slide1.xml").find("Approved Settings Changes") == std::string::npos ||
        ReadPart(deck.Value(), "ppt/slides/slide2.xml").find("Approved Verification Results") == std::string::npos ||
        deck.Value().ReadEntry("ppt/slides/slide3.xml").Succeeded())
      throw std::runtime_error("The exported PowerPoint does not match the approved outline");
    std::cout << "Live Preview code, CTest and PowerPoint workflows passed\n";
  } else {
    throw std::runtime_error("Use prepare-code, prepare-ppt or finish");
  }
  LiveTake(co_await runtime->CloseAsync());
}

Task<kit::GoalSnapshot> LiveReleaseGoal(const std::shared_ptr<kit::Conversation>& general) {
  auto goal = LiveTake(co_await general->Goals().ReadAsync());
  if (!goal || !agentkit_preview::IsWorkspaceRelease(goal->spec.definition))
    throw std::runtime_error("The General assistant did not propose a workspace release goal");
  co_return std::move(*goal);
}

Task<kit::GoalSnapshot> SetLiveReleaseCount(const std::shared_ptr<kit::Conversation>& general,
    kit::GoalSnapshot goal, std::int64_t count, std::size_t retiring_runs) {
  auto definition = goal.spec.definition;
  std::get<kit::JsonValue::Object>(definition.completion_conditions.value)
      .insert_or_assign("slide_count", kit::JsonValue{count});
  auto preview = LiveTake(co_await general->Goals().PreviewEditAsync(goal.ref.id, goal.revision, definition));
  if (preview.impact.retiring_linked_runs.size() != retiring_runs)
    throw std::runtime_error("The goal edit preview did not identify the correct retiring runs");
  LiveTake(co_await general->Goals().UpdateAsync(kit::NewCommandId(),
      kit::ChangeGoal{goal.ref.id, goal.revision, kit::EditGoal{std::move(definition), preview.token}}));
  auto revised = co_await LiveReleaseGoal(general);
  if (revised.ref.id != goal.ref.id || revised.ref.version != goal.ref.version + 1 ||
      agentkit_preview::ReleaseSlideCount(revised.spec.definition.completion_conditions) != static_cast<std::size_t>(count))
    throw std::runtime_error("The goal edit did not retain the goal and publish the new slide count");
  co_return revised;
}

Task<kit::InteractionSnapshot> StageLiveReleaseDeck(const std::shared_ptr<kit::Conversation>& ppt, kit::GoalRef goal,
    std::int64_t count, std::string filename) {
  using kit::JsonValue;
  auto brief = co_await LiveInteraction(ppt, "Presentation brief", goal);
  if (LiveForm(brief).at("slide_count") != JsonValue{count}) throw std::runtime_error("Unexpected release slide count");
  JsonValue answers{JsonValue::Object{
      {"audience", JsonValue{std::string("Engineering leads")}},
      {"style", JsonValue{std::string("One concise bullet per slide, with no more than 12 words per bullet.")}},
      {"length", JsonValue{std::to_string(count)}},
      {"reference_notes", JsonValue{std::string("Use the approved wireframe, implementation, verification report and screenshot.")}}}};
  LiveTake(co_await ppt->SubmitAsync(kit::NewCommandId(), {brief.id, brief.version, std::move(answers)}));
  auto edit = co_await LiveInteraction(ppt, "Review and edit outline", goal, 140s);
  auto outline = LiveTake(agentkit_preview::ReadPresentationOutline(LiveForm(edit).at("outline")));
  if (outline.slides.size() != static_cast<std::size_t>(count) ||
      std::ranges::any_of(outline.slides, [](const auto& slide) { return slide.bullets.empty(); }))
    throw std::runtime_error("The live release outline did not contain the requested substantive slides");
  outline.slides.back().title = "Release Test Evidence";
  auto approved = agentkit_preview::WritePresentationOutline(outline);
  LiveTake(co_await ppt->SubmitAsync(kit::NewCommandId(), {edit.id, edit.version, approved}));
  auto preview = co_await LiveInteraction(ppt, "Preview slides and choose a file name", goal);
  if (LiveForm(preview).at("outline") != approved || !LiveForm(preview).contains("screenshot_id"))
    throw std::runtime_error("The release preview lost the approved outline or verification screenshot");
  JsonValue destination{JsonValue::Object{{"file_name", JsonValue{std::move(filename)}}}};
  LiveTake(co_await ppt->SubmitAsync(kit::NewCommandId(), {preview.id, preview.version, std::move(destination)}));
  co_return co_await LiveInteraction(ppt, "Export PowerPoint", goal);
}

void CheckReleaseScreenshot(archive::ArchiveReader& deck, const File& data, std::string final_slide) {
  if (ReadPart(deck, "ppt/slides/_rels/" + final_slide + ".xml.rels").find("../media/implementation.png") == std::string::npos ||
      ReadPart(deck, "ppt/slides/" + final_slide + ".xml").find("Implementation screenshot") == std::string::npos)
    throw std::runtime_error("The verification slide did not include the screenshot");
  const auto embedded = ReadPart(deck, "ppt/media/implementation.png");
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(data.Child("workspace-evidence").Path()))) {
    if (entry.path().extension() != ".png") continue;
    auto png = File(entry.path().u8string()).ReadBytes();
    if (png.Succeeded() && std::ranges::equal(std::as_bytes(std::span(embedded)), png.Value())) {
      auto image = ImageAsset::FromEncoded(std::move(png).Value());
      if (image.PixelWidth() == 900 && image.PixelHeight() == 570) return;
    }
  }
  throw std::runtime_error("The exported screenshot does not match the captured CTest output");
}

Task<void> RunConfiguredRelease(std::shared_ptr<kit::AgentRuntime> runtime, kit::WorkspaceId workspace,
    kit::ProjectId project, File data, std::string phase) {
  const auto files = data.Child("code");
  const auto tested = files.Child("tested.txt");
  const auto exports = data.Child("exports");
  const auto old_output = exports.Child("LiveReleaseBeforeEdit.pptx");
  const auto output = exports.Child("LiveRelease.pptx");
  constexpr auto general_title = "Live workspace release";
  constexpr auto ppt_title = "Live release slides";
  constexpr auto verify_title = "Verify Release implementation";

  if (phase == "release-prepare") {
    kit::CreateSessionRequest target{.options = {
        .agent = agentkit_preview::PptAgent, .project = project, .workspace = workspace, .title = ppt_title}};
    LiveTake(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(target)));
    const auto general = co_await LiveConversation(runtime, project, workspace, agentkit_preview::GeneralAgent, general_title,
        "Design, implement, test, and create a release presentation for a minimal Settings screen. "
        "The existing HTML displays a Settings heading and a status paragraph. Give the setting's wireframe node the "
        "exact stable ASCII id mode and label Enabled. Implement exactly these two changes in the existing files: "
        "config.txt replaces mode=disabled with mode=enabled; settings.html replaces only the status paragraph text "
        "Disabled with Enabled. Preserve every other byte and both final newlines. Verify with CTest and prepare the release slides.");
    auto watching_general = co_await LiveObserve(general);
    co_await LiveRunCompleted(general);
    auto goal = co_await SetLiveReleaseCount(general, co_await LiveReleaseGoal(general), 10, 0);
    LiveTake(co_await general->Goals().UpdateAsync(kit::NewCommandId(),
        kit::ChangeGoal{goal.ref.id, goal.revision, kit::EnableGoal{}}));

    const auto design = co_await LiveConversation(runtime, project, workspace, agentkit_preview::DesignAgent, "Release design", {}, true);
    auto watching_design = co_await LiveObserve(design);
    auto review = co_await LiveInteraction(design, "Review design");
    auto board = LiveTake(agentkit_preview::ParseDesign(LiveForm(review).at("design")));
    if (std::ranges::find(board.nodes, "mode", &agentkit_preview::DesignNode::id) == board.nodes.end())
      throw std::runtime_error("The live wireframe lost the requested mode node");
    LiveTake(co_await design->SubmitAsync(kit::NewCommandId(), {review.id, review.version, kit::JsonValue{true}}));

    const auto code = co_await LiveConversation(runtime, project, workspace, agentkit_preview::CodeAgent, "Release implementation", {}, true);
    auto watching_code = co_await LiveObserve(code);
    auto patch = co_await LiveWorkspacePatch(code);
    const auto& approval = std::get<kit::ToolApproval>(patch.body);
    const auto published = design->Snapshot();
    const bool includes_design = published && std::ranges::any_of(published->artifacts, [&](const auto& ref) {
      return std::ranges::find(approval.preview.artifacts, ref) != approval.preview.artifacts.end();
    });
    if (!includes_design || approval.preview.summary.find(" mode") == std::string::npos)
      throw std::runtime_error("The live patch did not identify its approved design and mode node");
    CheckText(files.Child("config.txt"), "mode=disabled\n");
    CheckText(files.Child("settings.html"), live_settings_before);
    co_await LiveApprove(code, patch);

    const auto verification = co_await LiveConversation(runtime, project, workspace, agentkit_preview::VerificationAgent, verify_title, {}, true);
    auto watching_verification = co_await LiveObserve(verification);
    auto ctest = co_await LiveInteraction(verification, "Run workspace tests");
    CheckText(files.Child("config.txt"), "mode=enabled\n");
    CheckText(files.Child("settings.html"), live_settings_after);
    if (tested.Exists()) throw std::runtime_error("The live release ran CTest before approval");
    std::cout << "Pending release verification " << ctest.id.value << '\n';
  } else if (phase == "release-draft" || phase == "release-finish") {
    const auto general = co_await LiveConversation(runtime, project, workspace, agentkit_preview::GeneralAgent, general_title);
    const auto ppt = co_await LiveConversation(runtime, project, workspace, agentkit_preview::PptAgent, ppt_title);
    auto watching_general = co_await LiveObserve(general);
    auto watching_ppt = co_await LiveObserve(ppt);
    auto goal = co_await LiveReleaseGoal(general);
    if (phase == "release-draft") {
      const auto verification = co_await LiveConversation(runtime, project, workspace, agentkit_preview::VerificationAgent, verify_title);
      auto watching_verification = co_await LiveObserve(verification);
      auto ctest = co_await LiveInteraction(verification, "Run workspace tests");
      std::cout << "Recovered release verification " << ctest.id.value << '\n';
      if (tested.Exists()) throw std::runtime_error("The live release ran CTest before recovered approval");
      co_await LiveApprove(verification, ctest);
      auto stale = co_await StageLiveReleaseDeck(ppt, goal.ref, 10, "LiveReleaseBeforeEdit.pptx");
      CheckText(tested, "verified");
      if (old_output.Exists()) throw std::runtime_error("The original release deck was exported before approval");
      auto revised = co_await SetLiveReleaseCount(general, goal, 5, 1);
      auto outdated = co_await ppt->SubmitAsync(kit::NewCommandId(), {stale.id, stale.version, kit::ApprovalDecision::Approve});
      if (outdated.Succeeded() || (outdated.Error().code != kit::ErrorCode::AlreadyClosed &&
              outdated.Error().code != kit::ErrorCode::Conflict) || old_output.Exists())
        throw std::runtime_error("The stale export could still be approved after the goal edit");
      co_await LiveInteraction(ppt, "Presentation brief", revised.ref);
      std::cout << "Invalidated export " << stale.id.value << " by revising goal " << goal.ref.version
                << " to " << revised.ref.version << '\n';
    } else {
      auto save = co_await StageLiveReleaseDeck(ppt, goal.ref, 5, "LiveRelease.pptx");
      if (output.Exists() || old_output.Exists()) throw std::runtime_error("A release deck was exported before approval");
      std::cout << "Approving revised PowerPoint export " << save.id.value << '\n';
      co_await LiveApprove(ppt, save);
      co_await LiveRunCompleted(ppt);
      auto completion = co_await LiveInteraction(general, "Complete goal");
      co_await LiveApprove(general, completion);
      auto finished = co_await LiveReleaseGoal(general);
      if (finished.state != kit::GoalState::Completed || finished.ref != goal.ref || finished.tasks.size() != 3 ||
          !finished.evidence || finished.evidence->sources.size() != 5)
        throw std::runtime_error("The revised goal did not complete with the full delivery evidence");
      auto deck = archive::ArchiveReader::Open(output);
      if (!deck.Succeeded()) throw std::runtime_error(deck.Error().message);
      if (ReadPart(deck.Value(), "ppt/slides/slide5.xml").find("Release Test Evidence") == std::string::npos ||
          deck.Value().ReadEntry("ppt/slides/slide6.xml").Succeeded() || old_output.Exists())
        throw std::runtime_error("The exported PowerPoint does not match the revised release");
      CheckReleaseScreenshot(deck.Value(), data, "slide5");
      CheckText(files.Child("config.txt"), "mode=enabled\n");
      CheckText(files.Child("settings.html"), live_settings_after);
      CheckText(tested, "verified");
      std::cout << "Live model release, goal revision and verified export passed\n";
    }
  } else {
    throw std::runtime_error("Use release-prepare, release-draft or release-finish");
  }
  LiveTake(co_await runtime->CloseAsync());
}

void SendNewConversation(UiTest& preview, std::string title) {
  auto prompt = preview.Find(UiSelector::Key("new-conversation-composer"));
  FocusPreviewEditor(preview, prompt, "New conversation composer did not receive focus");
  prompt.ReplaceText(std::move(title));
  auto send = preview.Find(ButtonText("Send"));
  Wait(preview, [&] { return send.Exists() && send.One().enabled; }, "New conversation cannot send");
  send.Tap();
}

auto NavigationGroupLabel(UiTest& ui, const char* label) {
  return ui.Find(UiSelector::Key("session-directory")).Find(UiSelector::AllOf(UiSelector::Type<Text>(), UiSelector::Text(label)));
}

std::string NavigationGroupId(UiTest& ui, const char* label) {
  auto group = NavigationGroupLabel(ui, label);
  Wait(ui, [&] { return group.Exists() && group.One().enabled && group.One().in_viewport; }, label);
  return std::get<std::string>(*group.One().key).substr(std::string_view("group/").size());
}

void OpenNavigationGroupMenu(UiTest& ui, const char* label) {
  const auto actions = ui.FindSemantics(UiSemanticSelector::Identifier("group-actions/" + NavigationGroupId(ui, label)));
  const auto menu_items = ui.FindSemantics(UiSemanticSelector::Role(SemanticRole::MenuItem));
  const auto menu = ui.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::MenuItem),
      UiSemanticSelector::Label("New conversation in group"), UiSemanticSelector::Enabled(true)));
  Wait(ui, [&] { return !menu_items.Exists() && actions.Count() == 1 && actions.One().enabled && !actions.One().offscreen; },
      "The group actions trigger was not available");
  actions.PerformSemanticAction({SemanticActionKind::Activate, {}});
  const auto message = std::string("The group actions did not open for ") + label;
  Wait(ui, [&] { return menu.Count() == 1 && !menu.One().offscreen; }, message.c_str());
}

void SaveNavigationName(UiTest& ui, const char* value) {
  const auto surface = ui.Find(UiSelector::Key("preview-dialog-surface"));
  Wait(ui, [&] { return surface.Exists(); }, "The navigation dialog did not open on the Preview surface");
  auto name = surface.Find(UiSelector::Key("navigation-name"));
  const auto message = std::string("Navigation name editor did not receive focus for ") + value;
  FocusPreviewEditor(ui, name, message.c_str());
  name.ReplaceText(value);
  TapFormButton(ui, "Save");
  Wait(ui, [&] { return !surface.Exists(); }, "The navigation name dialog did not close");
}

struct DraftCreationGate {
  bool paused = false;
  bool waiting = false;
};

class GatedCreationStore final : public kit::SessionStore {
public:
  GatedCreationStore(std::shared_ptr<kit::SessionStore> store, std::shared_ptr<DraftCreationGate> gate)
      : store_(std::move(store)), gate_(std::move(gate)) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return store_->LoadAsync(); }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId commit) override {
    return store_->FindCommitAsync(std::move(commit));
  }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction transaction) override {
    if (gate_->paused && std::ranges::any_of(transaction.writes, [](const auto& write) {
          return write.collection == "agentkit.session" && !write.expected && write.data;
        })) {
      gate_->waiting = true;
      while (gate_->paused) co_await Delay(10ms);
      gate_->waiting = false;
    }
    co_return co_await store_->CommitAsync(std::move(transaction));
  }

private:
  std::shared_ptr<kit::SessionStore> store_;
  std::shared_ptr<DraftCreationGate> gate_;
};

void VerifyConversationNavigation(const File& root) {
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the navigation workspace");
  WriteFixture(workspace.Child("keep.txt"), "Workspace contents");
  const auto database = root.Child("sessions.sqlite3");
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  std::optional<kit::WorkspaceId> workspace_id;
  auto creation = std::make_shared<DraftCreationGate>();
  const auto hook = MakeRunSettingsTestInstaller(database, workspace, {},
      [&](auto started, std::optional<kit::WorkspaceId> registered, kit::ProjectId) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        workspace_id = registered;
        co_return;
      }, false, [creation](auto store) { return std::make_shared<GatedCreationStore>(std::move(store), creation); });
  const Application app{App, {.application_hooks = {hook}}};
  const UiTestOptions options{.viewport = {1120.0F, 940.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  constexpr auto design = "Settings navigation plan";
  constexpr auto renamed = "Approved navigation plan";
  constexpr auto deck = "Navigation release deck";
  kit::SessionId design_id;
  kit::SessionId deck_id;
  kit::InteractionId design_request;
  kit::InteractionId deck_request;
  const auto inspect = [&](UiTest& ui, std::function<Task<void>()> read) {
    bool done = false;
    std::string error;
    tasks->Launch([&]() -> Task<void> {
      try { co_await read(); }
      catch (const std::exception& failure) { error = failure.what(); }
      done = true;
    });
    Wait(ui, [&] { return done; }, "The navigation inspection did not finish");
    if (!error.empty()) throw std::runtime_error(error);
  };
  const auto execute_sql = [&](UiTest& ui, const char* statement) {
    inspect(ui, [&]() -> Task<void> {
      auto connection = co_await sqlite::Database::OpenAsync(database);
      if (!connection) throw std::runtime_error(connection.Error().Message());
      auto executed = co_await connection->ExecuteAsync(statement);
      if (!executed) throw std::runtime_error(executed.Error().Message());
    });
  };
  const auto menu_item = [](UiTest& ui, const char* label) {
    auto item = ui.Find(UiSelector::Text(label));
    Wait(ui, [&] { return item.Count() == 1 && item.One().in_viewport && item.One().enabled; }, label);
    ClickPreviewMouse(ui, item.One().visible_bounds);
    ui.Pump(250ms);
  };
  const auto save_name = [&](UiTest& ui, const char* value) {
    try {
      SaveNavigationName(ui, value);
    } catch (...) {
      static_cast<void>(root.Child("navigation-dialog.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
  };
  {
    UiTest ui(app, options);
    Wait(ui, [&] { return runtime && tasks && workspace_id; }, "The navigation workspace did not load");
    const auto heading = ui.Find(UiSelector::Key("new-conversation-heading"));
    const auto compose_box = ui.Find(UiSelector::Key("new-draft-composer"));
    const auto aligned = [&] {
      return heading.Exists() && compose_box.Exists() && heading.One().in_viewport && compose_box.One().in_viewport &&
          std::abs(heading.One().bounds.x - compose_box.One().bounds.x) < 1.0F &&
          compose_box.One().visible_bounds.width >= compose_box.One().bounds.width - 1.0F;
    };
    Wait(ui, aligned, "The new conversation heading and composer did not share a column");
    const auto choices = ui.Find(UiSelector::Key("draft-choices"));
    const auto assistant_choice = choices.Find(UiSelector::Key("draft-assistant"));
    const auto has_assistant = [&](std::string_view label) {
      return assistant_choice.Exists() && assistant_choice.Find(UiSelector::Text(std::string(label))).Exists();
    };
    const auto workspace_choice = choices.Find(UiSelector::Key("draft-workspace"));
    const auto group_choice = choices.Find(UiSelector::Key("draft-group"));
    const auto workspace_hint = ui.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Workspace")));
    const auto has_workspace_path = [&] {
      return std::ranges::any_of(workspace_hint.All(), [&](const auto& control) { return control.hint == workspace.Path(); });
    };
    const auto draft_context = [&] {
      if (!assistant_choice.Exists() || !workspace_choice.Exists() || !group_choice.Exists()) return false;
      const auto visibility = [&](const auto& item) {
        const auto node = item.One();
        return node.in_viewport && node.visible_bounds.width >= node.bounds.width - 1.0F &&
            node.bounds.y + node.bounds.height <= compose_box.One().bounds.y;
      };
      return visibility(assistant_choice) && visibility(workspace_choice) && visibility(group_choice);
    };
    Wait(ui, [&] { return draft_context() && has_assistant("General"); },
        "The draft context controls did not fit above the composer");
    ui.SetWindowMetrics({.viewport = {760.0F, 650.0F}});
    Wait(ui, [&] { return aligned() && draft_context(); }, "The medium window did not fit the draft alongside navigation");
    ui.SetWindowMetrics({.viewport = {520.0F, 550.0F}});
    Wait(ui, [&] { return aligned() && draft_context(); }, "The compact draft did not fit its context controls and composer");
    ui.SetWindowMetrics({.viewport = options.viewport});
    Wait(ui, [&] { return draft_context() && workspace_choice.One().enabled &&
        workspace_choice.Find(UiSelector::Text("workspace")).Exists(); }, "The startup workspace selection did not return at desktop width");
    const auto draft_input = ui.Find(UiSelector::Key("new-conversation-composer"));
    const auto permissions = ui.Find(UiSelector::Key("run-permissions"));
    const auto permission_summary = ui.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Permissions")));
    const auto workspace_row = ui.Find(UiSelector::Key("workspace-new/" + workspace_id->value));
    constexpr auto workspace_text = "Keep the workspace draft";
    ChooseDraftAssistant(ui, "PPT");
    Wait(ui, [&] { return permissions.Exists() && permissions.One().in_viewport; }, "The draft permissions did not mount");
    ClickPreviewMouse(ui, permissions.One().visible_bounds);
    menu_item(ui, "Auto Review");
    Wait(ui, [&] { return permission_summary.Exists() &&
        permission_summary.One().value == "Auto Review"; }, "The workspace permissions were not applied");
    FocusPreviewEditor(ui, draft_input, "The workspace draft did not receive focus");
    draft_input.ReplaceText(workspace_text);

    workspace_choice.Tap({.device_kind = PointerDeviceKind::Mouse});
    try {
      menu_item(ui, "No workspace");
    } catch (...) {
      static_cast<void>(root.Child("draft-workspace-menu.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
    Wait(ui, [&] { return workspace_choice.Find(UiSelector::Text("No workspace")).Exists() && !has_workspace_path() &&
        draft_input.One().value == "" && has_assistant("General") &&
        permission_summary.Exists() && permission_summary.One().value == "Ask"; },
        "The standalone draft inherited the workspace text or settings");
    constexpr auto standalone_title = "Standalone navigation check";
    FocusPreviewEditor(ui, draft_input, "The standalone draft did not receive focus");
    draft_input.ReplaceText(standalone_title);
    workspace_row.Tap();
    Wait(ui, [&] { return has_workspace_path() && draft_input.One().value == workspace_text &&
        has_assistant("PPT") && permission_summary.Exists() && permission_summary.One().value == "Auto Review"; },
        "The workspace did not recover its text, assistant and permissions");
    workspace_choice.Tap({.device_kind = PointerDeviceKind::Mouse});
    menu_item(ui, "No workspace");
    Wait(ui, [&] { return draft_input.One().value == standalone_title &&
        has_assistant("General") && permission_summary.Exists() && permission_summary.One().value == "Ask"; },
        "Returning to the standalone draft lost its text or settings");
    FocusPreviewEditor(ui, draft_input, "The restored standalone draft did not receive focus");
    creation->paused = true;
    ui.Find(ButtonText("Send")).Tap({.device_kind = PointerDeviceKind::Mouse});
    try {
      Wait(ui, [&] { return creation->waiting; }, "The standalone draft did not reach its creation commit");
    } catch (...) {
      static_cast<void>(root.Child("draft-creation.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
    workspace_row.Tap();
    Wait(ui, [&] { return has_workspace_path() && draft_input.One().value == workspace_text; },
        "A pending create blocked switching drafts");
    creation->paused = false;
    kit::SessionId standalone;
    auto standalone_entry = ui.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text(standalone_title));
    Wait(ui, [&] { return standalone_entry.Exists(); }, "The completed draft was omitted from the session list");
    inspect(ui, [&]() -> Task<void> {
      const auto listed = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
      const auto row = std::ranges::find(listed.sessions, standalone_title, &kit::SessionSummary::title);
      if (row == listed.sessions.end() || row->workspace || row->agent.id != agentkit_preview::GeneralAgent)
        throw std::runtime_error("The standalone draft inherited the startup workspace or changed its assistant");
      standalone = row->id;
    });
    if (!draft_input.Exists() || draft_input.One().value != workspace_text)
      throw std::runtime_error("The delayed create changed the active workspace draft");
    ui.Find(UiSelector::Key(standalone.value)).Tap();
    Wait(ui, [&] { return ShowsText(ui, "Received: Standalone navigation check"); }, "The created standalone conversation could not be opened");
    workspace_row.Tap();
    Wait(ui, [&] { return workspace_choice.Find(UiSelector::Text("workspace")).Exists() &&
        has_workspace_path() && draft_input.Exists() && draft_input.One().value == workspace_text &&
        draft_input.One().focused && ui.ActiveTextInput().has_value(); },
        "Opening a listed folder did not restore the saved draft and focus");
    for (const auto* name : {"Plans", "Release"}) {
      TapFormButton(ui, "New group");
      save_name(ui, name);
      NavigationGroupId(ui, name);
    }
    OpenNavigationGroupMenu(ui, "Release");
    menu_item(ui, "Move up");
    Wait(ui, [&] { return NavigationGroupLabel(ui, "Release").One().bounds.y < NavigationGroupLabel(ui, "Plans").One().bounds.y; },
        "Group order did not change");
    OpenNavigationGroupMenu(ui, "Plans");
    menu_item(ui, "New conversation in group");
    Wait(ui, [&] { return group_choice.Find(UiSelector::Text("Plans")).Exists(); }, "The new draft did not inherit its group");
    execute_sql(ui, "CREATE TRIGGER reject_navigation_update BEFORE UPDATE ON agentkit_preview_navigation "
                    "BEGIN SELECT RAISE(ABORT, 'Navigation fixture cannot write'); END");
    ChooseDraftAssistant(ui, "Design");
    SendNewConversation(ui, design);
    Wait(ui, [&] { return ui.Find(ButtonText("Review design")).Exists(); }, "The design conversation did not reach its review");
    auto retry = ui.Find(ButtonText("Retry saving groups"));
    auto design_row = PendingConversation(ui, std::string(design));
    const auto plans_key = NavigationGroupId(ui, "Plans");
    auto grouped_design = ui.Find(UiSelector::Key(plans_key)).Find(UiSelector::Text(design));
    Wait(ui, [&] { return retry.Exists() && design_row.Exists() && !grouped_design.Exists(); },
        "A failed group write did not leave the new conversation accessible in the root list");
    TapFormButton(ui, "New group");
    save_name(ui, "Recovery notes");
    if (NavigationGroupLabel(ui, "Recovery notes").Exists()) throw std::runtime_error("A later group edit bypassed the failed save");
    execute_sql(ui, "DROP TRIGGER reject_navigation_update");
    retry.Tap();
    Wait(ui, [&] { return !retry.Exists() && grouped_design.Exists() && NavigationGroupLabel(ui, "Recovery notes").Count() == 1; },
        "Retry did not save the original conversation membership and the queued edit");
    auto composer = ui.Find(UiSelector::Key("composer"));
    composer.Tap();
    composer.ReplaceText("Keep my conversation draft");
    NavigationGroupLabel(ui, "Plans").Tap();
    Wait(ui, [&] { return !design_row.Exists(); }, "The grouped conversation remained in the root list");
    ui.Find(UiSelector::Key("new-conversation")).Tap();
    ChooseDraftAssistant(ui, "PPT");
    SendNewConversation(ui, deck);
    Wait(ui, [&] { return ui.Find(ButtonText("Draft outline")).Exists(); }, "The slide conversation did not request its brief");
    inspect(ui, [&]() -> Task<void> {
      const auto pending = LiveTake(co_await runtime->Interactions().ListPendingAsync({}, {}));
      for (const auto& item : pending.items) {
        if (item.session.title == design) { design_id = item.session.id; design_request = item.interaction.id; }
        if (item.session.title == deck) { deck_id = item.session.id; deck_request = item.interaction.id; }
      }
      if (pending.items.size() != 2 || design_request.value.empty() || deck_request.value.empty())
        throw std::runtime_error("The active requests were not independently addressable");
    });
    const auto pending_navigation = ui.Find(UiSelector::Key("pending-navigation"));
    const auto pending_count = pending_navigation.Find(UiSelector::Key("pending-navigation-count"));
    Wait(ui, [&] { return pending_count.Exists() && pending_count.One().text == "2"; },
        "The sidebar did not count requests before opening the pending view");
    const auto actions = ui.Find(UiSelector::Key("session-actions/" + deck_id.value));
    const auto actions_icon = actions.Find(UiSelector::Key("session-actions-icon"));
    if (!actions.Exists() || !actions_icon.Exists()) throw std::runtime_error("The visible conversation menu target is missing");
    const auto menu_target = actions.One().bounds;
    const auto menu_icon = actions_icon.One().bounds;
    if (menu_target.width < 30.0F || menu_icon.x < menu_target.x + 6.0F ||
        menu_icon.x + menu_icon.width > menu_target.x + menu_target.width - 6.0F)
      throw std::runtime_error("The conversation menu glyph is clipped against its clickable target");
    pending_navigation.Tap();
    auto design_entry = ui.Find(UiSelector::Key("pending/" + design_request.value));
    auto deck_entry = ui.Find(UiSelector::Key("pending/" + deck_request.value));
    Wait(ui, [&] { return design_entry.Exists() && deck_entry.Exists(); }, "The pending page omitted an unopened request");
    design_entry.Tap();
    try {
      auto back = ui.Find(ButtonText("Back to conversation"));
      Wait(ui, [&] { return back.Exists() && back.One().in_viewport; }, "The design request did not open");
      TapFormButton(ui, "Review design");
      TapFormButton(ui, "Approve design");
    } catch (...) {
      static_cast<void>(root.Child("navigation-request.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
    Wait(ui, [&] { return ShowsText(ui, "This request is no longer pending."); }, "The focused request did not settle");
    TapFormButton(ui, "Back to conversation");
    Wait(ui, [&] { return ui.Find(UiSelector::Value("Keep my conversation draft")).Exists(); }, "The pending route lost the conversation draft");
    Wait(ui, [&] { return pending_count.Exists() && pending_count.One().text == "1"; },
        "The sidebar count did not drop after the design approval was resolved");
    pending_navigation.Tap();
    Wait(ui, [&] { return deck_entry.Exists() && deck_entry.One().in_viewport && !design_entry.Exists(); },
        "The pending view kept a completed request");
    deck_entry.Tap({.device_kind = PointerDeviceKind::Mouse});
    auto brief = ui.Find(ButtonText("Draft outline"));
    const auto brief_scroll = ui.Find(UiSelector::Key("ppt-form-scroll"));
    Wait(ui, [&] { return brief.Exists() && brief_scroll.Exists() && brief_scroll.One().in_viewport &&
        !ui.Find(ButtonText("Review design")).Exists(); },
        "The pending item opened a different form");

    ui.SetWindowMetrics({.viewport = {520.0F, 680.0F}});
    auto open_navigation = ui.Find(UiSelector::Key("open-navigation"));
    Wait(ui, [&] { return open_navigation.Exists() && open_navigation.One().in_viewport; },
        "The compact navigation control did not appear");
    open_navigation.Tap();
    auto new_conversation = ui.Find(UiSelector::Key("new-conversation"));
    Wait(ui, [&] { return new_conversation.Exists() && new_conversation.One().in_viewport &&
        new_conversation.One().bounds.x >= 0.0F; }, "The compact conversation drawer did not open");
    new_conversation.Tap();
    auto compact_draft = ui.Find(UiSelector::Key("new-conversation-composer"));
    Wait(ui, [&] { return compact_draft.Exists() && compact_draft.One().focused && ui.ActiveTextInput().has_value(); },
        "The compact drawer did not transfer focus to the new conversation");
    ui.Pump(32ms);
    const auto keyboard = ui.ActiveTextInput();
    constexpr auto typed = "Narrow window input";
    if (!compact_draft.One().focused || !keyboard ||
        ui.SendTextInput({keyboard->session_id, {{.kind = TextInputCommandKind::CommitText, .text = typed}}}).result_code !=
            TextInputResultCode::Ok)
      throw std::runtime_error("The compact drawer restored focus away from the new conversation");
    Wait(ui, [&] { return compact_draft.One().value.value_or("").find(typed) != std::string::npos; },
        "Keyboard input did not reach the new conversation");
  }
  runtime.reset();
  tasks.reset();
  {
    UiTest reopened(app, options);
    auto plans = NavigationGroupLabel(reopened, "Plans");
    Wait(reopened, [&] { return plans.Exists() && runtime && tasks; }, "The conversation groups did not reload");
    if (NavigationGroupLabel(reopened, "Release").One().bounds.y >= plans.One().bounds.y ||
        reopened.Find(UiSelector::Key(design_id.value)).Exists())
      throw std::runtime_error("Saved group order or collapse state changed after reopening");
    plans.Tap();
    auto row = reopened.Find(UiSelector::Key(design_id.value));
    Wait(reopened, [&] { return row.Exists(); }, "The saved group lost its member");
    row.Tap({.device_kind = PointerDeviceKind::Mouse});
    reopened.PressKey(Key::F10, {.shift = true});
    menu_item(reopened, "Rename conversation");
    save_name(reopened, renamed);
    Wait(reopened, [&] { return reopened.Find(UiSelector::AllOf(UiSelector::Key(design_id.value), UiSelector::Text(renamed))).Exists(); },
        "Renaming did not preserve the session identity");
    auto row_menu = reopened.Find(UiSelector::Key("session-actions/" + design_id.value));
    const auto session_action = [&](const char* label) {
      const auto item = reopened.Find(UiSelector::Text(label));
      Wait(reopened, [&] { return !item.Exists(); }, "The previous session actions did not close");
      for (int attempt = 0; attempt < 2 && !item.Exists(); ++attempt) {
        row_menu.Tap({.device_kind = PointerDeviceKind::Mouse});
        reopened.Pump(250ms);
      }
      menu_item(reopened, label);
    };
    kit::RunId design_run;
    kit::Revision source_binding{};
    inspect(reopened, [&]() -> Task<void> {
      const auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(design_id));
      const auto before = chat->Snapshot();
      if (before->summary.agent.id != agentkit_preview::DesignAgent || before->summary.active_run || before->runs.size() != 1)
        throw std::runtime_error("The old design conversation was not idle before transfer");
      design_run = before->runs.back().id;
      source_binding = before->summary.binding_revision;
    });
    auto transfer_draft = reopened.Find(UiSelector::Key("composer"));
    transfer_draft.Tap();
    transfer_draft.ReplaceText("Continue in the same conversation");
    session_action("Transfer to assistant");
    auto general = reopened.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::MenuItem),
        UiSemanticSelector::Label("General assistant"), UiSemanticSelector::Enabled(true)));
    Wait(reopened, [&] { return general.Exists(); }, "The transfer destination did not open");
    general.PerformSemanticAction({SemanticActionKind::Activate, {}});
    Wait(reopened, [&] { return !general.Exists() && ShowsText(reopened, "Assistant changed") &&
        reopened.Find(UiSelector::Value("Continue in the same conversation")).Exists(); },
        "Transfer changed conversations or lost the draft");
    FocusPreviewEditor(reopened, transfer_draft, "The transferred conversation did not restore input focus");
    reopened.Find(ButtonText("Send")).Tap({.device_kind = PointerDeviceKind::Mouse});
    try {
      Wait(reopened, [&] { return ShowsText(reopened, "Received: Continue in the same conversation"); },
          "The replacement assistant did not receive the next turn");
    } catch (...) {
      static_cast<void>(root.Child("navigation-transfer.snapshot").WriteString(reopened.CaptureSnapshot().ToString()));
      throw;
    }
    inspect(reopened, [&]() -> Task<void> {
      const auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(design_id));
      const auto after = chat->Snapshot();
      if (after->summary.id != design_id || after->summary.workspace != workspace_id ||
          after->summary.agent.id != agentkit_preview::GeneralAgent || after->summary.binding_revision != source_binding + 1 ||
          after->runs.size() != 2 || after->runs.front().id != design_run ||
          after->runs.front().agent.id != agentkit_preview::DesignAgent || after->runs.back().agent.id != agentkit_preview::GeneralAgent)
        throw std::runtime_error("Transfer did not preserve the old run and bind the next run to the new assistant");
    });
    const auto plans_key = std::get<std::string>(*plans.One().key).substr(std::string_view("group/").size());
    auto grouped = reopened.Find(UiSelector::Key(plans_key)).Find(UiSelector::Key(design_id.value));
    session_action("Move to group");
    menu_item(reopened, "Ungrouped");
    Wait(reopened, [&] { return row.Exists() && !grouped.Exists(); }, "The conversation did not move to the root list");
    session_action("Move to group");
    auto target_group = reopened.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::MenuItem),
        UiSemanticSelector::Label("Plans"), UiSemanticSelector::Enabled(true)));
    Wait(reopened, [&] { return target_group.Exists(); }, "The group destination did not open");
    target_group.PerformSemanticAction({SemanticActionKind::Activate, {}});
    Wait(reopened, [&] { return grouped.Exists(); }, "The conversation did not move back into the group");
    OpenNavigationGroupMenu(reopened, "Plans");
    menu_item(reopened, "Rename group");
    save_name(reopened, "Product planning");
    Wait(reopened, [&] { return NavigationGroupLabel(reopened, "Product planning").Exists(); }, "The group did not update its name");
    OpenNavigationGroupMenu(reopened, "Product planning");
    menu_item(reopened, "Remove group");
    Wait(reopened, [&] { return !NavigationGroupLabel(reopened, "Product planning").Exists() && row.Exists(); },
        "Removing the group also removed its conversation");
    session_action("Delete conversation");
    TapFormButton(reopened, "Delete");
    Wait(reopened, [&] { return !row.Exists() && !reopened.Find(ButtonText("Delete")).Exists(); },
        "The deleted conversation or its confirmation stayed open");
    inspect(reopened, [&]() -> Task<void> {
      const auto pending = LiveTake(co_await runtime->Interactions().ListPendingAsync({}, {}));
      if (pending.items.size() != 1 || pending.items.front().interaction.id != deck_request ||
          pending.items.front().session.workspace != workspace_id)
        throw std::runtime_error("Navigation changed another conversation's request or workspace");
    });
    CheckText(workspace.Child("keep.txt"), "Workspace contents");
    reopened.SetWindowMetrics({.viewport = {560.0F, 940.0F}});
    reopened.Pump();
    auto navigation = reopened.Find(UiSelector::Key("open-navigation"));
    auto pending_button = reopened.Find(UiSelector::Key("pending-navigation"));
    Wait(reopened, [&] { return navigation.Exists() && navigation.One().in_viewport &&
        (!pending_button.Exists() || !pending_button.One().in_viewport); }, "The narrow window kept the sidebar open");
    try {
      navigation.Tap();
      Wait(reopened, [&] {
        if (!pending_button.Exists()) return false;
        const auto button = pending_button.One();
        return button.enabled && button.in_viewport && button.visible_bounds.width >= button.bounds.width - 0.1F;
      }, "The navigation drawer did not open");
    } catch (...) {
      static_cast<void>(root.Child("navigation-drawer.snapshot").WriteString(reopened.CaptureSnapshot().ToString()));
      throw;
    }
    pending_button.Tap();
    auto remaining = reopened.Find(UiSelector::Key("pending/" + deck_request.value));
    Wait(reopened, [&] { return remaining.Exists() && remaining.One().in_viewport &&
        (!pending_button.Exists() || !pending_button.One().in_viewport); }, "Opening Pending did not dismiss the narrow navigation drawer");
    try {
      reopened.PumpAndSettle();
      remaining.Tap();
      const auto submit = reopened.Find(ButtonText("Draft outline"));
      Wait(reopened, [&] { return submit.Count() == 1 && submit.One().in_viewport && submit.One().enabled; },
          "The narrow window did not open the remaining request");
    } catch (...) {
      static_cast<void>(root.Child("navigation-narrow-request.snapshot").WriteString(reopened.CaptureSnapshot().ToString()));
      throw;
    }
  }
}

void VerifyNavigationConcurrency(const File& root) {
  const auto database = root.Child("sessions.sqlite3");
  const Application app{App, {.application_hooks = {MakePreviewInstaller(database, false)}}};
  const UiTestOptions options{.viewport = {1120.0F, 940.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  UiTest first(app, options);
  const auto menu = [](UiTest& window, const char* label) {
    auto item = window.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::MenuItem),
        UiSemanticSelector::Label(label), UiSemanticSelector::Enabled(true)));
    Wait(window, [&] { return item.Exists(); }, label);
    item.PerformSemanticAction({SemanticActionKind::Activate, {}});
  };
  const auto rename = [&](UiTest& window, const char* group) {
    OpenNavigationGroupMenu(window, group);
    menu(window, "Rename group");
  };
  const auto reactivate = [](UiTest& window) {
    window.UpdateWindowLifecycleState(WindowLifecycleState::Inactive);
    window.Pump();
    window.UpdateWindowLifecycleState(WindowLifecycleState::Active);
    window.Pump();
  };

  const auto new_group = first.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
      UiSemanticSelector::Label("New group"), UiSemanticSelector::Enabled(true)));
  for (const auto* group : {"Specifications", "Releases"}) {
    Wait(first, [&] { return new_group.Count() == 1 && !new_group.One().offscreen; }, "New group was not available");
    new_group.PerformSemanticAction({SemanticActionKind::Activate, {}});
    SaveNavigationName(first, group);
    NavigationGroupId(first, group);
  }
  SendNewConversation(first, "Shared navigation session");
  Wait(first, [&] { return ShowsText(first, "Received: Shared navigation session") && !first.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
      "The shared conversation was not committed");
  auto row = first.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text("Shared navigation session"));
  const auto session = std::get<std::string>(*row.One().key);
  UiTest second(app, options);
  Wait(second, [&] { return second.Find(UiSelector::Key(session)).Exists(); }, "The other window did not load the original session");
  const auto discard = [&](const char* message) {
    const auto action = second.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
        UiSemanticSelector::Label("Discard unsaved change")));
    Wait(second, [&] { return action.Count() == 1 && action.One().enabled && !action.One().offscreen; }, message);
    action.PerformSemanticAction({SemanticActionKind::Activate, {}});
    Wait(second, [&] { return !action.Exists(); }, message);
  };

  rename(first, "Specifications");
  rename(second, "Releases");
  SaveNavigationName(first, "Active specifications");
  SaveNavigationName(second, "Shipped releases");
  NavigationGroupId(first, "Active specifications");
  NavigationGroupId(second, "Shipped releases");
  reactivate(first);
  reactivate(second);
  NavigationGroupId(first, "Shipped releases");
  NavigationGroupId(second, "Active specifications");

  rename(first, "Active specifications");
  rename(second, "Active specifications");
  SaveNavigationName(first, "Decisions");
  NavigationGroupId(first, "Decisions");
  SaveNavigationName(second, "Stale specifications");
  Wait(second, [&] { return ShowsText(second, "This group was renamed in another window") && NavigationGroupLabel(second, "Decisions").Exists(); },
      "A stale rename replaced a committed name or did not refresh the groups");
  discard("Discarding the rename did not settle");
  rename(second, "Decisions");
  SaveNavigationName(second, "Design decisions");
  const auto group = NavigationGroupId(second, "Design decisions");
  reactivate(first);
  NavigationGroupId(first, "Design decisions");

  for (auto* window : {&first, &second}) {
    TapControl(*window, UiSelector::Key("session-actions/" + session), "Session menu");
    menu(*window, "Move to group");
  }
  menu(first, "Design decisions");
  const auto membership = UiSelector::Key(group);
  Wait(first, [&] { return first.Find(membership).Find(UiSelector::Key(session)).Exists(); }, "The first window did not move the conversation");
  menu(second, "Shipped releases");
  Wait(second, [&] { return ShowsText(second, "This conversation moved to another group") &&
      second.Find(membership).Find(UiSelector::Key(session)).Exists(); }, "A move from the old group state displaced the original result");
  discard("Discarding the move did not settle");
  UiTest reopened(app, options);
  auto reopened_group = reopened.Find(membership);
  Wait(reopened, [&] { return reopened_group.Exists() && reopened_group.Find(UiSelector::Key(session)).Exists() &&
      NavigationGroupLabel(reopened, "Shipped releases").Exists(); }, "Independent group edits or the winning move did not survive reopening");
}

void VerifyReleaseSkill(const File& root, bool selected_by_user) {
  using namespace agentkit_preview;
  const auto workspace = root.Child("project");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create a Skill acceptance workspace");
  const auto output = root.Child("exports").Child("Presentation.pptx");
  const std::string report = "Release deck: Verified settings\nAll 7 tests passed. Settings were checked in the workspace.";
  const std::string title = "Release deck: Verified settings";
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  auto start = [&](auto started, auto, auto) -> Task<void> {
    runtime = LiveTake(std::move(started));
    tasks = UseApplicationTaskScope();
    co_return;
  };
  const Application app{App, {.application_hooks = {MakeRunSettingsTestInstaller(
      root.Child("sessions.sqlite3"), workspace, {}, start)}}};
  const UiTestOptions options{.viewport = {1120.0F, 920.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()};
  const auto close = [&](UiTest& preview) {
    bool closed = false;
    tasks->Launch([&]() -> Task<void> {
      LiveTake(co_await runtime->CloseAsync());
      closed = true;
    });
    Wait(preview, [&] { return closed; }, "Preview could not close while waiting for an approval");
  };
  const auto snapshot = [&](UiTest& preview) {
    Wait(preview, [&] { return tasks && runtime; }, "Skill runtime did not start");
    std::shared_ptr<const kit::SessionSnapshot> data;
    tasks->Launch([&]() -> Task<void> {
      const auto sessions = LiveTake(co_await runtime->Sessions().ListAsync({.agent = GeneralAgent}, {}));
      const auto found = std::ranges::find(sessions.sessions, title, &kit::SessionSummary::title);
      if (found == sessions.sessions.end()) co_return;
      auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(found->id));
      data = chat->Snapshot();
    });
    Wait(preview, [&] { return bool(data); }, "Skill session snapshot was not available");
    return data;
  };
  const auto approval = [](const kit::SessionSnapshot& snapshot, std::string_view expected) {
    const auto found = std::ranges::find_if(snapshot.interactions, [&](const auto& item) {
      if (item.state != kit::InteractionState::Pending) return false;
      if (const auto* tool = std::get_if<kit::ToolApproval>(&item.body)) return tool->preview.title == expected;
      if (const auto* operation = std::get_if<kit::OperationApproval>(&item.body)) return operation->preview.title == expected;
      return false;
    });
    if (found == snapshot.interactions.end()) throw std::runtime_error("Expected Skill workflow approval is unavailable");
    return *found;
  };
  kit::RunId original;
  std::optional<kit::InteractionId> activation;
  kit::InteractionId export_approval;
  kit::SkillRef skill;
  const auto review = [&](UiTest& preview) {
    auto audience = preview.Find(UiSelector::Key("ppt-audience"));
    Wait(preview, [&] { return audience.Exists(); }, "The General Skill did not open a presentation brief");
    audience.Tap();
    audience.ReplaceText("Release team");
    TapFormButton(preview, "Draft outline");
    auto first = preview.Find(UiSelector::Key("slide-title-0"));
    Wait(preview, [&] { return first.Exists() && first.One().value == "Verification evidence"; },
         "The model did not draft the first section of the actual template");
    first.Tap();
    first.ReplaceText("Verified settings for release");
    TapFormButton(preview, "Next slide");
    auto second = preview.Find(UiSelector::Key("slide-title-1"));
    Wait(preview, [&] { return second.Exists() && second.One().value == "Release decision"; },
         "The model did not draft the second section of the actual template");
    TapFormButton(preview, "Confirm outline");
    TapFormButton(preview, "Review export");
    Wait(preview, [&] { return preview.Find(UiSelector::Text("Export PowerPoint")).Exists(); },
         "The Skill did not prepare normal export approval");
    if (output.Exists()) throw std::runtime_error("Skill exported the presentation before approval");
    const auto view = snapshot(preview);
    const auto& run = view->runs.back();
    if (run.id != original || run.active_skills.size() != 1 ||
        run.active_skills.front().trigger != (selected_by_user ? kit::SkillActivationTrigger::User : kit::SkillActivationTrigger::Model))
      throw std::runtime_error("Presentation used a different Run or activation trigger");
    const auto report_reads = std::ranges::count_if(view->tools, [](const auto& tool) {
      return tool.tool.id == ReadReport && tool.state == kit::ToolCallState::Succeeded;
    });
    if (report_reads != 1) throw std::runtime_error("The Skill did not read the report exactly once");
    skill = run.active_skills.front().skill;
    export_approval = approval(*view, "Export PowerPoint").id;
  };

  {
    UiTest preview(app, options);
    if (selected_by_user) {
      auto picker = preview.Find(UiSelector::Key("composer-skills"));
      Wait(preview, [&] { return picker.Exists(); }, "The new General draft did not offer Skills");
      picker.Tap();
      auto choice = preview.Find(UiSelector::Text("Release presentation"));
      Wait(preview, [&] { return choice.Exists(); }, "The packaged release Skill was not discoverable");
      choice.Tap();
      Wait(preview, [&] { return preview.Find(ButtonText("Skill: Release presentation")).Exists(); },
           "The chosen version was not attached to the draft");
    }
    SendNewConversation(preview, report);
    if (selected_by_user) {
      Wait(preview, [&] { return preview.Find(UiSelector::Key("ppt-audience")).Exists(); }, "Selected Skill did not start the brief");
      original = snapshot(preview)->runs.back().id;
      review(preview);
    } else {
      Wait(preview, [&] { return preview.Find(UiSelector::Text("Activate Skill")).Exists(); }, "The model did not propose Skill activation");
      auto view = snapshot(preview);
      original = view->runs.back().id;
      activation = approval(*view, "Activate Skill").id;
      if (!view->runs.back().active_skills.empty()) throw std::runtime_error("Model activated the Skill without confirmation");
    }
    close(preview);
  }
  if (!selected_by_user) {
    runtime.reset();
    tasks.reset();
    UiTest preview(app, options);
    auto row = PendingConversation(preview, title);
    Wait(preview, [&] { return row.Exists(); }, "Pending Skill session did not reopen");
    row.Tap();
    Wait(preview, [&] { return preview.Find(UiSelector::Text("Activate Skill")).Exists(); }, "Activation approval was lost across restart");
    if (approval(*snapshot(preview), "Activate Skill").id != activation)
      throw std::runtime_error("Restart replaced the Skill approval");
    preview.Find(ButtonText("Approve")).Tap();
    review(preview);
    close(preview);
  }

  runtime.reset();
  tasks.reset();
  UiTest preview(app, options);
  auto row = PendingConversation(preview, title);
  Wait(preview, [&] { return row.Exists(); }, "Skill export session did not reopen");
  row.Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Export PowerPoint")).Exists(); }, "Skill export approval was lost across restart");
  auto restored = snapshot(preview);
  if (restored->runs.back().id != original || restored->runs.back().active_skills.front().skill != skill ||
      approval(*restored, "Export PowerPoint").id != export_approval || output.Exists())
    throw std::runtime_error("Restore changed the pinned Skill or export approval");
  preview.Find(ButtonText("Approve")).Tap();
  Wait(preview, [&] { return output.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Skill did not export the approved PPTX");
  const auto finished = snapshot(preview);
  if (finished->runs.back().state != kit::RunState::Succeeded)
    throw std::runtime_error("The release Skill did not settle successfully");
  auto deck = archive::ArchiveReader::Open(output);
  if (!deck.Succeeded()) throw std::runtime_error(deck.Error().message);
  if (ReadPart(deck.Value(), "ppt/slides/slide1.xml").find("Verified settings for release") == std::string::npos ||
      ReadPart(deck.Value(), "ppt/slides/slide1.xml").find("All 7 tests passed") == std::string::npos ||
      ReadPart(deck.Value(), "ppt/slides/slide2.xml").find("Release decision") == std::string::npos)
    throw std::runtime_error("The PPTX did not contain the template sections, edited title and report");
}

void ShareWorkspaceScope(UiTest& preview) {
  auto paths = preview.Find(UiSelector::Key("code-scope"));
  Wait(preview, [&] { return paths.Exists(); }, "Code scope form did not mount");
  paths.Tap();
  paths.ReplaceText("config.txt\nsettings.html");
  preview.Find(ButtonText("Share files")).Tap();
}

void SubmitWorkspaceScope(UiTest& preview) {
  ShareWorkspaceScope(preview);
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Review workspace changes")).Exists(); },
       "The code patch did not reach its approval");
}

void PrepareCodeChange(UiTest& preview, std::string title) {
  ChooseDraftAssistant(preview, "Code");
  SendNewConversation(preview, std::move(title));
  SubmitWorkspaceScope(preview);
}

Task<kit::InteractionSnapshot> StageReviewPresentation(const std::shared_ptr<kit::Conversation>& chat, std::string filename,
    std::string verification = "Share verification evidence") {
  using kit::JsonValue;
  auto brief = co_await LiveInteraction(chat, "Presentation brief");
  JsonValue answers{JsonValue::Object{{"audience", JsonValue{std::string("The project team")}},
      {"style", JsonValue{std::string("Concise")}}, {"length", JsonValue{std::string("3 slides")}},
      {"reference_notes", JsonValue{std::string("Set up a versioned document for review")}}}};
  LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {brief.id, brief.version, std::move(answers)}));
  auto edit = co_await LiveInteraction(chat, "Review and edit outline");
  auto outline = LiveTake(agentkit_preview::ReadPresentationOutline(LiveForm(edit).at("outline")));
  outline.slides = {{"First draft", {"Scope and ownership"}}, {"Reviewed decisions", {"Confirm the implementation"}},
      {"Release notes", {std::move(verification)}}};
  const auto confirmed = agentkit_preview::WritePresentationOutline(outline);
  LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {edit.id, edit.version, confirmed}));
  auto preview = co_await LiveInteraction(chat, "Preview slides and choose a file name");
  if (LiveForm(preview).at("outline") != confirmed) throw std::runtime_error("The staged deck lost its confirmed outline");
  JsonValue destination{JsonValue::Object{{"file_name", JsonValue{std::move(filename)}}}};
  LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {preview.id, preview.version, std::move(destination)}));
  co_return co_await LiveInteraction(chat, "Export PowerPoint");
}

constexpr auto MarkdownFirstDelta = "## Streamed heading\r\n\r\nThe model is streaming";
constexpr auto MarkdownReportUrl = "https://example.test/report?part=2#verified";
constexpr auto MarkdownSelectionWord = "\xe6\x96\xb9\xe6\xa1\x88\xf0\x9f\x98\x80";
constexpr auto MarkdownSecondDelta = " another delta\n\n**\xe6\x96\xb9\xe6\xa1\x88\xf0\x9f\x98\x80** "
    "[\xe6\x96\xb9\xe6\xa1\x88\xf0\x9f\x98\x80](https://example.test/unselected) &amp;\n\n```cpp\nconst int ready = 1;\n```\n\n"
    "| Check | Result |\n| --- | --- |\n| Render | Pass |\n\n"
    "[Verification report](https://example.test/report?part=2#verified)\n\n[Local path](file:///report.txt)\n";

class ComposerStream final : public kit::ModelTurnStream {
public:
  ComposerStream(std::shared_ptr<std::atomic<int>> released, std::stop_token stop, std::string tail, bool routing)
      : released_(std::move(released)), stop_(stop), tail_(std::move(tail)), routing_(routing) {}

  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    using Result = kit::AgentResult<std::optional<kit::ModelEvent>>;
    const auto next = next_++;
    if (routing_) {
      switch (next) {
        case 0: co_return Result::Success(kit::ModelTextDelta{R"({"internal":"route"})", false});
        case 1: co_return Result::Success(kit::ModelStructuredOutput{kit::JsonValue{kit::JsonValue::Object{
            {"route", kit::JsonValue{std::string("answer")}}}}});
        case 2: co_return Result::Success(kit::ModelFinished{kit::ModelFinishReason::Completed, {}});
        default: co_return Result::Success(std::nullopt);
      }
    }
    switch (next) {
      case 0: co_return Result::Success(kit::ModelTextDelta{MarkdownFirstDelta});
      case 1:
        while (released_->load() == 0 && !stop_.stop_requested()) co_await Delay(10ms);
        if (stop_.stop_requested()) co_return Result::Success(std::nullopt);
        co_return Result::Success(kit::ModelTextDelta{MarkdownSecondDelta + tail_});
      case 2:
        while (released_->load() < 2 && !stop_.stop_requested()) co_await Delay(10ms);
        if (stop_.stop_requested()) co_return Result::Success(std::nullopt);
        co_return Result::Success(kit::ModelFinished{kit::ModelFinishReason::Completed, {}});
      default: co_return Result::Success(std::nullopt);
    }
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  std::shared_ptr<std::atomic<int>> released_;
  std::stop_token stop_;
  std::string tail_;
  bool routing_;
  int next_ = 0;
};

class ComposerModel final : public kit::ModelProvider {
public:
  explicit ComposerModel(std::shared_ptr<std::atomic<int>> released, std::shared_ptr<std::vector<std::string>> passages = {},
                         std::string tail = {})
      : released_(std::move(released)), passages_(std::move(passages)), tail_(std::move(tail)) {}

  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .structured_output = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    if (passages_) {
      for (const auto& item : request.items) {
        const auto* context = std::get_if<kit::ModelContent>(&item);
        const auto* text = context ? std::get_if<std::string>(&context->body) : nullptr;
        if (text && text->starts_with("Selected passage:\n")) passages_->push_back(*text);
      }
    }
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(
        std::make_unique<ComposerStream>(released_, request.stop, tail_, request.output_schema.has_value()));
  }

private:
  std::shared_ptr<std::atomic<int>> released_;
  std::shared_ptr<std::vector<std::string>> passages_;
  std::string tail_;
};

void VerifyComposerIme(const File& root) {
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the IME workspace");
  const auto released = std::make_shared<std::atomic<int>>(0);
  kit::ModelRegistration model{{{"composer-stream"}, 1}, {"compatible", "Google/GEMINI-2.5-Pro", "script", {}},
      std::make_shared<ComposerModel>(released)};
  const Application application{App, {.application_hooks = {
      MakeRunSettingsTestInstaller(root.Child("sessions.sqlite3"), workspace, {std::move(model)},
          [](auto started, auto, auto) -> Task<void> {
            LiveTake(std::move(started));
            co_return;
          })}}};
  UiTest ui(application, {.viewport = {1120.0F, 820.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  auto send = ui.Find(ButtonText("Send"));
  auto stop = ui.Find(UiSelector::Key("agent-chat-stop"));
  auto first = ui.Find(UiSelector::Key("new-conversation-composer"));
  auto composer = ui.Find(UiSelector::Key("composer"));
  auto send_mode = ui.Find(UiSelector::Key("send-mode"));
  const std::string chinese = "\xe4\xbd\xa0\xe5\xa5\xbd";
  const std::string prefix = "A\xf0\x9f\x98\x80";
  auto edit = [&](TextInputCommand command) {
    const auto active = ui.ActiveTextInput();
    if (!active || ui.SendTextInput({active->session_id, {std::move(command)}}).result_code != TextInputResultCode::Ok)
      throw std::runtime_error("The composer did not accept an IME command");
    ui.Pump();
  };
  Wait(ui, [&] { return first.Exists(); }, "New conversation composer did not mount");
  const auto composer_model_icon = ui.Find(UiSelector::Key("run-model-icon"));
  Wait(ui, [&] { return composer_model_icon.Exists() && composer_model_icon.One().in_viewport; },
       "The selected model icon was missing from the new conversation composer");
  ui.SetWindowMetrics({.viewport = {520.0F, 650.0F}});
  Wait(ui, [&] { return ui.Find(UiSelector::Key("open-navigation")).Exists(); }, "The compact layout did not mount");
  const auto menu_items = [&](const char* label) {
    return ui.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::MenuItem), UiSemanticSelector::Label(label)));
  };
  const auto check_anchor = [&](const char* key, const char* item_label) {
    const auto trigger = ui.Find(UiSelector::Key(key));
    Wait(ui, [&] { return trigger.Exists() && trigger.One().in_viewport; }, "The composer setting did not mount");
    ClickPreviewMouse(ui, trigger.One().visible_bounds);
    const auto item = menu_items(item_label);
    Wait(ui, [&] { return item.Exists() && !item.One().offscreen; }, item_label);
    if (item.One().bounds.y >= trigger.One().bounds.y)
      throw std::runtime_error("The setting menu did not open above its control");
    ui.PressKey(Key::Escape);
    Wait(ui, [&] { return !item.Exists(); }, "Escape did not dismiss the setting menu");
    ui.Pump(250ms);
  };
  check_anchor("run-model", "Google/GEMINI-2.5-Pro");
  check_anchor("run-permissions", "Auto Review");
  FocusPreviewEditor(ui, first, "The first composer did not receive focus");
  first.ReplaceText("Stream review");
  ui.PressKey(Key::Enter, {.shift = true});
  if (first.One().value != "Stream review\n") throw std::runtime_error("Shift+Enter did not insert a new line");
  edit({.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"});
  if (send.One().enabled || composer.Exists()) throw std::runtime_error("The first message can send provisional IME text");
  ui.PressKey(Key::Enter);
  if (composer.Exists()) throw std::runtime_error("Enter sent provisional IME text");
  edit({.kind = TextInputCommandKind::CommitText, .text = chinese});
  if (first.One().value != "Stream review\n" + chinese) throw std::runtime_error("The first composer lost committed text");
  Wait(ui, [&] { return send.One().enabled; }, "The committed first message cannot be sent");
  ui.PressKey(Key::Enter);
  Wait(ui, [&] { return composer.Exists() && stop.Exists() && send_mode.Exists() && ShowsText(ui, "The model is streaming"); },
       "The first model delta did not appear");
  if (!composer_model_icon.Exists() || !composer_model_icon.One().in_viewport)
    throw std::runtime_error("The selected model icon was missing from the active conversation composer");
  const auto user_bubble = ui.Find(UiSelector::Key("chat-user-bubble"));
  const auto user_avatar = ui.Find(UiSelector::Key("chat-user-avatar"));
  const auto agent_avatar = ui.Find(UiSelector::Key("chat-agent-avatar"));
  const auto model_avatar = ui.Find(UiSelector::Key("chat-model-avatar"));
  if (!user_avatar.Exists() || !agent_avatar.Exists() ||
      ui.Find(UiSelector::AllOf(UiSelector::Type<Text>(), UiSelector::Text("You"))).Exists() ||
      ui.Find(UiSelector::AllOf(UiSelector::Type<Text>(), UiSelector::Text("Assistant"))).Exists() ||
      ui.Find(UiSelector::AllOf(UiSelector::Type<Text>(), UiSelector::Text("General assistant"))).Exists())
    throw std::runtime_error("Chat identities should appear as avatars");
  if (!model_avatar.Exists()) throw std::runtime_error("The gateway model family did not supply an avatar");
  if (user_bubble.One().bounds.width >= 180.0F ||
      user_bubble.One().bounds.x + user_bubble.One().bounds.width >= user_avatar.One().bounds.x)
    throw std::runtime_error("A short outgoing message did not fit its text beside the right avatar");
  const auto original_send = send.One().bounds;
  const auto options = ui.Find(UiSelector::Key("run-model"));
  const auto original_options = options.One().bounds;
  const auto same_box = [](Rect left, Rect right) {
    return std::abs(left.x - right.x) < 1.0F && std::abs(left.y - right.y) < 1.0F &&
        std::abs(left.width - right.width) < 1.0F && std::abs(left.height - right.height) < 1.0F;
  };
  const auto mode = [&](const char* label, const char* send_label) {
    ClickPreviewMouse(ui, send_mode.One().visible_bounds);
    auto item = ui.Find(UiSelector::Text(label));
    Wait(ui, [&] { return item.Exists() && item.One().in_viewport && item.One().enabled; }, "Send mode did not open");
    item.Tap({.device_kind = PointerDeviceKind::Mouse});
    auto button = ui.Find(ButtonText(send_label));
    Wait(ui, [&] { return button.Exists() && button.One().in_viewport && !item.Exists(); }, "Send mode did not change");
    if (!same_box(original_send, button.One().bounds) || !same_box(original_options, options.One().bounds))
      throw std::runtime_error("Changing send mode moved the compact toolbar");
  };
  mode("Queue", "Queue message");
  mode("Send now", "Send");
  FocusPreviewEditor(ui, composer, "The running composer did not receive focus");
  composer.ReplaceText("One\nTwo\nThree\nFour");
  check_anchor("run-model", "Local demo model");
  if (!ShowsText(ui, "Streamed heading") || ShowsText(ui, MarkdownFirstDelta) ||
      ui.Find(UiSelector::Key("markdown-source-toggle")).Exists()) {
    throw std::runtime_error("The unfinished reply did not render its heading or exposed source selection");
  }
  if (ShowsText(ui, R"({"internal":"route"})"))
    throw std::runtime_error("A model payload marked for the host appeared in the conversation");

  FocusPreviewEditor(ui, composer, "The composer did not recover focus after closing the menu");
  composer.ReplaceText(prefix + "B");
  composer.SetSelection({3, 3});
  edit({.kind = TextInputCommandKind::UpdateComposition, .selection_after = TextSelection{5, 4}, .text = "ni"});
  const auto composing = ui.ActiveTextInput();
  if (!composing || composing->composition != TextRange{3, 5} || composing->selection != TextSelection{5, 4})
    throw std::runtime_error("The composer did not retain the UTF-16 composition and selection");
  if (send.One().enabled) throw std::runtime_error("Active Run allows provisional text to be sent");
  released->store(1);
  Wait(ui, [&] { return ShowsText(ui, "The model is streaming another delta"); }, "The gated model delta did not render");
  if (!ShowsText(ui, "Render") || !ShowsText(ui, "Pass") || ShowsText(ui, "```text"))
    throw std::runtime_error("The streaming Markdown did not render the table and code as formatted content");
  const auto streamed = ui.ActiveTextInput();
  if (!streamed || streamed->session_id != composing->session_id || streamed->composition != composing->composition ||
      streamed->selection != composing->selection || !composer.One().focused || composer.One().value != prefix + "niB")
    throw std::runtime_error("A streaming update changed the draft, selection or focus");

  edit({.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"});
  released->store(2);
  Wait(ui, [&] { return !stop.Exists() && !send_mode.Exists() && ShowsText(ui, "The model is streaming another delta"); },
       "The Run did not finish while composing");
  if (!same_box(original_send, send.One().bounds) || !same_box(original_options, options.One().bounds))
    throw std::runtime_error("Finishing the Run moved the compact toolbar");
  const auto finished = ui.ActiveTextInput();
  if (!finished || finished->session_id != composing->session_id || finished->composition != TextRange{3, 8} ||
      finished->selection != TextSelection{8, 8} || !composer.One().focused || send.One().enabled)
    throw std::runtime_error("Ending the Run changed or allowed submission of the active composition");
  edit({.kind = TextInputCommandKind::CancelComposition});
  if (composer.One().value != prefix + "B" || ui.ActiveTextInput()->selection != TextSelection{3, 3})
    throw std::runtime_error("Cancel after streaming did not restore the original draft and caret");
  edit({.kind = TextInputCommandKind::UpdateComposition, .text = "nihao"});
  edit({.kind = TextInputCommandKind::CommitText, .text = chinese});
  if (composer.One().value != prefix + chinese + "B" || !send.One().enabled)
    throw std::runtime_error("The committed Chinese draft is not ready to send");
  ClickPreviewMouse(ui, options.One().visible_bounds);
  const auto demo = menu_items("Local demo model");
  Wait(ui, [&] { return demo.Exists() && !demo.One().offscreen; }, "The model menu did not show the demo model");
  demo.PerformSemanticAction({SemanticActionKind::Activate, {}});
  Wait(ui, [&] { return !demo.Exists() && !composer_model_icon.Exists(); },
       "Selecting the demo model did not dismiss the menu and clear the composer brand");
  if (!model_avatar.Exists()) throw std::runtime_error("Changing the composer model replaced the reply's original avatar");
  FocusPreviewEditor(ui, composer, "Model selection did not leave an editable composer");
  ui.PressKey(Key::Enter);
  Wait(ui, [&] { return ShowsText(ui, prefix + chinese + "B") && composer.One().value == ""; },
       "Sending did not publish or clear the committed Chinese draft");
  const auto title = ui.Find(UiSelector::Key("agent-chat-heading"));
  const auto history = ui.Find(UiSelector::Key("agent-chat-history"));
  const auto box = ui.Find(UiSelector::Key("agent-chat-composer"));
  const auto area = ui.Find(UiSelector::Key("preview-content"));
  const auto aligned = [&] {
    const auto lane = box.One().bounds;
    const auto surface = area.One().bounds;
    const auto transcript = history.One().bounds;
    return title.One().in_viewport && box.One().in_viewport && history.One().in_viewport &&
        std::abs(lane.x - title.One().bounds.x) < 1.0F &&
        std::abs(lane.x - transcript.x) < 1.0F &&
        std::abs(lane.width - title.One().bounds.width) < 1.0F &&
        std::abs(lane.width - transcript.width) < 1.0F &&
        std::abs(lane.x + lane.width * 0.5F - surface.x - surface.width * 0.5F) < 1.0F &&
        box.One().visible_bounds.width >= lane.width - 1.0F;
  };
  Wait(ui, aligned, "Conversation title, transcript and composer did not share a centered column");
  ui.SetWindowMetrics({.viewport = {1700.0F, 900.0F}});
  Wait(ui, [&] { return aligned() && box.One().bounds.width > 1000.0F; },
       "The conversation did not expand with the wide window");
  Wait(ui, [&] { return agent_avatar.All().size() == 2 && model_avatar.All().size() == 1; },
       "Historical and unidentified model replies did not retain their respective avatars");
  ui.SetWindowMetrics({.viewport = {520.0F, 550.0F}});
  Wait(ui, aligned, "The compact conversation column did not fit the window");
}

void VerifyMarkdownImageTransport(std::string origin) {
  std::exception_ptr failure;
  bool finished = false;
  Application application{[]() -> View { return Text("Preview Markdown image transport"); },
      {.application_hooks = {[&](ApplicationContext&) {
        const auto host = UseApplication();
        UseApplicationTaskScope().Launch([&, host]() -> Task<void> {
          try {
            const auto loader = agentkit_preview::MakePreviewImageLoader();
            const auto asset = co_await loader->load({.destination = origin + "/diagram.png"});
            if (!asset.HasValue() || asset.PixelWidth() != 32 || asset.PixelHeight() != 20)
              throw std::runtime_error("The native image loader did not retrieve the PNG");
            for (const auto& destination : {origin + "/missing.png", origin + "/damaged.png", std::string("file:///private.png")}) {
              const auto refused = co_await loader->load({.destination = destination});
              if (refused.HasValue()) throw std::runtime_error("The native image loader accepted an invalid image");
            }
            finished = true;
          } catch (...) { failure = std::current_exception(); }
          host.Quit();
        });
      }}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  if (!finished) throw std::runtime_error("The native image transport test did not finish");
}

void VerifyMarkdownSource(const File& root, ImageAsset image = {}) {
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the Markdown test workspace");
  const auto released = std::make_shared<std::atomic<int>>(1);
  const auto passages = std::make_shared<std::vector<std::string>>();
  const bool verify_images = image.HasValue();
  const auto image_gate = std::make_shared<std::atomic<bool>>(false);
  const auto image_requests = std::make_shared<std::vector<std::string>>();
  const std::string image_tail = verify_images ?
      "\n![Delayed diagram](https://image.test/diagram.png)\n\n![Missing diagram](https://image.test/missing.png)\n\n"
      "![Damaged diagram](https://image.test/damaged.png)\n\n![Local diagram](file:///private.png)\n\n"
      "Before ![Inline icon](https://image.test/inline.png) after.\n\n**Following text**\n" : "";
  kit::ModelRegistration model{{{"composer-stream"}, 1}, {"preview", "composer-stream", "script", {}},
      std::make_shared<ComposerModel>(released, passages, image_tail)};
  const auto allowed = std::make_shared<bool>(true);
  const auto visits = std::make_shared<std::vector<kit::AgentChatLink>>();
  const auto preview_links = agentkit_preview::MakePreviewLinkHandler();
  auto links = std::make_shared<kit::AgentChatLinkHandler>(kit::AgentChatLinkHandler{
      .allows = [allowed, preview_links](const kit::AgentChatLink& link) {
        return *allowed && (preview_links ? preview_links->allows(link) : link.destination == MarkdownReportUrl);
      },
      .open = [visits](const kit::AgentChatLink& link) {
        visits->push_back(link);
        using Result = kit::AgentResult<void>;
        if (visits->size() == 1) return Result::Failure({kit::ErrorCode::InvalidState, "Browser unavailable"});
        return Result::Success();
      }});
  const auto database = root.Child("sessions.sqlite3");
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  const Application application{App, {.application_hooks = {
      MakeRunSettingsTestInstaller(database, workspace, {std::move(model)},
          [&](auto started, auto, auto) -> Task<void> {
            runtime = LiveTake(std::move(started));
            tasks = UseApplicationTaskScope();
            co_return;
          }),
      [links, image, image_gate, image_requests](ApplicationContext& context) {
        context.Provide(links);
        if (image.HasValue()) context.Provide(std::make_shared<kit::AgentChatImageLoader>(
            [image, image_gate, image_requests](kit::AgentChatImage request) -> Task<ImageAsset> {
              image_requests->push_back(request.destination);
              if (request.destination == "https://image.test/inline.png") co_return image;
              if (request.destination == "https://image.test/diagram.png") {
                while (!image_gate->load()) co_await Delay(10ms);
                co_return image;
              }
              co_return ImageAsset{};
            }));
      }}}};
  const float initial_height = verify_images ? 1230.0F : 850.0F;
  UiTest ui(application, {.viewport = {1080.0F, initial_height}, .resource_provider = std::make_shared<PreviewTestResources>()});
  SendNewConversation(ui, "Review the formatted report");
  auto report = ui.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Link),
      UiSemanticSelector::Label("Verification report")));
  Wait(ui, [&] { return ShowsText(ui, "Verification report"); }, "The unfinished report did not render");
  if (report.Exists()) throw std::runtime_error("A streaming link was interactive before the message settled");
  const auto image_semantics = [&](std::string label) {
    return ui.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Image),
        UiSemanticSelector::Label(std::move(label))));
  };
  if (verify_images) {
    Wait(ui, [&] { return ShowsText(ui, "Delayed diagram"); }, "The streaming image's alternative text was missing");
    if (!image_requests->empty() || image_semantics("Delayed diagram").Exists())
      throw std::runtime_error("A streaming image was loaded");
  }
  released->store(2);
  const std::string original = std::string(MarkdownFirstDelta) + MarkdownSecondDelta + image_tail;
  const std::string excerpt = std::string(MarkdownSelectionWord) + " &";
  const auto rendered = std::string(MarkdownSelectionWord) + " " + excerpt;
  auto paragraph = ui.Find(UiSelector::Key("agent-chat-history")).Find(UiSelector::Text(rendered));
  Wait(ui, [&] { return paragraph.Count() == 1 && paragraph.One().in_viewport && report.Exists(); },
      "The completed formatted reply did not render");
  const auto code = ui.Find(UiSelector::Key("markdown-code-block"));
  Wait(ui, [&] { return code.Exists() && code.Find(UiSelector::Text("C++")).Exists() &&
      code.Find(UiSelector::Key("markdown-copy-code")).Exists(); },
      "The readonly code block did not show its language and copy action");
  auto remove = ui.Find(ButtonText("Remove quote"));
  if (verify_images) {
    Wait(ui, [&] {
      return std::ranges::find(*image_requests, "https://image.test/diagram.png") != image_requests->end();
    }, "The completed message did not start loading its image");
    auto following = ui.Find(UiSelector::Text("Following text"));
    Wait(ui, [&] { return following.Exists() && following.One().in_viewport; }, "The text below the image was unavailable");
    const auto bounds = following.One().bounds;
    ui.TapAt({bounds.x + 1.0F, bounds.y + bounds.height / 2}, {.device_kind = PointerDeviceKind::Mouse});
    ui.PressKey(Key::Home);
    for (int step = 0; step < 13; ++step) ui.PressKey(Key::ArrowRight, {.shift = true});
    Wait(ui, [&] { return ui.Find(ButtonText("Quote")).Exists(); }, "The image prevented selecting the text below it");
    image_gate->store(true);
    Wait(ui, [&] {
      return image_semantics("Delayed diagram").Exists() && image_semantics("Inline icon").Exists() &&
          ui.Find(UiSelector::Type<Image>()).Count() >= 2 && !ShowsText(ui, "Delayed diagram");
    }, "A valid Markdown image did not replace its fallback or render beside its paragraph");
    for (const auto* alt : {"Missing diagram", "Damaged diagram", "Local diagram"}) {
      if (!ShowsText(ui, alt) || image_semantics(alt).Exists())
        throw std::runtime_error("An unavailable Markdown image lost its alternative text");
    }
    ui.PressKey(Key::ArrowRight, {.shift = true});
    TapFormButton(ui, "Quote");
    Wait(ui, [&] { return remove.Exists() && ui.Find(UiSelector::Text("Following text")).Count() == 2; },
        "Loading an image changed the focused Markdown selection");
    remove.Tap();
  }
  if (!report.Exists() || ui.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Link),
          UiSemanticSelector::Label("Local path"))).Exists()) {
    throw std::runtime_error("The formatted reply did not distinguish allowed and refused destinations");
  }
  *allowed = false;
  report.PerformSemanticAction({SemanticActionKind::Activate, {}});
  Wait(ui, [&] { return ShowsText(ui, "This link is unavailable"); }, "Activation did not recheck link policy");
  if (!visits->empty()) throw std::runtime_error("A link revoked before activation still reached the host");
  *allowed = true;
  ui.SetWindowMetrics({.viewport = {1080.0F, initial_height + 15.0F}});
  Wait(ui, [&] { return report.Exists(); }, "An allowed destination did not regain link semantics");
  report.PerformSemanticAction({SemanticActionKind::Activate, {}});
  Wait(ui, [&] { return ShowsText(ui, "Browser unavailable"); }, "The browser failure was not shown beside the message");
  report.PerformSemanticAction({SemanticActionKind::Activate, {}});
  Wait(ui, [&] { return !ShowsText(ui, "Browser unavailable"); }, "Successful retry did not clear the browser failure");
  if (visits->size() != 2 || visits->front().destination != MarkdownReportUrl ||
      visits->back().destination != MarkdownReportUrl || visits->front().message != visits->back().message ||
      visits->front().block != visits->back().block) {
    throw std::runtime_error("The host did not receive the exact destination and stable message block once per activation");
  }
  const auto whole = paragraph.One().bounds;
  const auto midline = whole.y + whole.height / 2;
  ui.Drag({whole.x + whole.width - 1.0F, midline}, {whole.x + 1.0F, midline},
      {.segments = 1, .pointer = {.device_kind = PointerDeviceKind::Mouse}});
  const auto toolbar = ui.Find(UiSelector::Key("message-selection-actions"));
  Wait(ui, [&] {
    if (toolbar.Count() != 1 || !toolbar.One().in_viewport) return false;
    const auto popup = toolbar.One().bounds;
    return popup.x <= whole.x + 8.0F && popup.x + popup.width > whole.x &&
        (std::abs(popup.y + popup.height - midline) < 32.0F || std::abs(popup.y - midline) < 32.0F) &&
        paragraph.One().bounds == whole;
  }, "The Markdown action popup did not follow the active selection endpoint");
  TapFormButton(ui, "Quote");
  Wait(ui, [&] { return remove.Exists() && ui.Find(UiSelector::Text(rendered)).Count() == 2; },
      "Reverse drag did not quote the formatted paragraph");
  if (visits->size() != 2) throw std::runtime_error("Dragging across a link activated it");
  remove.Tap();

  const auto select_excerpt = [&] {
    const auto bounds = paragraph.One().bounds;
    ui.TapAt({bounds.x + 1.0F, bounds.y + bounds.height / 2}, {.device_kind = PointerDeviceKind::Mouse});
    ui.PressKey(Key::Home);
    for (int i = 0; i < 4; ++i) ui.PressKey(Key::ArrowRight);
    for (int i = 0; i < 5; ++i) ui.PressKey(Key::ArrowRight, {.shift = true});
  };
  select_excerpt();
  TapFormButton(ui, "Quote");
  Wait(ui, [&] { return remove.Exists() && ui.Find(UiSelector::Text(excerpt)).Exists(); },
      "The returned quote did not preserve the rendered selection");
  if (!paragraph.Exists() || MessageText(ui, original) || ui.Find(UiSelector::Key("markdown-source-toggle")).Exists())
    throw std::runtime_error("Selecting the formatted text exposed the raw Markdown");

  select_excerpt();
  TapFormButton(ui, "Side chat");
  auto pane = ui.Find(UiSelector::Key("side-chat-pane"));
  auto question = pane.Find(UiSelector::Key("side-chat-question"));
  Wait(ui, [&] { return question.Exists() && question.One().focused && ui.ActiveTextInput() &&
      pane.Find(UiSelector::Text(excerpt)).Exists(); }, "Side chat did not show the rendered selection and focus the question");
  question.ReplaceText("Explain this selection");
  pane.Find(UiSelector::Key("chat-send/Send")).Tap();
  Wait(ui, [&] { return passages->size() >= 2; }, "The side-chat model did not receive the original source fragments");
  if (*passages != std::vector{std::string("Selected passage:\n") + MarkdownSelectionWord, std::string("Selected passage:\n &amp;")})
    throw std::runtime_error("The side chat sent unselected markup or expanded a rendered fragment");
  const auto link = visits->front();
  const auto label_start = original.find(std::string("[") + MarkdownSelectionWord) + 1;
  const auto entity_start = original.find(" &amp;");
  const std::vector<kit::SourceSlice> expected{
      {link.message, {}, kit::SourceTextRange{link.block, label_start, label_start + std::string_view(MarkdownSelectionWord).size()}},
      {link.message, {}, kit::SourceTextRange{link.block, entity_start, entity_start + std::string_view(" &amp;").size()}},
  };
  std::optional<std::vector<kit::SourceSlice>> source_refs;
  tasks->Launch([&]() -> Task<void> {
    const auto catalog = LiveTake(co_await runtime->Sessions().ListAsync({.include_ephemeral = true}, {}));
    for (const auto& summary : catalog.sessions) {
      const auto session = LiveTake(co_await runtime->Sessions().OpenAsync(summary.id));
      const auto snapshot = session->Snapshot();
      if (snapshot && snapshot->origin && snapshot->origin->kind == kit::SessionOriginKind::SideChat)
        source_refs = snapshot->origin->side_chat_selection;
    }
  });
  Wait(ui, [&] { return source_refs.has_value(); }, "Side chat source refs were not available");
  if (*source_refs != expected) throw std::runtime_error("Side chat lost the exact Markdown source version or UTF-8 ranges");

  pane.Find(ButtonText("Close")).Tap();
  auto composer = ui.Find(UiSelector::Key("composer"));
  Wait(ui, [&] { return !pane.Exists() && composer.Exists() && remove.Exists(); }, "Closing side chat lost the quoted draft");
  const auto submit_quote = [&](std::string message, const std::vector<kit::SourceSlice>& slices) {
    composer.Tap();
    composer.ReplaceText(message);
    TapFormButton(ui, "Send");
    auto stop = ui.Find(UiSelector::Key("agent-chat-stop"));
    Wait(ui, [&] { return ShowsText(ui, message) && !stop.Exists() && !remove.Exists(); }, "The cited message did not finish");
    std::optional<nlohmann::json> persisted;
    tasks->Launch([&]() -> Task<void> {
      const auto store = LiveTake(co_await agentkit_host::OpenSqliteSessionStoreAsync(database));
      const auto image = LiveTake(co_await store->LoadAsync());
      const auto row = std::ranges::find_if(image.records, [&](const auto& record) {
        return record.collection == "agentkit.session" && record.key == link.message.session.value;
      });
      if (row == image.records.end()) co_return;
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(row->data.data());
      const auto values = nlohmann::json::from_cbor(bytes, bytes + row->data.size());
      const auto& input = values.at("session").at("runs").back().at("input");
      if (input.at("text") == message) persisted = input.at("context");
    });
    Wait(ui, [&] { return persisted.has_value(); }, "The quoted source refs were not committed to SQLite");
    if (persisted->size() != slices.size()) throw std::runtime_error("The submitted quote lost Markdown fragments");
    for (std::size_t index = 0; index < slices.size(); ++index) {
      const auto& expected_source = std::get<kit::MessageRef>(slices[index].source);
      const auto& expected_range = *slices[index].text_range;
      const auto& stored = persisted->at(index);
      const auto& reference = stored.at("source").at(1);
      const auto& range = stored.at("text_range");
      if (reference.at("session") != expected_source.session.value || reference.at("message") != expected_source.message.value ||
          reference.at("version") != expected_source.version || range.at("leaf").at(1) != std::get<kit::BlockId>(expected_range.leaf).value ||
          range.at("begin_utf8") != expected_range.begin_utf8 || range.at("end_utf8") != expected_range.end_utf8)
        throw std::runtime_error("The persisted quote did not reference the selected original Markdown bytes");
    }
  };
  const std::string bridge = "Verify Markdown quote";
  submit_quote(bridge, expected);

  std::shared_ptr<const kit::SessionSnapshot> transcript;
  tasks->Launch([&]() -> Task<void> {
    const auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(link.message.session));
    transcript = chat->Snapshot();
  });
  Wait(ui, [&] { return static_cast<bool>(transcript); }, "The Markdown transcript could not be read");
  const auto first_message = std::ranges::find(transcript->messages, link.message, &kit::Message::ref);
  if (first_message == transcript->messages.end() || transcript->messages.end() - first_message != 3)
    throw std::runtime_error("The Markdown range fixture did not contain both replies and the intervening messages");
  const auto& middle = *std::next(first_message);
  const auto& last = *std::next(first_message, 2);
  const auto& last_block = last.blocks.front();
  const auto later_line = std::string("The model is streaming another delta");
  const std::string later_prefix = "The model";
  auto earlier_text = ui.Find(UiSelector::Key(link.message.message.value)).Find(UiSelector::Text(rendered));
  auto later_text = ui.Find(UiSelector::Key(last.ref.message.value)).Find(UiSelector::Text(later_line));
  ui.SetWindowMetrics({.viewport = {1080.0F, verify_images ? 2550.0F : 1750.0F}});
  Wait(ui, [&] { return earlier_text.Exists() && earlier_text.One().in_viewport && later_text.Exists() && later_text.One().in_viewport; },
      "Both Markdown range endpoints did not become visible");
  const auto select_endpoint = [&](const UiNodeQuery& text, int start, int length) {
    const auto bounds = text.One().bounds;
    ui.TapAt({bounds.x + 1.0F, bounds.y + bounds.height / 2}, {.device_kind = PointerDeviceKind::Mouse});
    ui.PressKey(Key::Home);
    for (int step = 0; step < start; ++step) ui.PressKey(Key::ArrowRight);
    for (int step = 0; step < length; ++step) ui.PressKey(Key::ArrowRight, {.shift = true});
  };
  auto extended = expected;
  std::vector<std::string> model_passages{MarkdownSelectionWord, " &amp;"};
  const auto add_source = [&](const kit::MessageRef& message, const kit::BlockId& block, std::string_view text) {
    const auto start = original.find(text);
    if (start == std::string::npos) throw std::runtime_error("A Markdown range fixture fragment was missing");
    extended.push_back({message, {}, kit::SourceTextRange{block, start, start + text.size()}});
    model_passages.emplace_back(text);
  };
  for (const auto* part : {"const int ready = 1;\n", "Check", "Result", "Render", "Pass", "Verification report", "Local path"})
    add_source(link.message, link.block, part);
  if (verify_images) {
    for (const auto* part : {"Delayed diagram", "Missing diagram", "Damaged diagram", "Local diagram", "Before ",
                             "Inline icon", " after.", "Following text"})
      add_source(link.message, link.block, part);
  }
  extended.push_back({middle.ref, {}, kit::SourceTextRange{middle.blocks.front().id, 0, bridge.size()}});
  model_passages.push_back(bridge);
  add_source(last.ref, last_block.id, "Streamed heading");
  add_source(last.ref, last_block.id, later_prefix);
  for (auto& part : model_passages) part.insert(0, "Selected passage:\n");
  auto earlier_excerpt = excerpt + "\nconst int ready = 1;\n\nCheck\nResult\nRender\nPass\nVerification report\nLocal path";
  if (verify_images) earlier_excerpt +=
      "\nDelayed diagram\nMissing diagram\nDamaged diagram\nLocal diagram\nBefore Inline icon after.\nFollowing text";
  const auto later_excerpt = "Streamed heading\n" + later_prefix;
  const auto range_quote = earlier_excerpt + '\n' + bridge + '\n' + later_excerpt;
  for (const bool reverse : {false, true}) {
    if (reverse) select_endpoint(later_text, 0, static_cast<int>(later_prefix.size()));
    else select_endpoint(earlier_text, 4, 5);
    TapFormButton(ui, "Extend selection");
    Wait(ui, [&] { return ShowsText(ui, "Select the other end of the range") && !ui.Find(ButtonText("Quote")).Exists(); },
        "Starting from a split Markdown selection did not retain its endpoint");
    if (reverse) select_endpoint(earlier_text, 4, 5);
    else select_endpoint(later_text, 0, static_cast<int>(later_prefix.size()));
    TapFormButton(ui, "Side chat");
    const auto sources = pane.Find(UiSelector::Key("side-chat-source"));
    Wait(ui, [&] { return sources.Exists() && sources.Find(UiSelector::Text("3 selected passages")).Exists() &&
        sources.Find(UiSelector::Text(earlier_excerpt)).Exists() &&
        sources.Find(UiSelector::Text(bridge)).Exists() &&
        sources.Find(UiSelector::Text(later_excerpt)).Exists(); }, "Side chat did not show only the rendered passages in the extended range");
    if (!reverse) {
      passages->clear();
      Wait(ui, [&] { return question.Exists() && question.One().focused && ui.ActiveTextInput(); },
          "The extended selection did not focus its side chat question");
      question.ReplaceText("Explain the Markdown range");
      pane.Find(UiSelector::Key("chat-send/Send")).Tap();
      Wait(ui, [&] { return passages->size() >= model_passages.size(); }, "The side chat did not receive all Markdown fragments");
      if (*passages != model_passages) throw std::runtime_error("The extended side chat sent incorrect source fragments");
      source_refs.reset();
      tasks->Launch([&]() -> Task<void> {
        const auto catalog = LiveTake(co_await runtime->Sessions().ListAsync({.include_ephemeral = true}, {}));
        for (const auto& row : catalog.sessions) {
          const auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(row.id));
          const auto snapshot = chat->Snapshot();
          if (snapshot && snapshot->origin && snapshot->origin->kind == kit::SessionOriginKind::SideChat &&
              snapshot->origin->side_chat_selection != expected) source_refs = snapshot->origin->side_chat_selection;
        }
      });
      Wait(ui, [&] { return source_refs.has_value() && pane.Find(UiSelector::Key("side-chat-composer")).Exists(); },
          "The extended side chat did not settle with source refs");
      if (*source_refs != extended) throw std::runtime_error("The extended side chat lost exact source versions or UTF-8 ranges");
    } else if (!pane.Find(UiSelector::Key("side-chat-composer")).Exists() || question.Exists()) {
      throw std::runtime_error("Reversing the range did not reopen the side chat for the same source slices");
    }
    pane.Find(ButtonText("Close")).Tap();
    TapFormButton(ui, "Quote");
    Wait(ui, [&] { return remove.Exists() && ShowsText(ui, range_quote); }, "The quote did not preserve the extended rendered range");
    submit_quote(reverse ? "Use the backward Markdown range" : "Use the forward Markdown range", extended);
  }
}

void VerifyLongConversation(const File& root) {
  using namespace agentkit_preview;
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the history workspace");
  struct HistoryFixture {
    std::shared_ptr<kit::Conversation> chat;
    std::optional<TaskScope> tasks;
    kit::MessageId initial_first;
    kit::BlockId old_attachment;
    bool seeded = false;
    std::string error;
  };
  const auto fixture = std::make_shared<HistoryFixture>();
  const auto installer = MakeRunSettingsTestInstaller(root.Child("sessions.sqlite3"), workspace, {},
      [fixture](auto started, std::optional<kit::WorkspaceId> registered, kit::ProjectId project) -> Task<void> {
        try {
          const auto runtime = LiveTake(std::move(started));
          if (!registered) throw std::runtime_error("The test workspace did not start");
          auto original = co_await LiveConversation(runtime, project, *registered, PptAgent, "Original review deck", "Project review");
          auto original_observer = co_await LiveObserve(original);
          auto approval = co_await StageReviewPresentation(original, "HistoryReview.pptx");
          co_await LiveApprove(original, approval);
          co_await LiveRunCompleted(original);
          const auto source = original->Snapshot();
          const auto boundary = std::ranges::find(source->messages.rbegin(), source->messages.rend(),
              kit::MessageState::Complete, &kit::Message::state);
          if (boundary == source->messages.rend()) throw std::runtime_error("The deck had no settled history boundary");
          kit::ForkSessionRequest copy{.from = {source->summary.id, boundary->ref.message, boundary->sequence},
              .options = {.agent = GeneralAgent, .project = project, .title = "Timeline acceptance"},
              .workspace = kit::WorkspaceChoice{kit::WorkspaceChoice::Kind::Existing, *registered}};
          auto created = LiveTake(co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), std::move(copy)));
          fixture->chat = LiveTake(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(created.outcome).session));
          auto observer = co_await LiveObserve(fixture->chat);
          for (int index = 0; index < 31; ++index) {
            const auto message = "History item " + std::to_string(index);
            LiveTake(co_await fixture->chat->SendAsync(kit::NewCommandId(), {.text = message}));
            co_await LiveRunCompleted(fixture->chat);
          }
          const auto tail = fixture->chat->Snapshot();
          if (!tail->older_history || tail->messages.size() != 50) throw std::runtime_error("The preceding history did not fall outside the default projection");
          fixture->initial_first = tail->messages.front().ref.message;
          auto history = LiveTake(co_await runtime->Sessions().ReadHistoryAsync(tail->summary.id, {.page_size = 100}));
          for (const auto& message : history.messages) {
            if (message.sequence >= tail->messages.front().sequence) break;
            for (const auto& block : message.blocks) {
              if (const auto* art = std::get_if<kit::ArtifactBlock>(&block.body); art && art->artifact.id.value.starts_with("presentation:")) {
                fixture->old_attachment = block.id;
                break;
              }
            }
            if (!fixture->old_attachment.value.empty()) break;
          }
          if (fixture->old_attachment.value.empty()) throw std::runtime_error("The old deck was not inherited in history");
          fixture->tasks = UseApplicationTaskScope();
          fixture->seeded = true;
        } catch (const std::exception& failure) { fixture->error = failure.what(); }
      });
  const Application application{App, {.application_hooks = {installer}}};
  UiTest ui(application, {.viewport = {1120.0F, 820.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  try {
    Wait(ui, [&] { return fixture->seeded || !fixture->error.empty(); }, "The long history could not be prepared", 120s);
    if (!fixture->error.empty()) throw std::runtime_error(fixture->error);
    ConversationRow(ui, "Timeline acceptance").Tap({.device_kind = PointerDeviceKind::Mouse});
    auto history = ui.Find(UiSelector::Key("agent-chat-history"));
    auto editor = ui.Find(UiSelector::Key("composer"));
    Wait(ui, [&] { return history.Exists() && MessageText(ui, "History item 30").has_value(); }, "Opening history did not show the latest turn");
    const auto attachment = UiSelector::Key("artifact-review/" + fixture->old_attachment.value);
    if (ui.Find(attachment).Exists()) throw std::runtime_error("Virtual history mounted an unrequested old message");
    auto earlier = history.ScrollUntil(ButtonText("Load earlier messages"), {.step = {0.0F, -480.0F}, .maximum_steps = 36});
    earlier.Tap();
    auto boundary = history.Find(UiSelector::Key(fixture->initial_first.value));
    Wait(ui, [&] { return boundary.Exists() && boundary.One().in_viewport &&
        std::abs(boundary.One().bounds.y - history.One().bounds.y) < 3.0F;
    }, "Loading earlier messages moved the prior page boundary");
    auto opener = history.ScrollUntil(attachment, {.step = {0.0F, -310.0F}, .maximum_steps = 36})
        .Find(ReviewCard("Review artifact v1"));
    editor.Tap();
    editor.ReplaceText("Keep the unsent timeline draft");
    const auto origin = opener.One().bounds.y;
    opener.Tap();
    Wait(ui, [&] { return ui.Find(UiSelector::Key("presentation-slide")).Exists() &&
        std::ranges::none_of(ui.Find(UiSelector::Key("conversation-goal")).All(), &UiNodeInfo::participates_in_layout) &&
        std::ranges::none_of(ui.Find(UiSelector::Key("comparison-entry")).All(), &UiNodeInfo::participates_in_layout); },
        "The historical deck did not open a dedicated review");
    TapFormButton(ui, "Next slide");
    auto append = [fixture](std::string message) {
      auto done = std::make_shared<bool>(false);
      fixture->tasks->Launch([fixture, message = std::move(message), done]() -> Task<void> {
        try {
          LiveTake(co_await fixture->chat->SendAsync(kit::NewCommandId(), {.text = message}));
          co_await LiveRunCompleted(fixture->chat);
        } catch (const std::exception& failure) { fixture->error = failure.what(); }
        *done = true;
      });
      return done;
    };
    const auto hidden = append("Added while the old deck was open");
    Wait(ui, [&] { return *hidden; }, "The conversation did not advance during review");
    if (!fixture->error.empty()) throw std::runtime_error(fixture->error);
    TapFormButton(ui, "Back to conversation");
    const auto at_origin = [&] { return opener.Exists() && opener.One().in_viewport && std::abs(opener.One().bounds.y - origin) < 3.0F; };
    Wait(ui, [&] { return at_origin() && opener.Find(UiSelector::Focused(true)).Exists(); },
        "Returning from review lost scroll position or attachment focus");
    if (editor.One().value != "Keep the unsent timeline draft") throw std::runtime_error("Returning to the timeline lost its draft");
    const auto comparison_entry = UiSelector::Key("comparison-entry");
    TapControl(ui, comparison_entry, "Compare alternatives");
    const auto back = ui.Find(UiSelector::Key("comparison/" + fixture->chat->Snapshot()->summary.id.value))
        .Find(ButtonText("Back to conversation"));
    Wait(ui, [&] { return back.Exists() && back.One().in_viewport; }, "The comparison page did not open");
    if (history.One().in_viewport) throw std::runtime_error("The obscured conversation still participated in layout");
    const auto comparing = append("Added while comparing alternatives");
    Wait(ui, [&] { return *comparing; }, "The conversation did not advance during comparison");
    if (!fixture->error.empty()) throw std::runtime_error(fixture->error);
    back.Tap();
    const auto comparison_focus = ui.Find(UiSelector::AllOf(comparison_entry, UiSelector::Focused(true)));
    Wait(ui, [&] { return at_origin() && comparison_focus.Exists(); },
        "Returning from comparison lost the historical attachment position or toolbar focus");
    if (editor.One().value != "Keep the unsent timeline draft") throw std::runtime_error("Comparison replaced the source draft");
    const auto browsing = append("Added while reading earlier history");
    Wait(ui, [&] { return *browsing; }, "The model did not complete while reading earlier history");
    ui.Pump(120ms);
    if (!fixture->error.empty()) throw std::runtime_error(fixture->error);
    if (!at_origin()) throw std::runtime_error("Streaming moved the reader away from the older message");
    const auto header = ui.Find(UiSelector::Key("agent-chat-heading"));
    const auto composer_panel = ui.Find(UiSelector::Key("agent-chat-composer"));
    const auto jump = ui.Find(ButtonText("Jump to latest"));
    Wait(ui, [&] {
      if (!jump.Exists() || !jump.One().in_viewport) return false;
      const auto control = jump.One().bounds;
      const auto input = composer_panel.One().bounds;
      const auto heading = header.One().bounds;
      return heading.height <= 40.0F && control.y > heading.y + heading.height &&
          input.y - (control.y + control.height) >= 0.0F && input.y - (control.y + control.height) < 30.0F &&
          std::abs(control.x + control.width / 2.0F - input.x - input.width / 2.0F) < 2.0F;
    }, "The conversation header or floating jump position did not fit");
    jump.Tap();
    Wait(ui, [&] { return MessageText(ui, "Added while reading earlier history").has_value(); }, "The jump control did not reveal the latest turn");
    const auto following = append("A following turn\n" + std::string(1100, 'x'));
    Wait(ui, [&] { return *following; }, "The following turn did not finish");
    if (!fixture->error.empty()) throw std::runtime_error(fixture->error);
    const auto newest = fixture->chat->Snapshot()->messages.back().ref.message;
    auto last_row = history.Find(UiSelector::Key(newest.value));
    Wait(ui, [&] { return last_row.Exists() && last_row.One().in_viewport && !ui.Find(ButtonText("Jump to latest")).Exists(); },
        "A reader at the end did not follow the growing output");
  } catch (...) {
    static_cast<void>(root.Child("timeline.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
    throw;
  }
}

void VerifyDomainReviewWindows(const File& root) {
  using namespace agentkit_preview;
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the domain review workspace");
  struct DomainReview {
    State<bool> ready{false};
    State<bool> show_design{false};
    State<kit::AgentChatDraftSources> draft{kit::AgentChatDraftSources{}};
    std::optional<TaskScope> tasks;
    std::shared_ptr<kit::AgentRuntime> runtime;
    std::shared_ptr<kit::Conversation> ppt;
    std::shared_ptr<kit::Conversation> design;
    kit::AgentChatReviewRequest deck;
    DesignReviewLocation board;
    std::vector<DesignNode> nodes;
    bool prepared = false;
    std::string error;
  };
  std::shared_ptr<DomainReview> fixture;
  auto install = MakeRunSettingsTestInstaller(root.Child("sessions.sqlite3"), workspace, {},
      [&fixture](auto started, std::optional<kit::WorkspaceId> id, kit::ProjectId project) -> Task<void> {
        const auto state = fixture;
        try {
          state->runtime = LiveTake(std::move(started));
          if (!id) throw std::runtime_error("The review workspace did not start");
          state->ppt = co_await LiveConversation(state->runtime, project, *id, PptAgent, "Review deck", "Plan the product release");
          auto ppt_observer = co_await LiveObserve(state->ppt);
          auto export_request = co_await StageReviewPresentation(state->ppt, "DomainReview.pptx");
          const auto& plan = std::get<kit::ToolApproval>(export_request.body);
          state->deck = {plan.preview.artifacts.at(0), 1, kit::AgentChatApprovalRef{export_request.id, export_request.version}};
          state->design = co_await LiveConversation(state->runtime, project, *id, DesignAgent, "Review artboard", "Design a settings page");
          auto design_observer = co_await LiveObserve(state->design);
          auto review = co_await LiveInteraction(state->design, "Review design");
          auto board = LiveTake(ParseDesign(LiveForm(review).at("design")));
          state->nodes = std::move(board.nodes);
          if (state->nodes.size() < 2) throw std::runtime_error("The artboard did not define two nodes");
          state->board = {kit::AgentChatApprovalRef{review.id, review.version}, state->nodes[1].id};
          state->tasks = UseApplicationTaskScope();
          state->prepared = true;
          state->ready = true;
        } catch (const std::exception& error) { state->error = error.what(); }
      });
  const Application application{[]() -> View {
    const auto state = UseService<DomainReview>();
    if (!state->ready.Get()) return Text("Preparing the review versions");
    const auto reveal = [&](const char* key) -> View {
      if (state->show_design.Get()) {
        return Column{DesignChat(state->design, state->runtime, {.draft_sources = state->draft}, state->board)}.With(Grow()).Key(key);
      }
      PresentationReviewPages seed{{{state->deck.artifact.id, state->deck.artifact.version}, kit::PartId{"slide-2"}}};
      return Column{DetachedArtifactReview(state->ppt, state->runtime, PptAgent, state->deck, std::move(seed), state->draft)}.With(Grow()).Key(key);
    };
    return Row{reveal("first-review"), reveal("second-review")};
  }, {.application_hooks = {[&fixture](ApplicationContext& context) {
        fixture = std::make_shared<DomainReview>();
        context.Provide(fixture);
      }, std::move(install)}}};
  UiTest ui(application, {.viewport = {1760.0F, 920.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  try {
    Wait(ui, [&] { return fixture->prepared || !fixture->error.empty(); }, "The domain reviews could not be prepared");
    if (!fixture->error.empty()) throw std::runtime_error(fixture->error);
    auto first = ui.Find(UiSelector::Key("first-review"));
    auto second = ui.Find(UiSelector::Key("second-review"));
    auto both_show = [&](const char* label) {
      const auto selector = UiSelector::Text(label);
      return first.Find(selector).Exists() && second.Find(selector).Exists();
    };
    Wait(ui, [&] { return both_show("2 / 3"); }, "The two PPT reviews did not open at the chosen slide");
    first.Find(ButtonText("Next slide")).Tap();
    second.Find(ButtonText("Previous slide")).Tap();
    Wait(ui, [&] { return first.Find(UiSelector::Text("3 / 3")).Exists() && second.Find(UiSelector::Text("1 / 3")).Exists(); },
        "Changing the slide affected another review");
    const auto output = root.Child("exports").Child("DomainReview.pptx");
    if (output.Exists()) throw std::runtime_error("Opening a review exported the deck before approval");
    first.Find(ButtonText("Approve reviewed change")).Tap();
    Wait(ui, [&] { return both_show("Change approved") && output.Exists(); }, "The PPT reviews did not observe one approved export");
    auto deck = archive::ArchiveReader::Open(output);
    if (!deck.Succeeded() || ReadPart(deck.Value(), "ppt/slides/slide2.xml").find("Reviewed decisions") == std::string::npos)
      throw std::runtime_error("The independent review exported a different deck version");
    fixture->tasks->Launch([fixture]() -> Task<void> { fixture->show_design = true; co_return; });
    const auto selection = [](const DesignNode& node) { return UiSelector::Key("design-selection/" + node.id); };
    const auto& initial = fixture->nodes[1];
    const auto& alternative = fixture->nodes.front();
    Wait(ui, [&] { return first.Find(selection(initial)).Exists() && second.Find(selection(initial)).Exists(); },
        "The two design reviews did not open at the chosen node");
    first.Find(ButtonText(alternative.label.c_str())).Tap();
    Wait(ui, [&] { return first.Find(selection(alternative)).Exists() && second.Find(selection(initial)).Exists(); },
        "Changing a design node affected another review");
    first.Find(ButtonText("Approve design")).Tap();
    Wait(ui, [&] {
      const auto view = fixture->design->Snapshot();
      return both_show("Review closed") && view && std::ranges::any_of(view->artifacts,
          [](const auto& artifact) { return artifact.id.value.starts_with("preview-design-"); });
    }, "The review surfaces did not observe the confirmed design");
    if (second.Find(ButtonText("Approve design")).Exists() || !second.Find(selection(initial)).Exists())
      throw std::runtime_error("The closed design review changed its selected node or retained the approval button");
  } catch (...) {
    static_cast<void>(root.Child("domain-review.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
    throw;
  }
}

void VerifyReviewApproval(const File& root) {
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the review workspace");
  const auto config = workspace.Child("config.txt");
  const auto settings = workspace.Child("settings.html");
  WriteFixture(config, "mode=disabled\n");
  WriteFixture(settings, SettingsPage(false));
  const auto database = root.Child("sessions.sqlite3");
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}}, std::make_shared<PatchModel>()};
  auto installer = MakeCodeTestInstaller(database, workspace, model, File(AGENTKIT_PREVIEW_CTEST), {},
      [&](auto started, auto, auto) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      });
  struct Review {
    std::shared_ptr<kit::Conversation> first;
    std::shared_ptr<kit::Conversation> second;
    kit::AgentChatReviewRequest request;
    kit::ToolCallId call;
  };
  struct ReviewFixture {
    State<std::shared_ptr<const Review>> review{std::shared_ptr<const Review>{}};
    State<bool> stale{true};
    State<kit::AgentChatDraftSources> draft_sources{kit::AgentChatDraftSources{}};
    std::shared_ptr<kit::AgentRuntime> runtime;
  };
  std::shared_ptr<ReviewFixture> fixture;
  std::shared_ptr<const Review> opened_review;
  const Application application{[]() -> View {
    const auto fixture = UseService<ReviewFixture>();
    const auto opened = fixture->review.Get();
    if (!opened) return App();
    auto second = opened->request;
    if (fixture->stale.Get()) { ++second.ticket; ++second.approval->version; }
    auto pane = [&](std::shared_ptr<kit::Conversation> conversation, kit::AgentChatReviewRequest request, const char* key) {
      kit::AgentChatOptions options{.draft_sources = fixture->draft_sources, .review_request = std::move(request),
          .review_only = true};
      return Column{agentkit_preview::CodeChat(std::move(conversation), fixture->runtime, std::move(options))}.With(Grow()).Key(key);
    };
    return Row{pane(opened->first, opened->request, "first-review"), pane(opened->second, second, "second-review")};
  }, {.application_hooks = {std::move(installer), [&fixture](ApplicationContext& context) {
    fixture = std::make_shared<ReviewFixture>();
    context.Provide(fixture);
  }}}};
  UiTest ui(application, {.viewport = {1500.0F, 940.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  PrepareCodeChange(ui, "Review workspace changes in two views");
  const auto fully_visible = [](const UiNodeQuery& query) {
    if (!query.Exists()) return false;
    const auto view = query.One();
    return view.in_viewport && view.visible_bounds.height >= view.bounds.height - 1.0F &&
        view.visible_bounds.width >= view.bounds.width - 1.0F && view.bounds.x >= 0.0F && view.bounds.y >= 0.0F &&
        view.bounds.x + view.bounds.width <= 520.0F && view.bounds.y + view.bounds.height <= 420.0F;
  };
  auto draft = ui.Find(UiSelector::Key("composer"));
  auto inline_approval = ui.Find(ButtonText("Approve"));
  ui.SetWindowMetrics({.viewport = {520.0F, 420.0F}});
  try {
    Wait(ui, [&] { return fully_visible(inline_approval) && fully_visible(draft); },
        "The inline decision and message input did not fit a short window");
  } catch (...) {
    static_cast<void>(root.Child("short-review.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
    throw;
  }
  draft.Tap();
  draft.ReplaceText("Keep this review draft");
  auto opener = ui.Find(ReviewCard("Review attachment 1"));
  if (!opener.Find(UiSelector::Text("config.txt")).Exists() ||
      !ui.Find(ReviewCard("Review attachment 2")).Find(UiSelector::Text("settings.html")).Exists() ||
      !ui.FindSemantics(UiSemanticSelector::Label("Review config.txt")).Exists()) {
    throw std::runtime_error("The approval attachments did not display their file names");
  }
  opener.Tap();
  auto patch = ui.Find(UiSelector::Key("artifact-text"));
  auto title = ui.Find(UiSelector::Key("artifact-review-title"));
  auto tabs = ui.Find(UiSelector::Key("review-attachments"));
  try {
    Wait(ui, [&] { return patch.Exists() && patch.One().in_viewport; }, "The selected patch did not open");
    Wait(ui, [&] { return fully_visible(ui.Find(ButtonText("Approve reviewed change"))) &&
        fully_visible(ui.Find(ButtonText("Back to conversation"))) && fully_visible(title) && title.One().bounds.height >= 20.0F &&
        fully_visible(tabs) && tabs.One().bounds.height >= 24.0F; }, "The review title, attachments, or actions were clipped in a short window");
  } catch (...) {
    static_cast<void>(root.Child("short-artifact-review.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
    throw;
  }
  tabs.Find(UiSelector::Text("settings.html")).Tap();
  Wait(ui, [&] { return patch.Exists() && ReviewedDiff(ui).starts_with("settings.html\n"); },
      "Switching files did not open the other attachment in the same approval");
  CheckText(config, "mode=disabled\n");
  CheckText(settings, SettingsPage(false));
  TapFormButton(ui, "Back to conversation");
  Wait(ui, [&] { return opener.Find(UiSelector::Focused(true)).Exists(); }, "Returning from the review did not focus its opener");
  if (draft.One().value != "Keep this review draft") throw std::runtime_error("Returning from the review lost the draft");
  ui.SetWindowMetrics({.viewport = {1500.0F, 940.0F}});
  std::string error;
  tasks->Launch([&]() -> Task<void> {
    try {
      const auto pending = LiveTake(co_await runtime->Interactions().ListPendingAsync({}, {}));
      if (pending.items.size() != 1) throw std::runtime_error("Expected one workspace approval");
      const auto& item = pending.items.front();
      const auto& approval = std::get<kit::ToolApproval>(item.interaction.body);
      auto first = LiveTake(co_await runtime->Sessions().OpenAsync(item.session.id));
      auto second = LiveTake(co_await runtime->Sessions().OpenAsync(item.session.id));
      fixture->runtime = runtime;
      opened_review = std::make_shared<const Review>(Review{std::move(first), std::move(second),
          {approval.preview.artifacts.front(), 1, kit::AgentChatApprovalRef{item.interaction.id, item.interaction.version}}, approval.call});
      fixture->review = opened_review;
    } catch (const std::exception& failure) { error = failure.what(); }
  });
  Wait(ui, [&] { return opened_review || !error.empty(); }, "The review binding was unavailable");
  if (!error.empty()) throw std::runtime_error(error);
  auto first = ui.Find(UiSelector::Key("first-review"));
  auto second = ui.Find(UiSelector::Key("second-review"));
  auto first_approve = first.Find(ButtonText("Approve reviewed change"));
  auto second_approve = second.Find(ButtonText("Approve reviewed change"));
  const auto review_changed = second.Find(UiSelector::Text("Approval changed. Open the current request to review its plan."));
  Wait(ui, [&] { return first_approve.Exists() && review_changed.Exists(); }, "The outdated approval was not isolated");
  if (second_approve.Exists()) throw std::runtime_error("An old review could approve a different version");
  tasks->Launch([fixture]() -> Task<void> { fixture->stale = false; co_return; });
  Wait(ui, [&] { return first_approve.One().in_viewport && second_approve.Exists() && second_approve.One().in_viewport; },
      "The current approval was not visible in both views");
  CheckText(config, "mode=disabled\n");
  auto diff = first.Find(UiSelector::Key("artifact-text"));
  const std::string lines = "-mode=disabled\n+mode=enabled";
  const auto offset = SelectDiffExcerpt(ui, diff, lines);
  auto quote = first.Find(ButtonText("Quote"));
  Wait(ui, [&] { return quote.Exists() && quote.One().in_viewport; }, "The review source could not be quoted");
  quote.Tap();
  const auto quote_matches = std::make_shared<std::optional<bool>>();
  tasks->Launch([fixture, quote_matches, artifact = opened_review->request.artifact, lines, offset]() -> Task<void> {
    while (!fixture->draft_sources->quote) co_await Delay(10ms);
    const auto& citation = *fixture->draft_sources->quote;
    *quote_matches = citation.source.source == kit::SourceRef{artifact} && citation.text == lines &&
        citation.source.text_range == kit::SourceTextRange{kit::PartId{"config.txt"}, offset, offset + lines.size()};
  });
  Wait(ui, [&] { return quote_matches->has_value(); }, "The selection did not reach the conversation draft");
  if (!**quote_matches) throw std::runtime_error("The quote did not retain the reviewed diff version and selected text");
  first_approve.Tap();
  bool competing_done = false;
  tasks->Launch([&]() -> Task<void> {
    try {
      const auto& opened = opened_review;
      const auto approval = *opened->request.approval;
      auto result = co_await opened->second->SubmitAsync(kit::NewCommandId(), {approval.id, approval.version, kit::ApprovalDecision::Approve});
      if (result.Succeeded() || (result.Error().code != kit::ErrorCode::AlreadyClosed && result.Error().code != kit::ErrorCode::Conflict))
        throw std::runtime_error("The competing submission was not rejected as a closed approval");
    } catch (const std::exception& failure) { error = failure.what(); }
    competing_done = true;
  });
  Wait(ui, [&] {
    const auto approved = UiSelector::Text("Change approved");
    return competing_done && (error.size() || (first.Find(approved).Exists() && second.Find(approved).Exists()));
  }, "The review views did not observe the same decision");
  if (!error.empty()) throw std::runtime_error(error);
  if (first_approve.Exists() || second_approve.Exists() || first.Find(UiSelector::Key("composer")).Exists() ||
      !first.Find(UiSelector::Key("artifact-text")).Exists() || !second.Find(UiSelector::Key("artifact-text")).Exists())
    throw std::runtime_error("Settling the approval changed the reviewed version or left approval controls in a review window");
  Wait(ui, [&] {
    const auto data = opened_review->first->Snapshot();
    const auto call = std::ranges::find(data->tools, opened_review->call, &kit::ToolCallSnapshot::id);
    return call != data->tools.end() && call->state == kit::ToolCallState::Succeeded;
  }, "The approved patch was not applied");
  CheckText(config, "mode=enabled\n");
  CheckText(settings, SettingsPage(true));
}

int VerifyNativeReviewWindows(const File& root) {
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the native review workspace");
  const auto config = workspace.Child("config.txt");
  const auto settings = workspace.Child("settings.html");
  WriteFixture(config, "mode=disabled\n");
  WriteFixture(settings, SettingsPage(false));
  WriteFixture(workspace.Child("CTestTestfile.cmake"), "add_test(verify \"" AGENTKIT_PREVIEW_CMAKE "\" -E true)\n");
  kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}}, std::make_shared<PatchModel>()};
  int result = 1;
  std::size_t windows = 0;
  auto installer = MakeCodeTestInstaller(root.Child("sessions.sqlite3"), workspace, model, File(AGENTKIT_PREVIEW_CTEST), {},
      [&](auto started, std::optional<kit::WorkspaceId> workspace_id, kit::ProjectId project) -> Task<void> {
        const auto app = UseApplication();
        try {
          const auto runtime = LiveTake(std::move(started));
          if (!workspace_id || !app.SupportsMultipleWindows()) throw std::runtime_error("A desktop host with native windows is required");
          const auto chat = co_await LiveConversation(runtime, project, *workspace_id, agentkit_preview::CodeAgent,
              "Native review window acceptance", "Enable the sample mode");
          auto observing = co_await LiveObserve(chat);
          const auto scope = co_await LiveInteraction(chat, "Choose files to share and edit");
          kit::JsonValue files{kit::JsonValue::Array{kit::JsonValue{std::string("config.txt")}, kit::JsonValue{std::string("settings.html")}}};
          LiveTake(co_await chat->SubmitAsync(kit::NewCommandId(), {scope.id, scope.version, std::move(files)}));
          const auto pending = co_await LiveInteraction(chat, "Review workspace changes");
          const auto& plan = std::get<kit::ToolApproval>(pending.body);
          if (plan.preview.artifacts.size() != 2 || plan.preview.targets.size() != 2)
            throw std::runtime_error("The native fixture did not prepare both file previews");
          const kit::AgentChatReviewRequest target{plan.preview.artifacts.front(), 1,
              kit::AgentChatApprovalRef{pending.id, pending.version}};
          auto other = LiveTake(co_await runtime->Sessions().OpenAsync(chat->Snapshot()->summary.id));
          auto observing_other = co_await LiveObserve(other);
          auto first = OpenPreviewArtifactReview(app, chat, runtime, agentkit_preview::CodeAgent, target, {});
          auto second = OpenPreviewArtifactReview(app, other, runtime, agentkit_preview::CodeAgent, target, {});
          if (windows != 3 || !first.IsOpen() || !second.IsOpen())
            throw std::runtime_error("The application did not open two independent review windows");
          CheckText(config, "mode=disabled\n");
          co_await LiveApprove(chat, pending);
          co_await LiveInteraction(other, "Run workspace tests");
          CheckText(config, "mode=enabled\n");
          CheckText(settings, SettingsPage(true));
          const auto observation = other->Snapshot();
          const auto decided = std::ranges::find(observation->interactions, pending.id, &kit::InteractionSnapshot::id);
          if (decided == observation->interactions.end() || decided->state != kit::InteractionState::Approved)
            throw std::runtime_error("The second window's conversation did not observe the decision");
          first.Close();
          if (!second.IsOpen()) throw std::runtime_error("Closing one review closed the other window");
          second.Close();

          auto ppt = co_await LiveConversation(runtime, project, *workspace_id, agentkit_preview::PptAgent,
              "Native PPT review", "Summarize the published design and tests");
          auto observing_ppt = co_await LiveObserve(ppt);
          const auto export_request = co_await StageReviewPresentation(ppt, "NativeReview.pptx");
          const auto deck = std::get<kit::ToolApproval>(export_request.body).preview.artifacts.at(0);
          kit::AgentChatReviewRequest ppt_target{deck, 1, kit::AgentChatApprovalRef{export_request.id, export_request.version}};
          agentkit_preview::PresentationReviewPages pages{{{deck.id, deck.version}, kit::PartId{"slide-2"}}};
          auto ppt_window = OpenPreviewArtifactReview(app, ppt, runtime, agentkit_preview::PptAgent, ppt_target, std::move(pages));
          auto design = co_await LiveConversation(runtime, project, *workspace_id, agentkit_preview::DesignAgent,
              "Native design review", "Design a settings page");
          auto observing_design = co_await LiveObserve(design);
          const auto artboard = co_await LiveInteraction(design, "Review design");
          const auto board = LiveTake(agentkit_preview::ParseDesign(LiveForm(artboard).at("design")));
          auto design_window = agentkit_preview::OpenPreviewDesignReview(app, design, runtime,
              {kit::AgentChatApprovalRef{artboard.id, artboard.version}, board.nodes.at(1).id}, board.title);
          if (windows != 5 || !ppt_window.IsOpen() || !design_window.IsOpen())
            throw std::runtime_error("The app did not create the PPT and design review windows");
          co_await LiveApprove(ppt, export_request);
          co_await LiveRunCompleted(ppt);
          LiveTake(co_await design->SubmitAsync(kit::NewCommandId(), {artboard.id, artboard.version, kit::JsonValue{true}}));
          co_await LiveRunCompleted(design);
          auto exported = archive::ArchiveReader::Open(root.Child("exports").Child("NativeReview.pptx"));
          if (!exported.Succeeded() || ReadPart(exported.Value(), "ppt/slides/slide2.xml").find("Reviewed decisions") == std::string::npos)
            throw std::runtime_error("The native PPT review did not export the selected deck");
          ppt_window.Close();
          if (!design_window.IsOpen()) throw std::runtime_error("Closing the PPT review closed the design window");
          design_window.Close();
          result = 0;
        } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; }
        app.Quit();
      });
  const Application application{App, {
      .window = {.title = "AgentKit Native Review Acceptance", .initial_size = {1080.0F, 760.0F}},
      .application_hooks = {std::move(installer)}, .window_hooks = {[&](WindowContext&) { ++windows; }}}};
  const auto status = RunApplication();
  if (!status && !result) std::cout << "Preview native code, PPT and design review windows opened, resolved their requests and closed independently\n";
  return status ? status : result;
}

struct SettingsTurn {
  kit::ModelProviderRef model;
  kit::RunId run;
  bool has_supplement;
  bool has_quote;
};

class SettingsPatchStream final : public kit::ModelTurnStream {
public:
  explicit SettingsPatchStream(std::string model) : model_(std::move(model)) {}

  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    using kit::JsonValue;
    using Result = kit::AgentResult<std::optional<kit::ModelEvent>>;
    if (index_++ == 0) {
      const JsonValue file{JsonValue::Object{
          {"path", JsonValue{std::string("config.txt")}},
          {"content", JsonValue{"mode=" + model_ + "\n"}}}};
      co_return Result::Success(kit::ModelStructuredOutput{
          JsonValue{JsonValue::Object{{"files", JsonValue{JsonValue::Array{file}}}}}});
    }
    if (index_ == 2) co_return Result::Success(kit::ModelFinished{kit::ModelFinishReason::Completed, {}});
    co_return Result::Success(std::nullopt);
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  std::string model_;
  unsigned index_ = 0;
};

class SettingsPatchModel final : public kit::ModelProvider {
public:
  SettingsPatchModel(std::string label, std::shared_ptr<std::vector<SettingsTurn>> calls)
      : label_(std::move(label)), calls_(std::move(calls)) {}

  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .structured_output = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    const bool supplement = std::ranges::any_of(request.items, [](const auto& item) {
      const auto* content = std::get_if<kit::ModelContent>(&item);
      const auto* body = content ? std::get_if<std::string>(&content->body) : nullptr;
      return body && *body == "Keep the current file scope";
    });
    const bool quote = std::ranges::any_of(request.items, [](const auto& item) {
      const auto* content = std::get_if<kit::ModelContent>(&item);
      const auto* body = content ? std::get_if<std::string>(&content->body) : nullptr;
      return content && content->role == kit::ModelContent::Role::Context && body && *body == "Configuration";
    });
    calls_->push_back({request.provider, request.run, supplement, quote});
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<SettingsPatchStream>(label_));
  }

private:
  std::string label_;
  std::shared_ptr<std::vector<SettingsTurn>> calls_;
};

std::vector<kit::ModelRegistration> SettingsModels(std::shared_ptr<std::vector<SettingsTurn>> calls) {
  std::vector<kit::ModelRegistration> models;
  for (const auto* label : {"A", "B", "C"}) {
    models.push_back({{{std::string("test.settings.") + label}, 1}, {"test", std::string("Model ") + label, "script", {}},
        std::make_shared<SettingsPatchModel>(label, calls)});
  }
  return models;
}

void VerifyPreviewRunSettings(const File& root) {
  const auto workspace = root.Child("project");
  if (workspace.Exists() || !workspace.CreateDirectories()) throw std::runtime_error("Use a fresh run configuration fixture");
  const auto config = workspace.Child("config.txt");
  WriteFixture(config, "mode=initial\n");
  const auto calls = std::make_shared<std::vector<SettingsTurn>>();
  auto models = SettingsModels(calls);
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  auto start = [&](auto started, auto, auto) -> Task<void> {
    runtime = LiveTake(std::move(started));
    tasks = UseApplicationTaskScope();
    co_return;
  };
  Application application{App, {.application_hooks = {MakeRunSettingsTestInstaller(
      root.Child("sessions.sqlite3"), workspace, models, start)}}};
  UiTest preview(application, {.viewport = {1100.0F, 940.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  Wait(preview, [&] { return runtime && preview.Find(UiSelector::Key("run-model")).Exists(); }, "Run settings did not mount");
  const auto choose = [&preview](const char* key, const char* label) {
    auto trigger = preview.Find(UiSelector::Key(key));
    Wait(preview, [&] { return trigger.Exists() && trigger.One().in_viewport; }, "Settings control did not mount");
    ClickPreviewMouse(preview, trigger.One().visible_bounds);
    auto item = preview.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::MenuItem), UiSemanticSelector::Label(label)));
    Wait(preview, [&] { return item.Exists() && item.One().enabled && !item.One().offscreen; }, label);
    item.PerformSemanticAction({SemanticActionKind::Activate, {}});
    Wait(preview, [&] { return !preview.FindSemantics(UiSemanticSelector::Role(SemanticRole::MenuItem)).Exists(); },
        "Selecting an option did not close the setting menu");
    preview.Pump(250ms);
  };
  const auto grant = [&] {
    choose("run-permissions", "Full Access");
    auto dialog = preview.Find(ButtonText("Allow"));
    Wait(preview, [&] { return dialog.Exists(); }, "Full Access did not ask for explicit confirmation");
    dialog.Tap();
    Wait(preview, [&] { return !dialog.Exists(); }, "Full Access dialog did not close");
    preview.Pump(250ms);
  };
  const auto enter = [&](const char* text, const char* button) {
    auto input = preview.Find(UiSelector::Key("composer"));
    FocusPreviewEditor(preview, input, "Conversation editor did not receive focus");
    input.ReplaceText(text);
    preview.Find(ButtonText(button)).Tap();
  };
  const auto scope = [&] {
    auto paths = preview.Find(UiSelector::Key("code-scope"));
    FocusPreviewEditor(preview, paths, "File scope editor did not receive focus");
    paths.ReplaceText("config.txt");
    preview.Find(ButtonText("Share files")).Tap();
  };
  const auto approve_patch = [&] {
    auto approval = preview.Find(ButtonText("Approve"));
    Wait(preview, [&] { return approval.Exists() && approval.One().in_viewport; }, "Code patch bypassed its mandatory approval");
    approval.Tap();
  };
  ChooseDraftAssistant(preview, "Code");
  choose("run-model", "Model A");
  SendNewConversation(preview, "Configuration timing");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("code-scope")).Exists(); }, "Model A run did not wait for file scope");

  choose("run-model", "Model B");
  choose("run-permissions", "Auto Review");
  Wait(preview, [&] { return ShowsText(preview, "This run  \xc2\xb7  Model A  \xc2\xb7  Ask"); },
       "Changing future settings changed the active binding");
  enter("Keep the current file scope", "Send");
  Wait(preview, [&] { return ShowsText(preview, "Waiting to apply"); }, "Direct supplement was not accepted by the active run");
  choose("send-mode", "Queue");
  enter("Queued on B", "Queue message");
  Wait(preview, [&] { return ShowsText(preview, "Model B  \xc2\xb7  Auto Review"); },
       "Queued input did not display its original settings");

  choose("run-model", "Model C");
  grant();
  choose("run-permissions", "Full Access");
  if (preview.Find(ButtonText("Allow")).Exists())
    throw std::runtime_error("Selecting an already confirmed Full Access mode asked again");
  preview.Pump(16ms);
  Wait(preview, [&] { return MessageText(preview, "Configuration timing").has_value(); }, "The quote source was not visible");
  auto source = *MessageText(preview, "Configuration timing");
  SelectHistoryText(preview, source, {0, 13});
  auto quote = preview.Find(ButtonText("Quote"));
  Wait(preview, [&] { return quote.Exists(); }, "The selected range could not be quoted");
  quote.Tap();
  Wait(preview, [&] { return preview.Find(ButtonText("Remove quote")).Exists(); }, "The queued draft did not include its quote");
  enter("Recover this input", "Queue message");
  Wait(preview, [&] { return ShowsText(preview, "2 queued"); }, "Full Access request did not enter the queue");
  preview.Pump(31min);
  scope();
  Wait(preview, [&] { return ShowsText(preview, "Review workspace changes"); }, "Model A did not produce a patch");
  CheckText(config, "mode=initial\n");
  approve_patch();
  Wait(preview, [&] { return preview.Find(UiSelector::Key("code-scope")).Exists(); }, "Queued Model B run did not start");
  CheckText(config, "mode=A\n");
  scope();
  Wait(preview, [&] { return ShowsText(preview, "Review workspace changes"); }, "Model B patch bypassed the manual boundary");
  CheckText(config, "mode=A\n");
  approve_patch();
  Wait(preview, [&] { return preview.Find(UiSelector::Key("code-scope")).Exists(); },
       "Queued Full Access did not survive the wait");
  CheckText(config, "mode=B\n");
  if (calls->size() != 2 || calls->at(0).model != models[0].ref || !calls->at(0).has_supplement ||
      calls->at(1).model != models[1].ref) throw std::runtime_error("Direct or queued model call used the wrong binding");

  scope();
  Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Queued Full Access did not settle", 25s);
  CheckText(config, "mode=C\n");
  if (calls->size() != 3 || calls->back().model != models[2].ref || !calls->back().has_quote ||
      preview.Find(ButtonText("Approve")).Exists()) throw std::runtime_error("Full Access did not apply to the queued model run");

  std::shared_ptr<const kit::SessionSnapshot> known;
  tasks->Launch([&]() -> Task<void> {
    const auto page = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
    auto conversation = LiveTake(co_await runtime->Sessions().OpenAsync(page.sessions.front().id));
    known = conversation->Snapshot();
  });
  Wait(preview, [&] { return bool(known); }, "Run configuration evidence did not load");
  using namespace agentkit_preview;
  if (known->runs.size() != 3 || known->queue.size() != 2 || known->runs[0].execution_profile != AskProfile ||
      known->runs[1].execution_profile != ReviewProfile || known->runs[0].steering.size() != 1 ||
      known->runs[0].steering.front().state != kit::SteerState::Applied ||
      known->queue[0].user_start != kit::RunStartBinding{models[1].ref, ReviewProfile} ||
      known->queue[0].state != kit::QueueItemState::Started || known->queue[0].run != known->runs[1].id ||
      known->queue[1].state != kit::QueueItemState::Started || known->queue[1].run != known->runs[2].id ||
      known->queue[1].reason || known->queue[1].user_start != kit::RunStartBinding{models[2].ref, known->runs[2].execution_profile}) {
    throw std::runtime_error("Stored run and queue bindings do not match the user selections");
  }

  auto composer = preview.Find(UiSelector::Key("composer"));
  FocusPreviewEditor(preview, composer, "Current conversation draft did not receive focus");
  composer.ReplaceText("Draft preserved by session");
  preview.Find(UiSelector::Key("new-conversation")).Tap();
  choose("run-model", "Local demo model");
  choose("run-permissions", "Auto Review");
  SendNewConversation(preview, "Pie chart:\n"
      R"({"title":"Team allocation","categories":[{"label":"Operations","value":6},{"label":"Product","value":2}]})");
  auto csv = preview.Find(ButtonText("Export CSV"));
  Wait(preview, [&] { return csv.Exists(); }, "The automatic review scenario did not render a chart");
  csv.Tap();
  auto output = root.Child("exports").Child("Report chart.csv");
  Wait(preview, [&] { return output.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Automatic review did not approve the new CSV");
  CheckText(output, "Category,Value\r\n\"Operations\",6\r\n\"Product\",2\r\n");
  std::shared_ptr<const kit::SessionSnapshot> chart;
  tasks->Launch([&]() -> Task<void> {
    const auto page = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
    const auto session = std::ranges::find(page.sessions, std::string("Pie chart:"), &kit::SessionSummary::title);
    if (session != page.sessions.end()) chart = LiveTake(co_await runtime->Sessions().OpenAsync(session->id))->Snapshot();
  });
  Wait(preview, [&] { return bool(chart); }, "Automatic review evidence did not load");
  const auto exported = std::ranges::find_if(chart->tools, [](const auto& tool) {
    return tool.tool.id.value == "preview.report.chart.export";
  });
  if (chart->runs.front().execution_profile != ReviewProfile || exported == chart->tools.end() || !exported->review ||
      exported->review->outcome != kit::ToolReviewOutcome::Approved ||
      std::ranges::any_of(chart->interactions, [](const auto& item) { return std::holds_alternative<kit::ToolApproval>(item.body); })) {
    throw std::runtime_error("Automatic review did not retain the plan decision or opened a manual approval");
  }
  preview.Find(UiSelector::Text("Configuration timing")).Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Value("Draft preserved by session")).Exists(); }, "Session switch lost the existing draft");
  choose("run-model", "Model B");
  choose("run-permissions", "Auto Review");
  bool saved = false;
  tasks->Launch([&]() -> Task<void> {
    auto stores = LiveTake(co_await OpenPreviewStoresAsync(root.Child("sessions.sqlite3")));
    const kit::UserRunOptions expected{models[1].ref, ReviewProfile};
    const auto id = known->summary.id;
    for (unsigned attempt = 0; attempt < 100 && !saved; ++attempt) {
      auto entries = LiveTake(co_await stores.preferences->LoadAsync(PreviewUser));
      const auto entry = entries.find(id);
      saved = entry != entries.end() && entry->second == expected;
      if (!saved) co_await Delay(10ms);
    }
  });
  Wait(preview, [&] { return saved; }, "Idle configuration changes were not committed");

  preview.Find(UiSelector::Key("new-conversation")).Tap();
  ChooseDraftAssistant(preview, "Code");
  choose("run-model", "Model A");
  SendNewConversation(preview, "Configuration supplement recovery");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("code-scope")).Exists(); }, "The recovery run did not wait for scope");
  preview.Pump(16ms);
  const auto original = MessageText(preview, "Configuration supplement recovery");
  if (!original) throw std::runtime_error("The supplement's source text was not visible");
  SelectHistoryText(preview, *original, {0, 13});
  TapFormButton(preview, "Quote");
  enter("Keep the current file scope", "Send");
  Wait(preview, [&] { return ShowsText(preview, "Waiting to apply"); }, "The supplement did not bind to the first run");
  TapControl(preview, UiSelector::Key("agent-chat-stop"), "Stop current task");
  const auto restore = preview.Find(UiSelector::Key("restore-supplement"));
  Wait(preview, [&] { return restore.Exists() && restore.One().enabled && ShowsText(preview, "Not applied"); },
       "The stopped run did not make its original supplement recoverable", 25s);
}

void VerifyRunSettingsReload(const File& root) {
  auto calls = std::make_shared<std::vector<SettingsTurn>>();
  auto loaded = [](auto started, auto, auto) -> Task<void> {
    LiveTake(std::move(started));
    co_return;
  };
  Application application{App, {.application_hooks = {MakeRunSettingsTestInstaller(root.Child("sessions.sqlite3"),
      root.Child("project"), SettingsModels(calls), loaded)}}};
  UiTest preview(application, {.viewport = {1100.0F, 940.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  auto conversation = preview.Find(UiSelector::Text("Configuration timing"));
  Wait(preview, [&] { return conversation.Exists(); }, "The persisted code conversation was not available");
  conversation.Tap();
  Wait(preview, [&] {
    const auto options = preview.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Model")));
    const auto permission = preview.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Permissions")));
    return options.Exists() && options.One().value.find("Model B") != std::string::npos &&
           permission.Exists() && permission.One().value == "Auto Review";
  }, "Reopening used a default or the last run binding instead of the saved preferences");
  if (preview.Find(ButtonText("Edit queued input")).Exists() || !calls->empty())
    throw std::runtime_error("Reopening repeated processed queue items");

  const auto recovery_session = preview.Find(UiSelector::Key("session-directory"))
      .Find(UiSelector::Text("Configuration supplement recovery"));
  Wait(preview, [&] { return recovery_session.Exists() && recovery_session.One().in_viewport; },
      "The stopped conversation did not survive restart");
  recovery_session.Tap({.device_kind = PointerDeviceKind::Mouse});
  auto restore = preview.Find(UiSelector::Key("restore-supplement"));
  auto composer = preview.Find(UiSelector::Key("composer"));
  auto draft = preview.Find(UiSelector::AllOf(UiSelector::Key("composer"), UiSelector::Value("Keep the current file scope")));
  Wait(preview, [&] { return restore.Exists() && restore.One().enabled; }, "The original supplement could not be restored after reopening");
  composer.Tap();
  Wait(preview, [&] { return composer.One().focused && preview.ActiveTextInput().has_value(); }, "The empty recovery composer did not receive input focus");
  composer.ReplaceText("Keep this other draft");
  Wait(preview, [&] { return !restore.One().enabled; }, "Restoring a supplement could overwrite a draft");
  composer.ReplaceText("");
  restore.Tap();
  Wait(preview, [&] { return draft.Exists() && ShowsText(preview, "Recovered reference\nConfiguration") &&
      preview.Find(ButtonText("Send as new task")).Exists(); }, "Restoring did not recover the original text and reference");
  if (preview.Find(UiSelector::Key("agent-chat-stop")).Exists() || !calls->empty())
    throw std::runtime_error("Restoring the original supplement automatically started a task");
  Wait(preview, [&] { return composer.One().focused && preview.ActiveTextInput().has_value(); }, "Restoring did not focus the editable draft");
  composer.ReplaceText("");
  TapFormButton(preview, "Remove reference");
  Wait(preview, [&] { return preview.Find(ButtonText("Send")).Exists(); }, "Clearing the recovered draft left its send mode active");

  composer.Tap();
  Wait(preview, [&] { return composer.One().focused && preview.ActiveTextInput().has_value(); }, "The replacement run composer did not receive input focus");
  composer.ReplaceText("Wait in a different run");
  TapFormButton(preview, "Send");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("code-scope")).Exists(); }, "The second run did not wait for scope");
  restore.Tap();
  Wait(preview, [&] { return draft.Exists() && !preview.Find(UiSelector::Key("send-mode")).Exists() &&
      preview.Find(ButtonText("Queue message")).Exists(); }, "The restored input could incorrectly steer a different run");
  preview.Find(UiSelector::Key("new-conversation")).Tap();
  Wait(preview, [&] { return recovery_session.Exists() && recovery_session.One().in_viewport; },
      "The active recovery session could not reopen");
  recovery_session.Tap({.device_kind = PointerDeviceKind::Mouse});
  Wait(preview, [&] { return draft.Exists() && preview.Find(ButtonText("Queue message")).Exists(); },
       "Switching conversations lost the recovered input or its destination");
  TapFormButton(preview, "Queue message");
  Wait(preview, [&] { return ShowsText(preview, "1 queued") && !draft.Exists(); }, "The recovery did not create a new queue item");
  if (!calls->empty()) throw std::runtime_error("The queued recovery started before the active task ended");
  TapControl(preview, UiSelector::Key("agent-chat-stop"), "Stop current task");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("code-scope")).Exists() && !ShowsText(preview, "1 queued"); },
       "The queued recovery did not start after the active run ended", 25s);
  auto paths = preview.Find(UiSelector::Key("code-scope"));
  paths.Tap();
  Wait(preview, [&] { return paths.One().focused && preview.ActiveTextInput().has_value(); }, "The queued run scope did not receive input focus");
  paths.ReplaceText("config.txt");
  TapFormButton(preview, "Share files");
  Wait(preview, [&] { return calls->size() == 1; }, "The queued recovery did not reach the model", 25s);
  if (!calls->front().has_supplement || !calls->front().has_quote)
    throw std::runtime_error("The new task did not receive the recovered original text and quote");
}

class UnconfirmedForkStore final : public kit::SessionStore {
public:
  UnconfirmedForkStore(std::shared_ptr<kit::SessionStore> store, std::shared_ptr<bool> interrupt)
      : store_(std::move(store)), interrupt_(std::move(interrupt)) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return store_->LoadAsync(); }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId commit) override {
    return store_->FindCommitAsync(std::move(commit));
  }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction request) override {
    if (*interrupt_ && std::ranges::any_of(request.writes, [](const auto& write) {
          return write.key.starts_with("workspace-fork:");
        })) {
      *interrupt_ = false;
      co_return kit::AgentResult<kit::Revision>::Failure({
          .code = kit::ErrorCode::CommitUncertain, .message = "The worktree session commit was not confirmed"});
    }
    co_return co_await store_->CommitAsync(std::move(request));
  }

private:
  std::shared_ptr<kit::SessionStore> store_;
  std::shared_ptr<bool> interrupt_;
};

struct ToolResultInterruption {
  kit::SessionId target;
  kit::ToolCallId call;
  bool happened = false;
};

class UnconfirmedToolResultStore final : public kit::SessionStore {
public:
  UnconfirmedToolResultStore(std::shared_ptr<kit::SessionStore> store, std::shared_ptr<ToolResultInterruption> interrupt,
                            std::function<File(const kit::ToolCallId&)> evidence)
      : store_(std::move(store)), interrupt_(std::move(interrupt)), evidence_(std::move(evidence)) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return store_->LoadAsync(); }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId commit) override {
    return store_->FindCommitAsync(std::move(commit));
  }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction request) override {
    if (!interrupt_->happened && !interrupt_->call.value.empty() && std::ranges::any_of(request.writes, [&](const auto& write) {
          if (write.collection != "agentkit.session" || write.key != interrupt_->target.value || !write.data ||
              !evidence_(interrupt_->call).Exists()) return false;
          const auto* bytes = reinterpret_cast<const std::uint8_t*>(write.data->data());
          const auto document = nlohmann::json::from_cbor(bytes, bytes + write.data->size());
          return std::ranges::any_of(document.at("session").at("data").at("tools"), [&](const auto& tool) {
            const auto state = tool.at("state").template get<kit::ToolCallState>();
            return tool.at("id").template get<std::string>() == interrupt_->call.value &&
                (state == kit::ToolCallState::Succeeded || state == kit::ToolCallState::PartiallyApplied);
          });
        })) {
      interrupt_->happened = true;
      co_return kit::AgentResult<kit::Revision>::Failure({.code = kit::ErrorCode::CommitUncertain,
          .message = "The tool outcome commit was interrupted after the host recorded an effect"});
    }
    co_return co_await store_->CommitAsync(std::move(request));
  }

private:
  std::shared_ptr<kit::SessionStore> store_;
  std::shared_ptr<ToolResultInterruption> interrupt_;
  std::function<File(const kit::ToolCallId&)> evidence_;
};

void VerifyWorktreeCopy(const File& root, bool discard = false) {
  const auto code = root.Child("repository");
  const auto copy = root.Child("review checkout");
  const auto occupied = root.Child("occupied");
  const auto scratch = root.Child("git");
  if (root.Exists() && !std::filesystem::is_empty(std::filesystem::u8path(root.Path())))
    throw std::runtime_error("Choose a fresh worktree acceptance directory");
  if (!code.CreateDirectories() || !scratch.CreateDirectories() || !occupied.CreateDirectories())
    throw std::runtime_error("Could not create the Git fixture");
  WriteFixture(occupied.Child("keep.txt"), "existing user data");
  const auto git = [&](const File& folder, std::vector<std::string> args) {
    auto result = LiveTake(agentkit_preview::RunGit(scratch, std::filesystem::u8path(folder.Path()), std::move(args)));
    if (result.exit_code != 0) throw std::runtime_error(result.output);
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) result.output.pop_back();
    return result.output;
  };
  const auto commit = [&](const char* message) {
    git(code, {"-c", "user.name=AgentKit", "-c", "user.email=agentkit@example.invalid", "-c", "commit.gpgsign=false",
        "commit", "-m", message});
  };
  git(code, {"init", "-b", "main"});
  git(code, {"config", "--local", "core.autocrlf", "false"});
  WriteFixture(code.Child("config.txt"), "mode=disabled\n");
  WriteFixture(code.Child("settings.html"), SettingsPage(false));
  WriteFixture(code.Child(".gitignore"), "ignored-notes.txt\n");
  git(code, {"add", "--", "config.txt", "settings.html", ".gitignore"});
  commit("Initial workspace");
  const auto pinned = git(code, {"rev-parse", "HEAD"});
  git(code, {"branch", "review/occupied", pinned});
  WriteFixture(code.Child("untracked.txt"), "user notes");
  kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}},
                               std::make_shared<PatchModel>()};
  auto database = root.Child("sessions.sqlite3");
  auto interrupt = std::make_shared<bool>(true);
  Application application{App, {.application_hooks = {MakeCodeTestInstaller(database, code, model, File(AGENTKIT_PREVIEW_CTEST),
      [interrupt](auto store) { return std::make_shared<UnconfirmedForkStore>(std::move(store), interrupt); })}}};
  const auto committed_copy = UiSelector::Text("Enable sample mode (copy)");
  const auto resume_button = ButtonText("Resume copy \xc2\xb7 review/fixed-base");
  const auto open_recovery = [&](UiTest& preview) {
    auto resume = preview.Find(resume_button);
    Wait(preview, [&] { return resume.Exists(); }, "The interrupted copy was not offered for recovery");
    resume.Tap();
  };
  const auto uncertain = UiSelector::Text("The worktree session commit was not confirmed");
  std::string dirty;
  {
    UiTest preview(application, {.viewport = {1120.0F, 920.0F},
        .resource_provider = std::make_shared<PreviewTestResources>()});
    PrepareCodeChange(preview, "Enable sample mode");
    CheckText(code.Child("config.txt"), "mode=disabled\n");
    auto source = preview.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text("Enable sample mode"));
    Wait(preview, [&] { return source.Count() == 1 && source.One().key.has_value(); },
        "The source worktree session is unavailable");
    const auto session = std::get<std::string>(*source.One().key);
    TapControl(preview, UiSelector::Key("session-actions/" + session), "Open source session actions");
    auto action = preview.Find(UiSelector::Text("Copy to worktree"));
    Wait(preview, [&] { return action.Exists() && action.One().in_viewport; }, "The worktree action did not open");
    action.Tap();
    auto destination = preview.Find(UiSelector::Key("worktree-directory"));
    auto branch = preview.Find(UiSelector::Key("worktree-branch"));
    const auto review_at = [&](const File& path, const char* name) {
      Wait(preview, [&] { return destination.Exists() && branch.Exists(); }, "The worktree fields are unavailable");
      destination.Tap();
      destination.ReplaceText(path.Path());
      branch.Tap();
      branch.ReplaceText(name);
      TapFormButton(preview, "Review worktree");
    };
    const auto rejected = [&](const char* message) {
      TapFormButton(preview, "Create worktree and copy");
      Wait(preview, [&] { return preview.Find(UiSelector::Text(message)).Exists(); }, message);
      TapFormButton(preview, "Edit details");
    };
    if (!discard) {
      review_at(occupied, "review/fixed-base");
      rejected("The worktree destination is already occupied");
      CheckText(occupied.Child("keep.txt"), "existing user data");
      review_at(copy, "review/occupied");
      rejected("The target branch already exists");
      if (copy.Exists()) throw std::runtime_error("A failed plan created the worktree destination");
    }
    review_at(copy, "review/fixed-base");
    auto reviewed = preview.Find(UiSelector::Text("Commit\n" + pinned));
    Wait(preview, [&] { return reviewed.Exists() && reviewed.One().in_viewport; }, "The fixed commit was not shown");
    if (copy.Exists()) throw std::runtime_error("Reviewing the worktree created its destination");
    WriteFixture(code.Child("advanced.txt"), "new HEAD");
    git(code, {"add", "--", "advanced.txt"});
    commit("Advance source after preview");
    WriteFixture(code.Child("staged.txt"), "staged after preview");
    git(code, {"add", "--", "staged.txt"});
    WriteFixture(code.Child("config.txt"), "user edit in source\n");
    dirty = git(code, {"status", "--porcelain", "-z"});
    TapFormButton(preview, "Create worktree and copy");
    Wait(preview, [&] { return preview.Find(uncertain).Exists(); },
        "The uncertain session commit was not reported");
    if (preview.Find(committed_copy).Exists() || !copy.Exists() ||
        git(code, {"worktree", "list", "--porcelain"}).find("locked AgentKit:") == std::string::npos)
      throw std::runtime_error("An unconfirmed copy was published or lost its Git ownership record");
    if (discard) {
      TapFormButton(preview, "Close");
      Wait(preview, [&] { return !preview.Find(ButtonText("Create worktree and copy")).Exists(); },
          "The initial copy dialog did not close");
      open_recovery(preview);
      TapFormButton(preview, "Discard copy");
      TapFormButton(preview, "Remove checkout");
      Wait(preview, [&] { return preview.Find(uncertain).Exists(); }, "Cleanup did not preserve the unknown commit");
      if (!copy.Exists()) throw std::runtime_error("Cleanup removed a checkout with an unknown commit");
    }
  }
  if (discard) {
    const auto plans = root.Child("worktree-plans");
    std::string id;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(plans.Path()))) {
      if (entry.path().extension() == ".owner") id = entry.path().stem().string();
    }
    if (id.empty()) throw std::runtime_error("Missing checkout owner");
    const auto registry = root.Child("workspaces").Child(id + ".workspace");
    auto registered = registry.ReadString();
    if (!registered.Succeeded()) throw std::runtime_error("Could not read the prepared workspace registration");
    const auto discarded = plans.Child(id + ".discarded");
    const auto isolated = root.Child(".agentkit-cleanup-" + id);
    const auto ignored = copy.Child("ignored-notes.txt");
    WriteFixture(ignored, "Keep ignored user data");
    {
      UiTest preview(application, {.viewport = {1120.0F, 920.0F}});
      open_recovery(preview);
      TapFormButton(preview, "Discard copy");
      TapFormButton(preview, "Remove checkout");
      auto preserved = preview.Find(UiSelector::Text("The checkout contains changes or extra files and was kept"));
      Wait(preview, [&] { return preserved.Exists(); }, "Ignored files did not prevent cleanup");
      CheckText(ignored, "Keep ignored user data");
      if (!ignored.Delete()) throw std::runtime_error("Could not reset the worktree test fixture");
      if (!isolated.CreateDirectories()) throw std::runtime_error("Could not create the cleanup conflict fixture");
      const auto other = isolated.Child("keep.txt");
      WriteFixture(other, "Unrelated directory");
      TapFormButton(preview, "Remove checkout");
      auto collision = preview.Find(UiSelector::Text("The cleanup directory is already occupied"));
      Wait(preview, [&] { return collision.Exists(); }, "An occupied cleanup directory was not reported");
      CheckText(other, "Unrelated directory");
      if (!copy.Exists() || !other.Delete() || !std::filesystem::remove(std::filesystem::u8path(isolated.Path())))
        throw std::runtime_error("Cleanup affected the original checkout or could not reset the isolated fixture");
      TapFormButton(preview, "Remove checkout");
      Wait(preview, [&] { return discarded.Exists() && !preview.Find(resume_button).Exists(); },
          "The abandoned checkout was not removed");
      if (copy.Exists() || isolated.Exists() || registry.Exists() || preview.Find(committed_copy).Exists() ||
          !PendingConversation(preview, "Enable sample mode").Exists() ||
          git(code, {"status", "--porcelain", "-z"}) != dirty ||
          git(code, {"rev-parse", "refs/heads/review/fixed-base"}) != pinned)
        throw std::runtime_error("Cleanup changed the source or left a published copy");
    }
    // Reconstruct the interrupted state after Git removal and before workspace unregistration.
    if (!discarded.Delete()) throw std::runtime_error("Could not reset the cleanup receipt fixture");
    WriteFixture(registry, registered.Value());
    {
      UiTest preview(application, {.viewport = {1120.0F, 920.0F}});
      auto finish = preview.Find(ButtonText("Finish cleanup \xc2\xb7 review/fixed-base"));
      Wait(preview, [&] { return finish.Exists(); }, "Partially finished cleanup did not survive restart");
      finish.Tap();
      TapFormButton(preview, "Remove checkout");
      Wait(preview, [&] { return discarded.Exists() && !finish.Exists(); }, "Cleanup did not settle after restart");
      if (copy.Exists() || isolated.Exists() || registry.Exists() || preview.Find(committed_copy).Exists())
        throw std::runtime_error("Resuming cleanup recreated or retained the abandoned copy");
    }
    std::cout << "Preview worktree cleanup, ignored-file protection, and restart passed\n";
    return;
  }
  {
    UiTest preview(application, {.viewport = {1120.0F, 920.0F}});
    auto resume = preview.Find(ButtonText("Resume copy \xc2\xb7 review/fixed-base"));
    Wait(preview, [&] { return resume.Exists(); }, "The interrupted copy was not offered after restart");
    resume.Tap();
    TapFormButton(preview, "Create worktree and copy");
    auto created = preview.Find(committed_copy);
    Wait(preview, [&] { return created.Exists() && !resume.Exists(); }, "The recovered worktree conversation did not open");
    if (!PendingConversation(preview, "Enable sample mode").Exists())
      throw std::runtime_error("Copying the conversation changed the source approval");
    if (git(copy, {"rev-parse", "HEAD"}) != pinned || git(copy, {"branch", "--show-current"}) != "review/fixed-base" ||
        !git(copy, {"status", "--porcelain", "-z"}).empty() || git(code, {"status", "--porcelain", "-z"}) != dirty ||
        copy.Child("advanced.txt").Exists() || copy.Child("staged.txt").Exists() || copy.Child("untracked.txt").Exists() ||
        git(code, {"worktree", "list", "--porcelain"}).find("locked AgentKit:") != std::string::npos)
      throw std::runtime_error("The destination did not use a clean checkout of the approved commit");
    CheckText(copy.Child("config.txt"), "mode=disabled\n");
    CheckText(code.Child("config.txt"), "user edit in source\n");
  }
  {
    UiTest preview(application, {.viewport = {1120.0F, 920.0F}});
    auto created = preview.Find(committed_copy);
    Wait(preview, [&] { return created.Exists(); }, "The worktree conversation was not restored");
    created.Tap();
    auto composer = preview.Find(UiSelector::Key("composer"));
    Wait(preview, [&] { return composer.Exists(); }, "The copied conversation did not open after restart");
    if (preview.Find(ButtonText("Approve")).Exists())
      throw std::runtime_error("The copied conversation inherited approval from the source");
    composer.Tap();
    composer.ReplaceText("Enable mode in the copied workspace");
    TapFormButton(preview, "Send");
    SubmitWorkspaceScope(preview);
    CheckText(copy.Child("config.txt"), "mode=disabled\n");
    TapFormButton(preview, "Approve");
    Wait(preview, [&] { return preview.Find(UiSelector::Text("Run workspace tests")).Exists(); },
        "The approved edit in the worktree did not finish");
    CheckText(copy.Child("config.txt"), "mode=enabled\n");
    CheckText(code.Child("config.txt"), "user edit in source\n");
  }
  std::cout << "Preview Git worktree fork, fixed commit, and restart passed\n";
}

void PrepareReleaseTarget(UiTest& preview) {
  ChooseDraftAssistant(preview, "PPT");
  SendNewConversation(preview, "Workspace release slides");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("ppt-audience")).Exists(); }, "Missing initial PPT form");
  preview.Find(UiSelector::Key("agent-chat-stop")).Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Workspace release slides")).Exists() &&
      !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The existing PPT target is still busy");
}

void SetReleaseSlideCount(UiTest& preview, const char* value, std::initializer_list<const char*> expected_impacts = {},
                          bool complete_automatically = false) {
  auto edit = preview.Find(ButtonText("Edit goal"));
  Wait(preview, [&] { return edit.Exists() && edit.One().enabled; }, "Release goal cannot be edited");
  edit.Tap();
  auto count = preview.Find(UiSelector::Key("goal-slide-count"));
  Wait(preview, [&] { return count.Exists(); }, "Missing release slide count");
  count.Tap();
  count.ReplaceText(value);
  if (complete_automatically) {
    TapControl(preview, UiSelector::Key("goal-confirm-completion"), "Disable completion confirmation");
  }
  preview.Find(ButtonText("Review goal change")).Tap();
  auto apply = preview.Find(ButtonText("Apply goal change"));
  Wait(preview, [&] { return apply.Exists() && apply.One().enabled; }, "The release goal change was not prepared");
  for (const auto* impact : expected_impacts) {
    if (!preview.Find(UiSelector::Text(impact)).Exists())
      throw std::runtime_error(std::string("The goal revision preview omitted: ") + impact);
  }
  apply.Tap();
  const auto expected = std::string(value) + " slides";
  Wait(preview, [&] { return preview.Find(UiSelector::Text(expected)).Exists(); }, "The release slide count was not updated");
}

void StartWorkspaceRelease(const Application& application, const File& config, const File& settings,
    std::string objective, const char* slide_count = nullptr) {
  constexpr auto transition_timeout = 25s;
  UiTest preview(application, {.viewport = {1080.0F, 920.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  PrepareReleaseTarget(preview);
  preview.Find(UiSelector::Key("new-conversation")).Tap();
  SendNewConversation(preview, std::move(objective));
  auto goal = preview.Find(ButtonText("Goal"));
  Wait(preview, [&] { return goal.Exists(); }, "Release conversation did not open");
  goal.Tap();
  auto enable = preview.Find(ButtonText("Enable"));
  Wait(preview, [&] { return enable.Exists() && enable.One().enabled; }, "The General assistant did not propose the release goal");
  if (slide_count) SetReleaseSlideCount(preview, slide_count);
  enable.Tap();
  auto design = PendingConversation(preview, "Release design");
  auto review = preview.Find(ButtonText("Review design"));
  try {
    Wait(preview, [&] { return design.Exists() && design.One().in_viewport; },
        "The release design did not request approval", transition_timeout);
    design.Tap();
    Wait(preview, [&] { return review.Exists() && review.One().in_viewport; }, "Release design not ready for review");
  } catch (...) {
    static_cast<void>(config.Parent()->Child("release-design.snapshot").WriteString(preview.CaptureSnapshot().ToString()));
    throw;
  }
  review.Tap();
  preview.Find(ButtonText("Approve design")).Tap();
  auto code = PendingConversation(preview, "Release implementation");
  Wait(preview, [&] { return code.Exists(); }, "The release implementation did not request workspace scope", transition_timeout);
  code.Tap();
  SubmitWorkspaceScope(preview);
  CheckText(config, "mode=disabled\n");
  CheckText(settings, SettingsPage(false));
  TapReviewCard(preview, "Review attachment 2");
  const auto diff = preview.Find(UiSelector::Key("artifact-text"));
  Wait(preview, [&] { return diff.Exists() && diff.One().in_viewport; }, "The release patch did not open in review");
  auto tabs = preview.Find(UiSelector::Key("review-attachments"));
  tabs.Find(UiSelector::Text("Design source")).Tap();
  const auto artboard = preview.Find(UiSelector::Key("design-source-artboard"));
  const auto approval = preview.Find(ButtonText("Approve reviewed change"));
  Wait(preview, [&] { return artboard.Exists() && artboard.One().in_viewport && tabs.Exists() && approval.Exists(); },
      "The approved design source did not stay in the patch review");
  CheckText(config, "mode=disabled\n");
  CheckText(settings, SettingsPage(false));
  tabs.Find(UiSelector::Text("settings.html")).Tap();
  Wait(preview, [&] { return diff.Exists() && ReviewedDiff(preview).starts_with("settings.html\n"); },
      "The code review did not return to the selected file");
  approval.Tap();
  auto verification = PendingConversation(preview, "Verify Release implementation");
  Wait(preview, [&] { return verification.Exists(); }, "The code assistant did not open independent verification", transition_timeout);
  CheckText(config, "mode=enabled\n");
  CheckText(settings, SettingsPage(true));
}

void RejectIncompleteVerification(const Application& application, const File& tested, const char* title, bool tests_passed) {
  UiTest preview(application, {.viewport = {1080.0F, 920.0F}});
  auto verification = PendingConversation(preview, "Verify Release implementation");
  Wait(preview, [&] { return verification.Exists(); }, "Missing verification for the failing release");
  verification.Tap();
  auto ctest = preview.Find(UiSelector::Text("Run workspace tests"));
  Wait(preview, [&] { return ctest.Exists(); }, "Missing test plan for the failing release");
  preview.Find(ButtonText("Approve")).Tap();
  Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Failed verification did not settle");
  if (tests_passed) {
    CheckText(tested, "verified");
    Wait(preview, [&] { return ShowsText(preview, "The workspace tests did not produce the required implementation screenshot"); },
        "The verification did not report the missing screenshot");
  }
  CheckCTestDisclosure(preview, tests_passed ? "100% tests passed" : "Intentional workspace verification failure");
  auto general = ConversationRow(preview, title);
  Wait(preview, [&] { return general.Exists(); }, "The failing release conversation disappeared");
  general.Tap();
  auto goal = preview.Find(ButtonText("Goal"));
  Wait(preview, [&] { return goal.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Failed verification did not reach the goal");
  goal.Tap();
  Wait(preview, [&] { return preview.Find(ButtonText("Resume")).Exists(); }, "Failed verification did not block the release goal");
  if (tested.Exists() != tests_passed || PendingConversation(preview, "Workspace release slides").Exists() ||
      preview.Find(ButtonText("Open completion approval")).Exists()) {
    throw std::runtime_error("A failed verification reached publication or completion approval");
  }
}

void ReviseWorkspaceRelease(const Application& application, const File& tested, const File& old_output) {
  constexpr auto transition_timeout = 25s;
  UiTest preview(application, {.viewport = {1080.0F, 920.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  auto verification = PendingConversation(preview, "Verify Release implementation");
  Wait(preview, [&] { return verification.Exists(); }, "Verification approval did not survive restart");
  verification.Tap();
  auto ctest = preview.Find(UiSelector::Text("Run workspace tests"));
  Wait(preview, [&] { return ctest.Exists(); }, "Missing scoped test plan after restart");
  if (tested.Exists()) throw std::runtime_error("The release ran CTest before approval");
  preview.Find(ButtonText("Approve")).Tap();
  Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Release verification did not finish", transition_timeout);
  CheckText(tested, "verified");
  auto ppt = PendingConversation(preview, "Workspace release slides");
  Wait(preview, [&] { return ppt.Exists(); }, "Verified implementation did not unlock the existing PPT conversation", transition_timeout);
  ppt.Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Goal requires 10 slides.")).Exists(); }, "The first PPT run did not use the ten-slide definition");
  TapFormButton(preview, "Draft outline");
  Wait(preview, [&] { return preview.Find(UiSelector::Text("1 / 10")).Exists(); }, "The ten-slide outline was not prepared");
  TapFormButton(preview, "Confirm outline");
  auto name = preview.Find(UiSelector::Key("ppt-file_name"));
  Wait(preview, [&] { return name.Exists(); }, "The original deck did not reach its export form");
  name.Tap();
  name.ReplaceText("WorkspaceReleaseBeforeEdit.pptx");
  TapFormButton(preview, "Review export");
  auto approval = preview.Find(UiSelector::Text("Export PowerPoint"));
  Wait(preview, [&] { return approval.Exists(); }, "The ten-slide export did not reach approval");
  if (old_output.Exists()) throw std::runtime_error("The old export ran before approval");
  preview.Find(UiSelector::Text("Workspace release: Ship settings")).Tap();
  auto goal = preview.Find(ButtonText("Goal"));
  Wait(preview, [&] { return goal.Exists(); }, "The release goal could not be reopened");
  goal.Tap();
  SetReleaseSlideCount(preview, "5", {"1 linked runs will stop for this revision."});
  if (old_output.Exists()) throw std::runtime_error("The original export ran when the goal changed");
}

void ReviewReleasedDesignImpact(UiTest& preview, const File& edited);

void FinishWorkspaceRelease(const Application& application, const File& tested, const File& output, const File& old_output) {
  UiTest preview(application, {.viewport = {1080.0F, 920.0F}});
  auto ppt = PendingConversation(preview, "Workspace release slides");
  Wait(preview, [&] { return ppt.Exists(); }, "The new revision did not resume after restart");
  ppt.Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Goal requires 5 slides.")).Exists(); }, "Restart exposed the old goal instead of the five-slide brief");
  if (preview.Find(UiSelector::Text("Export PowerPoint")).Exists() || old_output.Exists()) {
    throw std::runtime_error("The old export approval was restored");
  }
  CheckText(tested, "verified");
  TapFormButton(preview, "Draft outline");
  Wait(preview, [&] { return preview.Find(UiSelector::Text("1 / 5")).Exists(); },
      "The five-slide release outline did not appear after submitting the revised brief", 25s);
  TapFormButton(preview, "Confirm outline");
  auto name = preview.Find(UiSelector::Key("ppt-file_name"));
  Wait(preview, [&] { return name.Exists(); }, "Release PPTX did not reach the export form");
  for (int page = 1; page < 5; ++page) TapFormButton(preview, "Next slide");
  auto screenshot = preview.Find(UiSelector::Type<Image>());
  Wait(preview, [&] { return screenshot.Exists(); }, "The approved implementation screenshot did not load in the slide preview");
  if (!name.One().in_viewport) {
    name = preview.Find(UiSelector::Key("ppt-form-scroll")).ScrollUntil(UiSelector::Key("ppt-file_name"), {.step = {0.0F, 94.0F}});
  }
  name.Tap();
  name.ReplaceText("WorkspaceRelease.pptx");
  TapFormButton(preview, "Review export");
  auto approve = preview.Find(ButtonText("Approve"));
  Wait(preview, [&] { return approve.Exists() && approve.One().in_viewport; }, "Missing release export approval");
  if (output.Exists()) throw std::runtime_error("Release PPTX was exported before approval");
  approve.Tap();
  auto general = PendingConversation(preview, "Workspace release: Ship settings");
  Wait(preview, [&] { return general.Exists() && output.Exists(); }, "Release did not reach completion approval", 25s);
  auto deck = archive::ArchiveReader::Open(output);
  if (!deck.Succeeded()) throw std::runtime_error(deck.Error().message);
  if (ReadPart(deck.Value(), "ppt/slides/slide1.xml").find("Save changes") == std::string::npos ||
      ReadPart(deck.Value(), "ppt/slides/slide5.xml").find("Workspace tests passed") == std::string::npos ||
      deck.Value().ReadEntry("ppt/slides/slide6.xml").Succeeded() || old_output.Exists()) {
    throw std::runtime_error("Release PPTX does not reflect the approved design and verification");
  }
  CheckReleaseScreenshot(deck.Value(), *output.Parent()->Parent(), "slide5");
  general.Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Complete goal")).Exists(); }, "Missing final goal approval");
  approve.Tap();
  preview.Find(ButtonText("Goal")).Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Text("Completed")).Exists(); }, "The verified release goal did not complete");

  ReviewReleasedDesignImpact(preview, tested.Parent()->Child("settings.html"));
}

void ReviewReleasedDesignImpact(UiTest& preview, const File& edited) {
  auto design = preview.Find(UiSelector::Text("Release design"));
  Wait(preview, [&] { return design.Exists(); }, "The published release design was not in the conversation list");
  design.Tap();
  auto latest = preview.Find(ReviewCard("Review artifact v1"));
  Wait(preview, [&] { return latest.Exists(); }, "The approved design version could not be reopened");
  latest.Tap();
  auto save = preview.Find(ButtonText("Save changes"));
  Wait(preview, [&] { return save.Exists() && save.One().in_viewport; }, "The released Save changes node was not available");
  save.Tap();
  auto inspect = preview.Find(ButtonText("Inspect impact"));
  Wait(preview, [&] { return inspect.Exists() && inspect.One().in_viewport; }, "The published design did not offer an impact query");
  inspect.Tap();
  auto settings_part = preview.Find(ButtonText("Review settings.html"));
  auto config_part = preview.Find(ButtonText("Review config.txt"));
  Wait(preview, [&] {
    return settings_part.Exists() && preview.Find(ButtonText("Review slide 5 / Verification")).Exists() &&
        preview.Find(UiSelector::Text("Implementation screenshot  v1 (approximate scope)")).Exists();
  },
      "The save node did not lead to the changed settings page and verification slide");
  if (config_part.Exists()) throw std::runtime_error("The save node incorrectly affected the product mode configuration");
  const auto open_impact = [&](const char* label) {
    auto link = preview.Find(ButtonText(label));
    if (!link.One().in_viewport) {
      link = preview.Find(UiSelector::Key("design-review-scroll")).ScrollUntil(ButtonText(label), {.step = {0.0F, 130.0F}});
    }
    link.Tap();
  };
  WriteFixture(edited, "New local edits after the release");
  open_impact("Review settings.html");
  Wait(preview, [&] {
    const auto editors = preview.FindSemantics(UiSemanticSelector::Role(SemanticRole::TextField)).All();
    return std::ranges::any_of(editors, [](const auto& editor) {
      return editor.value.find(" checked> Enabled") != std::string::npos &&
          editor.value.find("> Disabled") != std::string::npos && editor.value.find("New local edits") == std::string::npos;
    });
  }, "The link did not open the applied file's recorded diff");
  CheckText(edited, "New local edits after the release");
  TapFormButton(preview, "Close review");
  Wait(preview, [&] { return settings_part.Exists() && settings_part.One().focused; },
      "Returning from an affected file did not restore the source node's impact action");
  open_impact("Review Implementation screenshot");
  Wait(preview, [&] { return preview.Find(UiSelector::Type<Image>()).Exists(); },
      "The approximate impact did not open the original screenshot");
  TapFormButton(preview, "Close review");
  open_impact("Review slide 5 / Verification");
  Wait(preview, [&] { return ShowsText(preview, "5 / 5") && ShowsText(preview, "Workspace tests passed"); },
      "The design impact did not open the published verification slide");
  TapFormButton(preview, "Close review");
  auto scroll = preview.Find(UiSelector::Key("design-review-scroll"));
  if (!preview.Find(ButtonText("Enabled")).One().in_viewport)
    static_cast<void>(scroll.ScrollUntil(ButtonText("Enabled"), {.step = {0.0F, -130.0F}}));
  auto mode = preview.Find(ButtonText("Enabled"));
  Wait(preview, [&] { return mode.Exists() && mode.One().in_viewport; }, "The released mode node was not available");
  mode.Tap();
  inspect.Tap();
  Wait(preview, [&] { return config_part.Exists() && settings_part.Exists(); }, "The mode node did not locate both of its changed files");
}

void VerifyStoredReleaseImpact(const File& root) {
  const auto database = root.Child("sessions.sqlite3");
  if (!database.Exists() || !root.Child("exports").Child("WorkspaceRelease.pptx").Exists())
    throw std::runtime_error("Use the directory from a completed release acceptance run");
  const auto workspace = root.Child("code");
  CheckText(workspace.Child("tested.txt"), "verified");
  const auto settings = workspace.Child("settings.html");
  const auto enabled = SettingsPage(true);
  const auto before = settings.ReadString();
  if (!before.Succeeded() || (before.Value() != enabled && before.Value() != "New local edits after the release"))
    throw std::runtime_error("The directory does not contain the release test's settings page");
  const kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}},
                                    std::make_shared<PatchModel>()};
  const Application application{App, {.application_hooks = {MakeCodeTestInstaller(database, workspace,
      model, File(AGENTKIT_PREVIEW_CTEST))}}};
  UiTest preview(application, {.viewport = {1080.0F, 920.0F}});
  ReviewReleasedDesignImpact(preview, settings);
}

enum class PublicationInterruption { None, Design, Presentation };

void VerifyUnpublishedAdoption(UiTest& ui, const std::shared_ptr<kit::AgentRuntime>& runtime, const std::optional<TaskScope>& tasks,
    const char* title, const char* criteria, const ToolResultInterruption& interrupted, kit::ArtifactRef baseline,
    const kit::ForkCandidate& candidate, const File& staged, bool presentation) {
  Wait(ui, [&] { return runtime && tasks; }, "Publication recovery did not start");
  std::shared_ptr<kit::Conversation> target;
  const kit::ArtifactRef unpublished{baseline.id, baseline.version + 1};
  const auto inspect = [&](std::function<Task<void>()> action) {
    bool done = false;
    std::string error;
    tasks->Launch([&]() -> Task<void> {
      try { co_await action(); }
      catch (const std::exception& failure) { error = failure.what(); }
      done = true;
    });
    Wait(ui, [&] { return done; }, "The publication check did not finish");
    if (!error.empty()) throw std::runtime_error(error);
  };
  kit::ArtifactPreview original_preview;
  inspect([&]() -> Task<void> {
    target = LiveTake(co_await runtime->Sessions().OpenAsync(interrupted.target));
    const auto snapshot = target->Snapshot();
    const auto call = std::ranges::find(snapshot->tools, interrupted.call, &kit::ToolCallSnapshot::id);
    if (snapshot->artifacts != std::vector{baseline} || call == snapshot->tools.end() || !call->issued_targets ||
        call->state != kit::ToolCallState::ResultUnknown)
      throw std::runtime_error("The interrupted adoption did not retain the original published version and call");
    const auto absent = co_await runtime->Artifacts().ReadAsync(unpublished);
    if (absent.Succeeded() || absent.Error().code != kit::ErrorCode::NotFound)
      throw std::runtime_error("The private staged file appeared as a registered version");
    original_preview = LiveTake(co_await runtime->Artifacts().PreviewAsync(baseline));
    if (presentation) {
      const auto selected = LiveTake(co_await agentkit_preview::ReadPresentationBytes(runtime, candidate.artifacts.front()));
      const auto bytes = staged.ReadBytes();
      if (!bytes.Succeeded() || bytes.Value() != selected) throw std::runtime_error("The original call did not stage the exact approved deck");
    } else {
      const auto bytes = staged.ReadString();
      if (!bytes.Succeeded()) throw std::runtime_error("The original call did not stage the approved design");
      const auto board = nlohmann::json::parse(bytes.Value());
      if (board.at("title").get<std::string>() != "Compact navigation")
        throw std::runtime_error("The original call did not stage the approved design");
    }
  });
  auto row = ConversationRow(ui, title);
  Wait(ui, [&] { return row.Exists() && row.One().in_viewport; }, "The target conversation did not reopen");
  row.Tap();
  TapFormButton(ui, "Compare alternatives");
  TapFormButton(ui, criteria);
  auto action = ui.Find(UiSelector::Key("comparison-adopt/" + candidate.id.value));
  TapFormButton(ui, "Check adoption result");
  Wait(ui, [&] { return ShowsText(ui, "Verified: no changes applied"); }, "The unregistered version was not reconciled as unpublished");
  const auto reconciled = target->Snapshot();
  const auto prior = std::ranges::find(reconciled->tools, interrupted.call, &kit::ToolCallSnapshot::id);
  if (prior == reconciled->tools.end() || prior->state != kit::ToolCallState::Failed || !prior->reconciliation ||
      prior->effects.size() != 1 || prior->effects.front().state != kit::ResourceEffectState::Unchanged ||
      reconciled->artifacts != std::vector{baseline})
    throw std::runtime_error("Reconciliation changed the target version or lost the no-write result");
  Wait(ui, [&] { return action.Exists() && action.One().in_viewport && action.One().enabled; }, "The reconciled candidate could not be submitted again");
  action.Tap({.device_kind = PointerDeviceKind::Mouse});
  const auto preview = presentation ? "Review PowerPoint adoption" : "Review design adoption";
  Wait(ui, [&] { return ShowsText(ui, preview); }, "The new adoption did not request a fresh approval");
  if (target->Snapshot()->artifacts != std::vector{baseline}) throw std::runtime_error("The new adoption published before approval");
  TapFormButton(ui, "Approve");
  Wait(ui, [&] { return target->Snapshot()->artifacts.size() == 2 && !target->Snapshot()->summary.active_run; },
      "The approved retry did not publish a version");
  inspect([&]() -> Task<void> {
    const auto result = LiveTake(co_await runtime->Artifacts().ReadAsync(unpublished));
    const auto visible = LiveTake(co_await runtime->Artifacts().PreviewAsync(unpublished));
    const auto selected = LiveTake(co_await runtime->Artifacts().PreviewAsync(candidate.artifacts.front()));
    const auto still_original = LiveTake(co_await runtime->Artifacts().PreviewAsync(baseline));
    if (!result.call || result.call == interrupted.call || visible.structured != selected.structured ||
        still_original.structured != original_preview.structured ||
        std::ranges::none_of(result.dependencies, [&](const auto& dependency) {
          return dependency.input.source == kit::SourceRef{candidate.artifacts.front()};
        })) throw std::runtime_error("The new version did not retain the selected content, original version and new call");
    if (presentation) {
      const auto adopted = LiveTake(co_await agentkit_preview::ReadPresentationBytes(runtime, unpublished));
      const auto original_stage = staged.ReadBytes();
      if (!original_stage.Succeeded() || adopted != original_stage.Value())
        throw std::runtime_error("The newly published deck differed from the candidate package");
    }
  });
}

void VerifyForkReview(const File& directory, PublicationInterruption scenario = PublicationInterruption::None) {
  const auto workspace = directory.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the comparison workspace");
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  const auto interruption = std::make_shared<ToolResultInterruption>();
  const auto staged_output = [directory, scenario](const kit::ToolCallId& call) {
    if (scenario == PublicationInterruption::Design) return directory.Child("designs").Child(call.value + ".design.json");
    return directory.Child("presentations").Child("adopted-" + call.value + ".pptx");
  };
  PreviewStoreAdapter adapt_store;
  if (scenario != PublicationInterruption::None) adapt_store = [interruption, staged_output](auto store) {
    return std::make_shared<UnconfirmedToolResultStore>(std::move(store), interruption, staged_output);
  };
  const auto hook = MakeRunSettingsTestInstaller(directory.Child("sessions.sqlite3"), workspace, {},
      [&](auto started, std::optional<kit::WorkspaceId>, kit::ProjectId) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      }, true, std::move(adapt_store));
  const Application app{App, {.application_hooks = {hook}}};
  constexpr auto title = "Comparison requirements";
  constexpr auto ppt_criteria = "Use the approved settings design";
  constexpr auto design_criteria = "Compare navigation approaches";
  constexpr auto deck_title = "Rollout comparison target";
  constexpr auto deck_criteria = "Compare two rollout presentations";
  const auto exports = directory.Child("exports");
  std::vector<kit::CandidateId> design_candidates;
  std::string adopted_name;
  kit::ArtifactRef interrupted_baseline;
  kit::ForkCandidate interrupted_candidate;
  const auto run_comparisons = [&] {
    UiTest ui(app, {.viewport = {1080.0F, 900.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    Wait(ui, [&] { return runtime && tasks; }, "Comparison workspace did not start");
    ChooseDraftAssistant(ui, "Design");
    auto composer = ui.Find(UiSelector::Key("new-conversation-composer"));
    FocusPreviewEditor(ui, composer, "The comparison draft did not receive focus");
    composer.ReplaceText(title);
    TapFormButton(ui, "Send");
    TapFormButton(ui, "Review design");
    TapFormButton(ui, "Approve design");
    Wait(ui, [&] { return ui.Find(ReviewCard("Review artifact v1")).Exists() && !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
        "The comparison source did not settle");

    std::shared_ptr<kit::Conversation> target;
    kit::ArtifactRef baseline;
    std::string error;
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto listed = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
        const auto source = std::ranges::find(listed.sessions, std::string(title), &kit::SessionSummary::title);
        if (source == listed.sessions.end()) throw std::runtime_error("The source session is unavailable");
        target = LiveTake(co_await runtime->Sessions().OpenAsync(source->id));
        baseline = target->Snapshot()->artifacts.back();
      } catch (const std::exception& failure) { error = failure.what(); }
    });
    Wait(ui, [&] { return target || !error.empty(); }, "The source session could not be inspected");
    if (!error.empty()) throw std::runtime_error(error);
    auto source_id = target->Snapshot()->summary.id;
    const auto read_review = [&](std::string criteria, bool ready) {
      std::optional<kit::ForkReviewSnapshot> review;
      tasks->Launch([&]() -> Task<void> {
        try {
          for (int attempt = 0; attempt < 120; ++attempt) {
            auto summaries = co_await runtime->ForkReviews().ListComparisonsAsync(source_id);
            if (summaries.Succeeded()) {
              const auto found = std::ranges::find(summaries.Value(), kit::JsonValue{criteria}, &kit::ForkReviewSummary::criteria);
              if (found != summaries.Value().end() && (!ready || found->state == kit::ForkReviewState::Ready)) {
                auto loaded = co_await runtime->ForkReviews().ReadComparisonAsync(found->id);
                if (loaded.Succeeded()) { review = std::move(loaded).Value(); co_return; }
                if (loaded.Error().code != kit::ErrorCode::CursorExpired) LiveTake(std::move(loaded));
              }
            } else if (summaries.Error().code != kit::ErrorCode::CursorExpired) {
              LiveTake(std::move(summaries));
            }
            co_await Delay(20ms);
          }
          error = "The comparison query did not settle";
        } catch (const std::exception& failure) { error = failure.what(); }
      });
      Wait(ui, [&] { return review || !error.empty(); }, "Could not find the committed comparison");
      if (!error.empty()) throw std::runtime_error(error);
      return std::move(*review);
    };
    const auto fill = [&](const char* key, const char* value) {
      auto input = ui.Find(UiSelector::Key(key));
      try {
        Wait(ui, [&] { return input.Exists() && input.One().in_viewport && input.One().enabled; }, "The comparison field did not mount");
      } catch (...) {
        static_cast<void>(directory.Child("comparison-field.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
        throw std::runtime_error("The comparison field did not mount: " + std::string(key) + " for " + value);
      }
      FocusPreviewEditor(ui, input, key);
      input.ReplaceText(value);
    };
    const auto start = [&](const char* criteria, const char* first, const char* second) {
      fill("comparison-criteria", criteria);
      fill("comparison-first", first);
      fill("comparison-second", second);
      TapFormButton(ui, "Start comparison");
      Wait(ui, [&] { return ShowsText(ui, "Approve comparison"); }, "The source did not show the comparison approval");
      TapFormButton(ui, "Approve");
      const auto created = read_review(criteria, false);
      TapFormButton(ui, "Compare alternatives");
      TapFormButton(ui, criteria);
      Wait(ui, [&] { return ui.Find(ButtonText("Close comparison")).Exists(); }, "The comparison did not open");
      return created;
    };
    const auto open_branch = [&](const kit::ForkCandidate& candidate) {
      auto open = ui.Find(UiSelector::Key("comparison-open/" + candidate.session.value));
      Wait(ui, [&] { return open.Exists() && open.One().in_viewport; }, "The candidate could not be opened from the comparison");
      open.Tap();
      try {
        Wait(ui, [&] { return ui.Find(ButtonText("Back to comparison")).Exists(); }, "The candidate lost its comparison return action");
      } catch (...) {
        static_cast<void>(directory.Child("comparison-navigation.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
        throw;
      }
    };
    const auto side_by_side = [&](const kit::ForkReviewSnapshot& review) {
      auto left = ui.Find(UiSelector::Key(review.candidates[0].id.value));
      auto right = ui.Find(UiSelector::Key(review.candidates[1].id.value));
      Wait(ui, [&] { return left.Exists() && right.Exists() && left.One().in_viewport && right.One().in_viewport; },
          "The comparison did not show both candidates together");
      if (left.One().bounds.x >= right.One().bounds.x || left.One().bounds.y != right.One().bounds.y)
        throw std::runtime_error("The desktop comparison did not place candidates side by side");
    };
    const auto interrupt_publication = [&](const char* criteria, const kit::ForkCandidate& selected) {
      interruption->target = source_id;
      const auto review = read_review(criteria, false);
      if (!review.adoption_call) throw std::runtime_error("The staged adoption did not have an original call");
      interruption->call = *review.adoption_call;
      interrupted_baseline = baseline;
      interrupted_candidate = selected;
      TapFormButton(ui, "Approve");
      Wait(ui, [&] { return interruption->happened; }, "The adoption was not interrupted between staging and publication");
    };

    auto source_draft = ui.Find(UiSelector::Key("composer"));
    FocusPreviewEditor(ui, source_draft, "The comparison source draft did not receive focus");
    source_draft.ReplaceText("Keep the comparison source draft");
    TapFormButton(ui, "Compare alternatives");
    auto powerpoint = ui.Find(UiSelector::Text("PowerPoint"));
    Wait(ui, [&] { return powerpoint.Exists(); }, "The comparison format selector did not load");
    powerpoint.Tap();
    const auto ppt = start(ppt_criteria, "Operations rollout", "Launch overview");
    for (std::size_t i = 0; i < ppt.candidates.size(); ++i) {
      open_branch(ppt.candidates[i]);
      TapFormButton(ui, "Draft outline");
      fill("slide-title-0", i == 0 ? "Operational milestones" : "Launch impact");
      TapFormButton(ui, "Add slide");
      fill("slide-title-1", i == 0 ? "Readiness checks" : "Release milestones");
      TapFormButton(ui, "Confirm outline");
      TapFormButton(ui, "Back to comparison");
    }
    const auto decks = read_review(ppt_criteria, true);
    if (decks.candidates.size() != 2 || !std::ranges::all_of(decks.candidates, [](const auto& item) {
          return item.state == kit::CandidateState::Ready && item.artifacts.size() == 1 && item.usage.tool_calls == 1;
        }) || decks.candidates[0].artifacts == decks.candidates[1].artifacts)
      throw std::runtime_error("The presentation comparison did not retain both candidate outputs");
    side_by_side(decks);
    Wait(ui, [&] { return ShowsText(ui, "Operational milestones") && ShowsText(ui, "Launch impact"); },
        "Both slide results did not appear together");
    TapFormButton(ui, "Next slide");
    Wait(ui, [&] { return ShowsText(ui, "Readiness checks") && ShowsText(ui, "Release milestones"); },
        "The page controls did not advance both candidates");
    std::set<std::string> titles;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(directory.Child("presentations").Path()))) {
      if (entry.path().extension() != ".pptx") continue;
      auto deck = archive::ArchiveReader::Open(File(entry.path().u8string()));
      if (!deck.Succeeded()) throw std::runtime_error("The staged candidate did not open as a PPTX");
      const auto xml = ReadPart(deck.Value(), "ppt/slides/slide1.xml");
      for (const auto* header : {"Operational milestones", "Launch impact"}) if (xml.find(header) != std::string::npos) titles.insert(header);
    }
    if (titles.size() != 2 || !std::filesystem::is_empty(std::filesystem::u8path(directory.Child("exports").Path())))
      throw std::runtime_error("Candidate PPTX files did not match the reviewed outlines and staging scope");

    auto back_to_source = ui.Find(UiSelector::Key("comparison/" + source_id.value)).Find(ButtonText("Back to conversation"));
    back_to_source.Tap();
    const auto source_entry = ui.Find(UiSelector::AllOf(UiSelector::Key("comparison-entry"), UiSelector::Focused(true)));
    Wait(ui, [&] { return source_entry.Exists() && source_entry.One().in_viewport && source_draft.Exists() &&
        source_draft.One().value == "Keep the comparison source draft"; },
        "Returning to the source after visiting a branch did not restore its comparison entry and draft");
    TapFormButton(ui, "Compare alternatives");
    side_by_side(decks);
    TapFormButton(ui, "All comparisons");
    auto new_comparison = ui.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Compare two approaches")));
    Wait(ui, [&] { return new_comparison.Count() == 1 && !new_comparison.One().offscreen && new_comparison.One().enabled; },
        "The comparison entry did not mount");
    new_comparison.PerformSemanticAction({SemanticActionKind::Activate, {}});
    const auto boards = start(design_criteria, "Compact navigation", "Spacious navigation");
    for (const auto& candidate : boards.candidates) {
      open_branch(candidate);
      TapFormButton(ui, "Review design");
      TapFormButton(ui, "Approve design");
      TapFormButton(ui, "Back to comparison");
    }
    const auto compared = read_review(design_criteria, true);
    if (compared.candidates.size() != 2 || std::ranges::any_of(compared.candidates, [](const auto& candidate) {
          return candidate.state != kit::CandidateState::Ready || candidate.artifacts.size() != 1;
        })) throw std::runtime_error("The design comparison lost a candidate version");
    for (const auto& candidate : compared.candidates) design_candidates.push_back(candidate.id);
    side_by_side(compared);
    const auto& selected = compared.candidates.front();
    const auto reference = selected.artifacts.front();
    const auto alternate = compared.candidates.back().artifacts.front();
    auto adopt = ui.Find(UiSelector::Key("comparison-adopt/" + selected.id.value));
    auto other_adoption = ui.Find(UiSelector::Key("comparison-adopt/" + compared.candidates.back().id.value));
    Wait(ui, [&] { return adopt.Exists() && adopt.One().in_viewport && adopt.One().enabled &&
        other_adoption.Exists() && other_adoption.One().enabled; }, "Both design versions did not load for adoption");
    adopt.Tap({.device_kind = PointerDeviceKind::Mouse});
    try {
      Wait(ui, [&] { return ShowsText(ui, "Review design adoption"); }, "The scoped design adoption preview did not render");
    } catch (...) {
      static_cast<void>(directory.Child("comparison-adoption.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
    if (target->Snapshot()->artifacts != std::vector{baseline}) throw std::runtime_error("The board changed before approval");
    TapReviewCard(ui, "Review attachment 2");
    Wait(ui, [&] { return ShowsText(ui, "Compact navigation"); }, "The approval did not open the selected board");
    TapFormButton(ui, "Back to conversation");
    if (scenario == PublicationInterruption::Design) {
      interrupt_publication(design_criteria, selected);
      return;
    }
    TapFormButton(ui, "Approve");
    Wait(ui, [&] { return target->Snapshot()->artifacts.size() == 2 && !target->Snapshot()->summary.active_run; },
        "The approved candidate did not update the target board");
    bool verified = false;
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto published = target->Snapshot()->artifacts.back();
        const auto record = LiveTake(co_await runtime->Artifacts().ReadAsync(published));
        const auto adopted = LiveTake(co_await runtime->Artifacts().PreviewAsync(published));
        const auto source = LiveTake(co_await runtime->Artifacts().PreviewAsync(reference));
        const auto unused = LiveTake(co_await runtime->Artifacts().PreviewAsync(alternate));
        const auto original = LiveTake(co_await runtime->Artifacts().PreviewAsync(baseline));
        if (published.id != baseline.id || published.version != baseline.version + 1 || adopted.structured != source.structured ||
            adopted.structured == original.structured || adopted.structured == unused.structured ||
            std::ranges::none_of(record.dependencies, [&](const auto& edge) { return edge.input.source == kit::SourceRef{reference}; }))
          throw std::runtime_error("Adoption did not publish the selected content and provenance as a new target version");
        verified = true;
      } catch (const std::exception& failure) { error = failure.what(); }
    });
    Wait(ui, [&] { return verified || !error.empty(); }, "Could not verify the applied version");
    if (!error.empty()) throw std::runtime_error(error);

    ui.Find(UiSelector::Key("new-conversation")).Tap();
    ChooseDraftAssistant(ui, "PPT");
    composer = ui.Find(UiSelector::Key("new-conversation-composer"));
    FocusPreviewEditor(ui, composer, "The presentation comparison draft did not receive focus");
    composer.ReplaceText(deck_title);
    TapFormButton(ui, "Send");
    TapFormButton(ui, "Draft outline");
    fill("slide-title-0", "Original rollout");
    TapFormButton(ui, "Confirm outline");
    auto name = ui.Find(UiSelector::Key("ppt-file_name"));
    Wait(ui, [&] { return name.Exists(); }, "The target deck did not stage");
    if (!name.One().in_viewport)
      name = ui.Find(UiSelector::Key("ppt-form-scroll")).ScrollUntil(UiSelector::Key("ppt-file_name"), {.step = {0.0F, 94.0F}});
    FocusPreviewEditor(ui, name, "The presentation export name did not receive focus");
    name.ReplaceText("OriginalDeck.pptx");
    TapFormButton(ui, "Review export");
    Wait(ui, [&] { return ShowsText(ui, "Export PowerPoint"); }, "The original deck export was not prepared");
    TapFormButton(ui, "Approve");
    const auto original_file = exports.Child("OriginalDeck.pptx");
    Wait(ui, [&] { return original_file.Exists() && !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The original deck did not export");
    const auto original_bytes = original_file.ReadBytes();
    if (!original_bytes.Succeeded()) throw std::runtime_error("The original export could not be read");

    target.reset();
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto listed = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
        const auto found = std::ranges::find(listed.sessions, std::string(deck_title), &kit::SessionSummary::title);
        if (found == listed.sessions.end()) throw std::runtime_error("The PowerPoint target is unavailable");
        target = LiveTake(co_await runtime->Sessions().OpenAsync(found->id));
        baseline = target->Snapshot()->artifacts.back();
      } catch (const std::exception& failure) { error = failure.what(); }
    });
    Wait(ui, [&] { return target || !error.empty(); }, "The PowerPoint target could not be inspected");
    if (!error.empty()) throw std::runtime_error(error);
    source_id = target->Snapshot()->summary.id;
    TapFormButton(ui, "Compare alternatives");
    const auto started = start(deck_criteria, "Executive rollout", "Field rollout");
    for (std::size_t i = 0; i < started.candidates.size(); ++i) {
      open_branch(started.candidates[i]);
      TapFormButton(ui, "Draft outline");
      fill("slide-title-0", i == 0 ? "Executive priorities" : "Field priorities");
      TapFormButton(ui, "Confirm outline");
      TapFormButton(ui, "Back to comparison");
    }
    const auto ready = read_review(deck_criteria, true);
    const auto& chosen = ready.candidates.front();
    const auto& other_deck = ready.candidates.back();
    if (chosen.artifacts.size() != 1 || other_deck.artifacts.size() != 1)
      throw std::runtime_error("The rollout comparison did not retain both decks");
    adopt = ui.Find(UiSelector::Key("comparison-adopt/" + chosen.id.value));
    other_adoption = ui.Find(UiSelector::Key("comparison-adopt/" + other_deck.id.value));
    Wait(ui, [&] { return adopt.Exists() && adopt.One().in_viewport && adopt.One().enabled &&
        other_adoption.Exists() && other_adoption.One().enabled; }, "Both decks did not load for adoption");
    adopt.Tap({.device_kind = PointerDeviceKind::Mouse});
    Wait(ui, [&] { return ShowsText(ui, "Review PowerPoint adoption"); }, "PowerPoint adoption did not request approval");
    const auto staging = directory.Child("presentations");
    const auto staged_before = staging.ListChildren();
    if (target->Snapshot()->artifacts != std::vector{baseline} || !staged_before.Succeeded() ||
        std::ranges::any_of(staged_before.Value(), [](const auto& file) {
          return std::filesystem::u8path(file.Path()).filename().string().starts_with("adopted-");
        })) throw std::runtime_error("The target deck was staged or changed before approval");
    TapReviewCard(ui, "Review attachment 2");
    Wait(ui, [&] { return ShowsText(ui, "Executive priorities"); }, "The approval did not open the selected deck");
    TapFormButton(ui, "Back to conversation");
    if (scenario == PublicationInterruption::Presentation) {
      interrupt_publication(deck_criteria, chosen);
      return;
    }
    TapFormButton(ui, "Approve");
    Wait(ui, [&] { return target->Snapshot()->artifacts.size() == 2 && !target->Snapshot()->summary.active_run; },
        "The approved candidate did not publish a deck version");

    Bytes chosen_bytes;
    verified = false;
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto result = target->Snapshot()->artifacts.back();
        const auto selected_ref = chosen.artifacts.front();
        const auto record = LiveTake(co_await runtime->Artifacts().ReadAsync(result));
        const auto candidate_record = LiveTake(co_await runtime->Artifacts().ReadAsync(selected_ref));
        adopted_name = candidate_record.metadata.name;
        const auto bytes = LiveTake(co_await agentkit_preview::ReadPresentationBytes(runtime, result));
        chosen_bytes = LiveTake(co_await agentkit_preview::ReadPresentationBytes(runtime, selected_ref));
        const auto old_bytes = LiveTake(co_await agentkit_preview::ReadPresentationBytes(runtime, baseline));
        const auto alternate_bytes = LiveTake(co_await agentkit_preview::ReadPresentationBytes(runtime, other_deck.artifacts.front()));
        if (result != kit::ArtifactRef{baseline.id, baseline.version + 1} || adopted_name.empty() ||
            record.metadata.name != adopted_name || bytes != chosen_bytes ||
            old_bytes != original_bytes.Value() || bytes == old_bytes || bytes == alternate_bytes ||
            record.parts.size() != candidate_record.parts.size() || record.parts.empty() ||
            !std::ranges::equal(record.parts, candidate_record.parts, [](const auto& left, const auto& right) {
              return left.id == right.id && left.label == right.label;
            }) || !std::ranges::all_of(record.parts, [&](const auto& part) {
              return std::ranges::any_of(record.dependencies, [&](const auto& dependency) {
                return dependency.output_part == part.id && dependency.input == kit::SourceSlice{selected_ref, {part.id}};
              });
            })) throw std::runtime_error("The adopted package or slide lineage differs from the selected candidate");
        verified = true;
      } catch (const std::exception& failure) { error = failure.what(); }
    });
    Wait(ui, [&] { return verified || !error.empty(); }, "Could not verify the adopted PowerPoint");
    if (!error.empty()) throw std::runtime_error(error);

    const auto adopted_card = ui.Find(ReviewCard("Review artifact v2"));
    Wait(ui, [&] {
      return adopted_card.Find(UiSelector::Text(adopted_name)).Exists() &&
          adopted_card.Find(UiSelector::Text("v2")).Exists();
    }, "The adopted deck card did not show its name and version");
    TapReviewCard(ui, "Review artifact v2");
    Wait(ui, [&] { return ShowsText(ui, "Executive priorities"); }, "The adopted version did not reopen for export");
    TapFormButton(ui, "Export PowerPoint");
    fill("ppt-review-export-name", "AdoptedDeck.pptx");
    TapFormButton(ui, "Review export");
    TapFormButton(ui, "Done");
    const auto adopted_file = exports.Child("AdoptedDeck.pptx");
    if (adopted_file.Exists()) throw std::runtime_error("The adopted deck was exported without approval");
    try {
      auto completion = ui.Find(UiSelector::Text("Return to the conversation to review the destination and approve the export."));
      Wait(ui, [&] { return !completion.Exists(); }, "The PowerPoint export dialog did not close");
      TapFormButton(ui, "Back to conversation");
      Wait(ui, [&] { return !ui.Find(ButtonText("Back to conversation")).Exists() && ui.Find(ButtonText("Approve")).Exists(); },
          "The export approval did not appear after returning to the conversation");
      TapFormButton(ui, "Approve");
    } catch (...) {
      static_cast<void>(directory.Child("adopted-export.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
    Wait(ui, [&] { return adopted_file.Exists() && !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The adopted version did not export");
    const auto exported = adopted_file.ReadBytes();
    const auto unchanged = original_file.ReadBytes();
    auto opened = archive::ArchiveReader::Open(adopted_file);
    if (!exported.Succeeded() || exported.Value() != chosen_bytes || !unchanged.Succeeded() ||
        unchanged.Value() != original_bytes.Value() || !opened.Succeeded() ||
        ReadPart(opened.Value(), "ppt/slides/slide1.xml").find("Executive priorities") == std::string::npos)
      throw std::runtime_error("The export did not preserve the exact approved package and original export");
  };
  run_comparisons();
  runtime.reset();
  tasks.reset();
  if (scenario != PublicationInterruption::None) {
    UiTest reopened(app, {.viewport = {1080.0F, 900.0F}});
    const bool presentation = scenario == PublicationInterruption::Presentation;
    VerifyUnpublishedAdoption(reopened, runtime, tasks, presentation ? deck_title : title,
        presentation ? deck_criteria : design_criteria, *interruption, interrupted_baseline, interrupted_candidate,
        staged_output(interruption->call), presentation);
    return;
  }
  UiTest reopened(app, {.viewport = {760.0F, 900.0F}});
  auto source_row = ConversationRow(reopened, title);
  Wait(reopened, [&] { return source_row.Exists(); }, "The comparison source did not reopen");
  source_row.Tap();
  TapFormButton(reopened, "Compare alternatives");
  Wait(reopened, [&] { return reopened.Find(ButtonText(ppt_criteria)).Exists(); }, "The presentation comparison was missing after restart");
  TapFormButton(reopened, design_criteria);
  Wait(reopened, [&] { return ShowsText(reopened, "Applied to the target"); }, "The reopened comparison lost the applied candidate");
  auto top = reopened.Find(UiSelector::Key(design_candidates.front().value));
  auto bottom = reopened.Find(UiSelector::Key(design_candidates.back().value));
  Wait(reopened, [&] { return top.Exists() && bottom.Exists(); }, "The narrow comparison did not retain both candidates");
  if (top.One().bounds.x != bottom.One().bounds.x || top.One().bounds.y >= bottom.One().bounds.y)
    throw std::runtime_error("The narrow comparison did not stack the candidates");
  auto ppt_row = ConversationRow(reopened, deck_title);
  Wait(reopened, [&] { return ppt_row.Exists() && ppt_row.One().in_viewport; }, "The presentation target did not reopen");
  ppt_row.Tap();
  auto timeline = reopened.Find(UiSelector::Key("agent-chat-history"));
  Wait(reopened, [&] { return timeline.Exists() && timeline.One().in_viewport; }, "The presentation timeline did not reopen");
  try {
    static_cast<void>(timeline.ScrollUntil(ReviewCard("Review artifact v2"), {.step = {0.0F, -220.0F}}));
    auto review = reopened.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
        UiSemanticSelector::Label("Review " + adopted_name + " v2")));
    Wait(reopened, [&] { return review.Exists(); }, "The reopened card did not announce the adopted name and version");
    review.PerformSemanticAction({SemanticActionKind::Focus, {}});
    reopened.PressKey(Key::Enter);
    Wait(reopened, [&] { return ShowsText(reopened, "Executive priorities"); }, "The adopted deck did not reopen after restart");
  } catch (...) {
    static_cast<void>(directory.Child("reopened-adoption.snapshot").WriteString(reopened.CaptureSnapshot().ToString()));
    throw;
  }
}

void VerifyCandidateWorktreeRecovery(const File& directory, bool requires_approval = false) {
  const auto main = directory.Child("repository");
  const auto scratch = directory.Child("git");
  if (!main.CreateDirectories() || !scratch.CreateDirectories()) throw std::runtime_error("Could not create the recovery repository");
  const auto git = [&](const File& folder, std::vector<std::string> args) {
    auto result = LiveTake(agentkit_preview::RunGit(scratch, std::filesystem::u8path(folder.Path()), std::move(args)));
    if (result.exit_code != 0) throw std::runtime_error(result.output);
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) result.output.pop_back();
    return result.output;
  };
  git(main, {"init", "-b", "main"});
  git(main, {"config", "--local", "core.autocrlf", "false"});
  WriteFixture(main.Child("readme.txt"), "original\n");
  WriteFixture(main.Child(".gitignore"), "ignored.txt\n");
  git(main, {"add", "--", "readme.txt", ".gitignore"});
  git(main, {"-c", "user.name=AgentKit", "-c", "user.email=agentkit@example.invalid", "-c", "commit.gpgsign=false",
      "commit", "-m", "Seed worktree recovery"});
  const auto commit = git(main, {"rev-parse", "HEAD"});
  const auto plans = directory.Child("worktree-plans");
  const auto checkouts = plans.Child("candidates");
  constexpr auto source_title = "Candidate recovery source";
  constexpr auto retry_criteria = "Recover the same comparison";
  constexpr auto canceled_criteria = "Compare another change";
  constexpr auto unused_title = "First unused option";
  constexpr auto blocked_title = "Occupied option";
  const auto recover_checkout = UiSelector::AllOf(UiSelector::Type<Button>(),
      UiSelector::Text("Review candidate checkout \xc2\xb7 " + std::string(unused_title)));
  auto interrupt = std::make_shared<bool>(true);
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  kit::CommandId approved_command;
  kit::InteractionId approval;
  std::vector<kit::ModelRegistration> models{{{{"preview.test-candidate"}, 1},
      {"preview", "candidate-fixture", "script", {}}, std::make_shared<PatchModel>()}};
  auto hook = MakeRunSettingsTestInstaller(directory.Child("sessions.sqlite3"), main, std::move(models),
      [&](auto started, std::optional<kit::WorkspaceId>, kit::ProjectId) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      }, requires_approval, [interrupt](auto store) {
        return std::make_shared<UnconfirmedForkStore>(std::move(store), interrupt);
      });
  const Application app{App, {.application_hooks = {hook}}};
  const auto review = [&](UiTest& ui, const char* criteria, const char* first, const char* second) {
    for (const auto& [key, text] : {std::pair{"comparison-criteria", criteria}, {"comparison-first", first}, {"comparison-second", second}}) {
      auto field = ui.Find(UiSelector::Key(key));
      Wait(ui, [&] { return field.Exists() && field.One().in_viewport; }, "The comparison form did not open");
      field.Tap();
      field.ReplaceText(text);
    }
    TapFormButton(ui, "Review worktrees");
    Wait(ui, [&] { return ShowsText(ui, "Base commit " + commit); }, "The common commit was not loaded");
  };
  struct PlannedCheckout {
    File path;
    std::string branch;
  };
  const auto plan_named = [&](std::string_view title) -> PlannedCheckout {
    auto entries = plans.ListChildren();
    if (!entries.Succeeded()) throw std::runtime_error("Worktree plans are missing");
    for (const auto& file : entries.Value()) {
      const auto path = std::filesystem::u8path(file.Path());
      if (path.extension() != ".plan") continue;
      auto contents = file.ReadString();
      if (!contents.Succeeded()) continue;
      const auto saved = nlohmann::json::parse(contents.Value());
      if (saved.contains("comparison") && saved.at("title").get<std::string>() == title) {
        return {File(saved.at("directory").get<std::string>()), saved.at("branch").get<std::string>()};
      }
    }
    throw std::runtime_error("Could not find the named checkout plan");
  };
  const auto open_source = [&](UiTest& ui) {
    auto row = ConversationRow(ui, source_title);
    Wait(ui, [&] { return row.Exists() && row.One().in_viewport; }, "The source conversation was not restored");
    row.Tap();
    TapFormButton(ui, "Compare alternatives");
  };
  {
    UiTest ui(app, {.viewport = {1120.0F, 940.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    ChooseDraftAssistant(ui, "Code");
    SendNewConversation(ui, source_title);
    Wait(ui, [&] { return ui.Find(UiSelector::Key("code-scope")).Exists(); }, "The comparison source did not start");
    TapControl(ui, UiSelector::Key("agent-chat-stop"), "Stop current task");
    Wait(ui, [&] { return !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The comparison source did not settle");
    TapFormButton(ui, "Compare alternatives");
    review(ui, retry_criteria, "Original first option", "Original second option");
    TapFormButton(ui, "Start comparison");
    if (requires_approval) {
      Wait(ui, [&] { return ShowsText(ui, "Approve comparison") && runtime && tasks; }, "The source did not show approval before checkout");
      if (plan_named("Original first option").path.Exists() || plan_named("Original second option").path.Exists())
        throw std::runtime_error("The comparison allocated worktrees before the original approval");
      std::string error;
      tasks->Launch([&]() -> Task<void> {
        try {
          const auto list = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
          const auto entry = std::ranges::find(list.sessions, std::string(source_title), &kit::SessionSummary::title);
          if (entry == list.sessions.end()) throw std::runtime_error("The approved comparison source is unavailable");
          const auto source = LiveTake(co_await runtime->Sessions().OpenAsync(entry->id));
          const auto view = source->Snapshot();
          const auto pending = std::ranges::find_if(view->interactions, [](const auto& interaction) {
            return interaction.state == kit::InteractionState::Pending && std::holds_alternative<kit::OperationApproval>(interaction.body);
          });
          if (pending == view->interactions.end()) throw std::runtime_error("The comparison had no original approval ID");
          approved_command = std::get<kit::OperationApproval>(pending->body).pending_command;
          approval = pending->id;
        } catch (const std::exception& failure) { error = failure.what(); }
      });
      Wait(ui, [&] { return !approved_command.value.empty() || !error.empty(); }, "The original approval could not be inspected");
      if (!error.empty()) throw std::runtime_error(error);
      TapFormButton(ui, "Approve");
    }
    Wait(ui, [&] { return !*interrupt && (requires_approval || ShowsText(ui, "The worktree session commit was not confirmed")); },
        "The comparison did not retain its uncertain receipt");
    for (const auto title : {"Original first option", "Original second option"}) {
      const auto plan = plan_named(title);
      if (git(plan.path, {"rev-parse", "HEAD"}) != commit) throw std::runtime_error("The interrupted comparison lost its checkout");
    }
    if (PendingConversation(ui, "Original first option").Exists())
      throw std::runtime_error("The comparison started before its commit");
  }
  runtime.reset();
  tasks.reset();
  {
    UiTest ui(app, {.viewport = {1120.0F, 940.0F}});
    open_source(ui);
    Wait(ui, [&] { return ui.Find(ButtonText(retry_criteria)).Exists(); }, "The committed command did not resume automatically");
    auto restored = checkouts.ListChildren();
    if (!restored.Succeeded() || restored.Value().size() != 2 ||
        !plan_named("Original first option").path.Exists() || !plan_named("Original second option").path.Exists())
      throw std::runtime_error("Resuming the comparison duplicated its checkouts");
    if (requires_approval) {
      Wait(ui, [&] { return runtime && tasks; }, "The approved comparison runtime did not reopen");
      bool verified = false;
      std::string error;
      tasks->Launch([&]() -> Task<void> {
        try {
          const auto progress = LiveTake(co_await runtime->Commands().ReadAsync(approved_command));
          if (progress.state != kit::CommandState::Applied || !progress.approval || progress.approval->interaction != approval ||
              !progress.result || !std::holds_alternative<kit::ForkReviewCreated>(*progress.result) ||
              std::get<kit::ForkReviewCreated>(*progress.result).branches.size() != 2)
            throw std::runtime_error("The original approval did not apply to the recovered comparison");
          verified = true;
        } catch (const std::exception& failure) { error = failure.what(); }
      });
      Wait(ui, [&] { return verified || !error.empty(); }, "The restored approval was not readable");
      if (!error.empty()) throw std::runtime_error(error);
      return;
    }
    TapFormButton(ui, "Compare two approaches");
    review(ui, canceled_criteria, unused_title, blocked_title);
    const auto occupied = plan_named(blocked_title);
    if (!occupied.path.CreateDirectories()) throw std::runtime_error("Could not create the occupied checkout fixture");
    WriteFixture(occupied.path.Child("keep.txt"), "keep this user data");
    TapFormButton(ui, "Start comparison");
    auto action = ui.Find(recover_checkout);
    Wait(ui, [&] { return action.Exists(); }, "The failed comparison did not offer its owned checkout");
    const auto unused = plan_named(unused_title);
    if (git(unused.path, {"rev-parse", "HEAD"}) != commit) throw std::runtime_error("The first checkout was not allocated");
    const auto ignored = unused.path.Child("ignored.txt");
    WriteFixture(ignored, "keep these edits");
    action.Tap();
    TapFormButton(ui, "Remove checkout");
    Wait(ui, [&] { return ShowsText(ui, "The checkout contains changes or extra files and was kept"); },
        "Candidate cleanup did not protect an ignored file");
    CheckText(ignored, "keep these edits");
    if (!ignored.Delete()) throw std::runtime_error("Could not remove the ignored-file fixture");
    TapFormButton(ui, "Close");
  }
  {
    UiTest ui(app, {.viewport = {1120.0F, 940.0F}});
    auto action = ui.Find(recover_checkout);
    Wait(ui, [&] { return action.Exists(); }, "The failed checkout recovery was lost on restart");
    const auto unused = plan_named(unused_title);
    action.Tap();
    TapFormButton(ui, "Remove checkout");
    Wait(ui, [&] { return !unused.path.Exists() && !action.Exists(); }, "The unused candidate checkout was not removed");
    if (git(main, {"worktree", "list", "--porcelain"}).find(unused.branch) != std::string::npos ||
        git(main, {"rev-parse", "refs/heads/" + unused.branch}) != commit)
      throw std::runtime_error("Cleanup did not remove only the registered worktree");
    CheckText(plan_named(blocked_title).path.Child("keep.txt"), "keep this user data");
    CheckText(main.Child("readme.txt"), "original\n");
    if (!plan_named("Original first option").path.Exists() || !plan_named("Original second option").path.Exists())
      throw std::runtime_error("Cleanup affected another comparison");
  }
}

enum class CodeAdoptionCase { Complete, Interrupted, Partial };

void VerifyCodeCandidates(const File& directory, CodeAdoptionCase scenario) {
  const bool recover_adoption = scenario != CodeAdoptionCase::Complete;
  const bool partial = scenario == CodeAdoptionCase::Partial;
  const auto main = directory.Child("repository");
  const auto scratch = directory.Child("git");
  if (!main.CreateDirectories() || !scratch.CreateDirectories()) throw std::runtime_error("Could not create the comparison repository");
  const auto git = [&](const File& folder, std::vector<std::string> arguments) {
    auto result = LiveTake(agentkit_preview::RunGit(scratch, std::filesystem::u8path(folder.Path()), std::move(arguments)));
    if (result.exit_code != 0) throw std::runtime_error(result.output);
    while (!result.output.empty() && (result.output.back() == '\r' || result.output.back() == '\n')) result.output.pop_back();
    return result.output;
  };
  git(main, {"init", "-b", "main"});
  git(main, {"config", "--local", "core.autocrlf", "false"});
  const auto original_page = SettingsPage(false);
  const auto with_user_title = [](std::string page) {
    const std::string_view heading = "<h1>Settings</h1>";
    return page.replace(page.find(heading), heading.size(), "<h1>Workspace settings</h1>");
  };
  const auto user_page = with_user_title(original_page);
  const auto adopted_page = with_user_title(SettingsPage(true));
  WriteFixture(main.Child("config.txt"), "mode=disabled\n");
  WriteFixture(main.Child("settings.html"), original_page);
  WriteFixture(main.Child("CTestTestfile.cmake"), std::string("add_test(candidate \"") +
      AGENTKIT_PREVIEW_CMAKE + "\" -P verify.cmake)\n");
  WriteFixture(main.Child("verify.cmake"), R"cmake(
file(READ "config.txt" mode)
file(READ "settings.html" page)
if (NOT mode MATCHES "^mode=(enabled|staged)" OR NOT page MATCHES "Enabled")
  message(FATAL_ERROR "The candidate change was not applied before the test")
endif ()
string(STRIP "${mode}" mode)
file(WRITE "tested.txt" "${mode}")
)cmake");
  git(main, {"add", "config.txt", "settings.html", "CTestTestfile.cmake", "verify.cmake"});
  git(main, {"-c", "user.name=AgentKit", "-c", "user.email=agentkit@example.invalid", "-c", "commit.gpgsign=false",
      "commit", "-m", "Prepare candidate comparison"});
  const auto commit = git(main, {"rev-parse", "HEAD"});
  WriteFixture(main.Child("private.txt"), "untracked user notes");
  const auto checkouts = directory.Child("worktree-plans").Child("candidates");
  constexpr auto title = "Compare sample mode";
  constexpr auto criteria = "Improve the sample settings";
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  const auto interrupt = std::make_shared<ToolResultInterruption>();
  const auto restore = directory.Child("workspace-evidence").Child("restore");
  PreviewStoreAdapter adapt_store;
  if (recover_adoption) adapt_store = [restore, interrupt, partial](auto store) {
    return std::make_shared<UnconfirmedToolResultStore>(std::move(store), interrupt, [restore, partial](const auto& call) {
      return restore.Child(call.value + (partial ? ".adoption-0.applied" : ".adoption-1.applied"));
    });
  };
  auto hook = MakeCodeTestInstaller(directory.Child("sessions.sqlite3"), main,
      {{{"preview.test-candidate"}, 1}, {"preview", "candidate-fixture", "script", {}}, std::make_shared<PatchModel>()},
      File(AGENTKIT_PREVIEW_CTEST), std::move(adapt_store), [&](auto started, std::optional<kit::WorkspaceId>, kit::ProjectId) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      });
  const Application app{App, {.application_hooks = {hook}}};
  const UiTestOptions options{.viewport = {1120.0F, 940.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  {
    UiTest ui(app, options);
    Wait(ui, [&] { return runtime && tasks; }, "Code candidate fixture did not start");
    ChooseDraftAssistant(ui, "Code");
    SendNewConversation(ui, title);
    Wait(ui, [&] { return ui.Find(UiSelector::Key("code-scope")).Exists(); }, "The source conversation did not start");
    TapControl(ui, UiSelector::Key("agent-chat-stop"), "Stop current task");
    Wait(ui, [&] { return !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The source conversation did not settle");
    std::shared_ptr<kit::Conversation> source;
    std::string error;
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto sessions = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
        const auto found = std::ranges::find(sessions.sessions, std::string(title), &kit::SessionSummary::title);
        if (found == sessions.sessions.end()) throw std::runtime_error("Cannot find the source conversation");
        source = LiveTake(co_await runtime->Sessions().OpenAsync(found->id));
      } catch (const std::exception& failure) { error = failure.what(); }
    });
    Wait(ui, [&] { return source || !error.empty(); }, "The source conversation could not be inspected");
    if (!error.empty()) throw std::runtime_error(error);
    const auto source_id = source->Snapshot()->summary.id;
    const auto read_review = [&](bool ready) {
      std::optional<kit::ForkReviewSnapshot> result;
      tasks->Launch([&]() -> Task<void> {
        try {
          for (int attempt = 0; attempt < 120; ++attempt) {
            auto listed = co_await runtime->ForkReviews().ListComparisonsAsync(source_id);
            if (listed.Succeeded() && !listed.Value().empty() &&
                (!ready || listed.Value().front().state == kit::ForkReviewState::Ready)) {
              auto read = co_await runtime->ForkReviews().ReadComparisonAsync(listed.Value().front().id);
              if (read.Succeeded()) { result = std::move(read).Value(); co_return; }
              if (read.Error().code != kit::ErrorCode::CursorExpired) LiveTake(std::move(read));
            }
            co_await Delay(20ms);
          }
          error = "Could not read the committed code comparison";
        } catch (const std::exception& failure) { error = failure.what(); }
      });
      Wait(ui, [&] { return result || !error.empty(); }, "The committed comparison did not appear");
      if (!error.empty()) throw std::runtime_error(error);
      return std::move(*result);
    };
    const auto fill = [&](const char* key, const char* value) {
      auto input = ui.Find(UiSelector::Key(key));
      Wait(ui, [&] { return input.Exists() && input.One().in_viewport && input.One().enabled; }, "The comparison field is unavailable");
      input.Tap();
      input.ReplaceText(value);
    };
    TapFormButton(ui, "Compare alternatives");
    fill("comparison-criteria", criteria);
    fill("comparison-first", "Enable mode directly");
    fill("comparison-second", "Keep mode staged");
    TapFormButton(ui, "Review worktrees");
    Wait(ui, [&] { return ShowsText(ui, "Base commit " + commit); }, "The common Git commit was not available for review");
    const auto plans = checkouts.ListChildren();
    if (!plans.Succeeded() || !plans.Value().empty()) throw std::runtime_error("Reviewing the candidates created a checkout");
    TapFormButton(ui, "Start comparison");
    try {
      Wait(ui, [&] { return ui.Find(ButtonText("Close comparison")).Exists(); }, "The code comparison did not open");
    } catch (...) {
      static_cast<void>(directory.Child("code-comparison.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw;
    }
    const auto created = read_review(false);
    const auto branches = checkouts.ListChildren();
    if (created.candidates.size() != 2 || !branches.Succeeded() || branches.Value().size() != 2 ||
        !created.candidates[0].validation || !created.candidates[1].validation ||
        created.candidates[0].validation->workspace == created.candidates[1].validation->workspace)
      throw std::runtime_error("The comparison did not allocate two distinct working trees");
    for (const auto& checkout : branches.Value()) {
      if (git(checkout, {"rev-parse", "HEAD"}) != commit || checkout.Child("private.txt").Exists())
        throw std::runtime_error("The candidate did not use the reviewed commit or inherited untracked files");
    }
    for (const auto& candidate : created.candidates) {
      auto open = ui.Find(UiSelector::Key("comparison-open/" + candidate.session.value));
      Wait(ui, [&] { return open.Exists() && open.One().in_viewport; }, "The candidate could not be opened");
      open.Tap({.device_kind = PointerDeviceKind::Mouse});
      SubmitWorkspaceScope(ui);
      TapFormButton(ui, "Approve");
      Wait(ui, [&] { return ShowsText(ui, "Run workspace tests"); }, "The isolated tests did not request approval");
      TapFormButton(ui, "Approve");
      Wait(ui, [&] { return !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The isolated test run did not finish", 20s);
      TapFormButton(ui, "Back to comparison");
    }
    const auto reviewed = read_review(true);
    const auto& selected = reviewed.candidates.front();
    if (std::ranges::any_of(reviewed.candidates, [](const auto& candidate) {
          return candidate.state != kit::CandidateState::Ready || candidate.artifacts.size() != 2 ||
              !candidate.validation || !candidate.validation->evidence ||
              candidate.validation->evidence->values != kit::JsonValue{std::string("Workspace tests passed")};
        })) throw std::runtime_error("The code candidates did not produce versioned diffs and passing test evidence");
    std::set<std::string> modes;
    for (const auto& checkout : branches.Value()) {
      const auto mode = checkout.Child("config.txt").ReadString();
      if (!mode.Succeeded()) throw std::runtime_error("Could not read the isolated change");
      CheckText(checkout.Child("tested.txt"), std::string_view(mode.Value()).substr(0, mode.Value().find('\n')));
      modes.insert(mode.Value());
    }
    if (modes != std::set<std::string>{"mode=enabled\n", "mode=staged\n"})
      throw std::runtime_error("The isolated candidate changes were not distinct");
    CheckText(main.Child("config.txt"), "mode=disabled\n");
    CheckText(main.Child("settings.html"), original_page);
    const auto request_adoption = [&] {
      auto review_button = ui.Find(UiSelector::Key("comparison-adopt/" + selected.id.value));
      auto alternative = ui.Find(UiSelector::Key("comparison-adopt/" + reviewed.candidates.back().id.value));
      Wait(ui, [&] { return review_button.Exists() && review_button.One().in_viewport && review_button.One().enabled &&
          alternative.Exists() && alternative.One().enabled; }, "Both candidate patches did not load for review");
      review_button.Tap({.device_kind = PointerDeviceKind::Mouse});
    };
    const auto choose = [&] {
      request_adoption();
      Wait(ui, [&] { return ShowsText(ui, "Review code adoption"); }, "The main workspace did not request adoption approval");
    };
    Wait(ui, [&] { return ShowsText(ui, "config.txt\n@@ -1 +1 @@\n-mode=disabled\n+mode=enabled\n") &&
        ShowsText(ui, "config.txt\n@@ -1 +1 @@\n-mode=disabled\n+mode=staged\n"); }, "The file diffs did not render side by side");
    choose();
    WriteFixture(main.Child("settings.html"), "User edited the second file while reviewing");
    TapFormButton(ui, "Approve");
    Wait(ui, [&] { return !source->Snapshot()->summary.active_run; }, "The stale approval did not settle");
    CheckText(main.Child("config.txt"), "mode=disabled\n");
    CheckText(main.Child("settings.html"), "User edited the second file while reviewing");
    TapFormButton(ui, "Compare alternatives");
    Wait(ui, [&] { return ShowsText(ui, "The baseline changed. Prepare a new plan."); }, "The comparison did not report the conflict");
    if (!recover_adoption) {
      request_adoption();
      Wait(ui, [&] {
        return ShowsText(ui, "Candidate and workspace changes overlap in settings.html. Resolve the overlapping edits before reviewing this candidate.");
      }, "Overlapping edits did not prevent a new adoption plan");
      if (source->Snapshot()->summary.active_run) throw std::runtime_error("An overlapping candidate started a new run");
      CheckText(main.Child("config.txt"), "mode=disabled\n");
      CheckText(main.Child("settings.html"), "User edited the second file while reviewing");

      WriteFixture(main.Child("config.txt"), "mode=enabled\n");
      WriteFixture(main.Child("settings.html"), adopted_page);
      request_adoption();
      Wait(ui, [&] { return ShowsText(ui, "The candidate changes are already present in the workspace."); },
          "An already applied candidate tried to prepare another write");
      WriteFixture(main.Child("settings.html"), user_page);
      choose();
      const auto prepared = source->Snapshot();
      const auto approval = std::ranges::find_if(prepared->interactions, [](const auto& entry) {
        return entry.state == kit::InteractionState::Pending && std::holds_alternative<kit::ToolApproval>(entry.body);
      });
      if (approval == prepared->interactions.end()) throw std::runtime_error("The remaining candidate file had no approval");
      const auto& targets = std::get<kit::ToolApproval>(approval->body).preview.targets;
      if (targets.size() != 1 || targets.front().target.value != main.Child("settings.html").Path())
        throw std::runtime_error("The new approval included a file that already contained the candidate change");
      TapFormButton(ui, "Deny");
      Wait(ui, [&] { return !source->Snapshot()->summary.active_run; }, "The scoped adoption did not settle");
      CheckText(main.Child("settings.html"), user_page);
      WriteFixture(main.Child("config.txt"), "mode=disabled\n");
      TapFormButton(ui, "Compare alternatives");
    }
    WriteFixture(main.Child("settings.html"), user_page);
    choose();
    TapReviewCard(ui, "Review attachment 1");
    Wait(ui, [&] { return ReviewedDiff(ui) == "config.txt\n@@ -1 +1 @@\n-mode=disabled\n+mode=enabled\n"; },
        "The approval did not show the prepared diff for the main workspace");
    TapFormButton(ui, "Back to conversation");
    TapReviewCard(ui, "Review attachment 2");
    Wait(ui, [&] {
      const auto diff = ReviewedDiff(ui);
      return diff.starts_with("settings.html\n") && diff.find("\n <main><h1>Workspace settings</h1>") != diff.npos &&
          diff.find("\n-<label><input name=\"mode\" type=\"checkbox\"> Disabled") != diff.npos &&
          diff.find("\n+<label><input name=\"mode\" type=\"checkbox\" checked> Enabled") != diff.npos;
    }, "The new approval did not show the candidate edit against the user-edited baseline");
    TapFormButton(ui, "Back to conversation");
    CheckText(main.Child("config.txt"), "mode=disabled\n");
    CheckText(main.Child("settings.html"), user_page);
    if (recover_adoption) {
      const auto current = read_review(false);
      if (!current.adoption_call) throw std::runtime_error("The adoption had no original call");
      interrupt->target = source_id;
      interrupt->call = *current.adoption_call;
      if (partial) concurrent_code_edit.store(std::make_shared<const ConcurrentCodeEdit>(
          ConcurrentCodeEdit{interrupt->call, main.Child("settings.html"), edited_during_adoption}));
      TapFormButton(ui, "Approve");
      Wait(ui, [&] { return interrupt->happened; }, "The result commit was not interrupted after the file writes");
      CheckText(main.Child("config.txt"), "mode=enabled\n");
      CheckText(main.Child("settings.html"), partial ? edited_during_adoption : adopted_page);
      if (partial && restore.Child(interrupt->call.value + ".adoption-1.issued").Exists())
        throw std::runtime_error("Adoption issued a replacement for the file edited after its first write");
    } else {
      TapFormButton(ui, "Approve");
      Wait(ui, [&] { return !source->Snapshot()->summary.active_run && !source->Snapshot()->artifacts.empty(); },
          "The selected changes were not registered in the main workspace");
      CheckText(main.Child("config.txt"), "mode=enabled\n");
      CheckText(main.Child("settings.html"), adopted_page);
      CheckText(main.Child("private.txt"), "untracked user notes");
      bool verified = false;
      tasks->Launch([&]() -> Task<void> {
        try {
          const auto current = source->Snapshot();
          const auto result = LiveTake(co_await runtime->Artifacts().ReadAsync(current->artifacts.back()));
          const auto winner = selected.artifacts.front();
          const auto verification = LiveTake(co_await runtime->Artifacts().ReadAsync(selected.artifacts.back()));
          if (verification.metadata.type != agentkit_preview::VerificationEvidence ||
              std::ranges::none_of(verification.dependencies, [&](const auto& edge) { return edge.input == kit::SourceSlice{winner}; }))
            throw std::runtime_error("The candidate test report did not bind the applied version");
          if (result.parts.size() != 2 || !std::ranges::all_of(result.parts, [&](const auto& part) {
                return std::ranges::any_of(result.dependencies, [&](const auto& edge) {
                  return edge.output_part == part.id && edge.input == kit::SourceSlice{winner, {part.id}};
                });
              })) throw std::runtime_error("The main workspace write lost the candidate source");
          verified = true;
        } catch (const std::exception& failure) { error = failure.what(); }
      });
      Wait(ui, [&] { return verified || !error.empty(); }, "The adopted patch evidence could not be read");
      if (!error.empty()) throw std::runtime_error(error);
    }
  }
  runtime.reset();
  tasks.reset();
  UiTest reopened(app, options);
  auto row = ConversationRow(reopened, title);
  Wait(reopened, [&] { return row.Exists(); }, "The original conversation did not reopen");
  row.Tap();
  TapFormButton(reopened, "Compare alternatives");
  TapFormButton(reopened, criteria);
  if (scenario == CodeAdoptionCase::Interrupted) {
    const auto marker = restore.Child(interrupt->call.value + ".adoption-0.applied");
    const auto held = restore.Child(interrupt->call.value + ".adoption-0.held");
    if (!marker.MoveTo(held, false)) throw std::runtime_error("Could not simulate an incomplete replacement receipt");
    WriteFixture(main.Child("settings.html"), "User edited the page after the interrupted adoption");
    TapFormButton(reopened, "Check adoption result");
    Wait(reopened, [&] { return ShowsText(reopened, "The original replacement could not be confirmed: config.txt"); },
        "Missing write evidence did not leave the adoption unresolved");
    if (!held.MoveTo(marker, false)) throw std::runtime_error("Could not restore replacement receipt for verification");
    TapFormButton(reopened, "Check adoption result");
    CheckText(main.Child("config.txt"), "mode=enabled\n");
    CheckText(main.Child("settings.html"), "User edited the page after the interrupted adoption");
  }
  if (partial) TapFormButton(reopened, "Check adoption result");
  Wait(reopened, [&] { return ShowsText(reopened, partial ? "Partial application verified" : "Applied to the target"); },
      "The adoption result was not restored");
  if (recover_adoption) {
    TapFormButton(reopened, "Review recovered changes");
    auto files = reopened.Find(UiSelector::Type<Checkbox>());
    try {
      Wait(reopened, [&] { return files.Exists(); }, "The recovered change did not offer an individual restore scope");
    } catch (...) {
      static_cast<void>(directory.Child("recovered-changes.snapshot").WriteString(reopened.CaptureSnapshot().ToString()));
      throw;
    }
    auto edited = reopened.Find(UiSelector::Key(main.Child("settings.html").Path()));
    if (partial) {
      if (edited.Exists() || files.Count() != 1 || !reopened.Find(UiSelector::Key(main.Child("config.txt").Path())).Exists())
        throw std::runtime_error("The restore scope must contain only the originally replaced file");
    } else {
      edited.Tap();
    }
    TapFormButton(reopened, "Prepare revert");
    Wait(reopened, [&] { return ShowsText(reopened, "Return to the conversation to review the reverse patch."); },
        "The verified call did not prepare a reverse patch");
    TapFormButton(reopened, "Back to conversation");
    Wait(reopened, [&] { return ShowsText(reopened, "Review reverse patch"); }, "The reverse change did not request approval");
    CheckText(main.Child("config.txt"), "mode=enabled\n");
    TapFormButton(reopened, "Approve");
    Wait(reopened, [&] { return !reopened.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The approved reverse change did not finish");
    CheckText(main.Child("config.txt"), "mode=disabled\n");
    CheckText(main.Child("settings.html"), partial ? edited_during_adoption : "User edited the page after the interrupted adoption");
  }
}

void VerifyDesignRevert(const File& directory) {
  const auto workspace = directory.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the design restore workspace");
  const auto designs = directory.Child("designs");
  const auto database = directory.Child("sessions.sqlite3");
  constexpr auto title = "Initial preferences";
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  const auto hook = MakeRunSettingsTestInstaller(database, workspace, {},
      [&](auto started, std::optional<kit::WorkspaceId>, kit::ProjectId) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      });
  const Application app{App, {.application_hooks = {hook}}};
  const auto open_snapshot = [&](UiTest& ui, const std::function<bool(const kit::SessionSnapshot&)>& matches) {
    Wait(ui, [&] { return runtime && tasks; }, "Design restore runtime did not start");
    std::shared_ptr<const kit::SessionSnapshot> snapshot;
    std::string error;
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto list = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
        const auto entry = std::ranges::find(list.sessions, title, &kit::SessionSummary::title);
        if (entry == list.sessions.end()) throw std::runtime_error("Design restore conversation unavailable");
        const auto session = LiveTake(co_await runtime->Sessions().OpenAsync(entry->id));
        for (int i = 0; i < 500; ++i) {
          const auto current = session->Snapshot();
          if (current && matches(*current)) { snapshot = current; co_return; }
          co_await Delay(5ms);
        }
        error = "Design restore state did not settle";
      } catch (const std::exception& fault) { error = fault.what(); }
    });
    Wait(ui, [&] { return snapshot || !error.empty(); }, "Design restore snapshot was unavailable");
    if (!error.empty()) throw std::runtime_error(error);
    return snapshot;
  };
  const auto send = [](UiTest& ui, std::string text) {
    auto composer = ui.Find(UiSelector::Key("composer"));
    if (!composer.Exists()) composer = ui.Find(UiSelector::Type<TextField>());
    composer.Tap();
    composer.ReplaceText(std::move(text));
    TapFormButton(ui, "Send");
    Wait(ui, [&] { return ui.Find(ButtonText("Review design")).Exists(); }, "The design draft did not reach review");
  };
  const auto approve_design = [](UiTest& ui) {
    TapFormButton(ui, "Review design");
    TapFormButton(ui, "Approve design");
  };
  const auto call_for = [](const kit::SessionSnapshot& state, kit::Revision version) {
    const auto call = std::ranges::find_if(state.tools, [&](const auto& item) {
      return std::ranges::any_of(item.artifacts, [&](const auto& ref) { return ref.version == version; });
    });
    if (call == state.tools.end()) throw std::runtime_error("The design version has no original call");
    return *call;
  };
  const auto file_for = [&](const kit::ToolCallSnapshot& call) { return designs.Child(call.id.value + ".design.json"); };
  kit::ArtifactRef previous;
  kit::ArtifactRef current;
  kit::SessionId session_id;
  kit::ToolCallId original_call;
  kit::InteractionSnapshot approval;
  std::string previous_content;
  std::string current_content;
  {
    UiTest ui(app, {.viewport = {1080.0F, 900.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    ChooseDraftAssistant(ui, "Design");
    send(ui, title);
    approve_design(ui);
    open_snapshot(ui, [](const auto& state) { return !state.summary.active_run && state.artifacts.size() == 1; });
    send(ui, "Compact preferences");
    approve_design(ui);
    auto v2 = open_snapshot(ui, [](const auto& state) { return !state.summary.active_run && state.artifacts.size() == 2; });
    const auto second = call_for(*v2, 2);
    previous = second.artifacts.front();
    previous_content = file_for(second).ReadString().Value();
    send(ui, "Expanded preferences");
    TapReviewCard(ui, "Review artifact v2");
    TapFormButton(ui, "Restore previous version");
    const auto queued = open_snapshot(ui, [](const auto& state) { return state.summary.queued_inputs == 1; });
    const auto queued_id = queued->queue.back().id;
    approve_design(ui);
    const auto updated = open_snapshot(ui, [&](const auto& state) {
      const auto item = std::ranges::find(state.queue, queued_id, &kit::QueuedInput::id);
      return !state.summary.active_run && state.artifacts.size() == 3 && item != state.queue.end() &&
          item->state == kit::QueueItemState::Rejected && item->reason && item->reason->code == kit::ErrorCode::Conflict;
    });
    const auto third = call_for(*updated, 3);
    current = third.artifacts.front();
    session_id = updated->summary.id;
    original_call = third.id;
    current_content = file_for(third).ReadString().Value();
    if (current_content == previous_content) throw std::runtime_error("The fixture did not publish a distinct design revision");
  }
  runtime.reset();
  tasks.reset();
  nlohmann::json original_dependencies;
  const auto use_legacy_publication = [&](bool preserve_predecessor) {
    bool done = false;
    std::exception_ptr failure;
    Application edit_store{[]() -> View { return Spacer(); }, {.application_hooks = {[&](ApplicationContext&) {
      UseApplicationTaskScope().Launch([&]() -> Task<void> {
        try {
          const auto store = LiveTake(co_await agentkit_host::OpenSqliteSessionStoreAsync(database));
          const auto image = LiveTake(co_await store->LoadAsync());
          const auto row = std::ranges::find(image.records, session_id.value, &kit::StoreValue::key);
          if (row == image.records.end()) throw std::runtime_error("The original design session was not stored");
          const auto* start = reinterpret_cast<const std::uint8_t*>(row->data.data());
          auto document = nlohmann::json::from_cbor(start, start + row->data.size());
          document["schema"] = 1;
          auto& session = document.at("session");
          auto& data = session.at("data");
          const auto call = std::ranges::find_if(data.at("tools"), [&](const auto& tool) {
            return tool.at("id") == original_call.value;
          });
          if (call == data.at("tools").end()) throw std::runtime_error("The design publication was not stored");
          call->erase("effects");
          bool found_result = false;
          for (auto& run : session.at("runs")) {
            for (auto& pair : run.at("tool_requests")) {
              auto& request = pair.at(1);
              if (request.at("call") != original_call.value) continue;
              request.at("result").at("effects") = nlohmann::json::array();
              found_result = true;
            }
          }
          if (!found_result) throw std::runtime_error("The original design publication result was not stored");
          const auto artifact = std::ranges::find_if(data.at("artifacts"), [&](const auto& item) {
            return item.at("record").at("call") == original_call.value;
          });
          if (artifact == data.at("artifacts").end()) throw std::runtime_error("The design artifact was not stored");
          auto& dependencies = artifact->at("record").at("dependencies");
          if (preserve_predecessor) {
            dependencies = original_dependencies;
          } else {
            original_dependencies = dependencies;
            if (original_dependencies.empty()) throw std::runtime_error("The stored design has no predecessor");
            dependencies = nlohmann::json::array();
          }
          const auto encoded = nlohmann::json::to_cbor(document);
          const auto* bytes = reinterpret_cast<const std::byte*>(encoded.data());
          kit::StoreTransaction update{{kit::NewCommandId().value}, image.revision,
              {{row->collection, row->key, row->revision, Bytes(bytes, bytes + encoded.size())}}};
          LiveTake(co_await store->CommitAsync(std::move(update)));
        } catch (...) {
          failure = std::current_exception();
        }
        done = true;
      });
    }}}};
    UiTest edit(edit_store);
    Wait(edit, [&] { return done; }, "Could not prepare the stored legacy design");
    if (failure) std::rethrow_exception(failure);
  };
  const auto open_design = [&](UiTest& ui) {
    auto entry = ConversationRow(ui, title);
    Wait(ui, [&] { return entry.Exists(); }, "The design conversation did not reopen");
    entry.Tap();
    const auto target = ReviewCard("Review artifact v3");
    auto card = ui.Find(target);
    Wait(ui, [&] { return card.Exists() && card.One().enabled; }, "The stored design card was unavailable");
    if (!card.One().in_viewport)
      card = ui.Find(UiSelector::Key("agent-chat-history")).ScrollUntil(target, {.step = {0.0F, -95.0F}});
    const auto visible = card.One().visible_bounds;
    ui.TapAt({visible.x + std::min(visible.width / 4.0F, 24.0F), visible.y + visible.height / 2.0F},
        {.device_kind = PointerDeviceKind::Mouse});
    Wait(ui, [&] { return ShowsText(ui, "Expanded preferences") && ShowsText(ui, "Version 3"); },
        "The stored design revision did not open");
  };
  use_legacy_publication(false);
  {
    UiTest ui(app, {.viewport = {1080.0F, 900.0F}});
    open_design(ui);
    const auto legacy = open_snapshot(ui, [](const auto& state) { return !state.summary.active_run; });
    if (!call_for(*legacy, 3).effects.empty() || ui.Find(ButtonText("Restore previous version")).Exists())
      throw std::runtime_error("A design without a recorded predecessor became restorable");
  }
  runtime.reset();
  tasks.reset();
  use_legacy_publication(true);
  {
    UiTest ui(app, {.viewport = {1080.0F, 900.0F}});
    open_design(ui);
    const auto legacy = open_snapshot(ui, [](const auto& state) { return !state.summary.active_run; });
    const auto repaired = call_for(*legacy, 3);
    if (repaired.id != original_call || repaired.state != kit::ToolCallState::Succeeded ||
        repaired.effects.size() != 1 || repaired.effects.front().undo != kit::UndoRef{"2", kit::UndoSupport::RestoreVersion})
      throw std::runtime_error("The original publication could not recover its exact previous version");
    TapControl(ui, ButtonText("Restore previous version"), "Restore previous version", "design-review-scroll");
    Wait(ui, [&] { return ui.Find(UiSelector::Text("Review design restore")).Exists(); }, "Restore confirmation did not open");
    TapReviewCard(ui, "Review attachment 2");
    Wait(ui, [&] { return ShowsText(ui, "Compact preferences") && ui.Find(UiSelector::Text("Version 2")).Exists(); },
        "The restore approval did not show the exact previous design");
    TapFormButton(ui, "Back to conversation");
    const auto pending = open_snapshot(ui, [](const auto& state) { return state.summary.pending_interactions == 1; });
    approval = pending->interactions.back();
    if (pending->artifacts.size() != 3 || std::get<kit::ToolApproval>(approval.body).preview.artifacts != std::vector{current, previous})
      throw std::runtime_error("The restore preview changed the design before approval");
  }
  runtime.reset();
  tasks.reset();
  {
    UiTest ui(app, {.viewport = {1080.0F, 900.0F}});
    auto entry = PendingConversation(ui, std::string(title));
    Wait(ui, [&] { return entry.Exists(); }, "The design restore approval did not survive restart");
    entry.Tap();
    const auto resumed = open_snapshot(ui, [](const auto& state) { return state.summary.pending_interactions == 1; });
    const auto& pending = resumed->interactions.back();
    if (pending.id != approval.id || pending.version != approval.version ||
        std::get<kit::ToolApproval>(pending.body).plan != std::get<kit::ToolApproval>(approval.body).plan) {
      throw std::runtime_error("Restart changed the approved design restore identity");
    }
    TapFormButton(ui, "Approve");
    const auto restored = open_snapshot(ui, [](const auto& state) { return !state.summary.active_run && state.artifacts.size() == 4; });
    const auto applied = call_for(*restored, 4);
    const auto ref = applied.artifacts.front();
    if (ref.id != current.id || applied.reverts != original_call || applied.effects.size() != 1 ||
        applied.source_changes.size() != 1 || applied.state != kit::ToolCallState::Succeeded) {
      throw std::runtime_error("The new design version did not record the original publication and restore");
    }
    CheckText(file_for(applied), previous_content);
    CheckText(file_for(call_for(*restored, 2)), previous_content);
    CheckText(file_for(call_for(*restored, 3)), current_content);
    bool checked = false;
    std::string audit_error;
    tasks->Launch([&]() -> Task<void> {
      try {
        auto result = LiveTake(co_await runtime->Artifacts().ReadAsync(ref));
        if (result.dependencies.size() != 1 || result.dependencies.front().input.source != kit::SourceRef{previous} ||
            result.source_change != applied.source_changes.front()) throw std::runtime_error("The design restore lost its copied source");
        kit::ArtifactImpactQuery query{.project = *restored->summary.project, .change_command = result.source_change};
        const auto change = LiveTake(co_await runtime->Artifacts().InspectImpactAsync(std::move(query))).recorded_change;
        if (change != kit::SourceChange{{current}, kit::SourceRef{ref}, kit::SourceChangeKind::Reverted})
          throw std::runtime_error("The design restore was not recorded against the replaced artifact version");
        checked = true;
      } catch (const std::exception& error) { audit_error = error.what(); }
    });
    Wait(ui, [&] { return checked || !audit_error.empty(); }, "The design restore audit did not finish");
    if (!audit_error.empty()) throw std::runtime_error(audit_error);
  }
}

enum class CodeRevertCase { Normal, Partial, InterruptedPartial };

void VerifyCodeRevert(const File& directory, CodeRevertCase scenario = CodeRevertCase::Normal) {
  const auto workspace = directory.Child("code");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the restore workspace");
  const auto config = workspace.Child("config.txt");
  const auto settings = workspace.Child("settings.html");
  WriteFixture(config, "mode=disabled\n");
  WriteFixture(settings, SettingsPage(false));
  constexpr auto title = "Enable mode for restore";
  constexpr auto prepare_label = "Prepare revert";
  constexpr auto status_label = "Return to the conversation to review the reverse patch.";
  kit::ModelRegistration model{{{"preview.revert-test"}, 1}, {"preview", "restore-fixture", "script", {}},
                               std::make_shared<PatchModel>()};
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  kit::ProjectId project;
  const auto interrupt = std::make_shared<ToolResultInterruption>();
  const auto restore = directory.Child("workspace-evidence").Child("restore");
  PreviewStoreAdapter adapt_store;
  if (scenario == CodeRevertCase::InterruptedPartial) {
    adapt_store = [restore, interrupt](auto store) {
      return std::make_shared<UnconfirmedToolResultStore>(std::move(store), interrupt, [restore](const auto& call) {
        return restore.Child(call.value + ".revert-0.applied");
      });
    };
  }
  const auto hook = MakeRunSettingsTestInstaller(directory.Child("sessions.sqlite3"), workspace, {model},
      [&](auto started, std::optional<kit::WorkspaceId>, kit::ProjectId current) -> Task<void> {
        runtime = LiveTake(std::move(started));
        project = std::move(current);
        tasks = UseApplicationTaskScope();
        co_return;
      }, false, std::move(adapt_store));
  Application application{App, {.application_hooks = {hook}}};
  const UiTestOptions options{.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  const auto snapshot = [&](UiTest& preview) {
    Wait(preview, [&] { return tasks && runtime; }, "Restore runtime did not start");
    std::shared_ptr<const kit::SessionSnapshot> result;
    tasks->Launch([&]() -> Task<void> {
      const auto list = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
      const auto found = std::ranges::find(list.sessions, title, &kit::SessionSummary::title);
      if (found == list.sessions.end()) co_return;
      auto session = LiveTake(co_await runtime->Sessions().OpenAsync(found->id));
      result = session->Snapshot();
    });
    Wait(preview, [&] { return bool(result); }, "Restore snapshot was not available");
    return result;
  };
  const auto open_artifact = [&](UiTest& preview) {
    auto review = preview.Find(ReviewCard("Review artifact v1"));
    Wait(preview, [&] { return review.Count() == 1 && review.One().in_viewport; }, "The applied change artifact was not available");
    review.Tap();
    Wait(preview, [&] { return preview.Find(ButtonText(prepare_label)).Exists(); }, "The applied change did not offer restore");
  };
  const auto prepare = [&](UiTest& preview) {
    TapFormButton(preview, prepare_label);
    Wait(preview, [&] { return preview.Find(UiSelector::Text(status_label)).Exists(); }, "The reverse patch was not accepted");
    TapFormButton(preview, "Back to conversation");
    Wait(preview, [&] { return preview.Find(UiSelector::Text("Review reverse patch")).Exists(); }, "The reverse patch did not await approval");
  };
  kit::ToolCallId original;
  kit::ArtifactRef evidence;
  kit::ResourceRef config_applied;
  kit::InteractionSnapshot restore_approval;
  const std::string concurrent_edit = "User edited the second file during revert\n";
  {
    UiTest preview(application, options);
    PrepareCodeChange(preview, title);
    TapFormButton(preview, "Approve");
    Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists() && preview.Find(ReviewCard("Review artifact v1")).Exists(); },
        "The original patch did not publish its audit evidence");
    auto applied = snapshot(preview);
    const auto& call = applied->tools.back();
    if (call.state != kit::ToolCallState::Succeeded || call.effects.size() != 2 || call.artifacts.size() != 1 ||
        !std::ranges::all_of(call.effects, [](const auto& effect) { return effect.after && effect.undo.has_value(); })) {
      throw std::runtime_error("The applied files did not record two restorable effects");
    }
    original = call.id;
    evidence = call.artifacts.front();
    config_applied = *call.effects.front().after;
    open_artifact(preview);
    prepare(preview);
    TapReviewCard(preview, "Review attachment 1");
    auto diff = preview.Find(UiSelector::Key("artifact-text"));
    Wait(preview, [&] { return diff.Exists() && !ReviewedDiff(preview).empty(); }, "The reverse diff did not open");
    if (ReviewedDiff(preview).find("-mode=enabled\n+mode=disabled") == std::string::npos)
      throw std::runtime_error("The reverse diff did not show the original code");
    TapFormButton(preview, "Back to conversation");
    if (scenario != CodeRevertCase::Normal) {
      const auto pending = snapshot(preview);
      interrupt->target = pending->summary.id;
      interrupt->call = std::get<kit::ToolApproval>(pending->interactions.back().body).call;
      concurrent_code_edit.store(std::make_shared<const ConcurrentCodeEdit>(ConcurrentCodeEdit{
          interrupt->call, settings, concurrent_edit}));
      TapFormButton(preview, "Approve");
      if (scenario == CodeRevertCase::InterruptedPartial) {
        Wait(preview, [&] { return interrupt->happened; }, "The reverse result commit was not interrupted after the file write");
      } else {
        Wait(preview, [&] { return ShowsText(preview, "Applied: config.txt\nSkipped: settings.html") &&
                                   !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "A partial reverse patch did not report both file results");
      }
      if (restore.Child(interrupt->call.value + ".revert-1.issued").Exists())
        throw std::runtime_error("The reverse patch attempted replacement after the user edit");
      CheckText(config, "mode=disabled\n");
      CheckText(settings, concurrent_edit);
    } else {
      WriteFixture(settings, "User edited after restore review\n");
      TapFormButton(preview, "Approve");
      Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "A stale restore did not settle");
      if (snapshot(preview)->tools.back().state != kit::ToolCallState::Conflict)
        throw std::runtime_error("The reverse patch did not record the version conflict");
      CheckText(config, "mode=enabled\n");
      CheckText(settings, "User edited after restore review\n");
      WriteFixture(settings, SettingsPage(true));
      open_artifact(preview);
      auto excluded = preview.Find(UiSelector::Type<Checkbox>());
      if (excluded.Count() != 2) throw std::runtime_error("The review did not show both applied files");
      const auto file = excluded.All().back();
      preview.Find(UiSelector::Key(std::get<std::string>(*file.key))).Tap();
      prepare(preview);
      restore_approval = snapshot(preview)->interactions.back();
      if (std::get<kit::ToolApproval>(restore_approval.body).preview.targets.size() != 1)
        throw std::runtime_error("The reverse patch did not bind the selected scope");
      CheckText(config, "mode=enabled\n");
    }
  }
  tasks.reset();
  runtime.reset();
  if (scenario != CodeRevertCase::Normal) {
    UiTest preview(application, options);
    auto entry = ConversationRow(preview, title);
    Wait(preview, [&] { return entry.Exists(); }, "The reverse change conversation did not survive restart");
    entry.Tap();
    auto review = preview.Find(ReviewCard("Review artifact v1"));
    Wait(preview, [&] { return review.Exists() && review.One().in_viewport; }, "The original change audit was unavailable");
    review.Tap();
    const auto interrupted = snapshot(preview);
    const auto& pending = interrupted->tools.back();
    if (pending.reverts != original || pending.id != interrupt->call)
      throw std::runtime_error("The original reverse call did not survive restart");
    const auto reverse_run = std::ranges::find(interrupted->runs, pending.run, &kit::RunSnapshot::id);
    if (reverse_run == interrupted->runs.end()) throw std::runtime_error("The original reverse Run was unavailable");
    const std::string later_edit = "User edited the reverted file after the interrupted write\n";
    if (scenario == CodeRevertCase::InterruptedPartial) {
      if (pending.state != kit::ToolCallState::ResultUnknown)
        throw std::runtime_error("An unconfirmed reverse write was not marked unknown");
      std::optional<kit::AgentResult<kit::Receipt<kit::ChangePlanAccepted>>> repeated;
      tasks->Launch([&]() -> Task<void> {
        kit::RevertRequest request{interrupted->summary.id, original, {{config_applied.id, config_applied}}};
        repeated = co_await runtime->Artifacts().PrepareRevertAsync(kit::NewCommandId(), std::move(request));
      });
      Wait(preview, [&] { return repeated.has_value(); }, "The overlapping reverse request did not settle");
      if (repeated->Succeeded() || repeated->Error().code != kit::ErrorCode::ResultUnknown)
        throw std::runtime_error("An unresolved reverse call did not block another request for the same file");
      const auto written = restore.Child(interrupt->call.value + ".revert-0.applied");
      const auto held = restore.Child(interrupt->call.value + ".revert-0.held");
      if (!written.MoveTo(held)) throw std::runtime_error("Could not simulate a missing reverse write receipt");
      WriteFixture(config, later_edit);
      TapFormButton(preview, "Check revert result");
      Wait(preview, [&] { return ShowsText(preview, "The original replacement could not be confirmed: config.txt"); },
          "A missing reverse write receipt did not leave the result unresolved");
      if (preview.Find(ButtonText(prepare_label)).Exists() || snapshot(preview)->tools.back().reconciliation)
        throw std::runtime_error("A missing receipt allowed a new reverse plan or settled the previous one");
      if (!held.MoveTo(written)) throw std::runtime_error("Could not restore the reverse write receipt");
      TapFormButton(preview, "Check revert result");
      Wait(preview, [&] { return ShowsText(preview, "Revert result verified"); }, "The original reverse call did not settle");
    }
    Wait(preview, [&] { return ShowsText(preview, "Reverted: config.txt") && ShowsText(preview, "Skipped: settings.html"); },
        "The original change audit did not show the confirmed file results");
    const auto restored = snapshot(preview);
    const auto& call = restored->tools.back();
    const auto prior = std::ranges::find(restored->tools, original, &kit::ToolCallSnapshot::id);
    const auto run = std::ranges::find(restored->runs, pending.run, &kit::RunSnapshot::id);
    if (prior == restored->tools.end() || prior->state != kit::ToolCallState::Succeeded || run == restored->runs.end() ||
        run->state != reverse_run->state || call.id != pending.id || call.state != kit::ToolCallState::PartiallyApplied ||
        call.source_changes.size() != 1 || call.effects.size() != 2 ||
        call.effects[0].resource.value != config.Path() || call.effects[0].state != kit::ResourceEffectState::Applied ||
        !call.effects[0].after || !call.effects[0].undo || call.effects[1].resource.value != settings.Path() ||
        call.effects[1].state != kit::ResourceEffectState::Unchanged || call.effects[1].undo ||
        call.reconciliation.has_value() != (scenario == CodeRevertCase::InterruptedPartial)) {
      throw std::runtime_error("The recovered reversal lost its result, original Run, or individual resource effects");
    }
    CheckText(config, scenario == CodeRevertCase::InterruptedPartial ? later_edit : "mode=disabled\n");
    CheckText(settings, concurrent_edit);
    bool checked = false;
    tasks->Launch([&]() -> Task<void> {
      kit::ArtifactImpactQuery query{.project = project, .change_command = call.source_changes.front()};
      const auto impact = LiveTake(co_await runtime->Artifacts().InspectImpactAsync(std::move(query)));
      checked = impact.recorded_change && impact.recorded_change->kind == kit::SourceChangeKind::Reverted &&
          impact.recorded_change->before.source == kit::SourceRef{config_applied} &&
          std::ranges::any_of(impact.affected, [&](const auto& item) { return item.affected == evidence; });
    });
    Wait(preview, [&] { return checked; }, "The partial reverse effect did not locate its original implementation evidence");
    return;
  }
  {
    UiTest preview(application, options);
    auto entry = PendingConversation(preview, std::string(title));
    Wait(preview, [&] { return entry.Exists(); }, "The restore approval did not survive restart");
    entry.Tap();
    const auto pending = snapshot(preview)->interactions.back();
    if (pending.id != restore_approval.id || pending.version != restore_approval.version ||
        std::get<kit::ToolApproval>(pending.body).plan != std::get<kit::ToolApproval>(restore_approval.body).plan)
      throw std::runtime_error("Restart changed the approved restore identity");
    TapFormButton(preview, "Approve");
    Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The approved reverse patch did not settle");
    CheckText(config, "mode=disabled\n");
    CheckText(settings, SettingsPage(true));
    const auto restored = snapshot(preview);
    const auto& call = restored->tools.back();
    const auto previous = std::ranges::find(restored->tools, original, &kit::ToolCallSnapshot::id);
    if (previous == restored->tools.end() || previous->state != kit::ToolCallState::Succeeded || call.reverts != original ||
        call.state != kit::ToolCallState::Succeeded || call.effects.size() != 1 || call.source_changes.size() != 1) {
      throw std::runtime_error("The original and reverse effects were not audited separately");
    }
    bool checked = false;
    std::string audit_error;
    tasks->Launch([&]() -> Task<void> {
      try {
        const auto queued = std::ranges::find_if(restored->queue, [&](const auto& item) { return item.run == call.run; });
        if (queued == restored->queue.end()) throw std::runtime_error("The completed restore lost its command");
        const auto command = std::get<kit::CommandId>(queued->input);
        const auto progress = LiveTake(co_await runtime->Commands().ReadAsync(command));
        kit::RevertRequest replay_request{restored->summary.id, original, {{config_applied.id, config_applied}}};
        const auto replay = LiveTake(co_await runtime->Artifacts().PrepareRevertAsync(command, std::move(replay_request)));
        if (progress.state != kit::CommandState::Applied || replay.stamp.commit != progress.accepted.commit ||
            std::get<kit::ChangePlanAccepted>(replay.outcome).queue_item != queued->id || !progress.result ||
            std::get<kit::ChangePlanAccepted>(*progress.result).call != call.id) {
          throw std::runtime_error("Restore replay did not preserve its receipt and resolved call");
        }
        kit::ArtifactImpactQuery query{.project = project, .change_command = call.source_changes.front()};
        const auto impact = LiveTake(co_await runtime->Artifacts().InspectImpactAsync(std::move(query)));
        checked = impact.recorded_change && impact.recorded_change->kind == kit::SourceChangeKind::Reverted &&
            impact.recorded_change->before.source == kit::SourceRef{config_applied} &&
            std::ranges::any_of(impact.affected, [&](const auto& item) { return item.affected == evidence; });
      } catch (const std::exception& error) {
        audit_error = error.what();
      }
    });
    Wait(preview, [&] { return checked || !audit_error.empty(); }, "The restore audit did not locate the implementation affected by the change");
    if (!audit_error.empty()) throw std::runtime_error(audit_error);
  }
  tasks.reset();
  runtime.reset();
  const Application without_code{App, {.application_hooks = {MakeRunSettingsTestInstaller(directory.Child("sessions.sqlite3"),
      workspace, {}, [&](auto started, auto, auto) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      })}}};
  constexpr auto unavailable = "The session's agent revision is unavailable";
  const auto open_session = [&](UiTest& preview) {
    auto entry = ConversationRow(preview, title);
    Wait(preview, [&] { return entry.Exists(); }, "The old code session did not reopen without its agent");
    entry.Tap();
  };
  const auto failure = UiSelector::Key("artifact-change-queue-error");
  {
    UiTest preview(without_code, options);
    open_session(preview);
    const auto before = snapshot(preview);
    open_artifact(preview);
    TapFormButton(preview, prepare_label);
    auto retry = preview.Find(ButtonText(prepare_label));
    Wait(preview, [&] { return ShowsText(preview, unavailable) && retry.Exists() && retry.One().enabled; },
        "The revert review did not report the rejected start and restore its controls");
    const auto after = snapshot(preview);
    const auto& queued = after->queue.back();
    if (after->queue.size() != before->queue.size() + 1 || queued.state != kit::QueueItemState::Rejected || queued.run ||
        !queued.reason || queued.reason->message != unavailable || after->runs.size() != before->runs.size() ||
        after->summary.pending_interactions || ShowsText(preview, status_label))
      throw std::runtime_error("A failed start appeared to prepare or execute a reverse patch");
    TapFormButton(preview, "Back to conversation");
    Wait(preview, [&] { return preview.Find(failure).Exists() && ShowsText(preview, unavailable); },
        "Leaving the review hid the rejected start");
  }
  tasks.reset();
  runtime.reset();
  {
    UiTest preview(without_code, options);
    open_session(preview);
    Wait(preview, [&] { return preview.Find(failure).Exists() && ShowsText(preview, unavailable); },
        "Reopening the conversation hid the rejected start");
    CheckText(config, "mode=disabled\n");
    CheckText(settings, SettingsPage(true));
  }
}

enum class EditorBaselineCase { Current, EditorChanged, DiskChanged, FullAccess };
enum class EditorAcceptanceStage { Complete, Prepare, Resume };

void VerifyCodeEditorBaseline(const File& directory, EditorBaselineCase scenario,
    EditorAcceptanceStage stage = EditorAcceptanceStage::Complete) {
  const auto workspace = directory.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the editor workspace");
  const auto config = workspace.Child("config.txt");
  const auto settings = workspace.Child("settings.html");
  const auto marker = workspace.Child("tested.txt");
  const auto approval_identity = directory.Child("approval.txt");
  const std::string disk = "mode=disabled\n";
  const std::string draft = disk + "owner=preview\n";
  const std::string title = "Enable mode using the unsaved editor draft";
  const bool resuming = stage == EditorAcceptanceStage::Resume;
  if (!resuming) {
    WriteFixture(config, disk);
    WriteFixture(settings, SettingsPage(false));
    WriteFixture(workspace.Child("verify.cmake"), "file(WRITE \"tested.txt\" \"executed\")\n");
    WriteFixture(workspace.Child("CTestTestfile.cmake"),
        "add_test(verify \"" AGENTKIT_PREVIEW_CMAKE "\" \"-P\" \"verify.cmake\")\n");
  }
  kit::ModelRegistration model{{{"preview.test-editor"}, 1}, {"preview", "editor-baseline", "script", {}},
                               std::make_shared<EditorPatchModel>(resuming)};
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  Application application{App, {.application_hooks = {MakeCodeTestInstaller(directory.Child("sessions.sqlite3"), workspace,
      std::move(model), File(AGENTKIT_PREVIEW_CTEST), {}, [&](auto started, auto, auto) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      })}}};
  UiTest preview(application, {.viewport = {1080.0F, 920.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  const auto check_editor_entry = [&] {
    auto title_view = preview.Find(UiSelector::Key("agent-chat-title"));
    auto header_view = preview.Find(UiSelector::Key("agent-chat-heading"));
    auto edit = preview.Find(UiSelector::Key("workspace-actions"));
    Wait(preview, [&] { return title_view.Exists() && header_view.Exists() && edit.Exists() && edit.One().in_viewport; },
        "Workspace action is missing from the conversation heading");
    const auto title = title_view.One().bounds;
    const auto heading = header_view.One().bounds;
    const auto action = edit.One().bounds;
    if (heading.height > 56.0F || title.width <= 0.0F || action.x < title.x + title.width - 1.0F ||
        action.y >= title.y + title.height || action.y + action.height <= title.y ||
        action.x + action.width > heading.x + heading.width + 1.0F || action.width >= heading.width) {
      throw std::runtime_error("Workspace action escaped the conversation column");
    }
  };
  const auto active_approval = [&] {
    Wait(preview, [&] { return tasks && runtime; }, "Editor acceptance runtime did not start");
    std::shared_ptr<const kit::SessionSnapshot> snapshot;
    tasks->Launch([&]() -> Task<void> {
      const auto list = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
      const auto found = std::ranges::find(list.sessions, title, &kit::SessionSummary::title);
      if (found == list.sessions.end()) co_return;
      auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(found->id));
      snapshot = chat->Snapshot();
    });
    Wait(preview, [&] { return bool(snapshot); }, "Editor approval snapshot was unavailable");
    for (const auto& interaction : snapshot->interactions) {
      const auto* approval = std::get_if<kit::ToolApproval>(&interaction.body);
      if (interaction.state == kit::InteractionState::Pending && approval && approval->preview.title == "Review workspace changes" && interaction.run) {
        if (scenario == EditorBaselineCase::FullAccess && (!snapshot->runs.back().execution_profile ||
            !snapshot->runs.back().execution_profile->id.value.starts_with("preview.full/"))) {
          throw std::runtime_error("The editor approval was not tested in Full Access");
        }
        return snapshot->summary.id.value + '\n' + interaction.run->value + '\n' + interaction.id.value + '\n' +
            std::to_string(interaction.version) + '\n' + approval->call.value + '\n' + approval->plan.value;
      }
    }
    throw std::runtime_error("Editor changes did not request an approval");
  };
  const auto enter_editor = [&](const char* entry, bool enter_path) {
    if (enter_path) TapWorkspaceAction(preview, entry);
    else TapFormButton(preview, entry);
    auto path = preview.Find(UiSelector::Key("workspace-editor-path"));
    Wait(preview, [&] { return path.Exists(); }, "Workspace editor path was unavailable");
    if (enter_path) {
      if (preview.Find(ButtonText("Open file")).One().enabled)
        throw std::runtime_error("Opening an editor file was available before entering its path");
      path.Tap();
      auto focused_path = preview.Find(UiSelector::AllOf(UiSelector::Key("workspace-editor-path"), UiSelector::Focused(true)));
      Wait(preview, [&] { return focused_path.Exists(); }, "Workspace editor path did not receive focus");
      path.ReplaceText("config.txt");
    } else if (path.One().value != "config.txt") {
      throw std::runtime_error("The diff did not locate the workspace source");
    }
    TapFormButton(preview, "Open file");
    auto editor = preview.Find(UiSelector::Key("workspace-editor"));
    Wait(preview, [&] { return editor.Exists() && editor.One().in_viewport; }, "Workspace code editor did not open the file");
    editor.Tap();
  };
  const auto close_editor = [&] {
    TapFormButton(preview, "Done");
    Wait(preview, [&] { return !preview.Find(UiSelector::Key("workspace-editor-path")).Exists(); },
        "Workspace editor did not close");
  };
  const auto append = [&](std::size_t offset, std::string text) {
    const auto active = preview.ActiveTextInput();
    if (!active) throw std::runtime_error("Workspace editor did not focus for editing");
    const auto end = static_cast<TextOffset>(offset);
    const TextInputCommand select{.kind = TextInputCommandKind::SetSelection, .selection_after = TextSelection{end, end}};
    const TextInputCommand insert{.kind = TextInputCommandKind::CommitText, .text = std::move(text)};
    if (preview.SendTextInput({active->session_id, {select, insert}}).result_code != TextInputResultCode::Ok)
      throw std::runtime_error("Could not edit the unsaved workspace file");
    preview.Pump();
  };
  if (resuming) {
    auto session = PendingConversation(preview, title);
    Wait(preview, [&] { return session.Exists() && session.One().in_viewport; }, "The original editor approval was not listed after restart");
    session.Tap();
    CheckText(approval_identity, active_approval());
  } else {
    ChooseDraftAssistant(preview, "Code");
    if (scenario == EditorBaselineCase::FullAccess) {
      auto permissions = preview.Find(UiSelector::Key("run-permissions"));
      Wait(preview, [&] { return permissions.Exists() && permissions.One().in_viewport; }, "Code permission control did not mount");
      ClickPreviewMouse(preview, permissions.One().visible_bounds);
      auto full_access = preview.Find(UiSelector::Text("Full Access"));
      Wait(preview, [&] { return full_access.Exists() && full_access.One().enabled; }, "Full Access was unavailable");
      full_access.Tap();
      TapFormButton(preview, "Allow");
      Wait(preview, [&] { return !preview.Find(ButtonText("Allow")).Exists(); }, "Full Access confirmation did not close");
    }
    SendNewConversation(preview, title);
    if (scenario == EditorBaselineCase::Current && stage == EditorAcceptanceStage::Complete) {
      check_editor_entry();
      preview.SetWindowMetrics({.viewport = {520.0F, 680.0F}});
      preview.Pump();
      check_editor_entry();
      preview.SetWindowMetrics({.viewport = {1080.0F, 920.0F}});
      preview.Pump();
    }
    enter_editor("Edit workspace file", true);
    append(disk.size(), "owner=preview\n");
    close_editor();
    CheckText(config, disk);
    SubmitWorkspaceScope(preview);
    if (scenario == EditorBaselineCase::FullAccess) (void)active_approval();
    if (stage == EditorAcceptanceStage::Prepare) WriteFixture(approval_identity, active_approval());
  }
  CheckText(config, disk);
  CheckText(settings, SettingsPage(false));
  TapReviewCard(preview, "Review attachment 1");
  auto diff = preview.Find(UiSelector::Key("artifact-text"));
  Wait(preview, [&] { return diff.Exists() && !ReviewedDiff(preview).empty(); }, "The editor-based patch preview did not open");
  if (std::ranges::any_of(preview.Find(ButtonText("Edit workspace file")).All(), &UiNodeInfo::in_viewport))
    throw std::runtime_error("Conversation workspace actions appeared on the diff review page");
  if (ReviewedDiff(preview).find("-mode=disabled\n+mode=enabled\n+owner=preview") == std::string::npos)
    throw std::runtime_error("The reviewed patch did not show everything that would reach disk");
  enter_editor("Open editor source", false);
  const bool stale_editor = scenario == EditorBaselineCase::EditorChanged;
  if (resuming) {
    if (!preview.Find(ButtonText("Discard draft")).Exists() ||
        ShowsText(preview, "The reviewed editor version is no longer current.") != stale_editor) {
      throw std::runtime_error("Reopened editor did not restore the correct draft version");
    }
  } else if (stale_editor) {
    append(draft.size(), "revision=2\n");
  }
  close_editor();
  TapFormButton(preview, "Back to conversation");
  check_editor_entry();
  if (stage == EditorAcceptanceStage::Prepare) return;
  const std::string external = "mode=overridden\n";
  if (scenario == EditorBaselineCase::DiskChanged) WriteFixture(config, external);
  TapFormButton(preview, "Approve");
  if (scenario == EditorBaselineCase::Current || scenario == EditorBaselineCase::FullAccess) {
    Wait(preview, [&] { return ShowsText(preview, "Run workspace tests"); }, "Approved editor patch did not request verification");
    TapFormButton(preview, "Approve");
    Wait(preview, [&] { return marker.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "Workspace verification did not finish");
    CheckText(config, "mode=enabled\nowner=preview\n");
    CheckText(settings, SettingsPage(true));
  } else {
    const auto reason = scenario == EditorBaselineCase::DiskChanged ? "File changed since the patch was drafted: config.txt"
        : "Editor or disk baseline changed: config.txt";
    Wait(preview, [&] { return ShowsText(preview, reason) && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The stale editor patch did not report the conflict");
    CheckText(config, scenario == EditorBaselineCase::DiskChanged ? external : disk);
    CheckText(settings, SettingsPage(false));
    if (marker.Exists()) throw std::runtime_error("Verification ran after a stale editor approval");
    enter_editor("Edit workspace file", true);
    TapFormButton(preview, "Discard draft");
    Wait(preview, [&] { return !preview.Find(ButtonText("Discard draft")).Exists() && preview.Find(ButtonText("Open file")).One().enabled; },
        "The stale editor draft was not discarded");
    close_editor();
    CheckText(config, scenario == EditorBaselineCase::DiskChanged ? external : disk);
  }
}

enum class CodeFailureCase { Partial, Verification, Interrupted, InterruptedPartial };

void VerifyCodeFailure(const File& directory, CodeFailureCase scenario) {
  const bool partial = scenario == CodeFailureCase::Partial || scenario == CodeFailureCase::InterruptedPartial;
  const bool interrupted = scenario == CodeFailureCase::Interrupted || scenario == CodeFailureCase::InterruptedPartial;
  const bool retry_before_patch = scenario == CodeFailureCase::Verification;
  const auto workspace = directory.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the code failure workspace");
  const auto config = workspace.Child("config.txt");
  const auto settings = workspace.Child("settings.html");
  const auto marker = workspace.Child("tested.txt");
  const std::string user_edit = "User edited the second file during the patch\n";
  WriteFixture(config, "mode=disabled\n");
  WriteFixture(settings, SettingsPage(false));
  WriteFixture(workspace.Child("verify.cmake"),
      "file(WRITE \"tested.txt\" \"executed\")\nstring(ASCII 27 escape)\n"
      "message(FATAL_ERROR \"${escape}[31mFixture verification failed${escape}[0m\")\n");
  WriteFixture(workspace.Child("CTestTestfile.cmake"),
      "add_test(verify \"" AGENTKIT_PREVIEW_CMAKE "\" \"-P\" \"verify.cmake\")\n");
  const std::string title = partial ? "Apply code patch with a concurrent edit" : "Apply code patch and run a failing test";
  kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}},
                               std::make_shared<PatchModel>(retry_before_patch)};
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::optional<TaskScope> tasks;
  const auto interruption = std::make_shared<ToolResultInterruption>();
  const auto restore = directory.Child("workspace-evidence").Child("restore");
  PreviewStoreAdapter adapt_store;
  if (interrupted) {
    adapt_store = [restore, interruption, partial](auto store) {
      return std::make_shared<UnconfirmedToolResultStore>(std::move(store), interruption, [restore, partial](const auto& call) {
        return restore.Child(call.value + (partial ? ".patch-0.applied" : ".patch-1.applied"));
      });
    };
  }
  auto hook = MakeCodeTestInstaller(directory.Child("sessions.sqlite3"), workspace, std::move(model), File(AGENTKIT_PREVIEW_CTEST),
      std::move(adapt_store),
      [&](auto started, auto, auto) -> Task<void> {
        runtime = LiveTake(std::move(started));
        tasks = UseApplicationTaskScope();
        co_return;
      });
  Application application{App, {.application_hooks = {hook}}};
  const UiTestOptions options{.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  const auto snapshot = [&](UiTest& preview) {
    Wait(preview, [&] { return tasks && runtime; }, "Code failure runtime did not start");
    std::shared_ptr<const kit::SessionSnapshot> result;
    tasks->Launch([&]() -> Task<void> {
      const auto list = LiveTake(co_await runtime->Sessions().ListAsync({}, {}));
      const auto found = std::ranges::find(list.sessions, title, &kit::SessionSummary::title);
      if (found == list.sessions.end()) co_return;
      auto chat = LiveTake(co_await runtime->Sessions().OpenAsync(found->id));
      result = chat->Snapshot();
    });
    Wait(preview, [&] { return bool(result); }, "Code failure snapshot was unavailable");
    return result;
  };
  kit::ToolCallId call;
  {
    UiTest preview(application, options);
    if (retry_before_patch) {
      ChooseDraftAssistant(preview, "Code");
      SendNewConversation(preview, title);
      ShareWorkspaceScope(preview);
      const auto retry = preview.Find(UiSelector::Key("agent-chat-retry"));
      Wait(preview, [&] { return retry.Exists() && retry.One().enabled &&
          ShowsText(preview, "Fixture model failed before applying the patch"); }, "The failed model run did not offer retry");
      const auto failed = snapshot(preview);
      if (failed->runs.size() != 1 || failed->runs.front().state != kit::RunState::Failed)
        throw std::runtime_error("The retry action did not start from a failed Run");
      auto draft = preview.Find(UiSelector::Key("composer"));
      draft.Tap();
      draft.ReplaceText("Keep the unsent draft");
      retry.Tap();
      SubmitWorkspaceScope(preview);
      const auto retried = snapshot(preview);
      if (retried->runs.size() != 2 || retried->runs.front().id != failed->runs.front().id ||
          retried->runs.back().trigger != kit::RunTrigger::Retry || retried->runs.back().retry_of != failed->runs.front().id ||
          retried->summary.active_run != retried->runs.back().id || draft.One().value != "Keep the unsent draft" || retry.Exists())
        throw std::runtime_error("Retry did not preserve the original Run and draft while starting one new Run");
    } else {
      PrepareCodeChange(preview, title);
    }
    const auto before = snapshot(preview);
    call = before->tools.back().id;
    if (before->tools.back().tool.id != agentkit_preview::ApplyCodePatch)
      throw std::runtime_error("The patch did not request approval");
    CheckText(config, "mode=disabled\n");
    CheckText(settings, SettingsPage(false));
    interruption->target = before->summary.id;
    interruption->call = call;
    if (partial) concurrent_code_edit.store(std::make_shared<const ConcurrentCodeEdit>(ConcurrentCodeEdit{call, settings, user_edit}));
    TapFormButton(preview, "Approve");
    if (interrupted) {
      Wait(preview, [&] { return interruption->happened; }, "The patch outcome commit was not interrupted after writing files");
      CheckText(config, "mode=enabled\n");
      CheckText(settings, partial ? user_edit : SettingsPage(true));
    } else if (partial) {
      Wait(preview, [&] { return ShowsText(preview, "Applied: config.txt\nSkipped: settings.html"); },
          "The patch did not report the applied and skipped files");
    } else {
      Wait(preview, [&] { return ShowsText(preview, "Run workspace tests"); }, "The test process did not request approval");
      if (marker.Exists()) throw std::runtime_error("The test process ran before approval");
      TapFormButton(preview, "Approve");
    }
    if (!interrupted)
      Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The unsuccessful code run did not settle");
  }
  tasks.reset();
  runtime.reset();
  UiTest reopened(application, options);
  auto entry = ConversationRow(reopened, title);
  Wait(reopened, [&] { return entry.Exists(); }, "The unsuccessful code conversation did not survive restart");
  entry.Tap();
  const auto saved = snapshot(reopened);
  const auto patch = std::ranges::find(saved->tools, call, &kit::ToolCallSnapshot::id);
  const auto retry = reopened.Find(UiSelector::Key("agent-chat-retry"));
  const auto expect_blocked_retry = [&](std::string message) {
    Wait(reopened, [&] { return ShowsText(reopened, message); }, "The conversation did not explain why retry was blocked");
    if (retry.Exists()) throw std::runtime_error("The conversation offered to retry a Run that could have written files");
  };
  constexpr auto applied_retry = "The original tool applied changes. Review them before starting a new request.";
  if (interrupted) {
    if (patch == saved->tools.end() || patch->state != kit::ToolCallState::ResultUnknown || !patch->issued_targets ||
        patch->issued_targets->size() != 2 || saved->summary.active_run) {
      throw std::runtime_error("The same issued patch did not reopen with its result unknown");
    }
    expect_blocked_retry("Check the original tool result before retrying.");
    const auto marker_file = restore.Child(call.value + ".patch-0.applied");
    const auto held = restore.Child(call.value + ".patch-0.held");
    if (!marker_file.MoveTo(held)) throw std::runtime_error("Could not simulate a missing patch receipt");
    const std::string later = "User edited the first file after the interrupted patch\n";
    if (!partial) WriteFixture(config, later);
    TapWorkspaceAction(reopened, "Check patch result");
    Wait(reopened, [&] { return ShowsText(reopened, "The original replacement could not be confirmed: config.txt"); },
        "Missing patch evidence did not leave the result unknown");
    expect_blocked_retry("Check the original tool result before retrying.");
    if (snapshot(reopened)->tools.back().reconciliation || reopened.Find(ButtonText("Review recovered patch")).Exists())
      throw std::runtime_error("Missing patch evidence settled the original call");
    if (!held.MoveTo(marker_file)) throw std::runtime_error("Could not restore the patch write receipt");
    TapWorkspaceAction(reopened, "Check patch result");
    expect_blocked_retry(applied_retry);
    TapWorkspaceAction(reopened, "Review recovered patch");
    const auto result = snapshot(reopened);
    const auto& confirmed = result->tools.back();
    if (confirmed.id != call || !confirmed.reconciliation || result->runs.back().id != saved->runs.back().id ||
        result->runs.back().state != saved->runs.back().state || result->summary.active_run || !confirmed.artifacts.empty() ||
        confirmed.state != (partial ? kit::ToolCallState::PartiallyApplied : kit::ToolCallState::Succeeded) ||
        confirmed.effects.size() != 2 || confirmed.effects[0].resource.value != config.Path() ||
        confirmed.effects[0].state != kit::ResourceEffectState::Applied || !confirmed.effects[0].undo ||
        confirmed.effects[1].resource.value != settings.Path() ||
        confirmed.effects[1].state != (partial ? kit::ResourceEffectState::Unchanged : kit::ResourceEffectState::Applied) ||
        marker.Exists() || std::ranges::any_of(result->tools, [](const auto& item) { return item.tool.id == agentkit_preview::RunCodeTests; })) {
      throw std::runtime_error("Patch recovery lost its original call or claimed unverified work");
    }
    Wait(reopened, [&] { return ShowsText(reopened, "Applied: config.txt") &&
                                ShowsText(reopened, partial ? "Skipped: settings.html" : "Applied: settings.html"); },
        "The recovered workspace change did not expose per-file results");
    CheckText(config, partial ? "mode=enabled\n" : later);
    CheckText(settings, partial ? user_edit : SettingsPage(true));
    if (partial) {
      if (reopened.Find(UiSelector::Type<Checkbox>()).Count() != 1)
        throw std::runtime_error("The recovered patch offered to revert a skipped file");
      auto prepare = reopened.FindSemantics(UiSemanticSelector::AllOf(
          UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Prepare revert")));
      Wait(reopened, [&] { return prepare.Count() == 1 && prepare.One().enabled && !prepare.One().offscreen; },
          "The recovered patch did not offer an enabled reverse change");
      prepare.PerformSemanticAction({SemanticActionKind::Activate, {}});
      try {
        Wait(reopened, [&] { return ShowsText(reopened, "Return to the conversation to review the reverse patch."); },
            "The recovered patch could not prepare a reverse change");
      } catch (...) {
        static_cast<void>(directory.Child("revert.snapshot").WriteString(reopened.CaptureSnapshot().ToString()));
        throw;
      }
      TapFormButton(reopened, "Back to conversation");
      Wait(reopened, [&] { return ShowsText(reopened, "Review reverse patch"); }, "The recovered change did not request approval");
      CheckText(config, "mode=enabled\n");
      TapFormButton(reopened, "Approve");
      Wait(reopened, [&] { return !reopened.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The approved reverse change did not finish");
      CheckText(config, "mode=disabled\n");
      CheckText(settings, user_edit);
    }
    return;
  }
  if (saved->runs.back().state != kit::RunState::Failed || patch == saved->tools.end() || patch->effects.size() != 2 ||
      patch->effects[0].state != kit::ResourceEffectState::Applied || !patch->effects[0].after || !patch->effects[0].undo ||
      ShowsText(reopened, "Workspace changes applied and tested"))
    throw std::runtime_error("The failed run lost the earlier file effect or claimed success");
  expect_blocked_retry(applied_retry);
  CheckText(config, "mode=enabled\n");
  const auto test = std::ranges::find_if(saved->tools, [](const auto& item) { return item.tool.id == agentkit_preview::RunCodeTests; });
  if (partial) {
    Wait(reopened, [&] { return ShowsText(reopened, "Applied: config.txt\nSkipped: settings.html"); },
        "The per-file result did not survive restart");
    if (patch->state != kit::ToolCallState::PartiallyApplied || !patch->reason || patch->reason->code != kit::ErrorCode::Conflict ||
        !patch->artifacts.empty() || patch->effects[1].state != kit::ResourceEffectState::Unchanged ||
        patch->effects[1].after || patch->effects[1].undo || test != saved->tools.end() || marker.Exists())
      throw std::runtime_error("A partial write lost its conflict, or published a complete patch or verification");
    CheckText(settings, user_edit);
  } else {
    if (patch->state != kit::ToolCallState::Succeeded || patch->effects[1].state != kit::ResourceEffectState::Applied ||
        test == saved->tools.end() || test->state != kit::ToolCallState::Failed || !test->reason ||
        !test->reason->message.starts_with("Tests failed (exit ") || !test->artifacts.empty())
      throw std::runtime_error("A nonzero CTest exit did not preserve the completed patch and failed verification");
    CheckText(settings, SettingsPage(true));
    CheckText(marker, "executed");
    Wait(reopened, [&] { return ShowsText(reopened, test->reason->message); }, "The failing CTest exit did not survive restart");
    const bool has_diagnostics = std::ranges::any_of(saved->messages, [&](const auto& message) {
      return message.run == test->run && message.author == kit::MessageAuthor::Tool &&
          std::ranges::any_of(message.blocks, [](const auto& block) {
            const auto* content = std::get_if<kit::TextBlock>(&block.body);
            return content && content->text.find("Fixture verification failed") != std::string::npos &&
                content->text.find('\x1b') == std::string::npos && content->text.find("[HANDLER_OUTPUT]") == std::string::npos &&
                content->text.find("[ERROR_MESSAGE]") == std::string::npos;
          });
    });
    if (!has_diagnostics) throw std::runtime_error("CTest diagnostics were unavailable after restart");
    const auto raw = directory.Child("test-logs").Child(test->id.value + ".log").ReadString();
    if (!raw.Succeeded() || raw.Value().find("\x1b[31mFixture verification failed\x1b[0m") == std::string::npos)
      throw std::runtime_error("The original CTest log did not preserve the process diagnostic");
    CheckCTestDisclosure(reopened, "Fixture verification failed");
  }
}

void SeedReleaseTestWorkspace(const File& files, std::string_view test_script) {
  if (!files.CreateDirectories()) throw std::runtime_error("Could not create the release workspace");
  WriteFixture(files.Child("config.txt"), "mode=disabled\n");
  WriteFixture(files.Child("settings.html"), SettingsPage(false));
  WriteFixture(files.Child("verify.cmake"), test_script);
  WriteFixture(files.Child("CTestTestfile.cmake"),
      "add_test(verify \"" AGENTKIT_PREVIEW_CMAKE "\" \"-P\" \"verify.cmake\")\n");
}

void VerifyReleaseFailure(const File& root, bool tests_pass) {
  const auto workspace = root.Child("code");
  SeedReleaseTestWorkspace(workspace, tests_pass ? WorkspaceTestScript("checked> Enabled", false) :
      "message(FATAL_ERROR \"Intentional workspace verification failure\")\n");
  kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}},
                               std::make_shared<PatchModel>()};
  Application application{App, {.application_hooks = {MakeCodeTestInstaller(root.Child("sessions.sqlite3"), workspace,
      std::move(model), File(AGENTKIT_PREVIEW_CTEST))}}};
  const auto* title = tests_pass ? "Workspace release: Require screenshot" : "Workspace release: Reject failed tests";
  StartWorkspaceRelease(application, workspace.Child("config.txt"), workspace.Child("settings.html"), title);
  RejectIncompleteVerification(application, workspace.Child("tested.txt"), title, tests_pass);
}

void VerifyReleaseCompletionRevision(const File& root, bool complete_automatically) {
  using namespace agentkit_preview;
  const auto files = root.Child("code");
  SeedReleaseTestWorkspace(files, WorkspaceTestScript("checked> Enabled", true));
  const auto tested = std::filesystem::u8path(files.Child("tested.txt").Path());
  const auto old_deck = root.Child("exports").Child("BeforeRevision.pptx");
  const auto new_deck = root.Child("exports").Child("AfterRevision.pptx");
  constexpr auto general_title = "Workspace release: Ship settings";
  constexpr auto ppt_title = "Release slides";
  const kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}},
                                    std::make_shared<PatchModel>()};
  kit::InteractionSnapshot old_approval;
  kit::InteractionId revised_brief;
  kit::GoalRef revised_goal;
  std::vector<kit::SourceSlice> prior_evidence;
  std::vector<std::byte> prior_deck_bytes;
  std::filesystem::file_time_type verified_at;
  constexpr auto screenshot_follow_up = "Use the screenshot conclusion";
  std::string screenshot_review_key;
  const auto expected_completion = complete_automatically ? kit::GoalCompletionPolicy::Verified :
      kit::GoalCompletionPolicy::RequireConfirmation;

  const auto require_invalidated = [&](const std::shared_ptr<kit::AgentRuntime>& runtime,
      const std::shared_ptr<kit::Conversation>& general) -> Task<void> {
    const auto rejected = co_await general->SubmitAsync(kit::NewCommandId(),
        {old_approval.id, old_approval.version, kit::ApprovalDecision::Approve});
    const auto snapshot = general->Snapshot();
    const auto prior = std::ranges::find(snapshot->interactions, old_approval.id, &kit::InteractionSnapshot::id);
    if (rejected.Succeeded() || rejected.Error().code != kit::ErrorCode::AlreadyClosed ||
        prior == snapshot->interactions.end() || prior->state != kit::InteractionState::Invalidated) {
      throw std::runtime_error("The old completion approval remained usable after the goal changed");
    }
    const auto command = std::get<kit::OperationApproval>(old_approval.body).pending_command;
    if (LiveTake(co_await runtime->Commands().ReadAsync(command)).state != kit::CommandState::Withdrawn) {
      throw std::runtime_error("The old evidence submission did not resolve as withdrawn");
    }
  };

  for (bool reopen : {false, true}) {
    bool awaiting_user = false;
    bool ui_finished = false;
    bool done = false;
    kit::InteractionId new_approval;
    std::exception_ptr failure;
    auto hook = MakeCodeTestInstaller(root.Child("sessions.sqlite3"), files, model, File(AGENTKIT_PREVIEW_CTEST), {},
        [&](auto started, auto workspace, auto project) -> Task<void> {
      try {
        const auto runtime = LiveTake(std::move(started));
        std::optional<kit::SourceSlice> screenshot_source;
        if (!workspace) throw std::runtime_error("The release workspace did not open");
        if (!reopen) {
          kit::CreateSessionRequest target{.options = {
              .agent = PptAgent, .project = project, .workspace = *workspace, .title = ppt_title}};
          LiveTake(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(target)));
        }
        const auto ppt = co_await LiveConversation(runtime, project, *workspace, PptAgent, ppt_title);
        std::optional<std::string> initial;
        if (!reopen) initial.emplace(general_title);
        const auto general = co_await LiveConversation(runtime, project, *workspace, GeneralAgent, general_title, initial);
        auto general_watch = co_await LiveObserve(general);
        auto ppt_watch = co_await LiveObserve(ppt);
        if (!reopen) {
          co_await LiveRunCompleted(general);
          auto goal = co_await SetLiveReleaseCount(general, co_await LiveReleaseGoal(general), 10, 0);
          LiveTake(co_await general->Goals().UpdateAsync(kit::NewCommandId(),
              kit::ChangeGoal{goal.ref.id, goal.revision, kit::EnableGoal{}}));
          const auto design = co_await LiveConversation(runtime, project, *workspace, DesignAgent, "Release design", {}, true);
          auto design_watch = co_await LiveObserve(design);
          const auto review = co_await LiveInteraction(design, "Review design");
          LiveTake(co_await design->SubmitAsync(kit::NewCommandId(), {review.id, review.version, kit::JsonValue{true}}));
          const auto code = co_await LiveConversation(runtime, project, *workspace, CodeAgent, "Release implementation", {}, true);
          auto code_watch = co_await LiveObserve(code);
          const auto scope = co_await LiveInteraction(code, "Choose files to share and edit");
          kit::JsonValue selection{kit::JsonValue::Array{kit::JsonValue{std::string("config.txt")}, kit::JsonValue{std::string("settings.html")}}};
          LiveTake(co_await code->SubmitAsync(kit::NewCommandId(), {scope.id, scope.version, std::move(selection)}));
          co_await LiveApprove(code, co_await LiveInteraction(code, "Review workspace changes"));
          const auto verification = co_await LiveConversation(runtime, project, *workspace, VerificationAgent, "Verify Release implementation", {}, true);
          auto verification_watch = co_await LiveObserve(verification);
          co_await LiveApprove(verification, co_await LiveInteraction(verification, "Run workspace tests"));
          co_await LiveApprove(ppt, co_await StageLiveReleaseDeck(ppt, goal.ref, 10, "BeforeRevision.pptx"));
          old_approval = co_await LiveInteraction(general, "Complete goal", {}, 15s);
          co_await LiveRunCompleted(general);
          goal = co_await LiveReleaseGoal(general);
          if (goal.state != kit::GoalState::AwaitingCompletionApproval || !goal.evidence ||
              goal.evidence->sources.size() != 5 || !goal.completion_review || goal.completion_review->definition != goal.ref) {
            throw std::runtime_error("The original release did not reach completion approval with verified evidence");
          }
          auto old_bytes = old_deck.ReadBytes();
          if (!old_bytes.Succeeded()) throw std::runtime_error(old_bytes.Error().message);
          prior_deck_bytes = std::move(old_bytes).Value();
          prior_evidence = goal.evidence->sources;
          verified_at = std::filesystem::last_write_time(tested);
          auto definition = goal.spec.definition;
          std::get<kit::JsonValue::Object>(definition.completion_conditions.value).at("slide_count") = kit::JsonValue{std::int64_t{5}};
          const auto edit = LiveTake(co_await general->Goals().PreviewEditAsync(goal.ref.id, goal.revision, definition));
          if (!edit.impact.resets_completion_review || !edit.impact.retiring_linked_runs.empty() ||
              edit.impact.invalidated_interactions != std::vector{old_approval.id}) {
            throw std::runtime_error("The edit preview did not identify the obsolete completion approval");
          }
          awaiting_user = true;
          auto revised = goal;
          const auto deadline = std::chrono::steady_clock::now() + 25s;
          while (revised.ref == goal.ref && std::chrono::steady_clock::now() < deadline) {
            co_await Delay(25ms);
            revised = co_await LiveReleaseGoal(general);
          }
          revised_goal = revised.ref;
          if (revised_goal.id != goal.ref.id || revised_goal.version != goal.ref.version + 1 || revised.completion_review ||
              revised.spec.definition.completion != expected_completion) {
            throw std::runtime_error("The edit did not publish a new definition with a fresh review state");
          }
          co_await require_invalidated(runtime, general);
          revised_brief = (co_await LiveInteraction(ppt, "Presentation brief", revised_goal, 15s)).id;
        } else {
          co_await require_invalidated(runtime, general);
          if ((co_await LiveInteraction(ppt, "Presentation brief", revised_goal, 15s)).id != revised_brief || new_deck.Exists()) {
            throw std::runtime_error("The revised presentation did not resume its original request after reopening");
          }
          co_await LiveApprove(ppt, co_await StageLiveReleaseDeck(ppt, revised_goal, 5, "AfterRevision.pptx"));
          auto finished = co_await LiveReleaseGoal(general);
          if (!complete_automatically) {
            const auto approval = co_await LiveInteraction(general, "Complete goal", {}, 15s);
            finished = co_await LiveReleaseGoal(general);
            if (approval.id == old_approval.id || !finished.completion_review ||
                finished.completion_review->definition != revised_goal) {
              throw std::runtime_error("The revised approval did not bind the new goal definition");
            }
            new_approval = approval.id;
            awaiting_user = true;
          }
          const auto deadline = std::chrono::steady_clock::now() + 25s;
          while (finished.state != kit::GoalState::Completed && std::chrono::steady_clock::now() < deadline) {
            if (complete_automatically && finished.state == kit::GoalState::AwaitingCompletionApproval)
              throw std::runtime_error("Automatic completion still requested completion approval");
            co_await Delay(25ms);
            finished = co_await LiveReleaseGoal(general);
          }
          const auto old_bytes = old_deck.ReadBytes();
          if (finished.state != kit::GoalState::Completed || finished.ref != revised_goal ||
              std::filesystem::last_write_time(tested) != verified_at ||
              !old_bytes.Succeeded() || old_bytes.Value() != prior_deck_bytes) {
            throw std::runtime_error("The revised goal repeated verification or changed the previously exported deck");
          }
          auto before = archive::ArchiveReader::Open(old_deck);
          auto after = archive::ArchiveReader::Open(new_deck);
          if (!before.Succeeded() || !after.Succeeded() ||
              before.Value().ReadEntry("ppt/slides/slide11.xml").Succeeded() ||
              after.Value().ReadEntry("ppt/slides/slide6.xml").Succeeded()) {
            throw std::runtime_error("The two exports did not match their approved slide counts");
          }
          CheckReleaseScreenshot(before.Value(), root, "slide10");
          CheckReleaseScreenshot(after.Value(), root, "slide5");
          if (!finished.evidence || finished.evidence->sources.size() != prior_evidence.size() ||
              !std::equal(prior_evidence.begin(), prior_evidence.end() - 1, finished.evidence->sources.begin()) ||
              prior_evidence.back() == finished.evidence->sources.back())
            throw std::runtime_error("The completed goal did not preserve its verified inputs and newly exported deck");
          const auto* deck = std::get_if<kit::ArtifactRef>(&finished.evidence->sources.back().source);
          if (!deck) throw std::runtime_error("The final release evidence did not identify a presentation");
          screenshot_source = kit::SourceSlice{*deck, {PresentationScreenshotPart(5)}};
          if (LiveTake(co_await ppt->ReadSelectionAsync(*screenshot_source)) != PresentationScreenshotSelection)
            throw std::runtime_error("The screenshot selection exposed content outside the picture");
          const auto visible = ppt->Snapshot();
          for (const auto& message : visible->messages) {
            for (const auto& block : message.blocks) {
              const auto* reference = std::get_if<kit::ArtifactBlock>(&block.body);
              if (reference && reference->artifact == *deck) screenshot_review_key = "artifact-review/" + block.id.value;
            }
          }
          if (screenshot_review_key.empty()) throw std::runtime_error("The released deck had no artifact review entry");
          if (complete_automatically) awaiting_user = true;
        }
        while (!ui_finished) co_await Delay(25ms);
        if (screenshot_source && !complete_automatically) {
          const auto visible = ppt->Snapshot();
          const auto message = std::ranges::find_if(visible->messages, [](const auto& message) {
            return std::ranges::any_of(message.blocks, [](const auto& block) {
              const auto* text = std::get_if<kit::TextBlock>(&block.body);
              return text && text->text == screenshot_follow_up;
            });
          });
          if (message == visible->messages.end() || !message->run || message->run != visible->summary.active_run ||
              std::ranges::any_of(visible->queue, [&](const auto& item) { return item.run == message->run; }) ||
              std::ranges::count_if(message->blocks, [&](const auto& block) {
                const auto* conclusion = std::get_if<kit::SideChatConclusion>(&block.body);
                return conclusion && conclusion->text == PresentationScreenshotSelection &&
                    conclusion->sources == std::vector{*screenshot_source};
              }) != 1) throw std::runtime_error("Direct sending did not deliver the original versioned picture conclusion");
        }
        LiveTake(co_await runtime->CloseAsync());
      } catch (...) {
        failure = std::current_exception();
      }
      done = true;
    });
    const Application application{App, {.application_hooks = {std::move(hook)}}};
    UiTest preview(application, {.viewport = {1080.0F, 920.0F},
        .resource_provider = std::make_shared<PreviewTestResources>()});
    Wait(preview, [&] { return awaiting_user || done; }, "The completion approval did not become available", 80s);
    if (failure) std::rethrow_exception(failure);
    if (!reopen) {
      auto general = PendingConversation(preview, general_title);
      Wait(preview, [&] { return general.Exists() && general.One().in_viewport; }, "The original completion request was missing from the conversation list");
      general.Tap();
      TapFormButton(preview, "Goal");
      const auto evidence = preview.Find(UiSelector::Key("goal-evidence"));
      Wait(preview, [&] { return preview.Find(UiSelector::Text("10 slides")).Exists() &&
          preview.Find(UiSelector::Key("goal-run-status")).Exists() && evidence.Exists() &&
          evidence.Find(UiSelector::Text("Workspace verification v1")).Exists() &&
          preview.Find(ButtonText("Open completion approval")).Exists(); },
          "Goal did not show the completion review, run status and evidence version");
      TapFormButton(preview, "Open completion approval");
      Wait(preview, [&] {
        return std::ranges::any_of(preview.Find(UiSelector::Key(old_approval.id.value)).All(),
            [](const auto& item) { return item.in_viewport; }) &&
            preview.Find(ButtonText("Back to conversation")).Exists() && preview.Find(ButtonText("Approve")).Exists();
      }, "Goal did not open the exact completion request");
      TapFormButton(preview, "Back to conversation");
      TapFormButton(preview, "Goal");
      SetReleaseSlideCount(preview, "5", {"1 pending interactions will close.", "Evidence must be verified against the new version."},
          complete_automatically);
      if (preview.Find(ButtonText("Open completion approval")).Exists())
        throw std::runtime_error("The Goal panel still offered the retired approval");
      TapFormButton(preview, "Close goal");
      Wait(preview, [&] { return !preview.Find(ButtonText("Approve")).Exists(); }, "The old completion approval remained actionable");
    } else if (complete_automatically) {
      TapControl(preview, UiSelector::Text(general_title), "Open the automatically completed goal's conversation");
      TapFormButton(preview, "Goal");
      Wait(preview, [&] { return ShowsText(preview, "Completed") && ShowsText(preview, "Complete the goal as soon as its result passes verification."); },
          "The goal did not display automatic completion after restart");
      if (preview.Find(ButtonText("Open completion approval")).Exists())
        throw std::runtime_error("The completed goal exposed a confirmation action");
      ui_finished = true;
    } else {
      TapControl(preview, UiSelector::Key("pending-navigation"), "Open the pending requests");
      auto current = preview.Find(UiSelector::Key("pending/" + new_approval.value));
      Wait(preview, [&] { return current.Exists() && current.One().in_viewport; }, "The new completion review was missing from the pending list");
      if (preview.Find(UiSelector::Key("pending/" + old_approval.id.value)).Exists())
        throw std::runtime_error("The retired completion review returned in the pending list after restart");
      current.Tap();
      try {
        auto request = preview.Find(UiSelector::Key(new_approval.value));
        auto back = preview.Find(ButtonText("Back to conversation"));
        Wait(preview, [&] { return request.Exists() && back.Exists() && back.One().in_viewport; },
            "The new completion request did not open from the pending list");
        auto approve = request.Find(ButtonText("Approve"));
        Wait(preview, [&] { return request.Exists() && approve.Exists() && approve.One().enabled && approve.One().in_viewport; },
            "The new completion request had no actionable approval");
        approve.Tap();
        Wait(preview, [&] { return ShowsText(preview, "This request is no longer pending."); }, "The new completion review was not approved");
      } catch (...) {
        static_cast<void>(root.Child("completion-approval.snapshot").WriteString(preview.CaptureSnapshot().ToString()));
        throw;
      }
      TapFormButton(preview, "Back to conversation");
      TapFormButton(preview, "Goal");
      Wait(preview, [&] { return preview.Find(UiSelector::Text("Completed")).Exists(); }, "Goal did not show the approved revision as completed");
      Wait(preview, [&] { return done || !screenshot_review_key.empty(); }, "The released picture did not become selectable");
      if (failure) std::rethrow_exception(failure);
      TapFormButton(preview, "Close goal");
      preview.Find(UiSelector::Text(ppt_title)).Tap();
      const auto review = UiSelector::Key(screenshot_review_key);
      auto card = preview.Find(review);
      Wait(preview, [&] { return card.Exists(); }, "The completed presentation did not reopen for review");
      if (!card.One().in_viewport)
        card = preview.Find(UiSelector::Key("agent-chat-history")).ScrollUntil(review, {.step = {0.0F, -95.0F}});
      auto review_button = preview.FindSemantics(UiSemanticSelector::Identifier(screenshot_review_key));
      Wait(preview, [&] { return review_button.Count() == 1 && !review_button.One().offscreen; },
          "The completed presentation review was not actionable");
      review_button.PerformSemanticAction({SemanticActionKind::Activate, {}});
      auto picture = preview.Find(ButtonText("Select implementation screenshot"));
      Wait(preview, [&] { return ShowsText(preview, "1 / 5"); }, "The selected review did not open the approved five-slide deck");
      if (picture.Exists()) throw std::runtime_error("The screenshot appeared on the wrong page");
      for (int page = 1; page < 5; ++page) TapFormButton(preview, "Next slide");
      Wait(preview, [&] { return picture.Exists() && picture.One().in_viewport && preview.Find(UiSelector::Type<Image>()).Exists(); },
          "The fifth slide did not display its selectable embedded screenshot");
      picture.Tap();
      AskAboutSelection(preview, std::string(PresentationScreenshotSelection));
      auto attachment = preview.Find(ButtonText("Remove conclusion"));
      Wait(preview, [&] { return attachment.Exists() && !preview.Find(UiSelector::Key("send-mode")).Exists(); },
          "The screenshot conclusion did not reach the idle conversation draft");
      auto composer = preview.Find(UiSelector::Key("composer"));
      composer.Tap();
      composer.ReplaceText(screenshot_follow_up);
      TapFormButton(preview, "Send");
      Wait(preview, [&] { return !attachment.Exists() && preview.Find(UiSelector::Key("ppt-audience")).Exists(); },
          "Sending the conclusion did not start the next presentation task");
    }
    ui_finished = true;
    Wait(preview, [&] { return done; }, "The release completion revision did not finish", 80s);
    if (failure) std::rethrow_exception(failure);
  }
}

constexpr auto code_activity_prompt =
    "Set mode=enabled in config.txt and replace Disabled with Enabled in settings.html only.";
constexpr auto code_activity_conclusion = "Workspace changes applied and tested";
constexpr auto timeline_layout_prompt = "Inspect the workspace for the transcript layout check.";

class WorkspaceToolStream final : public kit::ModelTurnStream {
public:
  explicit WorkspaceToolStream(std::vector<kit::ModelEvent> events, std::shared_ptr<std::atomic<int>> released = {})
      : events_(std::move(events)), released_(std::move(released)) {}

  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    using Result = kit::AgentResult<std::optional<kit::ModelEvent>>;
    if (index_ == events_.size()) co_return Result::Success(std::nullopt);
    while (released_ && static_cast<std::size_t>(released_->load()) < index_) co_await Delay(2ms);
    co_return Result::Success(std::move(events_[index_++]));
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<kit::ModelEvent> events_;
  std::shared_ptr<std::atomic<int>> released_;
  std::size_t index_ = 0;
};

class WorkspaceToolModel final : public kit::ModelProvider {
public:
  explicit WorkspaceToolModel(std::shared_ptr<std::atomic<int>> released = {}) : released_(std::move(released)) {}

  kit::ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = true, .structured_output = true};
  }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    using Result = kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>;
    using nlohmann::json;
    using agentkit_preview::DecodeJson;
    using agentkit_preview::EncodeJson;
    std::vector<kit::ModelEvent> events;
    std::shared_ptr<std::atomic<int>> gate;
    auto finish = kit::ModelFinishReason::Completed;
    if (request.output_schema) {
      events.emplace_back(kit::ModelStructuredOutput{DecodeJson({{"route", "answer"}})});
    } else if (request.tools.empty()) {
      events.emplace_back(kit::ModelTextDelta{"Workspace tools unavailable."});
    } else {
      const std::array names{"list_files", "search_files", "read_files", "apply_patch"};
      if (request.tools.size() != names.size() || std::ranges::any_of(names, [&](const auto* name) {
            return std::ranges::find(request.tools, name, &kit::ModelToolSpec::name) == request.tools.end();
          })) co_return Result::Failure({.code = kit::ErrorCode::InvalidState, .message = "Unexpected workspace tool offer"});
      const std::array arguments{
          json{{"directory", ""}},
          json{{"directory", "src"}, {"query", "mode=disabled"}},
          json{{"paths", {"src/config.txt"}}},
          json{{"changes", json::array({{{"path", "src/config.txt"}, {"before", "mode=disabled\n"}, {"after", "mode=enabled\n"}}})}},
      };
      const std::array expected{
          json{{"entries", json::array({{{"directory", true}, {"path", "src"}}})}, {"truncated", false}},
          json{{"entries", json::array({{{"line", 1}, {"path", "src/config.txt"}}})}, {"truncated", false}},
          json{{"files", json::array({{{"content", "mode=disabled\n"}, {"path", "src/config.txt"}}})}},
      };
      std::size_t index = 0;
      for (const auto& item : request.items) {
        const auto* result = std::get_if<kit::ModelToolResult>(&item);
        if (!result) continue;
        if (result->error) co_return Result::Failure(*result->error);
        if (index < expected.size() && (!result->value || EncodeJson(*result->value) != expected[index]))
          co_return Result::Failure({.code = kit::ErrorCode::InvalidState, .message = "Unexpected workspace tool result"});
        ++index;
      }
      const bool layout = released_ && std::ranges::any_of(request.items, [](const auto& item) {
        const auto* message = std::get_if<kit::ModelContent>(&item);
        const auto* text = message ? std::get_if<std::string>(&message->body) : nullptr;
        return text && *text == timeline_layout_prompt;
      });
      if (index < (layout ? 3 : names.size())) {
        const auto call = "workspace-" + std::to_string(index);
        events.emplace_back(kit::ModelToolStarted{call, names[index]});
        events.emplace_back(kit::ModelToolArguments{call, arguments[index].dump()});
        events.emplace_back(kit::ModelToolFinished{call});
        finish = kit::ModelFinishReason::ToolCalls;
      } else if (layout) {
        events.emplace_back(kit::ModelTextDelta{"## Workspace overview\n\nThe workspace contains a mode setting."});
        for (int chunk = 1; chunk <= 2; ++chunk) {
          std::string body;
          for (int paragraph = 0; paragraph < 12; ++paragraph)
            body += "\n\nThe workspace setting is checked before entering the application.";
          body += "\n\nStream checkpoint " + std::to_string(chunk) + ".";
          events.emplace_back(kit::ModelTextDelta{std::move(body)});
        }
        gate = released_;
      } else {
        events.emplace_back(kit::ModelTextDelta{"Workspace changes verified."});
      }
    }
    events.emplace_back(kit::ModelFinished{finish, {}});
    co_return Result::Success(std::make_unique<WorkspaceToolStream>(std::move(events), std::move(gate)));
  }

private:
  std::shared_ptr<std::atomic<int>> released_;
};

void VerifyWorkspaceTools(const File& root) {
  const auto workspace = root.Child("workspace");
  const auto config = workspace.Child("src").Child("config.txt");
  if (!workspace.Child("src").CreateDirectories() ||
      !config.WriteString("mode=disabled\n"))
    throw std::runtime_error("Could not initialize the workspace tool fixture");
  const auto released = std::make_shared<std::atomic<int>>(0);
  kit::ModelRegistration model{{{"workspace-tools-fixture"}, 1}, {"fixture", "workspace-tools", "script", {}},
      std::make_shared<WorkspaceToolModel>(released)};
  const Application application{App, {.application_hooks = {
      MakeRunSettingsTestInstaller(root.Child("sessions.sqlite3"), workspace, {std::move(model)},
          [](auto started, auto, auto) -> Task<void> { LiveTake(std::move(started)); co_return; })}}};
  UiTest ui(application, {.viewport = {1120.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  const auto approve = ui.Find(ButtonText("Approve"));
  const auto exercise = [&](const char* prompt) {
    SendNewConversation(ui, prompt);
    Wait(ui, [&] { return approve.Exists() && approve.One().in_viewport; }, "Workspace patch did not request approval", 15s);
    if (ui.Find(UiSelector::Key("code-scope")).Exists())
      throw std::runtime_error("Workspace tools still required a file path form");
    CheckText(config, "mode=disabled\n");
    approve.Tap();
    Wait(ui, [&] { return ShowsText(ui, "Workspace changes verified."); }, "The assistant did not complete its workspace changes", 15s);
    CheckText(config, "mode=enabled\n");
  };
  exercise("Find config.txt and change mode from disabled to enabled.");
  for (const auto* assistant : {"Code", "PPT", "Design"}) {
    if (!config.WriteString("mode=disabled\n")) throw std::runtime_error("Could not reset the workspace");
    ui.Find(UiSelector::Key("new-conversation")).Tap();
    ChooseDraftAssistant(ui, assistant);
    exercise("Inspect the workspace and update the mode in config.txt.");
  }

  if (!config.WriteString("mode=disabled\n")) throw std::runtime_error("Could not reset the workspace");
  ui.Find(UiSelector::Key("new-conversation")).Tap();
  ChooseDraftAssistant(ui, "Code");
  SendNewConversation(ui, timeline_layout_prompt);
  const auto history = ui.Find(UiSelector::Key("agent-chat-history"));
  const auto read = ui.Find(UiSelector::Text("Read workspace files · Completed"));
  const auto heading = ui.Find(UiSelector::Text("Workspace overview"));
  Wait(ui, [&] { return read.Exists() && heading.Exists(); }, "The reading step and streamed heading did not mount");
  const auto gap = [&] {
    const auto step = read.One().bounds;
    return heading.One().bounds.y - step.y - step.height;
  };
  const auto initial_gap = gap();
  if (initial_gap < 0.0F || initial_gap > 32.0F)
    throw std::runtime_error("The timeline reserved space for folded tool output");
  released->store(1);
  Wait(ui, [&] { return ShowsText(ui, "Stream checkpoint 1."); }, "The first longer reply did not start streaming");
  history.ScrollBy({0.0F, -10000.0F});
  Wait(ui, [&] { return read.Exists() && read.One().in_viewport && heading.Exists() && heading.One().in_viewport; },
      "The start of the streamed reply was not reachable");
  if (std::abs(gap() - initial_gap) > 4.0F)
    throw std::runtime_error("Streaming moved the heading away from the preceding work step");
  const auto read_y = read.One().bounds.y;
  read.Tap();
  Wait(ui, [&] { return gap() > initial_gap + 6.0F; }, "The reading details did not expand under their heading");
  if (std::abs(read.One().bounds.y - read_y) > 4.0F)
    throw std::runtime_error("Expanding a work step moved its heading");
  const auto expanded_gap = gap();
  released->store(2);
  Wait(ui, [&] { return ShowsText(ui, "Stream checkpoint 2."); }, "The second longer reply did not render");
  ui.Pump(16ms);
  if (!read.One().in_viewport || std::abs(read.One().bounds.y - read_y) > 4.0F || std::abs(gap() - expanded_gap) > 4.0F)
    throw std::runtime_error("Streaming moved the expanded work step or introduced empty space");
  read.Tap();
  try {
    Wait(ui, [&] { return std::abs(gap() - initial_gap) <= 4.0F; }, "Collapsing the step left space behind");
  } catch (...) {
    static_cast<void>(root.Child("timeline-layout.snapshot").WriteString(ui.CaptureSnapshot().ToString()));
    throw std::runtime_error("Collapsing the step left space behind: collapsed " + std::to_string(initial_gap) +
        ", expanded " + std::to_string(expanded_gap) + ", after closing " + std::to_string(gap()));
  }
  if (std::abs(read.One().bounds.y - read_y) > 4.0F)
    throw std::runtime_error("Collapsing a work step moved its heading");
  ui.Find(UiSelector::Key("jump-to-latest")).Tap();
  Wait(ui, [&] { return ui.Find(UiSelector::Text("Stream checkpoint 2.")).One().in_viewport; },
      "Jump to latest did not return to the streaming reply");
  released->store(3);
  Wait(ui, [&] { return !ui.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "The layout check did not finish");
}

void VerifyLiveWorkspaceTools(const File& root) {
  const auto workspace = root.Child("workspace");
  const auto config = workspace.Child("config.txt");
  if (workspace.Exists() || root.Child("sessions.sqlite3").Exists() || !root.Child("models").Exists() ||
      !workspace.CreateDirectories() || !config.WriteString("mode=disabled\n"))
    throw std::runtime_error("Use a fresh live workspace fixture with saved model profiles");
  std::exception_ptr failure;
  bool finished = false;
  const Application application{[]() -> View { return Spacer(); },
      {.window = {.title = "GanWork Workspace Tools Acceptance", .activatable = false},
      .quit_on_last_window_closed = false,
      .application_hooks = {MakeConfiguredPreviewTestInstaller(root.Child("sessions.sqlite3"), workspace,
          File(AGENTKIT_PREVIEW_CTEST), [&](auto started, auto workspace_id, auto project) -> Task<void> {
            const auto app = UseApplication();
            try {
              auto runtime = LiveTake(std::move(started));
              if (!workspace_id) throw std::runtime_error("The fixture workspace is unavailable");
              for (const auto& agent : {agentkit_preview::GeneralAgent, agentkit_preview::CodeAgent,
                       agentkit_preview::PptAgent, agentkit_preview::DesignAgent}) {
                auto session = co_await LiveConversation(runtime, project, *workspace_id, agent,
                    "Workspace tools acceptance", "List the workspace files, search for mode=disabled, and read the matching "
                    "file. Change only that line to mode=enabled, keeping the final newline. Summarize the actual change.");
                auto subscription = co_await LiveObserve(session);
                const auto patch = co_await LiveInteraction(session, "Review workspace changes");
                CheckText(config, "mode=disabled\n");
                co_await LiveApprove(session, patch);
                co_await LiveRunCompleted(session);
                CheckText(config, "mode=enabled\n");
                const auto snapshot = session->Snapshot();
                for (const auto& id : agentkit_preview::WorkspaceModelTools(false)) {
                  if (std::ranges::none_of(snapshot->tools, [&](const auto& tool) {
                        return tool.tool.id == id && tool.state == kit::ToolCallState::Succeeded;
                      })) throw std::runtime_error(agent.value + ": the live model did not complete " + id.value);
                }
                std::cout << agent.value << " listed, searched, read and patched the workspace." << std::endl;
                WriteFixture(config, "mode=disabled\n");
              }
              LiveTake(co_await runtime->CloseAsync());
              finished = true;
            } catch (...) { failure = std::current_exception(); }
            app.Quit();
          })}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  if (!finished) throw std::runtime_error("The native workspace tool acceptance did not finish");
}

void VerifyLiveCodeTurnActivity(const File& root) {
  const auto workspace = root.Child("code");
  SeedLiveReleaseWorkspace(workspace);
  bool passed = false;
  auto drive = [workspace, &passed](auto started, auto workspace_id, auto project) -> Task<void> {
    const auto app = UseApplication();
    try {
      auto runtime = LiveTake(std::move(started));
      if (!workspace_id) throw std::runtime_error("The test workspace did not start");
      auto session = co_await LiveConversation(runtime, project, *workspace_id, agentkit_preview::CodeAgent,
          code_activity_prompt, std::string(code_activity_prompt) +
          " Before your first tool call, write one short visible sentence about the work you will do. "
          "Use read_files for both files, then apply_patch and run_tests. Summarize the outcome in your final answer.");
      auto watching = co_await LiveObserve(session);
      bool saw_model_activity = false;
      bool saw_completion = false;
      auto events = LiveTake(co_await session->ObserveEventsAsync(0, [&](auto result) {
        if (!result.Succeeded()) return;
        for (const auto& event : result.Value()->events) {
          if (const auto* run = std::get_if<kit::RunSnapshot>(&event.body)) {
            saw_model_activity |= run->model_active;
            saw_completion |= run->state == kit::RunState::Succeeded && run->timing && run->timing->ended_at &&
                run->result.has_value();
          }
        }
      }));
      const auto patch = co_await LiveInteraction(session, "Review workspace changes");
      CheckText(workspace.Child("config.txt"), "mode=disabled\n");
      CheckText(workspace.Child("settings.html"), live_settings_before);
      co_await Delay(1100ms);
      co_await LiveApprove(session, patch);
      const auto test = co_await LiveInteraction(session, "Run workspace tests");
      CheckText(workspace.Child("config.txt"), "mode=enabled\n");
      CheckText(workspace.Child("settings.html"), live_settings_after);
      if (workspace.Child("tested.txt").Exists()) throw std::runtime_error("CTest ran before its separate approval");
      co_await LiveApprove(session, test);
      co_await LiveRunCompleted(session, 90s);
      CheckText(workspace.Child("tested.txt"), "verified");
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!saw_completion && std::chrono::steady_clock::now() < deadline) co_await Delay(25ms);
      const auto snapshot = session->Snapshot();
      const auto& run = snapshot->runs.back();
      if (!saw_model_activity || !saw_completion || !run.timing || !run.timing->ended_at ||
          *run.timing->ended_at <= run.timing->started_at || run.timing->waiting_for_input < 1s)
        throw std::runtime_error("The native turn did not publish model activity, measured wait time and completion");
      const auto find_tool = [&](const kit::ToolId& id) -> const kit::ToolCallSnapshot& {
        const auto call = std::ranges::find_if(snapshot->tools, [&](const auto& item) {
          return item.tool.id == id && item.state == kit::ToolCallState::Succeeded;
        });
        if (call == snapshot->tools.end() || !call->registered_sequence || !call->output_message ||
            std::ranges::find(snapshot->messages, *call->output_message, &kit::Message::ref) == snapshot->messages.end())
          throw std::runtime_error("The timeline lost the position or output for " + id.value);
        return *call;
      };
      const auto& read = find_tool(agentkit_preview::ReadWorkspaceFiles);
      const auto& write = find_tool(agentkit_preview::ApplyWorkspacePatch);
      const auto& tests = find_tool(agentkit_preview::RunWorkspaceTests);
      const auto first_text = std::ranges::find(snapshot->messages, kit::MessageAuthor::Agent, &kit::Message::author);
      const auto last_text = std::ranges::find(snapshot->messages.rbegin(), snapshot->messages.rend(),
          kit::MessageAuthor::Agent, &kit::Message::author);
      if (first_text == snapshot->messages.end() || last_text == snapshot->messages.rend() ||
          first_text->sequence >= *read.registered_sequence || *read.registered_sequence >= *write.registered_sequence ||
          *write.registered_sequence >= *tests.registered_sequence || *tests.registered_sequence >= last_text->sequence)
        throw std::runtime_error("Real model text and tool activity did not form an ordered timeline");
      if (write.artifacts.size() != 1) throw std::runtime_error("The applied files did not record implementation evidence");
      const auto record = LiveTake(co_await runtime->Artifacts().ReadAsync(write.artifacts.front()));
      const auto evidence = workspace.Parent()->Child("workspace-evidence");
      const auto config_diff = LiveTake(co_await agentkit_preview::PreviewAppliedFile(evidence, record, *snapshot, {"config.txt"}));
      const auto settings_diff = LiveTake(co_await agentkit_preview::PreviewAppliedFile(evidence, record, *snapshot, {"settings.html"}));
      if (config_diff.find("-mode=disabled") == std::string::npos || config_diff.find("+mode=enabled") == std::string::npos ||
          settings_diff.find("Enabled") == std::string::npos)
        throw std::runtime_error("The per-file diffs did not match the applied changes");
      const auto command = std::ranges::find(snapshot->messages, *tests.output_message, &kit::Message::ref);
      if (std::ranges::none_of(command->blocks, [](const auto& block) {
            const auto* text = std::get_if<kit::TextBlock>(&block.body);
            return text && text->text.find("--output-on-failure") != std::string::npos;
          })) throw std::runtime_error("CTest did not record the executed command");
      std::cout << "Elapsed " << (*run.timing->ended_at - run.timing->started_at).count()
                << " ms. Verified the live timeline, two applied diffs and the CTest command.\n";
      LiveTake(co_await runtime->CloseAsync());
      passed = true;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    app.Quit();
  };
  const Application application{App, {
      .window = {.title = "GanWork Live Acceptance"},
      .application_hooks = {MakeConfiguredPreviewTestInstaller(root.Child("sessions.sqlite3"), workspace,
          File(AGENTKIT_PREVIEW_CTEST), std::move(drive))}}};
  static_cast<void>(RunApplication());
  if (!passed) throw std::runtime_error("The native code turn acceptance failed");
}

void CheckCodeTimelineDetails(UiTest& preview) {
  const auto tests = UiSelector::Text("Run workspace tests · Completed");
  Wait(preview, [&] { return ShowsText(preview, "Read workspace files · Completed") && preview.Find(tests).Exists(); },
      "The read and verification steps did not reopen in the conversation");
  TapControl(preview, ButtonText("View changes"), "The completed turn could not locate its changes");
  const auto first_diff = UiSelector::Key("workspace-file-diff/config.txt");
  const auto second_diff = UiSelector::Key("workspace-file-diff/settings.html");
  TapControl(preview, first_diff, "The first recorded file was not available", "agent-chat-history");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("workspace-applied-diff/config.txt")).Exists() &&
      ReviewedDiff(preview).find("+mode=enabled") != std::string::npos; },
      "The config diff did not show the recorded applied version");
  TapControl(preview, second_diff, "The second recorded file was not available", "agent-chat-history");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("workspace-applied-diff/settings.html")).Exists() &&
      ReviewedDiff(preview).find("Enabled") != std::string::npos; },
      "The settings diff did not show the recorded applied version");
  TapControl(preview, second_diff, "The settings diff could not be closed", "agent-chat-history");
  TapControl(preview, tests, "The verification step could not expand", "agent-chat-history");
  Wait(preview, [&] { return preview.Find(UiSelector::Key("ctest-command")).Exists() &&
      preview.Find(UiSelector::Key("ctest-directory")).Exists() && preview.Find(UiSelector::Key("ctest-output")).Exists(); },
      "Expanding the verification step did not restore the command, directory and output");
  TapControl(preview, tests, "The verification step could not collapse", "agent-chat-history", -96.0F);
  Wait(preview, [&] { return !preview.Find(UiSelector::Key("ctest-output")).Exists(); }, "CTest output did not collapse");
}

void VerifyReopenedCodeTimeline(const File& root) {
  const Application application{App, {.application_hooks = {MakeConfiguredPreviewTestInstaller(
      root.Child("sessions.sqlite3"), root.Child("code"), File(AGENTKIT_PREVIEW_CTEST), {})}}};
  UiTest preview(application, {.viewport = {1180.0F, 1000.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  const auto entry = ConversationRow(preview, code_activity_prompt);
  Wait(preview, [&] { return entry.Exists() && entry.One().in_viewport; }, "The live conversation was not restored from SQLite");
  entry.Tap();
  CheckCodeTimelineDetails(preview);
  std::cout << "Reopened the live timeline, both applied diffs and the CTest output from SQLite.\n";
}

void VerifyCodeTurnActivity(const File& root) {
  const auto workspace = root.Child("code");
  SeedReleaseTestWorkspace(workspace, WorkspaceTestScript("checked> Enabled", true));
  const auto database = root.Child("sessions.sqlite3");
  kit::ModelRegistration fixture{{{"preview.activity-code"}, 1}, {"preview", "code-fixture", "script", {}},
      std::make_shared<PatchModel>(false, true)};
  const Application application{App, {.application_hooks = {
      MakeCodeTestInstaller(database, workspace, std::move(fixture), File(AGENTKIT_PREVIEW_CTEST))}}};
  constexpr auto reasoning = "Check the two approved files before proposing the edit.";
  const auto status = UiSelector::Key("chat-assistant-status");
  const auto elapsed = UiSelector::Key("chat-run-time");
  const auto work = UiSelector::Key("chat-run-activity");
  const auto summary = UiSelector::Key("chat-reasoning-toggle");
  const UiTestOptions options{.viewport = {1180.0F, 1000.0F}, .resource_provider = std::make_shared<PreviewTestResources>()};
  const auto wait = [&](UiTest& preview, const std::function<bool()>& ready, const char* message,
                        std::chrono::seconds timeout = 8s) {
    try { Wait(preview, ready, message, timeout); }
    catch (const std::exception&) {
      static_cast<void>(root.Child("activity.snapshot").WriteString(preview.CaptureSnapshot().ToString()));
      throw;
    }
  };
  std::string recorded_elapsed;
  {
    UiTest preview(application, options);
    ChooseDraftAssistant(preview, "Code");
    SendNewConversation(preview, code_activity_prompt);
    wait(preview, [&] { return preview.Find(status).Exists() && preview.Find(elapsed).Exists(); },
        "The assistant did not show a state and timer before model output");
    ShareWorkspaceScope(preview);
    wait(preview, [&] { return preview.Find(status).Exists() && preview.Find(status).One().text == "Waiting for approval" &&
        preview.Find(UiSelector::Text("Review workspace changes")).Exists(); },
        "The assistant did not show the pending workspace write", 90s);
    CheckText(workspace.Child("config.txt"), "mode=disabled\n");
    std::this_thread::sleep_for(1100ms);
    preview.Find(ButtonText("Approve")).Tap();
    wait(preview, [&] { return preview.Find(UiSelector::Text("Run workspace tests")).Exists() &&
        preview.Find(ButtonText("Approve")).Exists(); }, "The approved patch did not reach test approval", 25s);
    CheckText(workspace.Child("config.txt"), "mode=enabled\n");
    CheckText(workspace.Child("settings.html"), SettingsPage(true));
    if (workspace.Child("tested.txt").Exists()) throw std::runtime_error("The test ran without its separate approval");
    preview.Find(ButtonText("Approve")).Tap();
    wait(preview, [&] { return preview.Find(status).Exists() &&
        preview.Find(status).One().text == "Completed" && ShowsText(preview, code_activity_conclusion); },
        "The code turn did not settle with its conclusion", 25s);
    recorded_elapsed = preview.Find(elapsed).One().text.value_or("");
    if (!recorded_elapsed.starts_with("Elapsed ") || recorded_elapsed == "Elapsed 0m 0s")
      throw std::runtime_error("The code turn did not retain a measured elapsed time");
    if (!ShowsText(preview, "Read workspace files · Completed") || preview.Find(UiSelector::Key("ctest-output")).Exists())
      throw std::runtime_error("A completed turn did not retain compact tool steps");
    CheckText(workspace.Child("tested.txt"), "verified");
  }
  {
    UiTest preview(application, options);
    auto entry = ConversationRow(preview, code_activity_prompt);
    wait(preview, [&] { return entry.Exists() && entry.One().in_viewport; }, "The completed code conversation was not restored");
    entry.Tap();
    wait(preview, [&] { return preview.Find(elapsed).Exists() && preview.Find(elapsed).One().text == recorded_elapsed &&
        ShowsText(preview, code_activity_conclusion); }, "The elapsed time or conclusion changed after restart");
    CheckCodeTimelineDetails(preview);
    TapControl(preview, work, "The run details could not expand", "agent-chat-history");
    if (preview.Find(UiSelector::Text(reasoning)).Exists()) throw std::runtime_error("The reasoning summary opened by default");
    preview.Find(summary).Tap();
    wait(preview, [&] { return preview.Find(UiSelector::Text(reasoning)).Exists(); }, "The public reasoning summary did not reopen");
    preview.Find(summary).Tap();
    wait(preview, [&] { return !preview.Find(UiSelector::Text(reasoning)).Exists(); }, "The reasoning summary did not collapse");
    std::cout << recorded_elapsed << ". " << code_activity_conclusion << ".\n";
  }
}

void CodeRoundTrip(const File& database, const File& workspace, const File& logs, const File& release) {
  SeedReleaseTestWorkspace(workspace, WorkspaceTestScript("checked> Enabled", true));
  const auto config = workspace.Child("config.txt");
  const auto settings = workspace.Child("settings.html");
  const auto completed = workspace.Child("tested.txt");
  kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}},
                               std::make_shared<PatchModel>()};
  Application application{App, {.application_hooks = {MakeCodeTestInstaller(database, workspace, model,
                                                     File(AGENTKIT_PREVIEW_CTEST))}}};
  {
    UiTest preview(application, {.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    PrepareCodeChange(preview, "Enable product mode");
    CheckText(config, "mode=disabled\n");
    CheckText(settings, SettingsPage(false));
  }
  {
    UiTest preview(application, {.viewport = {1080.0F, 880.0F}});
    auto entry = PendingConversation(preview, "Enable product mode");
    Wait(preview, [&] { return entry.Exists(); }, "Code patch approval did not survive restart");
    entry.Tap();
    auto approve = preview.Find(ButtonText("Approve"));
    Wait(preview, [&] { return approve.Exists(); }, "Missing patch approval after restart");
    TapReviewCard(preview, "Review attachment 1");
    auto diff = preview.Find(UiSelector::Key("artifact-text"));
    Wait(preview, [&] { return diff.Exists() && !ReviewedDiff(preview).empty(); }, "The durable diff did not open");
    const std::string lines = "-mode=disabled\n+mode=enabled";
    SelectDiffExcerpt(preview, diff, lines);
    AskAboutSelection(preview, lines);
    CheckReturnedConclusion(preview, lines);
    CheckText(config, "mode=disabled\n");
    approve.Tap();
    Wait(preview, [&] { return preview.Find(UiSelector::Text("Run workspace tests")).Exists(); },
         "The approved patch did not reach test approval");
    CheckText(config, "mode=enabled\n");
    CheckText(settings, SettingsPage(true));
    if (completed.Exists()) throw std::runtime_error("Tests ran before process approval");
    approve.Tap();
    Wait(preview, [&] { return completed.Exists() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
         "The approved test process did not settle");
    CheckText(completed, "verified");
    bool has_result = false;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(logs.Path()))) {
      auto text = File(entry.path().u8string()).ReadString();
      has_result |= text.Succeeded() && text.Value().find("100% tests passed") != std::string::npos;
    }
    if (!has_result) throw std::runtime_error("The test process did not report success");
  }
  WriteFixture(config, "mode=disabled\n");
  WriteFixture(settings, SettingsPage(false));
  {
    UiTest preview(application, {.viewport = {1080.0F, 880.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    PrepareCodeChange(preview, "Enable mode after review");
    WriteFixture(settings, "User edited after review\n");
    preview.Find(ButtonText("Approve")).Tap();
    Wait(preview, [&] { return !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); }, "A stale patch did not settle");
    CheckText(config, "mode=disabled\n");
    CheckText(settings, "User edited after review\n");
  }
#if defined(AGENTKIT_PREVIEW_BROWSER)
  WriteFixture(settings, SettingsPage(false));
  if (!std::filesystem::remove(std::filesystem::u8path(completed.Path()))) throw std::runtime_error("Could not reset test marker");
  const auto old_release = release.Parent()->Child("WorkspaceReleaseBeforeEdit.pptx");
  StartWorkspaceRelease(application, config, settings, "Workspace release: Ship settings", "10");
  ReviseWorkspaceRelease(application, completed, old_release);
  const auto verified_at = std::filesystem::last_write_time(std::filesystem::u8path(completed.Path()));
  FinishWorkspaceRelease(application, completed, release, old_release);
  if (verified_at != std::filesystem::last_write_time(std::filesystem::u8path(completed.Path()))) {
    throw std::runtime_error("Revising the slide count repeated the approved workspace tests");
  }
#else
  std::cout << "Browser unavailable. The release screenshot fixture was skipped.\n";
#endif
}

void VerifySharedReports(const File& root) {
  using namespace agentkit_preview;
  using Value = kit::JsonValue;
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not create the shared report fixture");
  const std::string original = "One verification case passed.";
  const std::string replacement = "Two verification cases passed.";
  kit::SessionId manager;
  kit::SharedContextRef report;
  std::shared_ptr<kit::Conversation> ppt;
  std::optional<kit::InteractionSnapshot> first_approval;
  std::optional<kit::InteractionSnapshot> second_approval;
  std::exception_ptr failure;
  bool verified = false;
  const auto hook = MakeRunSettingsTestInstaller(root.Child("sessions.sqlite3"), workspace, {},
      [&](auto started, std::optional<kit::WorkspaceId> workspace_id, kit::ProjectId project) -> Task<void> {
        try {
          auto runtime = LiveTake(std::move(started));
          auto projects = runtime->Projects();
          const auto publish = [&](std::string title, std::string summary) -> Task<kit::SharedContextRef> {
            auto snapshot = LiveTake(co_await projects.ReadAsync(project));
            kit::ProposeSharedContext draft{snapshot.revision, {.title = std::move(title),
                .content = Value{Value::Object{{"summary", Value{std::move(summary)}}}},
                .access_profile = "preview.imported-presentation-report"}};
            auto receipt = LiveTake(co_await projects.UpdateContextAsync(project, kit::NewCommandId(), std::move(draft)));
            auto ref = std::get<kit::SharedContextRef>(receipt.outcome);
            kit::SharedContextCommand review = kit::ChangeSharedContext{ref, kit::RequestContextReview{}};
            LiveTake(co_await projects.UpdateContextAsync(project, kit::NewCommandId(), std::move(review)));
            kit::SharedContextCommand commit = kit::ChangeSharedContext{ref, kit::PublishContext{}};
            LiveTake(co_await projects.UpdateContextAsync(project, kit::NewCommandId(), std::move(commit)));
            co_return ref;
          };
          report = co_await publish("Verification report", original);
          auto alternative = co_await publish("Alternative report", "Another verification conclusion.");
          const auto curator = co_await LiveConversation(runtime, project, workspace_id.value(), GeneralAgent, "Manage shared reports",
              "Describe the project");
          manager = curator->Snapshot()->summary.id;
          kit::CreateSessionRequest request{.options = {.agent = PptAgent, .project = project, .workspace = workspace_id,
              .title = "Slides using the shared report"}, .initial = kit::SendInput{.text = "Prepare a release report",
              .context = {{report}}}};
          auto created = std::get<kit::SessionCreated>(LiveTake(co_await runtime->Sessions().CreateAsync(
              kit::NewCommandId(), std::move(request))).outcome);
          ppt = LiveTake(co_await runtime->Sessions().OpenAsync(created.session));
          auto observed = co_await LiveObserve(ppt);
          auto export_request = co_await StageReviewPresentation(ppt, "SharedReport.pptx", original);
          co_await LiveApprove(ppt, export_request);
          co_await LiveRunCompleted(ppt);
          const auto baseline = ppt->Snapshot()->artifacts.at(0);
          const auto prepare = [&](kit::SharedContextRef next) -> Task<kit::InteractionSnapshot> {
            const auto runs = ppt->Snapshot()->runs.size();
            kit::ReworkRequest change{.session = created.session, .baseline = baseline, .parts = {{"slide-3"}},
                .replacements = {{{report}, {std::move(next)}}}};
            LiveTake(co_await runtime->Artifacts().PrepareReworkAsync(kit::NewCommandId(), std::move(change)));
            const auto end = std::chrono::steady_clock::now() + 20s;
            while (ppt->Snapshot()->runs.size() == runs && std::chrono::steady_clock::now() < end) co_await Delay(25ms);
            if (ppt->Snapshot()->runs.size() == runs) throw std::runtime_error("Slide revision did not start");
            co_return co_await LiveInteraction(ppt, "Review PowerPoint rework");
          };
          const auto check_invalidation = [&](const kit::InteractionSnapshot& previous) -> Task<void> {
            const auto end = std::chrono::steady_clock::now() + 45s;
            while (ppt->Snapshot()->summary.active_run && std::chrono::steady_clock::now() < end) co_await Delay(25ms);
            const auto snapshot = ppt->Snapshot();
            const auto approval = std::ranges::find(snapshot->interactions, previous.id, &kit::InteractionSnapshot::id);
            if (snapshot->summary.active_run || approval == snapshot->interactions.end() ||
                approval->state != kit::InteractionState::Invalidated || snapshot->tools.back().state != kit::ToolCallState::Conflict ||
                snapshot->artifacts != std::vector{baseline}) {
              throw std::runtime_error("A shared report change did not invalidate the pending slide revision");
            }
            auto retry = co_await ppt->SubmitAsync(kit::NewCommandId(), {previous.id, previous.version, kit::ApprovalDecision::Approve});
            if (retry.Succeeded()) throw std::runtime_error("The superseded slide revision could still be approved");
          };
          first_approval = co_await prepare(alternative);
          co_await check_invalidation(*first_approval);
          kit::SharedContextRef version = report;
          ++version.version;
          auto published = LiveTake(co_await projects.ReadContextAsync(version));
          const auto revised = ReadImportedReport(published);
          if (!revised || revised->summary != replacement)
            throw std::runtime_error("The newly published verification did not match the reviewed summary");
          second_approval = co_await prepare(version);
          co_await check_invalidation(*second_approval);
          auto withdrawn = co_await projects.ReadContextAsync(version);
          if (withdrawn.Succeeded() || withdrawn.Error().code != kit::ErrorCode::NotFound)
            throw std::runtime_error("The withdrawn report could still be read");
          verified = true;
        } catch (...) { failure = std::current_exception(); }
      });
  const Application app{App, {.application_hooks = {hook}}};
  UiTest ui(app, {.viewport = {1120.0F, 940.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
  const auto wait = [&](const auto& condition, const char* message) {
    Wait(ui, [&] { return failure || condition(); }, message, 25s);
    if (failure) std::rethrow_exception(failure);
  };
  wait([&] { return first_approval.has_value(); }, "The first slide revision did not reach approval");
  TapControl(ui, UiSelector::Key("session-actions/" + manager.value), "Open the manager conversation menu");
  auto menu = ui.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::MenuItem),
      UiSemanticSelector::Label("Shared reports")));
  wait([&] { return menu.Exists(); }, "The conversation menu did not offer shared reports");
  menu.PerformSemanticAction({SemanticActionKind::Activate, {}});
  const auto select = [&](int version) { return UiSelector::Key("shared-report/" + report.item.value + "/" + std::to_string(version)); };
  TapControl(ui, select(1), "Open the original report");
  const auto edit = [&](const std::string& summary) {
    TapFormButton(ui, "New version");
    auto input = ui.Find(UiSelector::Key("shared-report-summary"));
    wait([&] { return input.Exists(); }, "The report summary editor did not open");
    input.Tap();
    input.ReplaceText(summary);
    TapFormButton(ui, "Save draft");
  };
  const auto still_pending = [&] {
    const auto snapshot = ppt->Snapshot();
    const auto approval = std::ranges::find(snapshot->interactions, first_approval->id, &kit::InteractionSnapshot::id);
    if (approval == snapshot->interactions.end() || approval->state != kit::InteractionState::Pending)
      throw std::runtime_error("An unpublished replacement invalidated the original slide approval");
  };
  edit(replacement);
  wait([&] { return ui.Find(ButtonText("Submit for review")).Exists(); }, "The new report did not remain a draft");
  still_pending();
  TapFormButton(ui, "Submit for review");
  wait([&] { return ui.Find(ButtonText("Publish version")).Exists(); }, "The draft did not enter review");
  still_pending();
  TapFormButton(ui, "Publish version");
  wait([&] { return second_approval.has_value(); }, "The published replacement could not be used for a new slide revision");
  TapControl(ui, select(2), "Select the newly published report");
  TapFormButton(ui, "Withdraw version");
  TapFormButton(ui, "Confirm withdrawal");
  wait([&] { return verified && !ui.Find(select(2)).Exists(); }, "The withdrawn report still appeared as usable");
  TapControl(ui, select(1), "Return to the retained published version");
  edit("Three verification cases passed.");
  wait([&] { return ui.Find(select(3)).Exists(); }, "A withdrawn version blocked editing or its version number was reused");
}

struct PresentationReworkFixture {
  File root;
  bool adoption_race = false;
  std::shared_ptr<kit::AgentRuntime> runtime;
  std::string report_button;
  std::shared_ptr<bool> deck_visible = std::make_shared<bool>(true);
  std::optional<agentkit_preview::PresentationLocation> whole_impact;
  std::optional<agentkit_preview::PresentationLocation> opened_impact;
  bool whole_review_checked = false;
  bool verified_before_approval = false;
  bool export_restored = false;
  bool complete = false;
  std::exception_ptr failure;
};

Task<std::shared_ptr<kit::AgentRuntime>> OpenPresentationReworkRuntime(
    File root, agentkit_preview::PresentationFiles files, std::shared_ptr<bool> deck_visible) {
  using namespace agentkit_preview;
  using Value = kit::JsonValue;
  const auto reports = root.Child("workspace-evidence");
  auto store = LiveTake(co_await agentkit_host::OpenSqliteSessionStoreAsync(root.Child("sessions.sqlite3")));
  auto weak = std::make_shared<std::weak_ptr<kit::AgentRuntime>>();
  auto runtime = LiveTake(kit::AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"preview.rework"}); },
      .access = [deck_visible](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const bool deck = std::ranges::any_of(request.sources, [](const auto& input) {
              const auto* ref = std::get_if<kit::ArtifactRef>(&input.source);
              return ref && ref->id.value.starts_with("presentation:");
            });
        if (deck && (request.capability == kit::Capability::SendToModel ||
            (request.capability == kit::Capability::ReadArtifact && !*deck_visible))) {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::Deny, "PowerPoint stays local"});
        }
        const auto approval = request.capability == kit::Capability::ReworkArtifact ||
            (request.capability == kit::Capability::ExecuteTool && request.tool && request.tool->id == ExportPresentation);
        co_return kit::AgentResult<kit::PolicyDecision>::Success(
            {approval ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow, {}});
      }, .tool_resources = PresentationResources(files, weak),
      .project = [](kit::ProjectId project) -> kit::Async<kit::ProjectBinding> {
        co_return kit::AgentResult<kit::ProjectBinding>::Success({std::move(project), {}});
      }, .artifact_store = WithWorkspaceEvidence(PresentationArtifactStore(files), reports),
      .changes = PresentationChanges(files, weak), .store = std::move(store)}));
  *weak = runtime;
  LiveTake(runtime->RegisterModel({{{"ppt.rework.model"}, 1}, {"test", "local", "script", {}}, std::make_shared<PatchModel>()}));
  LiveTake(RegisterPresentationTools(runtime, files));
  const kit::ToolId publish_report{"ppt.rework.report"};
  LiveTake(runtime->RegisterTool({.id = publish_report, .version = 1, .description = "Publish a verification report",
      .input_schema = {Value{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.preview = {.title = "Publish verification"},
            .execution_data = std::move(request.arguments)});
      },
      .execute = [reports](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        using Result = kit::AgentResult<kit::ToolExecutionResult>;
        auto report = co_await StageWorkspaceEvidence(reports, context.CallId(), std::string(VerificationEvidence),
            "Verification report", Value{Value::Object{{"summary", call.change.execution_data},
                                                       {"kind", Value{std::string(VerificationEvidence)}}}});
        if (!report.Succeeded()) co_return Result::Failure(report.Error());
        std::vector<kit::ArtifactDraft> saved;
        saved.push_back(std::move(report).Value());
        co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(saved)});
      }}));
  LiveTake(runtime->RegisterAgent({.definition = {.ref = {PptAgent, 1}, .display_name = "PowerPoint",
      .model = {"ppt.rework.model"}, .tools = {publish_report, GeneratePresentation, ExportPresentation}},
      .run = [publish_report, directory = files.exports.Path()](kit::RunContext context, kit::RunInput input)
          -> kit::Async<kit::RunCompletion> {
        if (input.artifact_action)
          co_return co_await RunPresentation(std::move(context), std::move(input), {}, directory);
        kit::ToolRequest tool;
        constexpr std::string_view variant = "Stage deck: ";
        if (input.input.text == "Stage deck" || input.input.text.starts_with(variant)) {
          auto title = input.input.text.starts_with(variant) ? input.input.text.substr(variant.size()) : "Release";
          PresentationOutline outline{std::move(title), {{"Overview", {"One verification case passed."}},
              {"Results", {"One verification case passed.", "Keep this observation."}}}};
          tool = {GeneratePresentation, WritePresentationOutline(outline)};
        } else {
          tool = {publish_report, Value{input.input.text}};
        }
        auto result = LiveTake(co_await context.CallToolAsync(std::move(tool)));
        if (result.outcome != kit::ToolOutcome::Succeeded)
          co_return kit::AgentResult<kit::RunCompletion>::Failure(*result.reason);
        co_return kit::AgentResult<kit::RunCompletion>::Success({.artifacts = std::move(result.published_artifacts)});
      },
      .restore = [directory = files.exports.Path()](kit::RunContext context, kit::RunInput input, kit::RunResume resume) {
        return RunPresentation(std::move(context), std::move(input), std::move(resume.checkpoint), directory);
      }}));
  LiveTake(co_await runtime->StartAsync());
  co_return runtime;
}

Task<agentkit_preview::PresentationLocation> VerifyCompetingPresentationAdoption(
    std::shared_ptr<PresentationReworkFixture> fixture, agentkit_preview::PresentationFiles files,
    std::shared_ptr<kit::AgentRuntime> runtime, std::shared_ptr<kit::Conversation> chat,
    kit::ArtifactRef baseline, kit::ArtifactRef old_report, kit::ArtifactRef new_report) {
  using namespace agentkit_preview;
  const auto original = chat->Snapshot();
  const auto session = original->summary.id;
  auto comparisons = runtime->ForkReviews();
  kit::ForkReviewRequest comparison{.from = {session, original->runs.back().id, original->through_sequence},
      .project = *original->summary.project,
      .candidates = {{{.agent = PptAgent}, {}, {.text = "Stage deck: Executive rollout"}},
          {{.agent = PptAgent}, {}, {.text = "Stage deck: Field rollout"}}},
      .comparison_criteria = kit::JsonValue{std::string("Adoption against a concurrent slide revision")}};
  const auto created = std::get<kit::ForkReviewCreated>(LiveTake(co_await comparisons.CreateComparisonAsync(
      kit::NewCommandId(), std::move(comparison))).outcome);
  std::shared_ptr<const kit::ForkReviewSnapshot> view;
  auto observing = LiveTake(co_await comparisons.ObserveComparisonAsync(created.review, [&](auto update) {
    if (update.Succeeded()) view = std::move(update).Value();
  }));
  const auto wait_until = [](auto ready, const char* message) -> Task<void> {
    const auto end = std::chrono::steady_clock::now() + 20s;
    while (!ready() && std::chrono::steady_clock::now() < end) co_await Delay(25ms);
    if (!ready()) throw std::runtime_error(message);
  };
  co_await wait_until([&] { return view && view->state == kit::ForkReviewState::Ready; }, "Deck candidates did not settle");
  const auto chosen = view->candidates.front();
  if (chosen.state != kit::CandidateState::Ready || chosen.artifacts.size() != 1)
    throw std::runtime_error(chosen.reason ? chosen.reason->message : "The selected deck was not staged");
  const auto candidate = chosen.artifacts.front();
  const auto selected_bytes = LiveTake(co_await ReadPresentationBytes(runtime, candidate));
  const auto original_bytes = LiveTake(co_await ReadPresentationBytes(runtime, baseline));

  kit::ReworkRequest rework{.session = session, .baseline = baseline, .parts = {{"slide-2"}},
      .replacements = {{{old_report}, {new_report}}}};
  LiveTake(co_await runtime->Artifacts().PrepareReworkAsync(kit::NewCommandId(), std::move(rework)));
  co_await wait_until([&] { return chat->Snapshot()->runs.size() > original->runs.size(); }, "The competing rework did not start");
  const auto review = co_await LiveInteraction(chat, "Review PowerPoint rework");
  kit::AdoptCandidateRequest adoption{created.review, view->revision, chosen.id, candidate, session, {},
      {PresentationTargetBaseline(baseline)}};
  const auto command = kit::NewCommandId();
  const auto queued = LiveTake(co_await comparisons.PrepareAdoptionAsync(command, adoption));
  const auto item = std::get<kit::ChangePlanAccepted>(queued.outcome).queue_item;
  auto pending = LiveTake(co_await comparisons.ReadComparisonAsync(created.review));
  if (!item || pending.adoption_state != kit::ToolCallState::Preparing || !pending.adoption_call ||
      chat->Snapshot()->artifacts != std::vector{baseline}) throw std::runtime_error("The adoption was not queued against v1");
  const auto old_call = *pending.adoption_call;
  const auto stale_file = files.staging.Child("adopted-" + old_call.value + ".pptx");
  co_await LiveApprove(chat, review);
  co_await wait_until([&] { return view && view->revision > pending.revision && view->adoption_call == old_call &&
      view->adoption_state && view->adoption_state != kit::ToolCallState::Preparing &&
      !chat->Snapshot()->summary.active_run; }, "The stale adoption did not settle after the competing rework");

  const kit::ArtifactRef revised{baseline.id, baseline.version + 1};
  const auto state = chat->Snapshot();
  const auto progress = LiveTake(co_await runtime->Commands().ReadAsync(command));
  const auto rejected = std::ranges::find(state->queue, *item, &kit::QueuedInput::id);
  if (progress.state != kit::CommandState::Failed || !progress.failure || progress.failure->code != kit::ErrorCode::Conflict ||
      rejected == state->queue.end() || rejected->state != kit::QueueItemState::Rejected || rejected->run ||
      std::ranges::find(state->tools, old_call, &kit::ToolCallSnapshot::id) != state->tools.end() || stale_file.Exists() ||
      state->artifacts != std::vector{baseline, revised}) throw std::runtime_error("The stale adoption reached execution or lost its version conflict");
  if (view->adoption_state != kit::ToolCallState::Conflict)
    throw std::runtime_error("The comparison concealed the queued baseline conflict");
  const auto revised_bytes = LiveTake(co_await ReadPresentationBytes(runtime, revised));
  if (revised_bytes == original_bytes || revised_bytes == selected_bytes)
    throw std::runtime_error("The competing rework did not publish its distinct PowerPoint result");

  observing.Reset();
  LiveTake(co_await runtime->CloseAsync());
  runtime = co_await OpenPresentationReworkRuntime(fixture->root, files, fixture->deck_visible);
  comparisons = runtime->ForkReviews();
  chat = LiveTake(co_await runtime->Sessions().OpenAsync(session));
  auto session_changes = co_await LiveObserve(chat);
  const auto restored = LiveTake(co_await comparisons.ReadComparisonAsync(created.review));
  const auto replay = LiveTake(co_await comparisons.PrepareAdoptionAsync(command, adoption));
  if (restored.adoption_state != kit::ToolCallState::Conflict || restored.adoption_call != old_call ||
      replay.stamp.commit != queued.stamp.commit || std::get<kit::ChangePlanAccepted>(replay.outcome).queue_item != item)
    throw std::runtime_error("Restart lost the rejected adoption or issued a duplicate");

  adoption.expected_review = restored.revision;
  const auto stale = co_await comparisons.PrepareAdoptionAsync(kit::NewCommandId(), adoption);
  if (stale.Succeeded() || stale.Error().code != kit::ErrorCode::Conflict)
    throw std::runtime_error("A new command accepted the original baseline");
  adoption.targets = {PresentationTargetBaseline(revised)};
  const auto run_count = chat->Snapshot()->runs.size();
  LiveTake(co_await comparisons.PrepareAdoptionAsync(kit::NewCommandId(), adoption));
  co_await wait_until([&] { return chat->Snapshot()->runs.size() > run_count; }, "Fresh adoption did not start");
  const auto approval = co_await LiveInteraction(chat, "Review PowerPoint adoption");
  co_await wait_until([&] {
    const auto snapshot = chat->Snapshot();
    const auto visible = std::ranges::find(snapshot->interactions, approval.id, &kit::InteractionSnapshot::id);
    if (visible == snapshot->interactions.end() || visible->version != approval.version) return false;
    return std::get<kit::ToolApproval>(visible->body).preview.artifacts == std::vector{revised, candidate};
  }, "Fresh approval did not expose the revised deck and selected candidate");
  if (chat->Snapshot()->artifacts != std::vector{baseline, revised})
    throw std::runtime_error("The fresh adoption changed the deck before approval");
  co_await LiveApprove(chat, approval);
  co_await LiveRunCompleted(chat);
  const kit::ArtifactRef adopted{baseline.id, revised.version + 1};
  const auto applied = LiveTake(co_await ReadPresentationBytes(runtime, adopted));
  const auto preserved = LiveTake(co_await ReadPresentationBytes(runtime, revised));
  if (applied != selected_bytes || preserved != revised_bytes || stale_file.Exists())
    throw std::runtime_error("Adoption overwrote the reworked version or published a different candidate");
  kit::ArtifactImpactQuery query{*original->summary.project, {{{old_report}, {}, kit::SourceChangeKind::Modified}}};
  const auto impact = LiveTake(co_await runtime->Artifacts().InspectImpactAsync(std::move(query)));
  const auto affected = std::ranges::find(impact.affected, adopted, &kit::ImpactItem::affected);
  if (affected == impact.affected.end() || !affected->approximate || !affected->parts.empty())
    throw std::runtime_error("The original report did not have a whole-presentation impact on the adopted deck");
  fixture->runtime = std::move(runtime);
  co_return PresentationLocation{session, adopted};
}

Task<void> ExercisePresentationRework(File root, std::shared_ptr<PresentationReworkFixture> fixture,
    State<std::shared_ptr<kit::Conversation>> displayed) {
  using namespace agentkit_preview;
  const PresentationFiles files{root.Child("presentations"), root.Child("exports")};
  if (!co_await files.exports.CreateDirectoriesAsync()) throw std::runtime_error("Could not create exports directory");
  auto runtime = co_await OpenPresentationReworkRuntime(root, files, fixture->deck_visible);
  const auto open = [runtime](kit::ProjectId project, std::string title) -> Task<std::shared_ptr<kit::Conversation>> {
    kit::CreateSessionRequest create{.options = {.agent = PptAgent, .project = std::move(project), .title = std::move(title)}};
    const auto created = std::get<kit::SessionCreated>(
        LiveTake(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(create))).outcome);
    co_return LiveTake(co_await runtime->Sessions().OpenAsync(created.session));
  };
  auto chat = co_await open({"release"}, "Release review");
  auto observer = co_await LiveObserve(chat);
  auto original_verification = co_await open({"release"}, "Original verification");
  auto verification = co_await open({"release"}, "Release verification");
  auto unrelated = co_await open({"other"}, "Unrelated verification");
  const auto stage = [](std::shared_ptr<kit::Conversation> chat, kit::SendInput input) -> Task<kit::ArtifactRef> {
    LiveTake(co_await chat->SendAsync(kit::NewCommandId(), std::move(input)));
    co_await LiveRunCompleted(chat);
    co_return chat->Snapshot()->runs.back().result->artifacts.at(0);
  };
  const auto old_report = co_await stage(original_verification, {.text = "One verification case passed."});
  kit::SendInput deck_input{.text = "Stage deck", .context = {{old_report}}};
  const auto baseline = co_await stage(chat, std::move(deck_input));
  const auto new_report = co_await stage(verification, {.text = "Two verification cases passed."});
  if (fixture->adoption_race) {
    const auto report_session = original_verification->Snapshot()->summary.id;
    fixture->whole_impact = co_await VerifyCompetingPresentationAdoption(fixture, files, runtime, chat, baseline, old_report, new_report);
    runtime = fixture->runtime;
    original_verification = LiveTake(co_await runtime->Sessions().OpenAsync(report_session));
  } else {
    co_await stage(unrelated, {.text = "Unrelated verification report."});
    const kit::ProjectId release_project{"release"};
    const kit::SourceChange change{{old_report}, kit::SourceRef{new_report}, kit::SourceChangeKind::Modified};
    LiveTake(co_await runtime->Artifacts().RecordSourceChangeAsync(release_project, kit::NewCommandId(), change));
  }
  const auto report_session = original_verification->Snapshot();
  for (const auto& message : report_session->messages) {
    for (const auto& block : message.blocks) {
      const auto* artifact = std::get_if<kit::ArtifactBlock>(&block.body);
      if (artifact && artifact->artifact == old_report) fixture->report_button = block.id.value;
    }
  }
  if (fixture->report_button.empty()) throw std::runtime_error("No review button was published for the verification report");
  fixture->runtime = runtime;
  displayed = original_verification;
  if (fixture->adoption_race) {
    const auto end = std::chrono::steady_clock::now() + 50s;
    while (!fixture->whole_review_checked && std::chrono::steady_clock::now() < end) co_await Delay(25ms);
    if (!fixture->whole_review_checked) throw std::runtime_error("The whole-presentation review did not finish");
    LiveTake(co_await runtime->CloseAsync());
    co_return;
  }
  const auto snapshot = chat->Snapshot();
  for (int attempt = 0; attempt < 900 && chat->Snapshot()->runs.size() == snapshot->runs.size(); ++attempt)
    co_await Delay(25ms);
  co_await LiveInteraction(chat, "Review PowerPoint rework");
  const auto before_export = chat->Snapshot()->runs.size();
  const kit::ArtifactRef revised{baseline.id, baseline.version + 1};
  const auto staged_file = files.staging.Child(baseline.id.value.substr(std::string_view("presentation:").size()) + "-v2.pptx");
  if (staged_file.Exists()) throw std::runtime_error("Rework wrote the new deck before approval");
  fixture->verified_before_approval = true;
  for (int attempt = 0; attempt < 600 && chat->Snapshot()->runs.size() == before_export; ++attempt) co_await Delay(25ms);
  const auto export_approval = co_await LiveInteraction(chat, "Export PowerPoint");
  const auto& original = std::get<kit::ToolApproval>(export_approval.body);
  const auto output = files.exports.Child("ReworkedRelease.pptx");
  if (output.Exists() || original.preview.artifacts != std::vector{revised} ||
      original.preview.targets.size() != 1 || original.preview.targets.front().target.value != output.Path())
    throw std::runtime_error("The pending export did not match the reviewed deck and destination");
  LiveTake(co_await runtime->CloseAsync());
  runtime = co_await OpenPresentationReworkRuntime(root, files, fixture->deck_visible);
  chat = LiveTake(co_await runtime->Sessions().OpenAsync(snapshot->summary.id));
  observer = co_await LiveObserve(chat);
  const auto restored = co_await LiveInteraction(chat, "Export PowerPoint");
  const auto& restored_write = std::get<kit::ToolApproval>(restored.body);
  if (restored.id != export_approval.id || restored.run != export_approval.run || restored.version != export_approval.version ||
      restored_write.call != original.call || restored_write.plan != original.plan ||
      restored_write.preview.artifacts != original.preview.artifacts || output.Exists())
    throw std::runtime_error("Restart changed the export approval or wrote the file early");
  fixture->runtime = runtime;
  displayed = chat;
  fixture->export_restored = true;
  co_await LiveRunCompleted(chat);
  const auto state = chat->Snapshot();
  if (state->runs.back().usage.model_turns || std::ranges::count(state->tools, ExportPresentation,
      [](const kit::ToolCallSnapshot& tool) { return tool.tool.id; }) != 1)
    throw std::runtime_error("Export contacted the model or repeated the tool call");
  const auto exported = output.ReadBytes();
  const auto staged = staged_file.ReadBytes();
  if (!exported.Succeeded() || !staged.Succeeded() || exported.Value() != staged.Value())
    throw std::runtime_error("Exported file differs from the approved reworked version");
  auto before = LiveTake(co_await runtime->Artifacts().PreviewAsync(baseline));
  auto after = LiveTake(co_await runtime->Artifacts().PreviewAsync(revised));
  const auto old_outline = LiveTake(ReadPresentationOutline(*before.structured));
  const auto new_outline = LiveTake(ReadPresentationOutline(*after.structured));
  if (old_outline.slides[0].bullets != new_outline.slides[0].bullets ||
      old_outline.slides[1].bullets[0] != "One verification case passed." ||
      new_outline.slides[1].bullets != std::vector<std::string>{"Two verification cases passed.", "Keep this observation."})
    throw std::runtime_error("Local rework changed slides outside the reviewed page or kept the old conclusion");
  const auto metadata = LiveTake(co_await runtime->Artifacts().ReadAsync(revised));
  if (metadata.dependencies.size() != 2 || metadata.dependencies[0].input.source != kit::SourceRef{old_report} ||
      metadata.dependencies[1].input.source != kit::SourceRef{new_report})
    throw std::runtime_error("Reworked PPTX did not preserve page-level report versions");
  const PresentationLocation stale_link{snapshot->summary.id, baseline, kit::PartId{"slide-2"}};
  auto old_location = co_await OpenPresentationReviewAsync(runtime, stale_link);
  if (old_location.Succeeded() || old_location.Error().code != kit::ErrorCode::Conflict)
    throw std::runtime_error("An impact link opened a superseded presentation version");
  const PresentationLocation missing_link{snapshot->summary.id, revised, kit::PartId{"slide-3"}};
  auto missing_location = co_await OpenPresentationReviewAsync(runtime, missing_link);
  if (missing_location.Succeeded() || missing_location.Error().code != kit::ErrorCode::NotFound)
    throw std::runtime_error("An impact link accepted a missing page");
  LiveTake(co_await runtime->CloseAsync());
}

[[huxerui::composable]]
View PresentationReworkFixtureView(File root, std::shared_ptr<PresentationReworkFixture> fixture) {
  const auto displayed = UseState(std::shared_ptr<kit::Conversation>{});
  const auto pages = UseState(agentkit_preview::PresentationReviewPages{});
  const auto jump = UseState(std::optional<kit::AgentChatReviewRequest>{});
  const auto navigation_error = UseState(std::string{});
  const auto tasks = UseTaskScope();
  Lifecycle([root, tasks, displayed, fixture] {
    auto exercise = tasks.Launch([root, displayed, fixture]() -> Task<void> {
      try { co_await ExercisePresentationRework(root, fixture, displayed); }
      catch (...) { fixture->failure = std::current_exception(); }
      fixture->complete = true;
    });
    return [exercise] { exercise.Cancel(); };
  }, fixture);
  if (auto session = displayed.Get()) {
    const auto current = session->Snapshot();
    if (!current) return Text("Opening conversation");
    auto runtime = fixture->runtime;
    kit::AgentChatOptions options{.review_request = jump.Get()};
    agentkit_preview::ConfigurePreviewArtifactReview(options, runtime, pages,
        [runtime, tasks, displayed, pages, jump, navigation_error, fixture](agentkit_preview::PresentationLocation target) {
          tasks.Launch([runtime, displayed, pages, jump, target, navigation_error, fixture]() -> Task<void> {
            auto opened = co_await agentkit_preview::OpenPresentationReviewAsync(runtime, target);
            if (!opened.Succeeded()) { navigation_error = opened.Error().message; co_return; }
            if (target.part) {
              auto positions = pages.Get();
              positions.insert_or_assign(std::pair{target.deck.id, target.deck.version}, *target.part);
              pages = std::move(positions);
            }
            const auto ticket = jump.Get() ? jump.Get()->ticket + 1 : 1;
            jump = kit::AgentChatReviewRequest{target.deck, ticket};
            fixture->opened_impact = target;
            displayed = std::move(opened).Value();
          });
        });
    return Column{agentkit_preview::PresentationChat(std::move(session), runtime, std::move(options))
                      .Key(current->summary.id.value).With(Grow()),
                  Text(navigation_error.Get())}.With(Grow());
  }
  return Text("Preparing presentation fixtures");
}

void VerifyPresentationRework(const File& root, bool adoption_race) {
  auto fixture = std::make_shared<PresentationReworkFixture>(PresentationReworkFixture{.root = root, .adoption_race = adoption_race});
  const Application host{[]() -> View {
    const auto current = UseService<PresentationReworkFixture>();
    return PresentationReworkFixtureView(current->root, current);
  }, {.application_hooks = {[fixture](ApplicationContext& context) {
    context.Provide(fixture);
    context.Provide(std::make_shared<kit::AgentChatSelection>());
  }}}};
  UiTest ui(host, {.viewport = {1080.0F, 900.0F}});
  Wait(ui, [&] { return fixture->complete || !fixture->report_button.empty(); }, "The verification report was not staged", 45s);
  if (fixture->failure) std::rethrow_exception(fixture->failure);
  auto report = ui.Find(UiSelector::Key(fixture->report_button));
  Wait(ui, [&] { return report.Exists() && report.One().in_viewport; }, "The verification report could not be opened");
  const auto composer = ui.Find(UiSelector::Key("composer"));
  composer.Tap();
  composer.ReplaceText("Keep this report draft");
  report.Tap();
  const auto history = ui.Find(UiSelector::Key("agent-chat-history"));
  const auto summary = history.Find(UiSelector::Key("artifact-inline-summary"));
  Wait(ui, [&] {
    if (!summary.Exists() || !summary.One().in_viewport || !report.One().in_viewport) return false;
    const auto card = report.One().bounds;
    const auto opened = summary.One().bounds;
    return opened.y >= card.y + card.height && opened.y <= card.y + card.height + 10.0F;
  }, "The verification summary did not expand under its message card");
  TapFormButton(ui, "Close preview");
  Wait(ui, [&] {
    return !summary.Exists() && report.One().focused && composer.One().value == "Keep this report draft";
  }, "Collapsing the report did not restore the card focus and draft");
  report.Tap();
  TapFormButton(ui, "Find affected slides");
  const auto denied = ui.Find(UiSelector::Text("This presentation is no longer an available impact."));
  if (adoption_race) {
    const auto whole = ui.Find(ButtonText("Review presentation"));
    const auto approximate = ui.Find(UiSelector::Text("Affected scope needs review"));
    Wait(ui, [&] { return whole.Count() == 1 && whole.One().in_viewport && approximate.Exists(); },
        "Approximate impact did not offer a whole-deck review");
    if (!ShowsText(ui, "Executive rollout.pptx v3") || ui.Find(ButtonText("Review slide 1 / Overview")).Exists() ||
        ui.Find(ButtonText("Review slide 2 / Results")).Exists())
      throw std::runtime_error("Approximate impact exposed an older version or invented a precise slide range");
    *fixture->deck_visible = false;
    whole.Tap();
    Wait(ui, [&] { return denied.Exists() && !whole.Exists(); }, "The whole-deck link bypassed current permission");
    if (fixture->opened_impact) throw std::runtime_error("The revoked link left the verification report");
    *fixture->deck_visible = true;
    TapFormButton(ui, "Find affected slides");
    TapFormButton(ui, "Review presentation");
    Wait(ui, [&] { return ShowsText(ui, "1 / 2") && ShowsText(ui, "Version 3"); },
        "The whole-deck link did not open the adopted version");
    if (!fixture->whole_impact || fixture->opened_impact != fixture->whole_impact || fixture->opened_impact->part)
      throw std::runtime_error("The review did not target the whole adopted deck in its owning conversation");
    TapFormButton(ui, "Next slide");
    Wait(ui, [&] { return ShowsText(ui, "2 / 2"); }, "The whole-deck review did not allow navigation to the remaining slide");
    fixture->whole_review_checked = true;
    Wait(ui, [&] { return fixture->complete; }, "PowerPoint adoption and impact acceptance did not finish");
    if (fixture->failure) std::rethrow_exception(fixture->failure);
    return;
  }
  const auto first = ui.Find(ButtonText("Review slide 1 / Overview"));
  const auto second = ui.Find(ButtonText("Review slide 2 / Results"));
  Wait(ui, [&] { return first.Exists() && second.Exists() && second.One().in_viewport; },
      "The impact list did not resolve both slide IDs");
  *fixture->deck_visible = false;
  second.Tap();
  Wait(ui, [&] { return denied.Exists() && !second.Exists(); }, "The stale impact link bypassed the current permission");
  *fixture->deck_visible = true;
  TapFormButton(ui, "Find affected slides");
  TapFormButton(ui, "Review slide 2 / Results");
  const auto located = ui.Find(UiSelector::Text("2 / 2"));
  Wait(ui, [&] { return located.Exists() && located.One().in_viewport; }, "The cross-conversation link opened the wrong slide");
  TapFormButton(ui, "Replace report on slide");
  const auto new_result = ui.Find(UiSelector::Text("Two verification cases passed."));
  Wait(ui, [&] { return new_result.Count() == 1 && new_result.One().in_viewport; }, "The replacement report was not offered");
  if (ui.Find(UiSelector::Text("Unrelated verification report.")).Exists())
    throw std::runtime_error("The replacement picker exposed a report from a different project");
  TapFormButton(ui, "Use: Verification report v1  /  Release verification");
  TapFormButton(ui, "Prepare slide change");
  TapFormButton(ui, "Done");
  Wait(ui, [&] { return !ui.Find(ButtonText("Done")).Exists(); }, "The source picker did not close");
  TapFormButton(ui, "Back to conversation");
  Wait(ui, [&] { return fixture->complete || fixture->verified_before_approval; }, "No slide change was prepared");
  if (fixture->failure) std::rethrow_exception(fixture->failure);
  const auto plan = ui.Find(UiSelector::Text("Review PowerPoint rework"));
  Wait(ui, [&] { return plan.Exists() && plan.One().in_viewport; }, "The proposed slide change did not appear in the conversation");
  const auto approve = ui.Find(ButtonText("Approve"));
  Wait(ui, [&] { return approve.Count() == 1 && approve.One().enabled && approve.One().in_viewport; },
      "The planned slide change could not be approved in the conversation");
  approve.Tap();
  const auto revised_deck = ui.Find(ReviewCard("Review artifact v2"));
  const auto stop = ui.Find(UiSelector::Key("agent-chat-stop"));
  Wait(ui, [&] { return fixture->complete ||
      (revised_deck.Count() == 1 && revised_deck.One().in_viewport && !stop.Exists()); },
      "No revised deck became available to review");
  if (fixture->failure) std::rethrow_exception(fixture->failure);
  revised_deck.Tap();
  TapFormButton(ui, "Export PowerPoint");
  auto name = ui.Find(UiSelector::Key("ppt-review-export-name"));
  Wait(ui, [&] { return name.Exists() && name.One().in_viewport; }, "The export filename dialog did not open");
  name.Tap();
  name.ReplaceText("ReworkedRelease.pptx");
  TapFormButton(ui, "Review export");
  TapFormButton(ui, "Done");
  Wait(ui, [&] { return !ui.Find(ButtonText("Done")).Exists(); }, "The export filename dialog did not close");
  Wait(ui, [&] { return fixture->complete || fixture->export_restored; }, "The export approval was not restored");
  if (fixture->failure) std::rethrow_exception(fixture->failure);
  auto return_to_chat = ui.Find(ButtonText("Back to conversation"));
  if (return_to_chat.Exists() && return_to_chat.One().in_viewport) return_to_chat.Tap();
  Wait(ui, [&] { return approve.Count() == 1 && approve.One().enabled && approve.One().in_viewport; },
      "The restored export approval could not be reached");
  approve.Tap();
  Wait(ui, [&] { return fixture->complete; }, "The restored PowerPoint export did not settle", 30s);
  if (fixture->failure) std::rethrow_exception(fixture->failure);
}

void VerifyMessageSideChat(const Application& application) {
  constexpr Size wide_viewport{1080.0F, 900.0F};
  constexpr Size narrow_viewport{420.0F, 620.0F};
  UiTest preview(application, {.viewport = wide_viewport});
  const std::string excerpt = "\xe6\x96\xb9\xe6\xa1\x88";
  const auto original = "Before " + excerpt + " after D:/profiles/" + std::string(180, 'x') + "/project/summary.json";
  SendNewConversation(preview, original);
  Wait(preview, [&] { return MessageText(preview, original).has_value() && !preview.Find(UiSelector::Key("agent-chat-stop")).Exists(); },
      "The finished source message was not selectable");
  auto composer = preview.Find(UiSelector::Key("composer"));
  composer.Tap();
  composer.ReplaceText("Keep the main draft");
  auto source = MessageText(preview, original).value();
  if (source.One().bounds.height <= 24.0F)
    throw std::runtime_error("A transcript line containing a long path did not wrap");
  SelectHistoryText(preview, source, {7, 9});
  preview.PressKey(Key::Backspace);
  if (source.One().text != original || preview.ActiveTextInput())
    throw std::runtime_error("Backspace edited a sent message");
  const auto history = preview.Find(UiSelector::Key("agent-chat-history"));
  const auto actions = preview.Find(UiSelector::Key("message-selection-actions"));
  auto side_chat = preview.Find(ButtonText("Side chat"));
  Wait(preview, [&] {
    if (!actions.Exists() || !actions.Find(ButtonText("Side chat")).Exists() || !side_chat.One().in_viewport) return false;
    const auto message = source.One().bounds;
    const auto command = side_chat.One().bounds;
    return !history.Find(UiSelector::Key("message-selection-actions")).Exists() &&
        (std::abs(command.y + command.height - message.y) < 22.0F ||
            std::abs(command.y - (message.y + message.height)) < 22.0F);
  }, "The selection actions did not float next to the source");
  SelectHistoryText(preview, source, {7, 7});
  Wait(preview, [&] { return !actions.Exists(); }, "A collapsed selection retained its actions");
  SelectHistoryText(preview, source, {7, 9});
  Wait(preview, [&] { return side_chat.Exists() && side_chat.One().in_viewport; }, "Reselecting did not restore side chat");
  side_chat.Tap();
  auto question = preview.Find(UiSelector::Key("side-chat-question"));
  auto pane = preview.Find(UiSelector::Key("side-chat-pane"));
  const auto close_side = pane.Find(ButtonText("Close"));
  Wait(preview, [&] { return pane.Exists() && question.Exists() && question.One().focused &&
      question.One().bounds.width >= pane.One().bounds.width - 64.0F &&
      source.One().in_viewport && composer.One().in_viewport &&
      composer.One().bounds.x + composer.One().bounds.width <= pane.One().bounds.x; },
      "Side chat did not open next to the visible conversation and focus its question");
  const auto splitters = [&] {
    auto handles = preview.FindSemantics(UiSemanticSelector::Role(SemanticRole::Separator)).All();
    std::erase_if(handles, [](const auto& handle) { return handle.offscreen || handle.bounds.height < 400.0F; });
    std::ranges::sort(handles, [](const auto& first, const auto& second) { return first.bounds.x < second.bounds.x; });
    return handles;
  };
  const auto drag = [&](std::size_t index, float distance) {
    const auto handles = splitters();
    if (handles.size() != 2) throw std::runtime_error("The wide workbench did not expose two resize handles");
    const auto bounds = handles[index].bounds;
    const Point start{bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F};
    preview.Drag(start, {start.x + distance, start.y}, {.pointer = {.device_kind = PointerDeviceKind::Mouse}});
  };
  const auto initial_handles = splitters();
  if (initial_handles.size() != 2) throw std::runtime_error("The wide workbench did not expose two resize handles");
  const auto initial_left_divider = initial_handles.front().bounds.x;
  drag(0, 36.0F);
  const auto moved_handles = splitters();
  if (moved_handles.size() != 2 || moved_handles.front().bounds.x < initial_left_divider + 30.0F)
    throw std::runtime_error("Dragging the left divider did not resize navigation");
  const auto resized_left_divider = moved_handles.front().bounds.x;
  const auto initial_side_width = pane.One().bounds.width;
  drag(1, 30.0F);
  const auto resized_side_width = pane.One().bounds.width;
  const auto resized_handles = splitters();
  if (resized_side_width > initial_side_width - 24.0F || resized_handles.size() != 2 ||
      std::abs(resized_handles.front().bounds.x - resized_left_divider) > 1.0F)
    throw std::runtime_error("Dragging the right divider did not resize side chat independently");
  composer.Tap();
  if (!composer.One().focused || composer.One().value != "Keep the main draft" || !pane.Exists())
    throw std::runtime_error("The main composer was not interactive while side chat was open");
  FocusPreviewEditor(preview, question, "The question editor did not accept focus");
  question.ReplaceText("Explain the excerpt");
  preview.PressKey(Key::Enter, {.shift = true});
  if (question.One().value != "Explain the excerpt\n" || pane.Find(UiSelector::Key("side-chat-composer")).Exists())
    throw std::runtime_error("The side chat question did not keep the main composer's multiline editing behavior");
  question.ReplaceText("Explain the excerpt");
  close_side.Tap();
  Wait(preview, [&] { return !pane.Exists() && side_chat.Exists() && side_chat.One().focused; },
      "Closing the draft did not restore its original selection and action focus");
  SelectHistoryText(preview, source, {0, 6});
  TapControl(preview, ButtonText("Side chat"), "The new selection did not offer side chat");
  Wait(preview, [&] { return question.Exists() && question.One().focused && question.One().value == ""; },
      "A different selection inherited the previous side-chat draft");
  close_side.Tap();
  Wait(preview, [&] { return !pane.Exists(); }, "The second draft did not close");
  SelectHistoryText(preview, source, {7, 9});
  TapControl(preview, ButtonText("Side chat"), "The original selection did not offer side chat");
  Wait(preview, [&] { return question.Exists() && question.One().focused && question.One().value == "Explain the excerpt"; },
      "Reopening the original selection did not restore its question draft");
  preview.PressKey(Key::Enter);
  const auto answer = "Selected text: " + excerpt;
  Wait(preview, [&] { return MessageText(preview, answer).has_value(); }, "The assistant did not receive the exact selected text");
  auto whole = preview.Find(UiSelector::Key("side-chat-return-reply"));
  auto attachment = preview.Find(ButtonText("Remove conclusion"));
  auto follow_up = pane.Find(UiSelector::Key("agent-chat-composer"));
  const auto follow_up_field = pane.Find(UiSelector::Key("side-chat-composer"));
  Wait(preview, [&] { return whole.Exists() && whole.One().in_viewport && follow_up.Exists() &&
      follow_up.One().bounds.y + follow_up.One().bounds.height <= pane.One().bounds.y + pane.One().bounds.height; },
      "The completed reply and follow-up input did not fit the side chat pane");
  Wait(preview, [&] { return follow_up_field.Exists() && follow_up_field.One().focused; },
      "The completed initial question did not focus the follow-up editor");
  follow_up_field.ReplaceText("Keep this follow-up draft");
  preview.SetWindowMetrics({.viewport = narrow_viewport});
  Wait(preview, [&] { return follow_up_field.Exists() && follow_up_field.One().in_viewport &&
      follow_up_field.One().value == "Keep this follow-up draft" && (!composer.Exists() || !composer.One().in_viewport); },
      "Narrowing the window did not keep the active side chat and its draft");
  preview.SetWindowMetrics({.viewport = wide_viewport});
  Wait(preview, [&] {
    const auto handles = splitters();
    return handles.size() == 2 && composer.Exists() && composer.One().in_viewport && whole.One().in_viewport &&
        follow_up_field.One().value == "Keep this follow-up draft" &&
        std::abs(pane.One().bounds.width - resized_side_width) < 1.0F &&
        std::abs(handles.front().bounds.x - resized_left_divider) < 1.0F;
  }, "Widening the window did not restore the split conversation and its pane widths");
  whole.Tap();
  Wait(preview, [&] { return !pane.Exists() && attachment.Exists() && preview.Find(UiSelector::Text(answer)).Exists() &&
      composer.Exists() && composer.One().focused && composer.One().value == "Keep the main draft"; },
      "The complete reply was not returned to the existing focused draft");
  attachment.Tap();
  Wait(preview, [&] { return !attachment.Exists() && MessageText(preview, original).has_value(); }, "The conclusion could not be removed");
  preview.Find(UiSelector::Key("new-conversation")).Tap();
  Wait(preview, [&] { return preview.Find(UiSelector::Key("new-conversation-composer")).Exists(); },
      "The main conversation did not switch away");
  preview.Find(UiSelector::Key("session-directory")).Find(UiSelector::Key("session-title")).Tap();
  Wait(preview, [&] { return MessageText(preview, original).has_value(); }, "The main conversation did not reopen");
  preview.SetWindowMetrics({.viewport = narrow_viewport});
  Wait(preview, [&] {
    const auto editor = MessageText(preview, original);
    if (!editor) return false;
    const auto bounds = editor->One().bounds;
    return bounds.x >= 0.0F && bounds.x + bounds.width <= narrow_viewport.width;
  }, "The source message did not enter the compact layout");
  source = MessageText(preview, original).value();
  SelectHistoryText(preview, source, {7, 9});
  Wait(preview, [&] { return side_chat.Exists() && side_chat.One().in_viewport; },
      "The original message could not reopen its side chat");
  side_chat.Tap();
  Wait(preview, [&] {
    return MessageText(preview, answer).has_value() && pane.Exists() && follow_up.Exists() && follow_up.One().in_viewport &&
        pane.One().bounds.width >= narrow_viewport.width - 1.0F && (!composer.Exists() || !composer.One().in_viewport) &&
        follow_up.One().visible_bounds.width >= follow_up.One().bounds.width - 1.0F &&
        follow_up_field.One().focused && follow_up_field.One().value == "Keep this follow-up draft";
  }, "The compact side chat did not restore its reply and full-width input");
  auto reply = MessageText(preview, answer).value();
  SelectHistoryText(preview, reply, {15, 17});
  auto bring_back = preview.Find(UiSelector::Key("side-chat-return-selection"));
  Wait(preview, [&] { return bring_back.Exists() && bring_back.One().in_viewport; },
      "The selected reply did not offer a visible return action");
  bring_back.Tap();
  Wait(preview, [&] {
    return attachment.Exists() && !preview.Find(UiSelector::Key("side-chat-composer")).Exists();
  }, "The conclusion did not return to the main draft");
  Wait(preview, [&] { return composer.Exists() && composer.One().in_viewport && composer.One().focused; },
      "The compact main draft did not receive focus after side chat");
  composer.ReplaceText("Use the selected conclusion");
  preview.Find(ButtonText("Send")).Tap();
  Wait(preview, [&] { return !attachment.Exists(); }, "Submitting the conclusion did not clear its draft attachment");
}

void VerifyCrossMessageSelection(const Application& application) {
  UiTest preview(application, {.viewport = {1080.0F, 900.0F}});
  const std::string prefix = "Earlier text ";
  const std::string tail = "\xf0\x9f\x93\x8c\xe6\x96\xb9\xe6\xa1\x88";
  const auto input = prefix + tail;
  const auto response = "Received: " + input;
  auto create = preview.Find(UiSelector::Key("new-conversation"));
  Wait(preview, [&] { return create.Exists(); }, "The new conversation action was unavailable");
  create.Tap();
  SendNewConversation(preview, input);
  Wait(preview, [&] { return MessageText(preview, response).has_value(); }, "The response was not selectable");
  const auto select_range = [&] {
    auto last = MessageText(preview, response).value();
    SelectHistoryText(preview, last, {0, 8});
    TapFormButton(preview, "Extend selection");
    Wait(preview, [&] { return ShowsText(preview, "Select the other end of the range"); }, "The range did not keep its first end");
    auto first = MessageText(preview, input).value();
    SelectHistoryText(preview, first,
        {TextEditingValue::FromText(prefix).selection.active, TextEditingValue::FromText(input).selection.active});
    Wait(preview, [&] { return !ShowsText(preview, "Select the other end of the range") &&
        preview.Find(ButtonText("Cancel range")).Exists(); }, "Completing the range left the endpoint prompt visible");
  };
  select_range();
  TapControl(preview, ButtonText("Side chat"), "The extended range did not offer side chat");
  auto pane = preview.Find(UiSelector::Key("side-chat-pane"));
  auto sources = pane.Find(UiSelector::Key("side-chat-source"));
  auto question = pane.Find(UiSelector::Key("side-chat-question"));
  Wait(preview, [&] { return question.Exists() && question.One().focused && sources.Find(UiSelector::Text("2 selected passages")).Exists(); },
      "The two-fragment side chat did not open");
  const auto earlier = sources.Find(UiSelector::Text(tail));
  const auto later = sources.Find(UiSelector::Text("Received"));
  if (!earlier.Exists() || !later.Exists() || earlier.One().bounds.y >= later.One().bounds.y)
    throw std::runtime_error("The reverse selection did not preview exact fragments in reading order");
  question.ReplaceText("What is in the range?");
  pane.Find(UiSelector::Key("chat-send/Send")).Tap();
  const auto answer = "Selected text: " + tail + "\nSelected text: Received";
  Wait(preview, [&] { return ShowsText(preview, answer) ||
      (ShowsText(preview, "Selected text: " + tail) && ShowsText(preview, "Selected text: Received")); },
      "The model did not receive both exact fragments in reading order");
  auto bring_back = pane.Find(UiSelector::Key("side-chat-return-reply"));
  Wait(preview, [&] { return bring_back.Exists() && bring_back.One().in_viewport; }, "The multi-source reply could not be returned");
  bring_back.Tap();
  auto composer = preview.Find(UiSelector::Key("composer"));
  Wait(preview, [&] { return !pane.Exists() && composer.Exists() && composer.One().focused &&
      preview.Find(ButtonText("Remove conclusion")).Exists() && !preview.Find(ButtonText("Cancel range")).Exists(); },
      "Returning the conclusion did not focus the draft after closing the range");
  select_range();
  TapFormButton(preview, "Quote");
  auto quote = preview.Find(ButtonText("Remove quote"));
  Wait(preview, [&] { return quote.Exists() && composer.One().focused && ShowsText(preview, tail + "\nReceived") &&
      !ShowsText(preview, "Select the other end of the range"); }, "The quote did not retain both excerpts in the main draft");
  composer.ReplaceText("Use the range and conclusion");
  TapFormButton(preview, "Send");
  Wait(preview, [&] { return !quote.Exists() && !preview.Find(ButtonText("Remove conclusion")).Exists(); },
      "Submitting the range and conclusion did not clear their draft references");
}

constexpr auto queued_slide_excerpt = "Brief & revise <option>";
constexpr auto queued_slide_input = "Use the slide three conclusion";

void QueuePresentationConclusion(const File& root) {
  if (root.Exists()) throw std::runtime_error("Use a fresh PowerPoint side chat directory");
  const auto output = root.Child("exports").Child("Presentation.pptx");
  const Application application{App, {.application_hooks = {MakePreviewInstaller(root.Child("sessions.sqlite3"), false)}}};
  PreparePresentation(application, output);
  UiTest preview(application, {.viewport = {1080.0F, 920.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  auto conversation = PendingConversation(preview, "Product rollout");
  Wait(preview, [&] { return conversation.Exists(); }, "The presentation did not recover its export approval");
  conversation.Tap();
  const auto card = ReviewCard("Review artifact v1");
  auto review = preview.Find(card);
  Wait(preview, [&] { return review.Count() == 1; }, "The staged presentation could not be reviewed before export");
  if (!review.One().in_viewport)
    review = preview.Find(UiSelector::Key("agent-chat-history")).ScrollUntil(card, {.step = {0.0F, -95.0F}});
  review.Tap();
  TapFormButton(preview, "Next slide");
  TapFormButton(preview, "Next slide");
  TapFormButton(preview, queued_slide_excerpt);
  AskAboutSelection(preview, queued_slide_excerpt);
  auto attachment = preview.Find(ButtonText("Remove conclusion"));
  Wait(preview, [&] { return attachment.Exists() && ShowsText(preview, queued_slide_excerpt); },
      "The slide conclusion was not in the export conversation's draft");
  auto composer = preview.Find(UiSelector::Key("composer"));
  composer.Tap();
  composer.ReplaceText(queued_slide_input);
  TapControl(preview, UiSelector::Key("send-mode"), "Send mode during export approval");
  auto queue = preview.Find(UiSelector::Text("Queue"));
  Wait(preview, [&] { return queue.Exists() && queue.One().enabled; }, "Queue mode was not offered during export approval");
  queue.Tap({.device_kind = PointerDeviceKind::Mouse});
  const auto enqueue = preview.Find(ButtonText("Queue message"));
  Wait(preview, [&] { return !queue.Exists() && enqueue.Exists() && enqueue.One().enabled; }, "Queue mode did not settle");
  enqueue.Tap({.device_kind = PointerDeviceKind::Mouse});
  Wait(preview, [&] { return ShowsText(preview, "1 queued") && !attachment.Exists(); }, "The slide conclusion was not accepted into the queue");
  if (output.Exists()) throw std::runtime_error("Queueing the conclusion bypassed the original export approval");
}

void ResumePresentationConclusion(const File& root) {
  const auto database = root.Child("sessions.sqlite3");
  if (!database.Exists()) throw std::runtime_error("Use the same PowerPoint side chat directory for resume");
  const auto output = root.Child("exports").Child("Presentation.pptx");
  const Application application{App, {.application_hooks = {MakePreviewInstaller(database, false)}}};
  UiTest preview(application, {.viewport = {1080.0F, 920.0F},
      .resource_provider = std::make_shared<PreviewTestResources>()});
  auto conversation = PendingConversation(preview, "Product rollout");
  Wait(preview, [&] { return conversation.Exists(); }, "The export approval was lost when relaunching");
  conversation.Tap();
  Wait(preview, [&] { return ShowsText(preview, "1 queued") && ShowsText(preview, "Export PowerPoint"); },
      "The queued slide conclusion did not recover with the original export approval");
  if (output.Exists()) throw std::runtime_error("The original export ran before approval");
  TapFormButton(preview, "Approve");
  Wait(preview, [&] {
    return output.Exists() && !ShowsText(preview, "1 queued") && preview.Find(UiSelector::Key("ppt-audience")).Exists() &&
        ShowsText(preview, queued_slide_input) && preview.Find(UiSelector::Text(queued_slide_excerpt)).Count() == 1;
  }, "The recovered queue did not deliver the original slide conclusion exactly once");
}

void VerifyModelCatalog(std::string url) {
  std::exception_ptr failure;
  bool finished = false;
  const Application app{[]() -> View { return Text("GanWork model catalog acceptance"); },
      {.application_hooks = {[&](ApplicationContext&) {
        const auto host = UseApplication();
        auto client = std::make_shared<HttpClient>();
        UseApplicationTaskScope().Launch([&, host, client]() -> Task<void> {
          try {
            auto catalog = LiveTake(co_await agentkit_preview::DiscoverModels(
                {.base_url = url, .protocol = "chat-completions", .provider = "compatible", .api_key = "catalog-check-key"}, client));
            const auto rich = std::ranges::find(catalog, "catalog-long", &agentkit_preview::DiscoveredModel::id);
            const auto basic = std::ranges::find(catalog, "catalog-basic", &agentkit_preview::DiscoveredModel::id);
            if (catalog.size() != 2 || rich == catalog.end() || basic == catalog.end())
              throw std::runtime_error("The model catalog did not merge and deduplicate all pages");
            if (rich->limits.context_window != 65536 || rich->limits.max_output_tokens != 8192 || rich->limits.max_input_tokens)
              throw std::runtime_error("The model catalog did not preserve the token-limit fields");
            if (basic->limits.context_window || basic->limits.max_input_tokens || basic->limits.max_output_tokens)
              throw std::runtime_error("The model catalog assigned a guessed limit");
            finished = true;
          } catch (...) { failure = std::current_exception(); }
          host.Quit();
        });
      }}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  if (!finished) throw std::runtime_error("The native model catalog test did not finish");
}

void VerifyLiveSettings(const File& root, const std::string& url, const std::string& model,
                        const std::string& second_url, const std::string& second_model) {
  if (root.Child("sessions.sqlite3").Exists() || root.Child("settings.json").Exists() || !root.CreateDirectories())
    throw std::runtime_error("Use a fresh Preview settings acceptance directory");
  const auto database = root.Child("sessions.sqlite3");
#ifdef _WIN32
  const auto* fixture_key = std::getenv("AGENTKIT_PREVIEW_TEST_API_KEY");
#else
  const char* fixture_key = nullptr;
#endif
  const bool authenticate = fixture_key && *fixture_key;
  constexpr auto key_reference = "${AGENTKIT_PREVIEW_TEST_API_KEY}";
  const auto open_settings = [](UiTest& ui) {
    auto open = ui.Find(UiSelector::Key("open-settings"));
    Wait(ui, [&] { return open.Exists(); }, "Settings did not become available");
    open.Tap();
    Wait(ui, [&] { return ui.Find(UiSelector::Key("preview-settings")).Exists(); }, "Settings did not open");
  };
  const auto fill = [](UiTest& ui, const char* key, const std::string& value) {
    const auto target = UiSelector::Key(key);
    auto field = ui.Find(target);
    Wait(ui, [&] { return field.Exists(); }, key);
    if (!field.One().in_viewport) field = ui.Find(UiSelector::Key("settings-scroll")).ScrollUntil(target, {.step = {0, 100}});
    field.Tap();
    field.ReplaceText(value);
  };
  const auto select = [](UiTest& ui, const char* key, const std::string& label) {
    try {
      const auto target = UiSelector::Key(key);
      auto field = ui.Find(target);
      Wait(ui, [&] { return field.Exists(); }, key);
      if (!field.One().in_viewport) field = ui.Find(UiSelector::Key("settings-scroll")).ScrollUntil(target, {.step = {0, 100}});
      field.Tap();
      auto option = ui.Find(UiSelector::Text(label));
      Wait(ui, [&] { return option.Exists() && option.One().in_viewport; }, "The requested setting option did not open");
      option.Tap();
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(key) + ": " + error.what());
    }
  };
  constexpr auto session_title = "Workbench model check";
  {
    const Application application{App, {.application_hooks = {MakePreviewInstaller(database, true)}}};
    UiTest ui(application, {.viewport = {1440.0F, 960.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    open_settings(ui);
    const auto form = ui.Find(UiSelector::Key("settings-content")).One().bounds;
    const auto save_button = ui.Find(UiSelector::Key("settings-save")).One().bounds;
    const auto back_button = ui.Find(UiSelector::Key("close-settings")).One().bounds;
    if (std::abs(form.x + form.width - save_button.x - save_button.width) > 2.0F ||
        std::abs(form.x + form.width - back_button.x - back_button.width) > 2.0F)
      throw std::runtime_error("The settings actions did not align with the form");
    const auto navigation = ui.Find(UiSelector::Key("workbench-navigation"));
    const auto settings = ui.Find(UiSelector::Key("preview-settings"));
    Wait(ui, [&] {
      return navigation.Exists() && navigation.One().in_viewport &&
          settings.One().bounds.x >= navigation.One().bounds.x + navigation.One().bounds.width - 1.0F;
    }, "Opening settings hid or covered the desktop conversation list");
    for (const auto& [base, name] : {std::pair{url, model}, std::pair{second_url, second_model}}) {
      ui.Find(UiSelector::Key("settings-add-model")).Tap();
      fill(ui, "add-model-url", base);
      fill(ui, "add-model-id", name);
      const auto save = ui.Find(UiSelector::Key("save-model"));
      const auto key = ui.Find(UiSelector::Key("add-model-key"));
      const auto key_semantics = ui.FindSemantics(UiSemanticSelector::Label("API Key"));
      Wait(ui, [&] { return key_semantics.Exists() && key_semantics.One().secure; }, "The API key field was not secure");
      if (authenticate) {
        if (name == model) {
          fill(ui, "add-model-key", "${INVALID-NAME}");
          save.Tap();
          Wait(ui, [&] { return ui.Find(UiSelector::Key("add-model-error")).Exists(); },
              "An invalid API key reference was accepted");
        }
        fill(ui, "add-model-key", name == model ? fixture_key : key_reference);
        if (key.One().value.has_value() || !key_semantics.One().value.empty()) {
          throw std::runtime_error("The API key was exposed to UI inspection");
        }
      }
      save.Tap();
      Wait(ui, [&] { return !ui.Find(UiSelector::Key("add-model-dialog")).Exists(); }, "The model was not saved");
    }
    select(ui, "settings-daily-model", model);
    select(ui, "settings-review-model", second_model);
    ui.Find(UiSelector::Key("settings-section-personalization")).Tap();
    select(ui, "settings-reply-tone", "Concise");
    fill(ui, "settings-instructions", "Use plain language and keep replies under two sentences.");
    ui.SetWindowMetrics({.viewport = {520.0F, 550.0F}});
    auto appearance = ui.Find(UiSelector::Key("settings-section-appearance"));
    auto model_section = ui.Find(UiSelector::Key("settings-section-models"));
    Wait(ui, [&] { return ui.Find(UiSelector::Key("open-navigation")).Exists() &&
        appearance.Exists() && appearance.One().in_viewport && model_section.Exists() &&
        std::abs(appearance.One().bounds.y - model_section.One().bounds.y) < 1.0F; },
        "Settings navigation was not visible at compact width");
    appearance.Tap();
    select(ui, "settings-theme", "Light");
    ui.Find(UiSelector::Key("settings-save")).Tap();
    Wait(ui, [&] { return root.Child("settings.json").Exists(); }, "Settings were not persisted");
    ui.Find(UiSelector::Key("open-navigation")).Tap();
    auto new_conversation = navigation.Find(UiSelector::Key("new-conversation"));
    Wait(ui, [&] {
      if (!new_conversation.Exists()) return false;
      const auto button = new_conversation.One();
      return button.in_viewport && button.visible_bounds.width >= button.bounds.width - 1.0F;
    }, "The compact navigation did not open over settings");
    new_conversation.Tap({.device_kind = PointerDeviceKind::Mouse});
    Wait(ui, [&] {
      const auto composer = ui.Find(UiSelector::Key("new-draft-composer"));
      return composer.Exists() && composer.One().in_viewport;
    }, "The conversation list did not return from settings to a new draft");
  }
#ifdef _WIN32
  if (authenticate) {
    const auto profiles = root.Child("models").ListChildren();
    if (!profiles.Succeeded()) throw std::runtime_error("Could not verify the saved model credentials");
    bool protected_key = false;
    bool environment_key = false;
    for (const auto& profile : profiles.Value()) {
      const auto raw = profile.ReadString();
      if (!raw.Succeeded() || raw.Value().find(fixture_key) != std::string::npos) {
        throw std::runtime_error("The model profile contained an unprotected API key or was unreadable");
      }
      const auto record = nlohmann::json::parse(raw.Value());
      if (record.at("model") == model) {
        const auto& encrypted = record.at("protected_api_key");
        protected_key = encrypted.is_array() && !encrypted.empty() && !record.contains("api_key_reference");
      } else if (record.at("model") == second_model) {
        environment_key = record.value("api_key_reference", std::string{}) == key_reference && !record.contains("protected_api_key");
      }
    }
    if (!protected_key || !environment_key) throw std::runtime_error("Model profiles did not save the selected credential types");
  }
#endif
  const auto file = root.Child("settings.json").ReadString();
  if (!file.Succeeded()) throw std::runtime_error("Could not read the saved settings");
  const auto saved = nlohmann::json::parse(file.Value());
  if (!saved.at("daily_model").at("id").get<std::string>().ends_with(model) ||
      !saved.at("review_model").at("id").get<std::string>().ends_with(second_model) ||
      saved.at("appearance") != 1 || saved.at("tone") != 1 ||
      saved.at("instructions") != "Use plain language and keep replies under two sentences.") {
    throw std::runtime_error("The settings retained a different theme, tone, or model role");
  }
  {
    const Application application{App, {.application_hooks = {MakePreviewInstaller(database, true)}}};
    UiTest ui(application, {.viewport = {1080.0F, 960.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    auto picker = ui.FindSemantics(UiSemanticSelector::AllOf(
        UiSemanticSelector::Role(SemanticRole::Button), UiSemanticSelector::Label("Model")));
    Wait(ui, [&] { return picker.Exists(); }, "Conversation options did not mount after restart");
    if (picker.One().value.find(model) == std::string::npos) {
      throw std::runtime_error("The daily model was not selected after restart");
    }
    const auto heading = ui.Find(UiSelector::Key("new-conversation-heading"));
    const auto composer = ui.Find(UiSelector::Key("new-draft-composer"));
    const auto context = ui.Find(UiSelector::Key("draft-choices"));
    const auto main = ui.Find(UiSelector::Key("preview-content"));
    const auto draft_fits = [&] {
      if (!heading.Exists() || !composer.Exists() || !context.Exists() || !main.Exists()) return false;
      const auto box = composer.One();
      const auto title = heading.One();
      const auto controls = context.One();
      const auto bounds = main.One().bounds;
      const auto bottom_gap = bounds.y + bounds.height - box.bounds.y - box.bounds.height;
      return title.in_viewport && box.in_viewport && controls.in_viewport &&
          std::abs(title.bounds.x - box.bounds.x) < 1.0F &&
          title.bounds.y + title.bounds.height <= controls.bounds.y &&
          controls.bounds.y + controls.bounds.height <= box.bounds.y && bottom_gap >= 0.0F && bottom_gap < 28.0F &&
          box.visible_bounds.width >= box.bounds.width - 1.0F &&
          controls.visible_bounds.width >= controls.bounds.width - 1.0F;
    };
    Wait(ui, draft_fits, "The desktop draft did not keep the composer at the bottom");
    ui.SetWindowMetrics({.viewport = {760.0F, 650.0F}});
    Wait(ui, draft_fits, "The medium draft did not keep the composer at the bottom");
    ui.SetWindowMetrics({.viewport = {520.0F, 550.0F}});
    Wait(ui, [&] { return draft_fits() && ui.Find(UiSelector::Key("open-navigation")).Exists(); },
        "The compact draft did not fit beside the navigation entry");
    TapControl(ui, UiSelector::Key("Check Markdown references"), "Draft starter");
    if (ui.Find(UiSelector::Key("new-conversation-composer")).One().value.value_or("").find("UTF-8") == std::string::npos)
      throw std::runtime_error("The draft starter did not prefill the composer");
    if (!ui.Find(UiSelector::Key("run-model")).Exists() || !ui.Find(UiSelector::Key("run-permissions")).Exists() ||
        !ui.Find(UiSelector::Key("draft-assistant")).Exists()) {
      throw std::runtime_error("The draft controls were not available directly");
    }
  }
  const auto expected_model = saved.at("daily_model").at("id").get<std::string>();
  const auto workspace = root.Child("workspace");
  if (!workspace.CreateDirectories()) throw std::runtime_error("Could not initialize the live test workspace");
  std::exception_ptr failure;
  bool completed = false;
  auto drive = [&](auto started, auto, auto project) -> Task<void> {
    const auto application = UseApplication();
    try {
      auto runtime = LiveTake(std::move(started));
      auto created = LiveTake(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
          .options = {.agent = agentkit_preview::GeneralAgent, .project = std::move(project), .title = session_title}}));
      auto session = LiveTake(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(created.outcome).session));
      std::size_t streamed_bytes = 0;
      bool streamed_growth = false;
      auto watching = LiveTake(co_await session->ObserveAsync([&](auto snapshot) {
        if (!snapshot.Succeeded()) return;
        for (const auto& message : snapshot.Value()->messages) {
          if (message.author != kit::MessageAuthor::Agent || message.state != kit::MessageState::Streaming) continue;
          for (const auto& block : message.blocks) {
            if (const auto* text = std::get_if<kit::TextBlock>(&block.body); text && text->text.size() > streamed_bytes) {
              streamed_growth |= streamed_bytes != 0;
              streamed_bytes = text->text.size();
            }
          }
        }
      }));
      LiveTake(co_await session->SendAsync(kit::NewCommandId(), {.text = "Explain how bubble sort works in five short sentences."}));
      const auto deadline = std::chrono::steady_clock::now() + 45s;
      while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = session->Snapshot();
        if (snapshot && !snapshot->summary.active_run && !snapshot->runs.empty()) {
          const auto& run = snapshot->runs.back();
          if (run.failure) throw std::runtime_error(run.failure->message);
          if (run.model.id.value != expected_model || run.state != kit::RunState::Succeeded ||
              run.usage.model_turns != 2 || !streamed_growth || snapshot->messages.size() != 2 ||
              snapshot->messages.back().state != kit::MessageState::Complete) {
            throw std::runtime_error("The restored daily model did not stream one completed answer after routing");
          }
          completed = true;
          break;
        }
        co_await Delay(30ms);
      }
      LiveTake(co_await runtime->CloseAsync());
    } catch (...) { failure = std::current_exception(); }
    application.Quit();
  };
  const Application native{App, {
      .window = {.title = "AgentKit Settings Live Acceptance", .initial_size = {1080.0F, 760.0F},
          .chrome_mode = WindowChromeMode::Custom},
      .application_hooks = {MakeConfiguredPreviewTestInstaller(database, workspace, File(AGENTKIT_PREVIEW_CTEST), std::move(drive))}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  if (!completed) throw std::runtime_error("The native model request did not finish");
  {
    const Application application{App, {.application_hooks = {MakePreviewInstaller(database, true)}}};
    UiTest ui(application, {.viewport = {1080.0F, 760.0F}, .resource_provider = std::make_shared<PreviewTestResources>()});
    auto draft = ui.Find(UiSelector::Key("new-draft-composer"));
    auto session = ui.Find(UiSelector::Key("session-directory")).Find(UiSelector::Text(session_title));
    Wait(ui, [&] { return draft.Exists() && session.Exists() && session.One().in_viewport; },
        "The completed real conversation was missing from the desktop workbench");
    const auto draft_box = draft.One().bounds;
    session.Tap();
    auto composer = ui.Find(UiSelector::Key("agent-chat-composer"));
    Wait(ui, [&] { return composer.Exists() && composer.One().in_viewport; }, "The saved conversation did not open");
    const auto chat_box = composer.One().bounds;
    if (std::abs(draft_box.x - chat_box.x) >= 1.0F || std::abs(draft_box.width - chat_box.width) >= 1.0F ||
        std::abs(draft_box.y + draft_box.height - chat_box.y - chat_box.height) >= 1.0F) {
      throw std::runtime_error("The composer moved sideways or lost its bottom alignment when opening a conversation");
    }
    const auto user = ui.Find(UiSelector::Key("chat-user-bubble"));
    const auto agent = ui.Find(UiSelector::Key("chat-agent-bubble"));
    Wait(ui, [&] { return user.Exists() && agent.Exists() && user.One().in_viewport && agent.One().in_viewport; },
        "The real conversation did not render both chat bubbles");
    if (user.One().bounds.x <= agent.One().bounds.x + 30.0F)
      throw std::runtime_error("The user and assistant messages were not aligned to opposite sides");
  }
}

void VerifyLocalization(const File& root) {
  if (!root.CreateDirectories()) throw std::runtime_error("Could not create the localization fixture");
  const auto package = Resources::OpenPackage(File(AGENTKIT_TEST_RESOURCE_PACKAGE));
  if (!package.GetVectorImage(::app::images::goal).HasValue() ||
      !package.GetVectorImage(huxerui_agentkit::images::arrow_down).HasValue()) {
    throw std::runtime_error("A Preview or library icon was not in the resource package");
  }
  const auto verify = [&](const char* language, const char* new_conversation, const char* send, const char* settings,
                          const char* models, const char* api_key, const char* placeholder) {
    const ResourceConfiguration configuration{.locale = Locale::FromLanguageTag(language)};
    const Application app{App, {.application_hooks = {MakePreviewInstaller(root.Child(std::string(language) + ".sqlite3"), true)}}};
    UiTest ui(app, {.viewport = {1080.0F, 900.0F}, .resources = configuration});
    const auto heading = ui.Find(UiSelector::Key("new-conversation-heading"));
    const auto send_button = ui.Find(UiSelector::Key("chat-send/Send"));
    const auto open_settings = ui.Find(UiSelector::Key("open-settings"));
    try {
      Wait(ui, [&] { return heading.Exists() && heading.One().text == new_conversation && send_button.Exists() &&
          open_settings.Exists() && open_settings.Find(UiSelector::Text(settings)).Exists(); },
          "The conversation workbench did not use the requested locale");
      if (package.GetString(huxerui_agentkit::strings::ui_send, configuration) != send)
        throw std::runtime_error("The shared send label did not use the requested locale");
    } catch (const std::exception&) {
      static_cast<void>(root.Child(std::string(language) + ".snapshot").WriteString(ui.CaptureSnapshot().ToString()));
      throw std::runtime_error(std::string(language) + " workbench did not use the requested locale. Fixture: " + root.Path());
    }
    open_settings.Tap();
    const auto categories = ui.Find(UiSelector::Key("settings-section-models"));
    const auto add = ui.Find(UiSelector::Key("settings-add-model"));
    Wait(ui, [&] { return categories.Exists() && categories.Find(UiSelector::Text(models)).Exists() && add.Exists(); },
        "The settings page did not use the requested locale");
    add.Tap();
    const auto field = ui.FindSemantics(UiSemanticSelector::Label(api_key));
    Wait(ui, [&] { return field.Count() == 1 && field.One().placeholder == placeholder && field.One().secure; },
        "The API key field did not preserve its localized placeholder");
  };
  verify("en-US", "New conversation", "Send", "Settings", "Models", "API Key", "Optional. Paste a key or enter ${ENV_NAME}");
  verify("zh-Hans-CN",
       "\xe6\x96\xb0\xe5\xbb\xba\xe5\xaf\xb9\xe8\xaf\x9d", "\xe5\x8f\x91\xe9\x80\x81", "\xe8\xae\xbe\xe7\xbd\xae",
      "\xe6\xa8\xa1\xe5\x9e\x8b", "API \xe5\xaf\x86\xe9\x92\xa5", "\xe9\x80\x89\xe5\xa1\xab\xe3\x80\x82\xe7\xb2\x98\xe8\xb4\xb4\xe5\xaf\x86\xe9\x92\xa5\xe6\x88\x96\xe8\xbe\x93\xe5\x85\xa5 ${ENV_NAME}");
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--localization") {
    const auto root = File((std::filesystem::temp_directory_path() /
        ("agentkit-localization-" + kit::NewCommandId().value)).u8string());
    try {
      VerifyLocalization(root);
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--code-turn-activity") {
    const bool live = argc == 3;
    const auto path = live ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-turn-activity-" + kit::NewCommandId().value);
    try {
      const File root(path.u8string());
      if (root.Child("sessions.sqlite3").Exists() || root.Child("code").Exists() || (live && !root.Child("models").Exists()))
        throw std::runtime_error("Use a fresh acceptance directory with saved models for the live run");
      if (live) VerifyLiveCodeTurnActivity(root);
      else VerifyCodeTurnActivity(root);
      if (!live) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
      }
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << path.string() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--code-timeline-reopen") {
    try {
      VerifyReopenedCodeTimeline(File(std::filesystem::absolute(std::filesystem::u8path(argv[2])).u8string()));
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--workspace-tools") {
    const auto path = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("ganwork-workspace-tools-" + kit::NewCommandId().value);
    try {
      if (argc == 3) VerifyLiveWorkspaceTools(File(path.u8string()));
      else VerifyWorkspaceTools(File(path.u8string()));
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << path.string() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--model-catalog") {
    try {
      VerifyModelCatalog(argv[2]);
      std::cout << "Native model catalog pagination, deduplication, and optional token limits passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--native-model-settings") {
    const File directory(std::filesystem::absolute(std::filesystem::u8path(argv[2])).u8string());
    const Application application{App, {.window = {.title = "GanWork Model Settings Acceptance", .initial_size = {1080.0F, 760.0F}},
        .application_hooks = {MakePreviewInstaller(directory.Child("sessions.sqlite3"), true)}}};
    return RunApplication();
  }
  if (argc == 7 && std::string_view(argv[1]) == "--live-settings") {
    try {
      VerifyLiveSettings(File(std::filesystem::absolute(std::filesystem::u8path(argv[2])).u8string()),
          argv[3], argv[4], argv[5], argv[6]);
      std::cout << "Preview settings saved two live model profiles and sent a request using the restored daily model\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--release-impact") {
    try {
      VerifyStoredReleaseImpact(File(std::filesystem::absolute(std::filesystem::u8path(argv[2])).u8string()));
      std::cout << "Published design impact, recorded diff, screenshot and slide navigation passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 4) && std::string_view(argv[1]) == "--artifact-side-chat") {
    const std::string_view phase = argc == 4 ? argv[2] : "";
    if (argc == 4 && phase != "prepare" && phase != "resume") {
      std::cerr << "Select prepare or resume\n";
      return 1;
    }
    const auto root = argc == 4 ? std::filesystem::absolute(std::filesystem::u8path(argv[3])) :
        std::filesystem::temp_directory_path() / ("agentkit-artifact-side-chat-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (phase != "resume") QueuePresentationConclusion(fixture);
      if (phase != "prepare") ResumePresentationConclusion(fixture);
      if (argc == 2) {
        std::error_code error;
        const auto parent = std::filesystem::canonical(root.parent_path(), error);
        const auto resolved = error ? std::filesystem::path{} : std::filesystem::canonical(root, error);
        if (!error && resolved == parent / root.filename()) std::filesystem::remove_all(resolved, error);
      }
      std::cout << "Preview queued slide conclusion " << (phase == "prepare" ? "stored" : "recovered") << '\n';
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--release-verification-failure") {
    const std::string_view scenario = argv[2];
    if (scenario != "test-failure" && scenario != "missing-screenshot") {
      std::cerr << "Select test-failure or missing-screenshot\n";
      return 1;
    }
    const auto root = argc == 4 ? std::filesystem::absolute(std::filesystem::u8path(argv[3])) :
        std::filesystem::temp_directory_path() / ("agentkit-release-verification-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh release verification directory");
      VerifyReleaseFailure(fixture, scenario == "missing-screenshot");
      if (argc == 3) {
        std::error_code error;
        const auto parent = std::filesystem::canonical(root.parent_path(), error);
        const auto resolved = error ? std::filesystem::path{} : std::filesystem::canonical(root, error);
        if (!error && resolved == parent / root.filename()) std::filesystem::remove_all(resolved, error);
      }
      std::cout << "Release verification blocked publication: " << scenario << '\n';
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && (std::string_view(argv[1]) == "--goal-completion-revision" ||
                                  std::string_view(argv[1]) == "--goal-completion-automatic")) {
    const bool complete_automatically = std::string_view(argv[1]) == "--goal-completion-automatic";
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-goal-revision-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh release completion acceptance directory");
      VerifyReleaseCompletionRevision(fixture, complete_automatically);
      if (argc == 2) {
        std::error_code error;
        const auto tmp = std::filesystem::canonical(std::filesystem::temp_directory_path(), error);
        const auto resolved = error ? std::filesystem::path{} : std::filesystem::canonical(root, error);
        if (!error && resolved == tmp / root.filename()) std::filesystem::remove_all(resolved, error);
      }
      std::cout << (complete_automatically ? "Revised release verified and completed automatically\n" :
          "Release completion revision, verified export and screenshot side chat passed\n");
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--markdown-image-transport") {
    try { VerifyMarkdownImageTransport(argv[2]); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 && std::string_view(argv[1]) == "--composer-ime") ||
      (argc == 3 && std::string_view(argv[1]) == "--markdown-images")) {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-composer-ime-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      ImageAsset image;
      if (argc == 2) VerifyComposerIme(fixture);
      else {
        auto bytes = File(argv[2]).ReadBytes();
        if (!bytes.Succeeded()) throw std::runtime_error("The Markdown image fixture could not be opened");
        image = ImageAsset::FromEncoded(std::move(bytes).Value());
      }
      VerifyMarkdownSource(fixture.Child("markdown-source"), std::move(image));
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << (argc == 3 ? "Preview loaded Markdown images and preserved the selected source ranges\n" :
          "Preview composer preserved IME draft and focus during streaming\n");
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && std::string_view(argv[1]) == "--domain-review-windows") {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-domain-review-" + kit::NewCommandId().value);
    try {
      VerifyDomainReviewWindows(File(root.u8string()));
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview PPT and design review positions and decisions passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && std::string_view(argv[1]) == "--long-conversation") {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-timeline-" + kit::NewCommandId().value);
    try {
      VerifyLongConversation(File(root.u8string()));
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview history paging, review anchor and stream following passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--native-review-approval") {
    try { return VerifyNativeReviewWindows(File(std::filesystem::absolute(std::filesystem::path(argv[2])).u8string())); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--native-preview-compact") {
    try {
      const File database(std::filesystem::absolute(std::filesystem::path(argv[2])).u8string());
      if (!database.Exists()) throw std::runtime_error("Choose an existing Preview acceptance database");
      auto installer = MakePreviewInstaller(database, false);
      if (argc == 4) {
        const File workspace(std::filesystem::absolute(std::filesystem::path(argv[3])).u8string());
        kit::ModelRegistration model{{{"preview.test-code"}, 1}, {"preview", "code-fixture", "script", {}}, std::make_shared<PatchModel>()};
        installer = MakeCodeTestInstaller(database, workspace, std::move(model), File(AGENTKIT_PREVIEW_CTEST));
      }
      const Application application{App, {
          .window = {.title = "AgentKit Compact Preview Acceptance", .initial_size = {520.0F, 680.0F}},
          .show_debug_overlay = false,
          .application_hooks = {std::move(installer)}}};
      return RunApplication();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 2 && std::string_view(argv[1]) == "--review-approval") {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-review-" + kit::NewCommandId().value);
    try {
      VerifyReviewApproval(File(root.u8string()));
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview detached review binding and approval passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && std::string_view(argv[1]) == "--conversation-navigation") {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-navigation-" + kit::NewCommandId().value);
    try {
      VerifyConversationNavigation(File(root.u8string()));
      VerifyNavigationConcurrency(File((root / "concurrent").u8string()));
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview conversation navigation and pending requests passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && (std::string_view(argv[1]) == "--candidate-worktree-recovery" ||
                    std::string_view(argv[1]) == "--approved-candidate-recovery")) {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-candidate-recovery-" + kit::NewCommandId().value);
    try {
      VerifyCandidateWorktreeRecovery(File(root.u8string()), std::string_view(argv[1]) == "--approved-candidate-recovery");
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview candidate worktree recovery and cleanup passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && (std::string_view(argv[1]) == "--code-candidates" || std::string_view(argv[1]) == "--code-adoption-recovery" ||
                    std::string_view(argv[1]) == "--code-adoption-partial")) {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-code-candidates-" + kit::NewCommandId().value);
    try {
      const std::string_view mode = argv[1];
      const auto scenario = mode == "--code-adoption-partial" ? CodeAdoptionCase::Partial :
          mode == "--code-adoption-recovery" ? CodeAdoptionCase::Interrupted : CodeAdoptionCase::Complete;
      VerifyCodeCandidates(File(root.u8string()), scenario);
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview " << mode.substr(2) << " passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && (std::string_view(argv[1]) == "--fork-review" || std::string_view(argv[1]) == "--design-adoption-recovery" ||
                    std::string_view(argv[1]) == "--ppt-adoption-recovery")) {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-fork-review-" + kit::NewCommandId().value);
    try {
      const std::string_view mode = argv[1];
      const auto scenario = mode == "--design-adoption-recovery" ? PublicationInterruption::Design :
          mode == "--ppt-adoption-recovery" ? PublicationInterruption::Presentation : PublicationInterruption::None;
      VerifyForkReview(File(root.u8string()), scenario);
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Preview " << mode.substr(2) << " passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && (std::string_view(argv[1]) == "--code-revert" || std::string_view(argv[1]) == "--design-revert" ||
                                  std::string_view(argv[1]) == "--code-revert-recovery")) {
    const bool design = std::string_view(argv[1]) == "--design-revert";
    const bool recovery = std::string_view(argv[1]) == "--code-revert-recovery";
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ((design ? "agentkit-design-revert-" : "agentkit-code-revert-") + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh restore acceptance directory");
      if (design) VerifyDesignRevert(fixture);
      else if (recovery) {
        VerifyCodeRevert(fixture.Child("partial"), CodeRevertCase::Partial);
        VerifyCodeRevert(fixture.Child("interrupted"), CodeRevertCase::InterruptedPartial);
      }
      else VerifyCodeRevert(fixture);
      if (argc == 2) {
        std::error_code error;
        const auto tmp = std::filesystem::canonical(std::filesystem::temp_directory_path(), error);
        const auto resolved = error ? std::filesystem::path{} : std::filesystem::canonical(root, error);
        if (!error && resolved == tmp / root.filename()) std::filesystem::remove_all(resolved, error);
      }
      std::cout << (design ? "Preview design" : "Preview code") << " restore, approval recovery, version conflict and impact passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if (argc == 2 && std::string_view(argv[1]) == "--shared-reports") {
    const auto root = std::filesystem::temp_directory_path() / ("agentkit-shared-reports-" + kit::NewCommandId().value);
    try {
      VerifySharedReports(File(root.u8string()));
      std::error_code error;
      std::filesystem::remove_all(root, error);
      std::cout << "Shared report review, publication and withdrawal passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\nFixture: " << root.string() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && (std::string_view(argv[1]) == "--presentation-rework" ||
                                  std::string_view(argv[1]) == "--ppt-adoption-baseline")) {
    const bool adoption_race = std::string_view(argv[1]) == "--ppt-adoption-baseline";
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-presentation-rework-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh PowerPoint rework acceptance directory");
      VerifyPresentationRework(fixture, adoption_race);
      if (argc == 2) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
      std::cout << (adoption_race ? "PowerPoint stale and reviewed adoption baselines passed\n" :
          "PowerPoint page rework and approved export passed\n");
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 3 && (std::string_view(argv[1]) == "--code-editor-prepare" ||
                    std::string_view(argv[1]) == "--code-editor-resume")) {
    const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
    const auto stage = std::string_view(argv[1]) == "--code-editor-prepare" ? EditorAcceptanceStage::Prepare : EditorAcceptanceStage::Resume;
    try {
      const File fixture(root.u8string());
      if (fixture.Exists() != (stage == EditorAcceptanceStage::Resume)) {
        throw std::runtime_error("Prepare must use a fresh directory and resume must use the same directory");
      }
      VerifyCodeEditorBaseline(fixture.Child("current"), EditorBaselineCase::Current, stage);
      VerifyCodeEditorBaseline(fixture.Child("editor-changed"), EditorBaselineCase::EditorChanged, stage);
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\nFixture: " << root.string() << '\n';
      return 1;
    }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--code-editor") {
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-code-editor-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh editor baseline acceptance directory");
      VerifyCodeEditorBaseline(fixture.Child("current"), EditorBaselineCase::Current);
      VerifyCodeEditorBaseline(fixture.Child("editor-changed"), EditorBaselineCase::EditorChanged);
      VerifyCodeEditorBaseline(fixture.Child("disk-changed"), EditorBaselineCase::DiskChanged);
      VerifyCodeEditorBaseline(fixture.Child("full-access"), EditorBaselineCase::FullAccess);
      if (argc == 2) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
      std::cout << "Unsaved editor baseline acceptance passed\n";
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\nFixture: " << root.string() << '\n';
      return 1;
    }
  }
  if ((argc == 2 || argc == 3) && (std::string_view(argv[1]) == "--code-failures" ||
                                  std::string_view(argv[1]) == "--code-patch-recovery")) {
    const bool recovery = std::string_view(argv[1]) == "--code-patch-recovery";
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-code-failures-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh code failure acceptance directory");
      if (recovery) {
        VerifyCodeFailure(fixture.Child("interrupted"), CodeFailureCase::Interrupted);
        VerifyCodeFailure(fixture.Child("interrupted-partial"), CodeFailureCase::InterruptedPartial);
      } else {
        VerifyCodeFailure(fixture.Child("partial"), CodeFailureCase::Partial);
        VerifyCodeFailure(fixture.Child("verification"), CodeFailureCase::Verification);
      }
      if (argc == 2) {
        std::error_code error;
        const auto tmp = std::filesystem::canonical(std::filesystem::temp_directory_path(), error);
        const auto resolved = error ? std::filesystem::path{} : std::filesystem::canonical(root, error);
        if (!error && resolved == tmp / root.filename()) std::filesystem::remove_all(resolved, error);
      }
      std::cout << (recovery ? "Interrupted code patch recovery passed\n" : "Partial code patch and failed CTest passed\n");
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\nFixture: " << root.string() << '\n';
      return 1;
    }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--presentation-failures") {
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-presentation-failures-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh presentation failure acceptance directory");
      VerifyPresentationFailures(fixture);
      if (argc == 2) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
      std::cout << "PPTX generation failure and export conflict recovery passed\n";
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\nFixture: " << root.string() << '\n';
      return 1;
    }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--run-settings") {
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-run-settings-" + kit::NewCommandId().value);
    try {
      VerifyPreviewRunSettings(File(root.u8string()));
      VerifyRunSettingsReload(File(root.u8string()));
      std::cout << "Preview run settings, restart, queued references and scoped approval passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--reload-run-settings") {
    try {
      const File fixture(std::filesystem::absolute(std::filesystem::u8path(argv[2])).u8string());
      if (!fixture.Child("sessions.sqlite3").Exists() || !fixture.Child("project").Exists())
        throw std::runtime_error("Use an existing run settings fixture");
      VerifyRunSettingsReload(fixture);
      std::cout << "Preview run preferences and queue references restored from disk\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--release-skill") {
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-release-skill-" + kit::NewCommandId().value);
    try {
      const File fixture(root.u8string());
      if (fixture.Exists()) throw std::runtime_error("Use a fresh release Skill acceptance directory");
      VerifyReleaseSkill(fixture.Child("user"), true);
      VerifyReleaseSkill(fixture.Child("model"), false);
      if (argc == 2) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
      std::cout << "Preview release Skill, approvals, template and PPTX passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--report-chart") {
    const auto root = argc == 3 ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-report-chart-" + kit::NewCommandId().value);
    try {
      VerifyReportChart(File(root.u8string()));
      if (argc == 2) {
        std::error_code error;
        std::filesystem::remove(root / "exports" / std::filesystem::u8path(ReportChartFilename), error);
        for (const auto* name : {"sessions.sqlite3", "sessions.sqlite3-wal", "sessions.sqlite3-shm",
                                "exports", "workspaces", "worktree-plans"}) {
          std::filesystem::remove(root / name, error);
        }
        std::filesystem::remove(root, error);
      }
      std::cout << "Preview report chart, filter and recovered export passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--side-chat") {
    const auto root = argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::temp_directory_path() /
        ("agentkit-side-chat-" + agentkit::NewCommandId().value);
    try {
      const auto database = huxerui::File(root.u8string()).Child("sessions.sqlite3");
      Application application{App, {.application_hooks = {MakePreviewInstaller(database, false)}}};
      VerifyMessageSideChat(application);
      VerifyCrossMessageSelection(application);
      if (argc == 2) {
        std::error_code error;
        for (const auto* name : {"sessions.sqlite3", "sessions.sqlite3-wal", "sessions.sqlite3-shm",
                                "exports", "workspaces", "worktree-plans"}) {
          std::filesystem::remove(root / name, error);
        }
        std::filesystem::remove(root, error);
      }
      std::cout << "Preview side chat selection and conclusion draft passed\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 2 || argc == 3) &&
      (std::string_view(argv[1]) == "--worktree" || std::string_view(argv[1]) == "--worktree-discard")) {
    const auto keep = argc == 3;
    const auto root = keep ? std::filesystem::absolute(std::filesystem::u8path(argv[2])) :
        std::filesystem::temp_directory_path() / ("agentkit-worktree-" + kit::NewCommandId().value);
    int exit = 0;
    try {
      const File fixture(root.u8string());
      if (std::string_view(argv[1]) == "--worktree") {
        VerifyWorktreeCopy(fixture);
        VerifyWorktreeCopy(fixture.Child("discard"), true);
      } else {
        VerifyWorktreeCopy(fixture, true);
      }
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; exit = 1; }
    if (!keep) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
    return exit;
  }
  if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--configured-preview") {
    const File data(argv[2]);
    const auto workspace = data.Child("code");
    const auto phase = argc == 4 ? std::string(argv[3]) : std::string{};
    if (argc == 4) {
      for (const auto* name : {"HUXERUI_AGENTKIT_BASE_URL", "HUXERUI_AGENTKIT_MODEL", "HUXERUI_AGENTKIT_API_KEY"}) {
        const auto* value = std::getenv(name);
        if (!value || !*value) { std::cerr << "Configure " << name << '\n'; return 1; }
      }
    }
    if (phase == "release-prepare") {
#if defined(AGENTKIT_PREVIEW_BROWSER)
      try {
        if (workspace.Exists() || data.Child("sessions.sqlite3").Exists())
          throw std::runtime_error("Choose a fresh directory for live release acceptance");
        SeedLiveReleaseWorkspace(workspace);
      } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
      }
#else
      std::cerr << "Configure a Chromium browser to run live release acceptance\n";
      return 1;
#endif
    }
    if (!workspace.Exists()) {
      std::cerr << "Create the acceptance workspace in the supplied directory first\n";
      return 1;
    }
    int result = 1;
    ConfiguredPreviewDriver drive;
    if (argc == 4) {
      drive = [data, phase, &result](auto started, auto workspace, auto project) -> Task<void> {
        const auto app = UseApplication();
        try {
          auto runtime = LiveTake(std::move(started));
          if (!workspace) throw std::runtime_error("The test workspace did not start");
          if (phase.starts_with("review-"))
            co_await RunConfiguredChartReview(runtime, project, data, phase);
          else if (phase.starts_with("skill-"))
            co_await RunConfiguredReleaseSkill(runtime, project, *workspace, data, phase);
          else if (phase.starts_with("release-"))
            co_await RunConfiguredRelease(std::move(runtime), *workspace, std::move(project), data, phase);
          else
            co_await RunConfiguredPreview(std::move(runtime), *workspace, std::move(project), data, phase);
          result = 0;
        } catch (const std::exception& error) {
          std::cerr << error.what() << '\n';
        }
        app.Quit();
      };
    }
    const Application application{App, {
        .window = {.title = "GanWork Live Acceptance"},
        .application_hooks = {MakeConfiguredPreviewTestInstaller(data.Child("sessions.sqlite3"), workspace,
            File(AGENTKIT_PREVIEW_CTEST), std::move(drive))}}};
    const auto exit = RunApplication();
    return argc == 4 ? result : exit;
  }
  const auto keep = argc == 2;
  const auto root = keep ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() /
      ("agentkit-preview-" + agentkit::NewCommandId().value);
  const auto path = root / "sessions.sqlite3";
  const auto staging = root / "presentations";
  const auto output = root / "exports" / "Presentation.pptx";
  const auto briefing = root / "exports" / "DesignBriefing.pptx";
  const auto release = root / "exports" / "WorkspaceRelease.pptx";
  const auto code = root / "code";
  const auto logs = root / "test-logs";
  const auto registered = root / "workspaces";
  const auto designs = root / "designs";
  const auto evidence = root / "workspace-evidence";
  auto cleanup = [&] {
    if (keep) return;
    std::error_code error;
    std::filesystem::remove(path, error);
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
      auto related = path;
      related += suffix;
      std::filesystem::remove(related, error);
    }
    std::filesystem::remove(output, error);
    std::filesystem::remove(briefing, error);
    std::filesystem::remove(release, error);
    std::filesystem::remove(output.parent_path() / "WorkspaceReleaseBeforeEdit.pptx", error);
    std::filesystem::remove(output.parent_path(), error);
    for (const auto& entry : std::filesystem::directory_iterator(staging, error)) {
      if (entry.path().extension() == ".pptx") std::filesystem::remove(entry.path(), error);
    }
    std::filesystem::remove(staging, error);
    for (const auto* file : {"config.txt", "settings.html", "verify.cmake", "CTestTestfile.cmake", "tested.txt"}) {
      std::filesystem::remove(code / file, error);
    }
    for (const auto& entry : std::filesystem::directory_iterator(code / "Testing" / "Temporary", error)) {
      if (entry.is_regular_file()) std::filesystem::remove(entry.path(), error);
    }
    std::filesystem::remove(code / "Testing" / "Temporary", error);
    std::filesystem::remove(code / "Testing", error);
    std::filesystem::remove(code, error);
    for (const auto& entry : std::filesystem::directory_iterator(logs, error)) {
      if (entry.path().extension() == ".log") std::filesystem::remove(entry.path(), error);
    }
    std::filesystem::remove(logs, error);
    for (const auto& entry : std::filesystem::directory_iterator(registered, error)) {
      if (entry.path().extension() == ".workspace") std::filesystem::remove(entry.path(), error);
    }
    std::filesystem::remove(registered, error);
    for (const auto& entry : std::filesystem::directory_iterator(designs, error)) {
      if (entry.path().extension() == ".json") std::filesystem::remove(entry.path(), error);
    }
    std::filesystem::remove(designs, error);
    for (const auto& entry : std::filesystem::directory_iterator(evidence, error)) {
      if (entry.path().extension() == ".json" || entry.path().extension() == ".png") std::filesystem::remove(entry.path(), error);
    }
    for (const auto& entry : std::filesystem::directory_iterator(evidence / "restore", error)) {
      if (entry.path().extension() == ".before" || entry.path().extension() == ".after") std::filesystem::remove(entry.path(), error);
    }
    std::filesystem::remove(evidence / "restore", error);
    std::filesystem::remove(evidence, error);
    std::filesystem::remove(root, error);
  };
  try {
    Application application{App, {.application_hooks = {MakePreviewInstaller(File(path.u8string()), false)}}};
    CreateAndApprove(application);
    ReopenConversation(application);
    PreparePresentation(application, File(output.u8string()));
    RestartAndExport(application, File(output.u8string()), staging);
    StageDesign(application, designs);
    ApproveAndReviseDesign(application, designs);
    PlanBriefing(application);
    DeliverBriefing(application, File(briefing.u8string()));
    ReviewCopiedPresentation(application);
#if defined(_WIN32) || defined(__linux__)
    CodeRoundTrip(File(path.u8string()), File(code.u8string()), File(logs.u8string()), File(release.u8string()));
#endif
    cleanup();
    std::cout << "Preview conversation, PPTX export, and workspace approval checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    cleanup();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
