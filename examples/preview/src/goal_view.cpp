#include <app_resources.h>

#include "goal_view.h"
#include "preview_theme.h"
#include "release_workflow.h"

#include <huxerui/huxerui.h>
#include <algorithm>
#include <charconv>
#include <limits>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;

struct GoalDraft {
  TextEditingValue objective;
  TextEditingValue conditions;
  TextEditingValue tokens;
  TextEditingValue turns;
  TextEditingValue tools;
  TextEditingValue minutes;
  TextEditingValue slides;
  std::optional<kit::GoalRef> definition;
  bool require_approval = true;
  bool new_goal = false;
  bool budget_only = false;
  bool operator==(const GoalDraft&) const = default;
};

struct ReviewedGoalEdit {
  kit::Revision revision;
  kit::GoalEditPreview preview;
};

struct GoalEditorState {
  State<std::optional<GoalDraft>> draft{std::optional<GoalDraft>{}};
  State<std::shared_ptr<const ReviewedGoalEdit>> impact{std::shared_ptr<const ReviewedGoalEdit>{}};
  State<std::string> error{std::string{}};
  State<bool> busy{false};
  State<bool> advanced{false};
};

StringVariant Status(kit::GoalState state) {
  switch (state) {
    case kit::GoalState::Proposed: return ::app::strings::ui_ready_to_enable;
    case kit::GoalState::Active: return ::app::strings::ui_active;
    case kit::GoalState::Blocked: return ::app::strings::ui_blocked;
    case kit::GoalState::Paused: return ::app::strings::ui_paused;
    case kit::GoalState::BudgetLimited: return ::app::strings::ui_budget_reached;
    case kit::GoalState::Verifying: return ::app::strings::ui_verifying_evidence;
    case kit::GoalState::AwaitingCompletionApproval: return ::app::strings::ui_awaiting_completion_approval;
    case kit::GoalState::Completed: return ::app::strings::ui_completed;
    case kit::GoalState::Cancelled: return ::app::strings::ui_cancelled;
  }
  return {};
}

StringVariant RunStatus(std::optional<kit::RunState> state) {
  if (!state) return ::app::strings::ui_run_idle;
  switch (*state) {
    case kit::RunState::Running: return ::app::strings::ui_run_running;
    case kit::RunState::WaitingForInput: return ::app::strings::ui_run_waiting_for_input;
    case kit::RunState::WaitingForReply: return ::app::strings::ui_run_waiting_for_reply;
    case kit::RunState::WaitingForDependency: return ::app::strings::ui_run_waiting_for_dependency;
    case kit::RunState::Cancelling: return ::app::strings::ui_run_stopping;
    case kit::RunState::Succeeded: return ::app::strings::ui_run_succeeded;
    case kit::RunState::Failed: return ::app::strings::ui_run_failed;
    case kit::RunState::Cancelled: return ::app::strings::ui_run_cancelled;
  }
  return ::app::strings::ui_run_idle;
}

[[huxerui::composable]]
View GoalArtifactLabel(std::shared_ptr<kit::AgentRuntime> runtime, kit::ArtifactRef artifact) {
  const auto scope = UseTaskScope();
  const auto name = UseState(std::string{});
  Lifecycle([runtime, artifact, scope, name] {
    name = std::string{};
    auto worker = scope.Launch([runtime, artifact, name]() -> Task<void> {
      auto record = co_await runtime->Artifacts().ReadAsync(artifact);
      if (record.Succeeded()) name = record.Value().metadata.name;
    });
    return [worker] { worker.Cancel(); };
  }, runtime, artifact);
  return Text(StringVariant::Format(::app::strings::ui_0_v_1, name.Get().empty() ? UseString(::app::strings::ui_artifact) : name.Get(), artifact.version),
      TextRole::Label);
}

GoalDraft EditDraft(const kit::GoalSnapshot& goal, bool budget_only) {
  const auto field = [](const auto& amount) { return TextEditingValue::FromText(amount ? std::to_string(*amount) : ""); };
  const auto& budget = goal.spec.budget;
  const auto& definition = goal.spec.definition;
  const auto* conditions = std::get_if<std::string>(&definition.completion_conditions.value);
  const auto minutes = budget.deadline ? std::optional{std::chrono::ceil<std::chrono::minutes>(*budget.deadline).count()} : std::nullopt;
  return {
      .objective = TextEditingValue::FromText(definition.objective),
      .conditions = TextEditingValue::FromText(conditions ? *conditions : std::string{}),
      .tokens = field(budget.tokens), .turns = field(budget.model_turns),
      .tools = field(budget.tool_calls), .minutes = field(minutes),
      .slides = field(ReleaseSlideCount(definition.completion_conditions)), .definition = goal.ref,
      .require_approval = definition.completion == kit::GoalCompletionPolicy::RequireConfirmation,
      .budget_only = budget_only};
}

kit::GoalDefinition ReadDefinition(const GoalDraft& draft, kit::GoalDefinition definition) {
  definition.objective = draft.objective.text;
  definition.completion = draft.require_approval ? kit::GoalCompletionPolicy::RequireConfirmation :
      kit::GoalCompletionPolicy::Verified;
  if (IsWorkspaceRelease(definition)) {
    auto& settings = std::get<kit::JsonValue::Object>(definition.completion_conditions.value);
    if (draft.slides.text.empty()) {
      settings.erase("slide_count");
    } else {
      std::int64_t count = 0;
      const auto parsed = std::from_chars(draft.slides.text.data(), draft.slides.text.data() + draft.slides.text.size(), count);
      if (parsed.ec != std::errc{} || parsed.ptr != draft.slides.text.data() + draft.slides.text.size() || count < 1 || count > 30) {
        throw std::invalid_argument("Slide count must be a whole number between 1 and 30.");
      }
      settings.insert_or_assign("slide_count", kit::JsonValue{count});
    }
  } else {
    const auto* conditions = std::get_if<std::string>(&definition.completion_conditions.value);
    if ((conditions && *conditions != draft.conditions.text) ||
        (std::holds_alternative<std::nullptr_t>(definition.completion_conditions.value) && !draft.conditions.text.empty())) {
      definition.completion_conditions = draft.conditions.text.empty() ? kit::JsonValue{} : kit::JsonValue{draft.conditions.text};
    }
  }
  return definition;
}

kit::Budget ReadBudget(const GoalDraft& draft, const kit::Budget& prior) {
  const auto limit = [](const TextEditingValue& input, const char* label) -> std::optional<std::uint64_t> {
    if (input.text.empty()) return {};
    std::uint64_t number = 0;
    const auto result = std::from_chars(input.text.data(), input.text.data() + input.text.size(), number);
    if (result.ec != std::errc{} || result.ptr != input.text.data() + input.text.size()) {
      throw std::invalid_argument(std::string(label) + " must be a whole number.");
    }
    return number;
  };
  auto budget = prior;
  budget.tokens = limit(draft.tokens, "Tokens");
  budget.model_turns = limit(draft.turns, "Model turns");
  budget.tool_calls = limit(draft.tools, "Tool calls");
  const auto minutes = limit(draft.minutes, "Minutes");
  if (minutes && !*minutes) throw std::invalid_argument("Duration must be at least one minute.");
  if (minutes && *minutes > static_cast<std::uint64_t>(std::numeric_limits<kit::Duration::rep>::max() / 60000)) {
    throw std::invalid_argument("The duration is too large.");
  }
  if (!prior.deadline || !minutes || std::chrono::ceil<std::chrono::minutes>(*prior.deadline).count() != *minutes) {
    budget.deadline = minutes ? std::optional{kit::Duration(static_cast<kit::Duration::rep>(*minutes * 60000))} : std::nullopt;
  }
  return budget;
}

std::string UsageText(const kit::GoalSnapshot& goal) {
  const auto limit = [](std::uint64_t used, const auto& maximum) {
    return std::to_string(used) + (maximum ? " / " + std::to_string(*maximum) : "");
  };
  const auto& budget = goal.spec.budget;
  return limit(goal.usage.input_tokens + goal.usage.output_tokens, budget.tokens) + " tokens   \xc2\xb7   " +
      limit(goal.usage.model_turns, budget.model_turns) + " model turns   \xc2\xb7   " +
      limit(goal.usage.tool_calls, budget.tool_calls) + " tool calls";
}

[[huxerui::composable]]
View GoalPanel(std::shared_ptr<kit::Conversation> conversation, std::shared_ptr<kit::AgentRuntime> runtime,
    std::optional<kit::RunState> run, std::shared_ptr<GoalEditorState> state, bool docked,
    std::function<void()> close, std::function<void(kit::PendingApproval)> open_completion) {
  const auto scope = UseTaskScope();
  const auto commands = UseApplicationTaskScope();
  const auto snapshot = UseState(std::shared_ptr<const std::optional<kit::GoalSnapshot>>{});
  const auto editor = state->draft;
  const auto impact = state->impact;
  const auto error = state->error;
  const auto busy = state->busy;
  const auto advanced = state->advanced;
  const auto objective_focus = UseViewHandle();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto button_style = UseEnvironment<ButtonStyle>();
  const auto primary = PreviewPrimaryButton(button_style, colors);
  const auto secondary = PreviewSecondaryButton(button_style, colors);
  auto fields = UseEnvironment<TextFieldStyle>();
  fields.show_label = false;
  auto destructive = secondary;
  destructive.label_style.foreground = colors.error;
  Lifecycle([scope, conversation, snapshot, error] {
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto worker = scope.Launch([conversation, snapshot, error, subscription]() -> Task<void> {
      auto observed = co_await conversation->Goals().ObserveAsync([snapshot, error](auto change) {
        if (change.Succeeded()) snapshot = std::move(change).Value();
        else { snapshot = std::shared_ptr<const std::optional<kit::GoalSnapshot>>{}; error = change.Error().message; }
      });
      if (observed.Succeeded()) *subscription = std::move(observed).Value();
      else error = observed.Error().message;
    });
    return [worker, subscription] { worker.Cancel(); subscription->reset(); };
  }, conversation);

  const auto now = snapshot.Get();
  const auto goal = now && *now ? &**now : nullptr;
  const auto edit = editor.Get();
  const bool editing_objective = edit && !edit->budget_only;
  Lifecycle([scope, objective_focus, editing_objective] {
    if (editing_objective) scope.Post([objective_focus] { objective_focus.RequestFocus(true); });
  }, editing_objective);
  auto update = [commands, conversation, error, busy, editor, impact](kit::GoalCommand command) {
    if (busy.Get()) return;
    busy = true;
    commands.Launch([conversation, error, busy, editor, impact, command = std::move(command)]() mutable -> Task<void> {
      auto result = co_await conversation->Goals().UpdateAsync(kit::NewCommandId(), std::move(command));
      busy = false;
      if (!result.Succeeded()) { error = result.Error().message; impact = std::shared_ptr<const ReviewedGoalEdit>{}; co_return; }
      error = "";
      editor = std::optional<GoalDraft>{};
      impact = std::shared_ptr<const ReviewedGoalEdit>{};
    });
  };
  std::vector<View> content;
  std::optional<View> footer;
  if (goal || run) content.push_back(Text(RunStatus(run), TextRole::Label)
      .With(Foreground(colors.on_surface_variant)).Key("goal-run-status"));
  if (!error->empty()) content.push_back(Text(error.Get()).With(Foreground(colors.error)).Key("goal-error"));
  if (!now) {
    if (error->empty()) content.push_back(Text(::app::strings::ui_loading_goal));
  } else if (edit) {
    const auto& draft = edit;
    auto field = [&](StringVariant label, TextEditingValue GoalDraft::* member, const char* key, bool multiline = false) {
      const std::size_t minimum_lines = member == &GoalDraft::objective ? 4 : 3;
      auto input = TextField((*draft).*member).Label(label).Variant(TextFieldVariant::Outlined)
          .LineLimits(multiline ? TextFieldLineLimits::MultiLine(minimum_lines, 7) : TextFieldLineLimits::SingleLine())
          .OnChanged([editor, impact, member](const TextEditingValue& value) {
            auto changed = editor.Get();
            if (!changed) return;
            (*changed).*member = value;
            editor = std::move(changed);
            impact = std::shared_ptr<const ReviewedGoalEdit>{};
          }).With(Enabled(!busy.Get())).Key(key);
      if (member == &GoalDraft::objective) input = std::move(input).With(objective_focus);
      return PreviewLabeledField(label, std::move(input), colors);
    };
    if (!draft->budget_only) {
      content.push_back(field(::app::strings::ui_objective, &GoalDraft::objective, "goal-objective", true));
      const auto* definition = !draft->new_goal && *now ? &(*now)->spec.definition : nullptr;
      if (definition && IsWorkspaceRelease(*definition)) {
        content.push_back(field(::app::strings::ui_slide_count, &GoalDraft::slides, "goal-slide-count"));
      } else if (!definition || std::holds_alternative<std::nullptr_t>(definition->completion_conditions.value) ||
                 std::holds_alternative<std::string>(definition->completion_conditions.value)) {
        content.push_back(field(::app::strings::ui_completion_conditions, &GoalDraft::conditions, "goal-conditions", true));
      } else {
        content.push_back(Text(::app::strings::ui_completion_conditions_are_supplied_by_the_workflow, TextRole::Label));
      }
      content.push_back(Text(::app::strings::ui_goal_completion, TextRole::Label));
      content.push_back(Checkbox(::app::strings::ui_require_approval_after_verification, draft->require_approval)
          .OnChanged([editor, impact](bool require) {
            auto changed = editor.Get();
            if (!changed) return;
            changed->require_approval = require;
            editor = std::move(changed);
            impact = std::shared_ptr<const ReviewedGoalEdit>{};
          }).With(Enabled(!busy.Get())).Key("goal-confirm-completion"));
      content.push_back(Text(draft->require_approval ? ::app::strings::ui_goal_confirm_description :
          ::app::strings::ui_goal_automatic_description, TextRole::Label).With(Foreground(colors.on_surface_variant)));
    }
    if (draft->new_goal) {
      const bool expanded = advanced.Get();
      const auto toggle = [advanced] { advanced = !advanced.Get(); };
      content.push_back(Row{
          Image(expanded ? ::app::images::chevron_down : ::app::images::chevron_right).Tint(colors.on_surface_variant)
              .With(Frame{.width = 14.0F, .height = 14.0F}),
          Text(::app::strings::ui_goal_advanced).Style({Font::System(13.0F), colors.on_surface_variant}),
      }.OnClick(toggle).On<ViewEvents::KeyDown>([toggle](const KeyEvent& event) {
        if (event.key != Key::Enter && event.key != Key::Space) return false;
        toggle();
        return true;
      }).With(Frame{.min_height = 32.0F}, Padding(EdgeInsets::Symmetric(6.0F, 0.0F)), Spacing(6.0F),
          CrossAlign(CrossAxisAlignment::Center), PreviewRowIndication(theme, 6.0F), Focusable{},
          PointerCursor(PointerCursorKind::Hand),
          Semantics{.role = SemanticRole::Button, .label = ::app::strings::ui_goal_advanced, .expanded = expanded}).Key("goal-advanced"));
    }
    if (draft->budget_only || (draft->new_goal && advanced.Get())) {
      content.push_back(Text(::app::strings::ui_budget, TextRole::Title));
      content.push_back(Text(::app::strings::ui_leave_optional_limits_empty, TextRole::Label));
      content.push_back(Row {
        field(::app::strings::ui_tokens, &GoalDraft::tokens, "goal-budget-tokens").With(Grow()),
        field(::app::strings::ui_model_turns, &GoalDraft::turns, "goal-budget-turns").With(Grow()),
      }.With(Spacing(10.0F)));
      content.push_back(Row {
        field(::app::strings::ui_tool_calls, &GoalDraft::tools, "goal-budget-tools").With(Grow()),
        field(::app::strings::ui_minutes, &GoalDraft::minutes, "goal-budget-minutes").With(Grow()),
      }.With(Spacing(10.0F)));
    }
    if (const auto preview = impact.Get()) {
      const auto& changes = preview->preview.impact;
      content.push_back(Text(::app::strings::ui_review_changes, TextRole::Title));
      if (changes.retiring_run) content.push_back(Text(::app::strings::ui_the_current_goal_run_will_finish_stopping));
      if (const auto n = changes.in_flight_calls.size()) content.push_back(Text(std::to_string(n) + " in-flight tool calls will be reconciled."));
      if (const auto n = changes.invalidated_interactions.size()) content.push_back(Text(std::to_string(n) + " pending interactions will close."));
      if (const auto n = changes.continuing_runs.size()) content.push_back(Text(std::to_string(n) + " independent runs will continue as started."));
      if (const auto n = changes.retiring_linked_runs.size()) content.push_back(Text(std::to_string(n) + " linked runs will stop for this revision."));
      if (const auto n = changes.withdrawn_requests.size()) content.push_back(Text(std::to_string(n) + " queued requests will be withdrawn."));
      if (changes.resets_completion_review) content.push_back(Text(::app::strings::ui_evidence_must_be_verified_against_the_new));
    }
    const auto commit = [commands, conversation, snapshot, editor, busy, error, impact, update] {
      const auto edit = editor.Get();
      const auto current = snapshot.Get();
      if (!edit || busy.Get() || !current) return;
      try {
        if (edit->new_goal) {
          update(kit::ProposeGoal{{ReadDefinition(*edit, {}), ReadBudget(*edit, {})}});
        } else if (!*current || edit->definition != (*current)->ref) {
          impact = std::shared_ptr<const ReviewedGoalEdit>{};
          error = "The goal changed. Discard this edit and reopen the latest version.";
        } else if (edit->budget_only) {
          update(kit::ChangeGoal{(*current)->ref.id, (*current)->revision, kit::SetGoalBudget{ReadBudget(*edit, (*current)->spec.budget)}});
        } else {
          auto definition = ReadDefinition(*edit, (*current)->spec.definition);
          const auto ref = (*current)->ref;
          const auto revision = (*current)->revision;
          if (const auto preview = impact.Get()) {
            update(kit::ChangeGoal{ref.id, preview->revision, kit::EditGoal{std::move(definition), preview->preview.token}});
          } else {
            busy = true;
            commands.Launch([conversation, ref, revision, definition = std::move(definition), busy, impact, error]() mutable -> Task<void> {
              auto prepared = co_await conversation->Goals().PreviewEditAsync(ref.id, revision, std::move(definition));
              busy = false;
              if (prepared.Succeeded()) {
                impact = std::make_shared<const ReviewedGoalEdit>(ReviewedGoalEdit{revision, std::move(prepared).Value()});
                error = "";
              } else error = prepared.Error().message;
            });
          }
        }
      } catch (const std::invalid_argument& invalid) {
        error = invalid.what();
      }
    };
    const auto action = draft->new_goal ? ::app::strings::ui_save_goal : draft->budget_only ? ::app::strings::ui_save_goal_budget :
        impact.Get() ? ::app::strings::ui_apply_goal_change : ::app::strings::ui_review_goal_change;
    footer = Row {
      ProvideEnvironment(secondary, Button(::app::strings::ui_discard).OnClick([editor, impact, error] {
        editor = std::optional<GoalDraft>{};
        impact = std::shared_ptr<const ReviewedGoalEdit>{};
        error = "";
      }).With(Enabled(!busy.Get()))),
      Spacer(),
      ProvideEnvironment(primary, Button(action).OnClick(commit)
          .With(Enabled(!busy.Get() && (draft->budget_only || !draft->objective.text.empty())))),
    }.With(Spacing(8.0F));
  } else if (!goal || goal->state == kit::GoalState::Completed || goal->state == kit::GoalState::Cancelled) {
    if (goal) {
      content.push_back(Text(goal->spec.definition.objective, TextRole::Title));
      content.push_back(Text(Status(goal->state)));
      content.push_back(Text(StringVariant::Format(::app::strings::ui_version_0, goal->ref.version), TextRole::Label));
      content.push_back(Text(UsageText(*goal), TextRole::Label));
    }
    if (!goal) content.push_back(Text(::app::strings::ui_goal_empty_description)
        .Style({Font::System(14.0F), colors.on_surface_variant}));
    content.push_back(Row {ProvideEnvironment(secondary, Button(::app::strings::ui_set_goal).OnClick([editor, advanced] {
      advanced = false;
      editor = GoalDraft{.new_goal = true};
    })), Spacer()});
  } else {
    content.push_back(Column {
      Text(StringVariant::Format(::app::strings::ui_status_version, UseString(Status(goal->state)), goal->ref.version), TextRole::Label)
          .With(Foreground(colors.on_primary_container)),
      Text(goal->spec.definition.objective, TextRole::Title)
          .Style({Font::System(19.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
      Text(UsageText(*goal), TextRole::Label).With(Foreground(colors.on_surface_variant)),
    }.With(Padding(14.0F), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch),
        Background(colors.surface_container), CornerRadius(10.0F), Border{colors.outline}));
    if (const auto slides = ReleaseSlideCount(goal->spec.definition.completion_conditions)) {
      content.push_back(Text(std::to_string(*slides) + " slides", TextRole::Label));
    }
    if (goal->reason) content.push_back(Text(*goal->reason));
    const auto start = [update, ref = goal->ref, revision = goal->revision](kit::GoalChange change) {
      update(kit::ChangeGoal{ref.id, revision, std::move(change)});
    };
    std::vector<View> actions;
    if (goal->completion_review) {
      actions.push_back(ProvideEnvironment(primary, Button(::app::strings::ui_open_completion_approval).OnClick(
          [open_completion, approval = goal->completion_review->approval] { open_completion(approval); })));
    } else if (goal->state == kit::GoalState::Proposed) {
      actions.push_back(ProvideEnvironment(primary,
          Button(::app::strings::ui_enable).OnClick([start] { start(kit::EnableGoal{}); }).With(Enabled(!busy.Get()))));
    } else if (goal->state == kit::GoalState::Paused || goal->state == kit::GoalState::Blocked ||
        goal->state == kit::GoalState::BudgetLimited) {
      actions.push_back(ProvideEnvironment(primary,
          Button(::app::strings::ui_resume).OnClick([start] { start(kit::ResumeGoal{}); }).With(Enabled(!busy.Get()))));
    } else {
      actions.push_back(ProvideEnvironment(secondary,
          Button(::app::strings::ui_pause).OnClick([start] { start(kit::PauseGoal{}); }).With(Enabled(!busy.Get()))));
    }
    actions.push_back(ProvideEnvironment(secondary, Button(::app::strings::ui_edit_goal)
        .OnClick([editor, draft = EditDraft(*goal, false)] { editor = draft; }).With(Enabled(!busy.Get()))));
    actions.push_back(ProvideEnvironment(secondary, Button(::app::strings::ui_budget)
        .OnClick([editor, draft = EditDraft(*goal, true)] { editor = draft; }).With(Enabled(!busy.Get()))));
    actions.push_back(ProvideEnvironment(destructive,
        Button(::app::strings::ui_cancel_goal).OnClick([start] { start(kit::CancelGoal{}); }).With(Enabled(!busy.Get()))));
    content.push_back(Flow(std::move(actions)).With(Spacing(8.0F)));
  }
  if (goal && !edit) {
    const auto& definition = goal->spec.definition;
    if (const auto* conditions = std::get_if<std::string>(&definition.completion_conditions.value); conditions && !conditions->empty()) {
      content.push_back(Column{Text(::app::strings::ui_completion_conditions, TextRole::Label), Text(*conditions)}.With(Spacing(4.0F)));
    }
    content.push_back(Text(definition.completion == kit::GoalCompletionPolicy::RequireConfirmation ?
        ::app::strings::ui_goal_confirm_description : ::app::strings::ui_goal_automatic_description, TextRole::Label));
    if (goal->verification && !goal->verification->reason.empty() && goal->verification->reason != goal->reason)
      content.push_back(Text(goal->verification->reason));
    const auto* sources = goal->verification && !goal->verification->verified.empty() ? &goal->verification->verified :
        goal->evidence ? &goal->evidence->sources : nullptr;
    if (sources && !sources->empty()) {
      std::vector<View> evidence{Text(::app::strings::ui_evidence, TextRole::Label)
          .Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})};
      std::vector<kit::SourceRef> shown;
      for (const auto& source : *sources) {
        if (std::ranges::find(shown, source.source) != shown.end()) continue;
        shown.push_back(source.source);
        if (const auto* artifact = std::get_if<kit::ArtifactRef>(&source.source))
          evidence.push_back(GoalArtifactLabel(runtime, *artifact)
              .Key("evidence/" + artifact->id.value + "/" + std::to_string(artifact->version)));
        else if (const auto* message = std::get_if<kit::MessageRef>(&source.source))
          evidence.push_back(Text(StringVariant::Format(::app::strings::ui_message_v_0, message->version), TextRole::Label));
        else if (const auto* item = std::get_if<kit::SharedContextRef>(&source.source))
          evidence.push_back(Text(StringVariant::Format(::app::strings::ui_shared_context_v_0, item->version), TextRole::Label));
        else if (const auto* resource = std::get_if<kit::ResourceRef>(&source.source))
          evidence.push_back(Text(StringVariant::Format(::app::strings::ui_resource_0, resource->version), TextRole::Label));
      }
      content.push_back(Column(std::move(evidence)).With(Spacing(6.0F)).Key("goal-evidence"));
    }
  }
  std::vector<View> sections{
    PreviewPaneHeader(::app::images::goal, ::app::strings::ui_goal, ::app::strings::ui_close_goal, "goal-close", close, theme),
    ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
    ScrollView(Column(std::move(content)).With(Spacing(12.0F), Padding(14.0F), CrossAlign(CrossAxisAlignment::Stretch)))
        .With(Grow(), ScrollBar()).Key("goal-panel-scroll"),
  };
  if (footer) {
    sections.push_back(ProvideEnvironment(DividerStyle{colors.outline}, Divider()));
    sections.push_back(std::move(*footer).With(Padding(EdgeInsets::Symmetric(14.0F, 10.0F))));
  }
  auto panel = Column(std::move(sections)).With(CrossAlign(CrossAxisAlignment::Stretch));
  if (docked) {
    return ProvideEnvironment(fields,
        std::move(panel).With(Grow(), Background(colors.surface))).Key("goal-panel");
  }
  return Row {Spacer(), ProvideEnvironment(fields, std::move(panel).With(Frame{.width = 596.0F})), Spacer()}
      .With(Grow(), Padding(compact ? 14.0F : 24.0F), CrossAlign(CrossAxisAlignment::Stretch),
          Background(colors.surface)).Key("goal-panel");
}

} // namespace

[[huxerui::composable]]
View SessionWorkspace(std::shared_ptr<kit::Conversation> conversation, View chat,
                      std::shared_ptr<kit::AgentRuntime> runtime, std::optional<kit::RunState> run,
                      bool show_goal, std::function<void()> close_goal,
                      std::function<void(kit::PendingApproval)> open_completion) {
  const bool docked = show_goal && UseViewportClass() == ViewportClass::Expanded;
  const auto editor = UseState(std::make_shared<GoalEditorState>());
  const auto layout = UseState(DockTree::Split(Axis::Horizontal, {
      DockTree::Tabs({"conversation"}).With(Frame{.min_width = 350.0F}, Grow()),
      DockTree::Tabs({"goal"}).With(Frame{.width = 364.0F, .min_width = 304.0F, .max_width = 500.0F}, Grow(0.0F)),
  }));
  std::vector<DockItem> panes{DockItem("conversation", ::app::strings::ui_back_to_conversation, chat).Closable(false)};
  if (show_goal) panes.push_back(DockItem("goal", ::app::strings::ui_goal,
      GoalPanel(conversation, runtime, run, editor.Get(), docked, close_goal, std::move(open_completion))).Closable(false));
  return DockLayout(std::move(panes), docked ? layout.Get() : DockTree::Tabs({show_goal ? "goal" : "conversation"}))
      .TabBar([](const DockTabBarContext&) { return View{}; })
      .OnChanged([layout, docked](const DockTree& tree) { if (docked) layout = tree; }).With(Grow());
}

} // namespace agentkit_preview
