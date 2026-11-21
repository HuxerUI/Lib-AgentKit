#include <huxerui/agentkit.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include "test_resources.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {

using namespace huxerui;
using namespace huxerui::agentkit;
using namespace huxerui::testing;
using namespace std::chrono_literals;

const ProjectId workshop{"workshop.open"};
const ProjectId restricted{"workshop.restricted"};
const PrincipalId technician{"workshop.technician"};
const AgentId general{"workshop.general"};
const AgentId inventory{"workshop.inventory"};
const AgentId receipt_side_chat{"workshop.receipt_side_chat"};
const ToolId reserve{"workshop.reserve_equipment"};
const ModelId model_id{"workshop.local"};
const ContextProviderId handbook{"workshop.handbook"};
const ContentTypeRef reservation{"workshop.reservation", 1};
constexpr auto receipt_type = "application/vnd.workshop.receipt";
const huxerui::agentkit::ResourceId ledger{"workshop/caliper"};
const SourceSlice policy{ResourceRef{{"workshop/handbook"}, "v1"}};
constexpr auto policy_text = "Calipers may be booked after approval.";
constexpr auto receipt_excerpt = "Booked equipment: caliper.";
constexpr auto receipt_question = "What does the quoted receipt say?";
constexpr auto receipt_reply = "The workshop booked a caliper.";

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(AgentResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void Take(AgentResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

JsonSchema FieldSchema(std::string field) {
  JsonValue::Object property{{field, JsonValue{JsonValue::Object{{"type", JsonValue{std::string("string")}}}}}};
  return {JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}},
      {"required", JsonValue{JsonValue::Array{JsonValue{std::move(field)}}}},
      {"additionalProperties", JsonValue{false}}, {"properties", JsonValue{std::move(property)}}}}};
}

const std::string& Field(const JsonValue& value, const char* key) {
  return std::get<std::string>(std::get<JsonValue::Object>(value.value).at(key).value);
}

class LocalStream final : public ModelTurnStream {
public:
  explicit LocalStream(std::vector<ModelEvent> events) : events_(std::move(events)) {}

  Async<std::optional<ModelEvent>> ReadAsync() override {
    if (next_ == events_.size()) co_return AgentResult<std::optional<ModelEvent>>::Success(std::nullopt);
    co_await Delay(5ms);
    if (next_ == events_.size()) co_return AgentResult<std::optional<ModelEvent>>::Success(std::nullopt);
    co_return AgentResult<std::optional<ModelEvent>>::Success(std::move(events_[next_++]));
  }

  Async<ModelStopOutcome> StopAsync() override {
    next_ = events_.size();
    co_return AgentResult<ModelStopOutcome>::Success(ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<ModelEvent> events_;
  std::size_t next_ = 0;
};

class LocalModel final : public ModelProvider {
public:
  ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    std::string text;
    bool handbook_received = false;
    bool receipt_received = false;
    const ModelToolResult* result = nullptr;
    for (const auto& item : request.items) {
      if (const auto* content = std::get_if<ModelContent>(&item)) {
        const auto* body = std::get_if<std::string>(&content->body);
        if (body && content->role == ModelContent::Role::User) { text = *body; result = nullptr; }
        if (body && content->role == ModelContent::Role::Context && *body == policy_text) handbook_received = true;
        if (body && content->role == ModelContent::Role::Context &&
            *body == std::string("Selected passage:\n") + receipt_excerpt) receipt_received = true;
      } else if (const auto* reply = std::get_if<ModelToolResult>(&item)) {
        result = reply;
      }
    }
    std::vector<ModelEvent> events;
    if (result) {
      Check(result->provider_call_id == "reservation", "The result lost the provider's call ID");
      if (result->error) {
        Check(result->error->code == ErrorCode::PermissionDenied, "The tool denial was not delivered to the model");
        events.push_back(ModelTextDelta{"Reservation refused by workshop policy"});
      } else {
        Check(result->value && Field(*result->value, "equipment") == "caliper", "The model did not receive the booked equipment");
        events.push_back(ModelTextDelta{"Reservation confirmed"});
      }
    } else if (text.starts_with("Reserve ")) {
      Check(text != "Reserve caliper" || handbook_received, "The model did not receive the selected authorized handbook");
      Check(request.tools.size() == 1 && request.tools.front().name == reserve.value, "Host tool was not offered to the model");
      events = {ModelTextDelta{"Preparing reservation"}, ModelToolStarted{"reservation", reserve.value},
          ModelToolArguments{"reservation", R"({"equipment":")"},
          ModelToolArguments{"reservation", text.substr(8) + "\"}"}, ModelToolFinished{"reservation"},
          ModelFinished{ModelFinishReason::ToolCalls, {}}};
      co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<LocalStream>(std::move(events)));
    } else if (text == receipt_question) {
      Check(receipt_received, "The side chat did not receive the verified receipt excerpt");
      events.push_back(ModelTextDelta{receipt_reply});
    } else {
      events.push_back(ModelTextDelta{text == "Audit this receipt" ? "Receipt checked" : "Reservation details explained"});
    }
    events.push_back(ModelFinished{ModelFinishReason::Completed, {}});
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<LocalStream>(std::move(events)));
  }
};

using DispatchReceipt = std::function<Async<Receipt<RunAccepted>>(CommandId, ArtifactAction)>;
using DraftSources = State<AgentChatDraftSources>;

struct Inventory {
  PrincipalId principal{"workshop.supervisor"};
  bool receipts_visible = true;
  int bookings = 0;
  std::map<std::string, JsonValue> receipts;
};

struct Workshop {
  std::shared_ptr<Inventory> ledger = std::make_shared<Inventory>();
  DispatchReceipt dispatch_receipt;
  std::shared_ptr<AgentRuntime> runtime;
  std::shared_ptr<Conversation> chat;
  std::optional<DraftSources> draft_sources;
  std::exception_ptr failure;
};

std::shared_ptr<AgentRuntime> ConfigureWorkshop(const std::shared_ptr<Workshop>& shop, TaskScope tasks) {
  const auto data = shop->ledger;
  const auto host = std::weak_ptr{shop};
  auto runtime = Take(AgentRuntime::Create({.tasks = tasks,
      .principal = [data] { return AgentResult<PrincipalId>::Success(data->principal); },
      .access = [data](PolicyRequest request) -> Async<PolicyDecision> {
        using Result = AgentResult<PolicyDecision>;
        if (request.actor.principal == technician && request.project == restricted)
          co_return Result::Success({AccessDecision::Deny, "Restricted workshop project"});
        if (request.capability == Capability::ReadArtifact && !data->receipts_visible)
          co_return Result::Success({AccessDecision::Deny, "Receipt access withdrawn"});
        if (request.capability == Capability::SendToModel && !request.sources.empty() && request.recipient_model != model_id)
          co_return Result::Success({AccessDecision::Deny, "Handbook can only be sent to the workshop model"});
        if (request.capability == Capability::PrepareTool && request.tool && request.tool->id == reserve &&
            (!request.invocation || Field(request.invocation->arguments, "equipment") != "caliper"))
          co_return Result::Success({AccessDecision::Deny, "Equipment is outside the permitted inventory"});
        const auto decision = request.capability == Capability::ExecuteTool && request.tool && request.tool->id == reserve
            ? AccessDecision::RequireConfirmation : AccessDecision::Allow;
        co_return Result::Success({decision, {}});
      },
      .tool_resources = [data](ToolInvocation request) -> Async<std::vector<ResourceAccess>> {
        const auto mode = request.phase == ToolAccessPhase::Prepare ? ResourceAccessMode::Read : ResourceAccessMode::Write;
        co_return AgentResult<std::vector<ResourceAccess>>::Success({{ledger, ResourceRef{ledger, std::to_string(data->bookings)}, mode}});
      },
      .artifact_store = {
          .preview = [data](ArtifactResolveRequest request) -> Async<ArtifactPreview> {
            const auto found = data->receipts.find(request.storage_handle);
            if (found == data->receipts.end())
              co_return AgentResult<ArtifactPreview>::Failure({.code = ErrorCode::NotFound, .message = "Receipt unavailable"});
            co_return AgentResult<ArtifactPreview>::Success({.artifact = request.artifact, .structured = found->second});
          },
          .selection = [data](ArtifactSelectionRequest request) -> Async<std::string> {
            const auto found = data->receipts.find(request.resource.storage_handle);
            if (found == data->receipts.end())
              co_return AgentResult<std::string>::Failure({.code = ErrorCode::NotFound, .message = "Receipt unavailable"});
            Check(request.parts.empty() && Field(found->second, "equipment") == "caliper", "Wrong receipt excerpt requested");
            co_return AgentResult<std::string>::Success(receipt_excerpt);
          }},
      .on_error = [host](std::exception_ptr failure) { if (auto shop = host.lock()) shop->failure = failure; }}));
  Take(runtime->RegisterModel({{model_id, 1}, {"workshop", "Equipment assistant", "local"}, std::make_shared<LocalModel>()}));
  Take(runtime->RegisterContextProvider(handbook, [](ContextRequest request) -> Async<ContextSnapshot> {
    Check(request.actor.principal == technician && request.recipient == model_id && request.selected == std::vector{policy},
        "The provider received an unauthorized or different context request");
    co_return AgentResult<ContextSnapshot>::Success({.fragments = {{policy, {ModelContent::Role::Context, std::string(policy_text)}}}});
  }));
  Take(runtime->RegisterContentComponent({reservation, FieldSchema("equipment"),
      {{"explain", {JsonValue{true}}, [](ContentActionContext context, JsonValue) -> Async<ContentActionPlan> {
        Check(Field(context.block->data, "equipment") == "caliper", "The component action was bound to a different item");
        co_return AgentResult<ContentActionPlan>::Success(SendInput{.text = "Explain this reservation"});
      }}}}));
  Take(runtime->RegisterArtifactAction({receipt_type, "audit", {JsonValue{true}},
      [](ArtifactActionContext context, JsonValue) -> Async<SendInput> {
        co_return AgentResult<SendInput>::Success({.text = "Audit this receipt", .context = {{context.artifact}}});
      }}));
  Take(runtime->RegisterTool({.id = reserve, .version = 1, .description = "Reserve workshop equipment",
      .input_schema = FieldSchema("equipment"), .output_schema = FieldSchema("equipment"), .effect = ToolEffect::WriteResource,
      .prepare = [](ToolPrepareContext context, ToolRequest request) -> Async<PreparedChange> {
        Check(context.readable.size() == 1 && context.readable.front().mode == ResourceAccessMode::Read,
            "Preparing the reservation acquired a write grant");
        co_return AgentResult<PreparedChange>::Success({.preview = {.title = "Reserve caliper"},
            .targets = {{ledger, context.readable.front().current}}, .execution_data = std::move(request.arguments),
            .undo_support = UndoSupport::None});
      },
      .execute = [data](ToolContext context, PreparedToolCall call) -> Async<ToolExecutionResult> {
        const auto access = context.Resources();
        Check(access.size() == 1 && access[0].resource == ledger && access[0].mode == ResourceAccessMode::Write &&
            access[0].current == call.change.targets.at(0).expected, "Execution did not receive the approved write baseline");
        auto booking = std::move(call.change.execution_data);
        const auto handle = context.CallId().value;
        Check(data->receipts.emplace(handle, booking).second, "The reservation was written twice");
        ++data->bookings;
        CustomBlock card{reservation, booking, "Equipment reservation"};
        ArtifactDraft receipt{.type = receipt_type, .name = "Reservation receipt", .storage_handle = handle};
        co_return AgentResult<ToolExecutionResult>::Success({.outcome = ToolOutcome::Succeeded, .model_value = std::move(booking),
            .blocks = {{{}, std::move(card)}}, .new_artifacts = {std::move(receipt)},
            .effects = {{ledger, ResourceEffectState::Applied, access[0].current, ResourceRef{ledger, std::to_string(data->bookings)}}}});
      }}));
  Take(runtime->RegisterAgent({{.ref = {general, 1}, .display_name = "General assistant", .model = model_id,
      .tools = {reserve}, .content_types = {reservation}, .context = {handbook}}}));
  Take(runtime->RegisterAgent({{.ref = {inventory, 1}, .display_name = "Inventory assistant", .model = model_id,
      .tools = {reserve}, .content_types = {reservation}, .context = {handbook}}}));
  Take(runtime->RegisterAgent({{.ref = {receipt_side_chat, 1}, .display_name = "Receipt assistant", .model = model_id}}));
  Take(runtime->ValidateRegistration());
  return runtime;
}

using VisibleChat = State<std::shared_ptr<Conversation>>;

Task<void> OpenWorkshop(std::shared_ptr<Workshop> shop, TaskScope tasks, std::shared_ptr<VisibleChat> visible,
                        std::shared_ptr<AgentChatSelection> selection) {
  try {
    auto runtime = ConfigureWorkshop(shop, tasks);
    Take(co_await runtime->StartAsync());
    CreateSessionRequest private_chat{.options = {.agent = inventory, .project = restricted, .title = "Restricted inventory",
        .retention = SessionRetention::Ephemeral}};
    const auto private_created = Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(private_chat)));
    const auto id = std::get<SessionCreated>(private_created.outcome).session;
    Take(co_await runtime->Sessions().OpenAsync(id));
    shop->ledger->principal = technician;
    Take(co_await runtime->RefreshAccessAsync());
    const auto project = co_await runtime->Projects().ReadAsync(restricted);
    Check(!project.Succeeded() && project.Error().code == ErrorCode::PermissionDenied, "Restricted project was readable");
    const auto denied = co_await runtime->Sessions().OpenAsync(id);
    Check(!denied.Succeeded() && denied.Error().code == ErrorCode::PermissionDenied, "Restricted inventory chat could be opened");
    auto listed = Take(co_await runtime->Sessions().ListAsync({.include_ephemeral = true}, {}));
    Check(std::ranges::none_of(listed.sessions, [&](const auto& row) { return row.id == id; }), "Restricted chat was listed");
    CreateSessionRequest request{.options = {.agent = general, .project = workshop, .retention = SessionRetention::Ephemeral},
        .initial = SendInput{.text = "Reserve caliper", .context = {policy}}};
    const auto created = Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request)));
    auto chat = Take(co_await runtime->Sessions().OpenAsync(std::get<SessionCreated>(created.outcome).session));
    *selection = {runtime, receipt_side_chat};
    shop->runtime = std::move(runtime);
    shop->chat = chat;
    *visible = std::move(chat);
  } catch (...) { shop->failure = std::current_exception(); }
}

void VerifyWorkshop() {
  const auto shop = std::make_shared<Workshop>();
  std::optional<TaskScope> tasks;
  const Application app{[]() -> View {
    const auto visible = UseService<VisibleChat>();
    if (visible && visible->Get()) {
      const auto host = std::weak_ptr{UseService<Workshop>()};
      return ProvideEnvironment(AgentChatLayoutStyle{.show_run_activity = true},
          AgentChat(visible->Get(), {
              .draft_sources = *UseService<DraftSources>(),
              .render_artifact = [host](const ArtifactPreview& preview, std::shared_ptr<Conversation> chat,
                  DraftSources draft) -> std::optional<View> {
                if (preview.type != receipt_type || !preview.structured) return {};
                auto registered = host.lock();
                Check(registered && registered->draft_sources, "The workshop review did not receive a draft");
                const auto dispatch = [chat, ref = preview.artifact](CommandId id, ArtifactAction action) -> Async<Receipt<RunAccepted>> {
                  if (action.artifact != ref)
                    co_return AgentResult<Receipt<RunAccepted>>::Failure({ErrorCode::PermissionDenied, "Different receipt version"});
                  co_return co_await chat->DispatchArtifactActionAsync(std::move(id), std::move(action));
                };
                registered->dispatch_receipt = dispatch;
                const auto tasks = UseApplicationTaskScope();
                return Column{
                  Text("Workshop receipt: " + Field(*preview.structured, "equipment")),
                  Button("Audit receipt").OnClick([tasks, dispatch, ref = preview.artifact, host] {
                    tasks.Launch([dispatch, ref, host]() -> Task<void> {
                      try { Take(co_await dispatch(NewCommandId(), {ref, "audit", JsonValue{true}})); }
                      catch (...) { if (auto shop = host.lock()) shop->failure = std::current_exception(); }
                    });
                  }),
                  AgentChatSourceActions(chat, SourceSlice{preview.artifact}, draft),
                };
              },
              .render_block = [host](const Message& message, const ContentBlock& block,
                  std::shared_ptr<Conversation> chat) -> std::optional<View> {
                const auto* card = std::get_if<CustomBlock>(&block.body);
                if (!card || card->type != reservation) return {};
                Check(message.ref.version, "The reservation card did not receive a versioned origin");
                const auto tasks = UseApplicationTaskScope();
                return Column{
                  Text("Reserved equipment: " + Field(card->data, "equipment")),
                  Button("Explain reservation").OnClick([tasks, chat, origin = CustomBlockRef{message.ref, block.id}, host] {
                    tasks.Launch([chat, origin, host]() -> Task<void> {
                      try { Take(co_await chat->DispatchContentActionAsync(NewCommandId(), {origin, "explain", JsonValue{true}})); }
                      catch (...) { if (auto shop = host.lock()) shop->failure = std::current_exception(); }
                    });
                  }),
                };
              },
              .tool_label = [](const ToolRef& tool) -> std::string {
                return tool.id == reserve ? "Equipment reservation" : std::string{};
              }}));
    }
    return Text("Opening workshop");
  }, {.application_hooks = {[&](ApplicationContext& context) {
    auto visible = std::make_shared<VisibleChat>(std::shared_ptr<Conversation>{});
    auto selection = std::make_shared<AgentChatSelection>();
    auto sources = std::make_shared<DraftSources>(AgentChatDraftSources{});
    shop->draft_sources = *sources;
    context.Provide(sources);
    context.Provide(visible);
    context.Provide(shop);
    context.Provide(selection);
    tasks = UseApplicationTaskScope();
    tasks->Launch([shop, visible, selection, scope = *tasks] { return OpenWorkshop(shop, scope, visible, selection); });
  }}}};
  UiTest ui(app, {.viewport = {1100.0F, 900.0F},
      .resource_provider = std::make_shared<agentkit_test::PackagedResources>()});
  const auto wait = [&](const auto& ready, const char* message) {
    const auto end = std::chrono::steady_clock::now() + 15s;
    while (!shop->failure && !ready() && std::chrono::steady_clock::now() < end) {
      ui.Pump(10ms);
      std::this_thread::sleep_for(2ms);
    }
    if (shop->failure) std::rethrow_exception(shop->failure);
    Check(ready(), message);
  };
  const auto tap = [&](const char* label) {
    auto button = ui.Find(UiSelector::Text(label));
    wait([&] { return button.Exists() && button.One().enabled && button.One().in_viewport; }, label);
    button.Tap();
  };
  const auto run = [&](const auto& job) {
    bool done = false;
    tasks->Launch([&]() -> Task<void> {
      try { co_await job(); }
      catch (...) { shop->failure = std::current_exception(); }
      done = true;
    });
    wait([&] { return done; }, "Host operation did not settle");
  };
  const auto settled = [&](std::size_t runs) {
    wait([&] {
      const auto snapshot = shop->chat->Snapshot();
      return snapshot && snapshot->runs.size() == runs && !snapshot->summary.active_run;
    }, "Workshop chat did not finish the expected Run");
    const auto snapshot = shop->chat->Snapshot();
    const auto& completed = snapshot->runs.back();
    if (completed.state != RunState::Succeeded)
      throw std::runtime_error(completed.failure ? completed.failure->message : "Workshop Run did not succeed");
  };
  wait([&] { return ui.Find(UiSelector::Text("Approve")).Exists(); }, "Reservation approval did not appear");
  Check(shop->ledger->bookings == 0, "The reservation was applied before approval");
  tap("Approve");
  settled(1);
  Check(shop->ledger->bookings == 1 && shop->chat->Snapshot()->artifacts.size() == 1, "Approved reservation did not issue exactly one receipt");
  const auto reservation_snapshot = shop->chat->Snapshot();
  const auto activity = ui.Find(UiSelector::Key("chat-turn/" + reservation_snapshot->runs.front().id.value));
  const auto status = activity.Find(UiSelector::Key("chat-assistant-status"));
  const auto work = activity.Find(UiSelector::Key("chat-run-activity"));
  wait([&] { return activity.Exists() && status.Exists() && status.One().text == "Completed" &&
      work.Exists() && work.One().in_viewport; }, "The completed reservation activity did not mount");
  work.Tap();
  Check(reservation_snapshot->tools.size() == 1, "The reservation did not retain exactly one tool call");
  const auto tool = activity.Find(UiSelector::Key(reservation_snapshot->tools.front().id.value));
  wait([&] { return activity.Exists() && tool.Exists(); }, "The reservation tool activity did not expand");
  const auto label = tool.One().text.value_or("");
  if (label != "Equipment reservation \xc2\xb7 Completed")
    throw std::runtime_error("Unexpected reservation activity: " + label);
  work.Tap();
  wait([&] { return ui.Find(UiSelector::Text("Reserved equipment: caliper")).Exists(); }, "The host content renderer did not mount");
  tap("Explain reservation");
  settled(2);
  std::cout << "Reservation approved and custom component action completed" << std::endl;

  auto composer = ui.Find(UiSelector::Key("composer"));
  composer.Tap();
  composer.ReplaceText("Keep the workshop draft");
  const auto ref = shop->chat->Snapshot()->artifacts.front();
  auto review = ui.FindSemantics(UiSemanticSelector::AllOf(UiSemanticSelector::Role(SemanticRole::Button),
      UiSemanticSelector::Label("Review Reservation receipt v1")));
  wait([&] { return review.Exists(); }, "Receipt review action was missing from the transcript");
  review.PerformSemanticAction({SemanticActionKind::Activate, {}});
  const auto receipt = ui.Find(UiSelector::Text("Workshop receipt: caliper"));
  wait([&] { return receipt.Exists() && receipt.One().in_viewport; }, "The registered artifact renderer did not mount");
  run([&]() -> Task<void> {
    ArtifactAction wrong_version{{ref.id, ref.version + 1}, "audit", JsonValue{true}};
    const auto denied = co_await shop->dispatch_receipt(NewCommandId(), std::move(wrong_version));
    Check(!denied.Succeeded() && denied.Error().code == ErrorCode::PermissionDenied, "The renderer dispatched a different version");
  });
  tap("Audit receipt");
  settled(3);
  std::cout << "Registered receipt renderer and version-bound action completed" << std::endl;
  tap("Side chat");
  const auto side_chat = ui.Find(UiSelector::Key("side-chat-dialog"));
  const auto excerpt = side_chat.Find(UiSelector::Text(receipt_excerpt));
  const auto question = side_chat.Find(UiSelector::Key("side-chat-question"));
  wait([&] { return excerpt.Exists() && question.Exists() && question.One().focused; }, "Authorized receipt excerpt did not open");
  question.ReplaceText(receipt_question);
  tap("Ask");
  const auto reply = side_chat.Find(UiSelector::Value(receipt_reply));
  const auto bring_back = side_chat.Find(UiSelector::Key("side-chat-return-reply"));
  wait([&] { return reply.Exists() && bring_back.Exists(); }, "Receipt reply was not offered for the draft");
  std::shared_ptr<Conversation> receipt_chat;
  SourceSlice reply_source;
  run([&]() -> Task<void> {
    const auto list = Take(co_await shop->runtime->Sessions().ListAsync({.agent = receipt_side_chat, .include_ephemeral = true}, {}));
    Check(list.sessions.size() == 1, "The receipt side chat was not found");
    receipt_chat = Take(co_await shop->runtime->Sessions().OpenAsync(list.sessions.front().id));
    const auto snapshot = receipt_chat->Snapshot();
    const auto answer = std::ranges::find_if(snapshot->messages, [](const auto& message) {
      return message.author == MessageAuthor::Agent && message.state == MessageState::Complete;
    });
    Check(answer != snapshot->messages.end(), "The receipt reply did not finish");
    reply_source = {answer->ref};
    Check(Take(co_await receipt_chat->ReadSelectionAsync(reply_source)) == receipt_reply, "The reply reference resolved a different passage");
  });
  run([&]() -> Task<void> {
    shop->ledger->receipts_visible = false;
    Take(co_await shop->runtime->RefreshAccessAsync());
    const auto denied = co_await shop->chat->PreviewArtifactAsync(ref);
    Check(!denied.Succeeded() && denied.Error().code == ErrorCode::PermissionDenied, "Revoked receipt was still readable");
    const auto selection = co_await shop->chat->ReadSelectionAsync({ref});
    Check(!selection.Succeeded() && selection.Error().code == ErrorCode::PermissionDenied,
        "Revoked receipt could still be selected");
    const auto selected_reply = co_await receipt_chat->ReadSelectionAsync(reply_source);
    Check(!selected_reply.Succeeded() && selected_reply.Error().code == ErrorCode::PermissionDenied,
        "The old reply reference bypassed source revocation");
    ArtifactAction action{ref, "audit", JsonValue{true}};
    const auto blocked = co_await shop->dispatch_receipt(NewCommandId(), std::move(action));
    Check(!blocked.Succeeded() && blocked.Error().code == ErrorCode::PermissionDenied, "A retained renderer bypassed revocation");
  });
  const auto withdrawn = side_chat.Find(UiSelector::Text("Selection access changed. Close this side chat and select the text again."));
  wait([&] { return withdrawn.Exists(); }, "Revocation did not replace the open side chat with its access notice");
  Check(!excerpt.Exists() && !reply.Exists() && !bring_back.Exists() &&
      !side_chat.Find(UiSelector::Key("side-chat-composer")).Exists(),
      "Revocation left receipt content or a side-chat action visible");
  side_chat.Find(UiSelector::Text("Close")).Tap();
  wait([&] { return !side_chat.Exists() && !receipt.Exists(); }, "Revocation did not dismiss the open receipt");
  Check(composer.One().value == "Keep the workshop draft" && !(*shop->draft_sources)->conclusion,
      "Revocation changed the main draft");
  composer.Tap();
  composer.ReplaceText("Reserve oscilloscope");
  tap("Send");
  settled(4);
  const auto snapshot = shop->chat->Snapshot();
  Check(snapshot->tools.back().reason && snapshot->tools.back().reason->code == ErrorCode::PermissionDenied &&
      shop->ledger->bookings == 1, "Tool denial did not preserve the original inventory");
  Check(snapshot->artifacts.empty(), "The snapshot exposed a revoked receipt");
  run([&]() -> Task<void> {
    shop->ledger->receipts_visible = true;
    Take(co_await shop->runtime->RefreshAccessAsync());
  });
  wait([&] {
    const auto refreshed = shop->chat->Snapshot();
    return refreshed && refreshed->artifacts == std::vector{ref};
  }, "Restoring access did not expose the original receipt");
  run([&]() -> Task<void> { Take(co_await shop->runtime->CloseAsync()); });
  shop->dispatch_receipt = {};
  shop->chat.reset();
  shop->runtime.reset();
}

} // namespace

int main() {
  try {
    VerifyWorkshop();
    std::cout << "External workshop application integration passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
