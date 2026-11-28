#include "run_plan.h"

#include "preview_json.h"
#include "preview_theme.h"

#include <algorithm>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
namespace strings = ::app::strings;

kit::JsonSchema PlanSchema() {
  return {DecodeJson(nlohmann::json::parse(R"({
    "type": "object",
    "required": ["steps"],
    "additionalProperties": false,
    "properties": {
      "steps": {
        "type": "array", "minItems": 2, "maxItems": 8,
        "items": {
          "type": "object",
          "required": ["title", "status"],
          "additionalProperties": false,
          "properties": {
            "title": {"type": "string", "minLength": 1, "maxLength": 160},
            "status": {"type": "string", "enum": ["pending", "in_progress", "completed", "blocked"]},
            "detail": {"type": "string", "maxLength": 320}
          }
        }
      }
    }
  })"))};
}

bool BelongsToRun(const kit::SessionSnapshot& snapshot, const kit::RunId& root, kit::RunId run) {
  while (run != root) {
    const auto child = std::ranges::find(snapshot.runs, run, &kit::RunSnapshot::id);
    if (child == snapshot.runs.end() || !child->parent) return false;
    run = *child->parent;
  }
  return true;
}

View PlanAction(std::vector<View> content, StringVariant label, std::function<void()> activate,
    const ThemeSpec& theme, std::optional<bool> expanded = {}) {
  return Row(std::move(content)).OnClick(activate).On<ViewEvents::KeyDown>([activate](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    activate();
    return true;
  }).With(Frame{.min_height = 30.0F}, Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center),
      Padding(EdgeInsets::Symmetric(6.0F, 0.0F)), PreviewRowIndication(theme, 5.0F), Focusable{},
      Semantics{.role = SemanticRole::Button, .label = std::move(label), .expanded = expanded},
      PointerCursor(PointerCursorKind::Hand));
}

} // namespace

kit::AgentResult<void> RegisterRunPlan(kit::AgentRuntime& runtime) {
  const auto schema = PlanSchema();
  auto registered = runtime.RegisterContentComponent({RunPlanContent, schema});
  if (!registered.Succeeded()) return registered;
  return runtime.RegisterTool({
      .id = UpdateRunPlan, .version = 1,
      .description = "Publish or replace the current turn's visible task plan for complex work. Send the complete ordered list "
          "of short steps, with accurate statuses after each milestone and an optional brief result per step.",
      .input_schema = schema, .effect = kit::ToolEffect::Read,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.execution_data = std::move(request.arguments)});
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto plan = EncodeJson(call.change.execution_data);
        const auto& steps = plan.at("steps");
        auto fallback = UseService<Resources>()->GetString(strings::ui_run_plan);
        for (const auto& step : steps) fallback += "\n" + step.at("title").get<std::string>();
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
            .model_value = DecodeJson({{"recorded", true}}),
            .blocks = {{{"plan"}, kit::CustomBlock{RunPlanContent, std::move(call.change.execution_data), std::move(fallback)}}}});
      }});
}

Task<void> RecordRunPlan(kit::RunContext run, std::string_view revision, std::vector<RunPlanStep> steps) {
  auto rows = nlohmann::json::array();
  for (auto& step : steps) {
    rows.push_back({{"title", std::move(step.title)}, {"status", step.status}, {"detail", std::move(step.detail)}});
  }
  auto receipt = co_await run.StartToolAsync({"ganwork:plan/" + run.Id().value + "/" + std::string(revision)},
      {UpdateRunPlan, DecodeJson({{"steps", std::move(rows)}})});
  if (!receipt.Succeeded()) co_return;
  if (const auto* call = std::get_if<kit::ToolCallAccepted>(&receipt.Value().outcome))
    static_cast<void>(co_await run.WaitForToolAsync(call->call));
}

[[huxerui::composable]]
View RunPlanCard(kit::RunId run, kit::JsonValue plan, std::shared_ptr<kit::Conversation> conversation,
    std::shared_ptr<kit::AgentRuntime> runtime, std::function<void(kit::SessionId, kit::InteractionId)> open_request) {
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto snapshot = conversation->Snapshot();
  const auto found = snapshot ? std::ranges::find(snapshot->runs, run, &kit::RunSnapshot::id) :
      std::vector<kit::RunSnapshot>::const_iterator{};
  const auto state = snapshot && found != snapshot->runs.end() ? found->state : kit::RunState::Succeeded;
  const bool finished = state == kit::RunState::Succeeded || state == kit::RunState::Failed || state == kit::RunState::Cancelled;
  std::optional<kit::CollaborationMessageId> awaited_reply;
  if (!finished && found != snapshot->runs.end()) {
    for (const auto& wait : found->waits) {
      if (const auto* message = std::get_if<kit::CollaborationMessageId>(&wait.subject)) {
        awaited_reply = *message;
        break;
      }
    }
  }
  const auto remote = UseState(std::shared_ptr<const kit::SessionSnapshot>{});
  const auto tasks = UseTaskScope();
  Lifecycle([runtime, remote, tasks, awaited_reply] {
    remote = std::shared_ptr<const kit::SessionSnapshot>{};
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto worker = tasks.Launch([runtime, remote, awaited_reply, subscription]() -> Task<void> {
      if (!awaited_reply) co_return;
      auto record = co_await runtime->Collaboration().ReadMessageAsync(*awaited_reply);
      if (!record.Succeeded()) co_return;
      auto target = co_await runtime->Sessions().OpenAsync(record.Value().summary.to);
      if (!target.Succeeded()) co_return;
      auto observed = co_await target.Value()->ObserveAsync([remote](auto change) {
        remote = change.Succeeded() ? std::move(change).Value() : std::shared_ptr<const kit::SessionSnapshot>{};
      });
      if (observed.Succeeded()) *subscription = std::move(observed).Value();
    });
    return [worker, subscription] { worker.Cancel(); subscription->reset(); };
  }, runtime, awaited_reply);
  const auto live_expanded = UseState(true);
  const auto history_expanded = UseState(false);
  const auto disclosure = finished ? history_expanded : live_expanded;
  const bool open = disclosure.Get();
  const auto data = EncodeJson(plan);
  const auto& steps = data.at("steps");
  const auto count = std::ranges::count_if(steps, [](const auto& step) { return step.at("status") == "completed"; });
  const auto progress = StringVariant::Format(strings::ui_run_plan_progress_0_1, count, steps.size());
  std::vector<View> card{PlanAction({
      Image(open ? ::app::images::chevron_down : ::app::images::chevron_right).Tint(colors.on_surface_variant)
          .With(Frame{.width = 12.0F, .height = 12.0F}),
      Text(strings::ui_run_plan, TextRole::Label).Style({Font::System(13.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
          .With(Grow()),
      Text(progress, TextRole::Label).With(Foreground(colors.on_surface_variant)),
  }, strings::ui_run_plan, [disclosure] { disclosure = !disclosure.Get(); }, theme, open).Key("run-plan-toggle")};

  if (open) {
    std::vector<const kit::InteractionSnapshot*> pending;
    if (snapshot && !finished) {
      for (const auto& item : snapshot->interactions) {
        if (item.state == kit::InteractionState::Pending && item.run && BelongsToRun(*snapshot, run, *item.run))
          pending.push_back(&item);
      }
    }
    const auto receiver = remote.Get();
    if (awaited_reply && receiver) {
      const auto receiving = std::ranges::find(receiver->runs, awaited_reply, &kit::RunSnapshot::work_request);
      if (receiving != receiver->runs.end()) {
        for (const auto& item : receiver->interactions) {
          if (item.state == kit::InteractionState::Pending && item.run && BelongsToRun(*receiver, receiving->id, *item.run))
            pending.push_back(&item);
        }
      }
    }
    const auto show_requests = [&](std::vector<View>& views) {
      for (const auto* item : pending) {
        const auto title = std::visit([](const auto& body) {
          using Body = std::decay_t<decltype(body)>;
          if constexpr (std::is_same_v<Body, kit::FormRequest>) return body.title;
          else return body.preview.title;
        }, item->body);
        const StringVariant label = title.empty() ? StringVariant{strings::ui_open_request} : StringVariant{title};
        views.push_back(Row{PlanAction({
            Text(label, TextRole::Label).With(Foreground(colors.primary)),
            Image(::app::images::chevron_right).Tint(colors.primary).With(Frame{.width = 12.0F, .height = 12.0F}),
        }, label, [open_request, owner = item->session, id = item->id] { open_request(owner, id); }, theme)
            .Key("run-plan-approval/" + item->id.value)});
      }
      pending.clear();
    };
    for (const auto& step : steps) {
      const auto status = step.at("status").get<std::string>();
      const bool active = status == "in_progress";
      StringVariant label = strings::ui_run_plan_not_started;
      auto ink = colors.on_surface_variant;
      if (status == "completed") {
        label = strings::ui_completed;
        ink = PreviewSuccess(theme.brightness);
      } else if (status == "blocked" || (active && !pending.empty())) {
        label = active ? strings::ui_run_plan_waiting : strings::ui_blocked;
        ink = PreviewWarning(theme.brightness);
      } else if (active && !finished) {
        label = strings::ui_in_progress;
        ink = colors.primary;
      } else if (active) {
        label = state == kit::RunState::Failed ? strings::ui_failed : state == kit::RunState::Cancelled ?
            strings::ui_cancelled : strings::ui_run_plan_incomplete;
      }
      const auto detail = step.value("detail", std::string{});
      std::vector<View> row{Row{
          Spacer().With(Frame{.width = 6.0F, .height = 6.0F}, Background(ink), CornerRadius(3.0F)),
          Text(step.at("title").get<std::string>(), TextRole::Label).With(Grow()),
          Text(label, TextRole::Label).With(Foreground(ink)).Key("run-plan-step-status"),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))};
      if (!detail.empty()) {
        row.push_back(Text(detail, TextRole::Label).With(Foreground(colors.on_surface_variant), Padding(EdgeInsets{.left = 14.0F})));
      }
      if (active) show_requests(row);
      card.push_back(Column(std::move(row)).With(Spacing(3.0F), Padding(EdgeInsets{.top = 4.0F, .right = 6.0F, .left = 6.0F}),
          CrossAlign(CrossAxisAlignment::Stretch)));
    }
    show_requests(card);
  }
  return Column(std::move(card)).With(Spacing(4.0F), Padding(6.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Background(colors.surface_container_low), Border{colors.outline, 1.0F}, CornerRadius(8.0F))
      .Key("run-plan/" + run.value);
}

} // namespace agentkit_preview
