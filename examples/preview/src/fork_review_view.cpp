#include <app_resources.h>

#include "fork_review_view.h"

#include "code_tools.h"
#include "code_workflow.h"
#include "design.h"
#include "general_workflow.h"
#include "presentation.h"
#include "presentation_tools.h"
#include "preview_json.h"
#include "preview_theme.h"
#include "worktree.h"
#include "workspace_evidence.h"

#include <huxerui/huxerui.h>
#include <algorithm>
#include <map>
#include <ranges>
#include <tuple>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using ReviewId = kit::ForkReviewId;
using SelectReview = std::function<void(std::optional<ReviewId>)>;
using OpenSession = std::function<void(kit::SessionId)>;
using ReviewList = std::vector<kit::ForkReviewSummary>;

ButtonStyle ComparisonLinkStyle(ButtonStyle style, const ColorScheme& colors) {
  style.background = Color::Transparent();
  style.disabled_background = Color::Transparent();
  style.label_style = {Font::System(13.0F).WithWeight(FontWeight::Medium), colors.on_primary_container};
  style.disabled_label = colors.on_surface_variant;
  style.padding = EdgeInsets::Symmetric(8.0F, 4.0F);
  style.minimum_height = 34.0F;
  style.corner_radii = CornerRadii{6.0F};
  return style;
}

std::string ReviewTitle(const kit::JsonValue& criteria) {
  const auto* text = std::get_if<std::string>(&criteria.value);
  return text && !text->empty() ? *text : "Candidate comparison";
}

StringVariant ReviewStatus(kit::ForkReviewState state) {
  switch (state) {
  case kit::ForkReviewState::Running: return ::app::strings::ui_in_progress;
  case kit::ForkReviewState::Ready: return ::app::strings::ui_ready_to_review;
  case kit::ForkReviewState::Closed: return ::app::strings::ui_closed;
  }
  return "";
}

StringVariant CandidateStatus(kit::CandidateState state) {
  switch (state) {
  case kit::CandidateState::Queued: return ::app::strings::ui_queued;
  case kit::CandidateState::Running: return ::app::strings::ui_in_progress;
  case kit::CandidateState::Ready: return ::app::strings::ui_ready;
  case kit::CandidateState::Failed: return ::app::strings::ui_failed;
  case kit::CandidateState::Cancelled: return ::app::strings::ui_cancelled;
  }
  return "";
}

std::string AdoptionStatus(kit::ToolCallState state, bool reconciled) {
  switch (state) {
  case kit::ToolCallState::Succeeded: return "Applied to the target";
  case kit::ToolCallState::AwaitingApproval: return "Waiting for approval in the target conversation";
  case kit::ToolCallState::Conflict: return "The baseline changed. Prepare a new plan.";
  case kit::ToolCallState::Denied: return "Adoption declined";
  case kit::ToolCallState::Failed: return reconciled ? "Verified: no changes applied" : "Adoption failed";
  case kit::ToolCallState::Cancelled: return "Adoption cancelled";
  case kit::ToolCallState::PartiallyApplied:
    if (reconciled) return "Partial application verified";
    [[fallthrough]];
  case kit::ToolCallState::ResultUnknown: return "Check the applied changes before trying again";
  default: return "Preparing the adoption";
  }
}

bool CanPrepareAdoption(const kit::ForkReviewSnapshot& review) {
  if (review.state == kit::ForkReviewState::Closed) return false;
  if (!review.adoption_state) return true;
  switch (*review.adoption_state) {
  case kit::ToolCallState::Denied:
  case kit::ToolCallState::Conflict:
  case kit::ToolCallState::Failed:
  case kit::ToolCallState::Cancelled: return true;
  case kit::ToolCallState::PartiallyApplied: return review.adoption_reconciled;
  default: return false;
  }
}

bool SameComparison(const kit::ForkReviewSnapshot& current, const kit::ForkReviewSnapshot& next) {
  if (current.id != next.id || current.revision != next.revision || current.request != next.request ||
      current.selected != next.selected || current.adoption_state != next.adoption_state) return false;
  const auto verification = [](const kit::CandidateVerification& value) {
    return std::tie(value.model, value.workspace, value.workspace_revision, value.conditions, value.evidence);
  };
  return std::ranges::equal(current.candidates, next.candidates, [&](const auto& left, const auto& right) {
    return left.id == right.id && left.state == right.state && left.artifacts == right.artifacts &&
        left.reason == right.reason && left.usage.input_tokens == right.usage.input_tokens &&
        left.usage.output_tokens == right.usage.output_tokens && left.usage.tool_calls == right.usage.tool_calls &&
        std::chrono::duration_cast<std::chrono::seconds>(left.elapsed) ==
            std::chrono::duration_cast<std::chrono::seconds>(right.elapsed) &&
        bool(left.validation) == bool(right.validation) &&
        (!left.validation || verification(*left.validation) == verification(*right.validation));
  });
}

struct ComparisonSubmission {
  kit::CommandId command;
  kit::ForkReviewRequest request;
  std::vector<WorktreePreview> worktrees;
};

using ComparisonReceipt = std::variant<ReviewId, kit::PendingApproval>;

kit::Async<ComparisonReceipt> SubmitComparison(std::shared_ptr<kit::AgentRuntime> runtime, const ComparisonSubmission& submission) {
  using Result = kit::AgentResult<ComparisonReceipt>;
  const auto receipt = co_await runtime->ForkReviews().CreateComparisonAsync(submission.command, submission.request);
  if (receipt.Succeeded()) {
    if (const auto* created = std::get_if<kit::ForkReviewCreated>(&receipt.Value().outcome))
      co_return Result::Success(created->review);
  }
  if (receipt.Succeeded() || receipt.Error().code == kit::ErrorCode::CommitUncertain) {
    auto known = co_await runtime->Commands().ReadAsync(submission.command);
    if (known.Succeeded()) {
      if (const auto* created = known.Value().result ? std::get_if<kit::ForkReviewCreated>(&*known.Value().result) : nullptr)
        co_return Result::Success(created->review);
      if (known.Value().failure) co_return Result::Failure(*known.Value().failure);
      if (known.Value().approval) co_return Result::Success(*known.Value().approval);
    }
    if (receipt.Succeeded()) co_return Result::Success(std::get<kit::PendingApproval>(receipt.Value().outcome));
  }
  co_return Result::Failure(receipt.Error());
}

[[huxerui::composable]]
View NewComparison(std::shared_ptr<kit::AgentRuntime> runtime, std::shared_ptr<kit::Conversation> source,
    ComparisonWorkspaceServices services, SelectReview select, OpenSession open_approval, std::function<void()> keep_form) {
  const auto tasks = UseTaskScope();
  const auto source_view = source->Snapshot();
  const auto criteria = UseState(TextEditingValue::FromText({}));
  const auto first = UseState(TextEditingValue::FromText({}));
  const auto second = UseState(TextEditingValue::FromText({}));
  const auto workspace = source_view ? source_view->summary.workspace : std::nullopt;
  const bool can_compare_code = workspace && services.worktrees && services.code_available;
  const auto domain = UseState(source_view && source_view->summary.agent.id == PptAgent ? std::size_t{1} :
      can_compare_code && source_view->summary.agent.id == CodeAgent ? std::size_t{2} : std::size_t{0});
  const auto pending = UseState(std::shared_ptr<const ComparisonSubmission>{});
  const auto busy = UseState(false);
  const auto retriable = UseState(false);
  const auto error = UseState(std::string{});
  const auto input = UseState(std::shared_ptr<const kit::ArtifactMetadata>{});
  const auto unavailable = UseState(false);
  const auto ref = source_view && !source_view->artifacts.empty() ? std::optional{source_view->artifacts.back()} : std::nullopt;
  Lifecycle([runtime, tasks, input, unavailable, ref] {
    input = std::shared_ptr<const kit::ArtifactMetadata>{};
    unavailable = false;
    if (!ref) return std::function<void()>{[] {}};
    const auto load = tasks.Launch([runtime, input, unavailable, ref]() -> Task<void> {
      auto artifact = co_await runtime->Artifacts().ReadAsync(*ref);
      if (artifact.Succeeded()) input = std::make_shared<const kit::ArtifactMetadata>(artifact.Value().metadata);
      else unavailable = true;
    });
    return std::function<void()>{[load] { load.Cancel(); }};
  }, ref);
  const bool reading_input = ref && (!input.Get() || input.Get()->ref != ref) && !unavailable.Get();
  const auto submit = [runtime, source, select, open_approval, tasks, criteria, first, second, domain, pending, input, busy, error,
                       ref, reading_input, worktrees = services.worktrees, principal = services.principal, retriable,
                       refresh = services.refresh_worktrees] {
    if (busy.Get()) return;
    auto submission = pending.Get();
    if (!submission) {
      if (reading_input) return;
      const auto snapshot = source->Snapshot();
      if (!snapshot || !snapshot->summary.project || snapshot->summary.lifecycle == kit::SessionLifecycle::Deleting) {
        error = "The source conversation is unavailable.";
        return;
      }
      const auto anchor = std::ranges::find_if(snapshot->messages.rbegin(), snapshot->messages.rend(), [&](const auto& message) {
        return message.state == kit::MessageState::Complete && message.ref.session == snapshot->summary.id;
      });
      if (anchor == snapshot->messages.rend()) {
        error = "Send the requirements in the source conversation first.";
        return;
      }
      const auto header = criteria->text;
      kit::ForkReviewRequest request{
          .from = {snapshot->summary.id, anchor->ref.message, anchor->sequence}, .project = *snapshot->summary.project,
          .inputs = {{anchor->ref}}, .comparison_criteria = kit::JsonValue{header},
          .shared_budget = {.model_turns = 12, .tool_calls = 16}};
      if (input.Get() && input.Get()->ref == ref) request.inputs.push_back({input.Get()->ref});
      const auto assistant = domain.Get() == 0 ? DesignAgent : domain.Get() == 1 ? PptAgent : CodeAgent;
      const kit::WorkspaceChoice shared{kit::WorkspaceChoice::Kind::Existing, snapshot->summary.workspace.value_or(kit::WorkspaceId{})};
      for (const auto* approach : {&first.Get(), &second.Get()}) {
        const auto title = approach->text.substr(0, approach->text.find('\n'));
        request.candidates.push_back({{.agent = assistant, .title = title}, shared,
            {.text = approach->text + "\n\nShared requirements:\n" + header}});
      }
      auto draft = std::make_shared<ComparisonSubmission>(ComparisonSubmission{kit::NewCommandId(), std::move(request)});
      if (assistant == CodeAgent) {
        if (!worktrees || !snapshot->summary.workspace) { error = "Open a Git workspace to compare code."; return; }
        busy = true;
        error = "";
        tasks.Launch([worktrees, principal, workspace = *snapshot->summary.workspace, draft, pending, busy, error]() -> Task<void> {
          auto plans = co_await worktrees->PrepareComparisonAsync(principal, workspace, draft->command, draft->request);
          busy = false;
          if (!plans.Succeeded()) { error = plans.Error().message; co_return; }
          draft->worktrees = std::move(plans).Value();
          for (std::size_t i = 0; i < draft->worktrees.size(); ++i)
            draft->request.candidates[i].workspace = *draft->worktrees[i].request.workspace;
          pending = std::move(draft);
        });
        return;
      }
      submission = std::move(draft);
      pending = submission;
    }
    busy = true;
    error = "";
    tasks.Launch([runtime, select, open_approval, pending, busy, error, submission, retriable, refresh]() -> Task<void> {
      auto receipt = co_await SubmitComparison(runtime, *submission);
      busy = false;
      if (receipt.Succeeded()) {
        pending = std::shared_ptr<const ComparisonSubmission>{};
        if (const auto* review = std::get_if<ReviewId>(&receipt.Value())) select(*review);
        else open_approval(std::get<kit::PendingApproval>(receipt.Value()).session);
      } else {
        retriable = receipt.Error().code == kit::ErrorCode::CommitUncertain || receipt.Error().code == kit::ErrorCode::Busy;
        if (!retriable.Get()) pending = std::shared_ptr<const ComparisonSubmission>{};
        error = receipt.Error().message;
        if (refresh && !submission->worktrees.empty()) refresh();
      }
    });
  };
  const bool edit = !pending.Get() && !busy.Get();
  std::vector<StringVariant> domains{"Design", "PowerPoint"};
  if (can_compare_code) domains.push_back("Code");
  const auto colors = UseTheme().colors;
  const auto field_style = PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), colors);
  std::vector<View> content{
      Text(::app::strings::ui_new_comparison, TextRole::Title),
      Text(::app::strings::ui_comparison_explainer).With(Foreground(colors.on_surface_variant)),
      SegmentedButton(std::move(domains), domain.Get()).OnChanged([domain, keep_form](std::size_t value) {
          domain = value;
          keep_form();
        })
          .With(Enabled(edit)),
      Text(reading_input ? StringVariant{::app::strings::ui_reading_the_source_version} : input.Get() ?
          StringVariant::Format(::app::strings::ui_0_v_1, input.Get()->name, input.Get()->ref.version) : StringVariant{::app::strings::ui_source_conversation_history}, TextRole::Label),
      TextField(criteria).Label(::app::strings::ui_shared_requirements).Variant(TextFieldVariant::Outlined).Key("comparison-criteria")
          .OnChanged([criteria, keep_form](const TextEditingValue& value) { criteria = value; keep_form(); }).With(Enabled(edit)),
      TextField(first).Label(::app::strings::ui_first_approach).Variant(TextFieldVariant::Outlined).Key("comparison-first")
          .OnChanged([first, keep_form](const TextEditingValue& value) { first = value; keep_form(); }).With(Enabled(edit)),
      TextField(second).Label(::app::strings::ui_second_approach).Variant(TextFieldVariant::Outlined).Key("comparison-second")
          .OnChanged([second, keep_form](const TextEditingValue& value) { second = value; keep_form(); }).With(Enabled(edit)),
  };
  if (pending.Get() && !pending.Get()->worktrees.empty()) {
    const auto& planned = pending.Get()->worktrees;
    content.push_back(Text(StringVariant::Format(::app::strings::ui_base_commit_0, planned.front().commit), TextRole::Label));
    for (const auto& plan : planned) {
      content.push_back(Column{Text(plan.request.options.title, TextRole::Label), Text(plan.branch), Text(plan.directory)}
          .With(Spacing(3.0F)).Key(plan.plan.id.value));
    }
  }
  if (!error->empty()) content.push_back(Text(error.Get(), TextRole::Label));
  std::vector<View> actions{Spacer()};
  if (pending.Get() && !retriable.Get()) {
    actions.push_back(ProvideEnvironment(ComparisonLinkStyle(UseEnvironment<ButtonStyle>(), colors),
        Button(::app::strings::ui_edit_details).OnClick([pending] { pending = std::shared_ptr<const ComparisonSubmission>{}; })
            .With(Enabled(!busy.Get()))));
  }
  actions.push_back(Button(retriable.Get() ? ::app::strings::ui_check_submission : !pending.Get() && domain.Get() == 2
          ? ::app::strings::ui_review_worktrees : ::app::strings::ui_start_comparison).OnClick(submit)
      .With(Enabled(!busy.Get() && (pending.Get() || (!reading_input && !criteria->text.empty() && !first->text.empty() &&
          !second->text.empty() && first->text != second->text)))));
  content.push_back(Row(std::move(actions)).With(Spacing(8.0F)));
  return ProvideEnvironment(field_style, Column(std::move(content)).With(
      Spacing(12.0F), Frame{.max_width = 660.0F}, CrossAlign(CrossAxisAlignment::Stretch)));
}

struct CandidatePreview {
  kit::ArtifactMetadata metadata;
  std::shared_ptr<const DesignBoard> board;
  std::optional<PresentationOutline> deck;
  std::vector<std::string> code_diffs;
  std::string message;
};

using Previews = std::map<std::pair<kit::ArtifactId, kit::Revision>, CandidatePreview>;

kit::Async<CandidatePreview> LoadPreview(std::shared_ptr<kit::AgentRuntime> runtime, File evidence, kit::ArtifactRef ref) {
  using Result = kit::AgentResult<CandidatePreview>;
  auto record = co_await runtime->Artifacts().ReadAsync(ref);
  if (!record.Succeeded()) co_return Result::Failure(record.Error());
  if (record.Value().metadata.type == ImplementationEvidence) {
    auto diffs = co_await PreviewCodeCandidate(runtime, std::move(evidence), ref);
    if (!diffs.Succeeded()) co_return Result::Failure(diffs.Error());
    co_return Result::Success({.metadata = record.Value().metadata, .code_diffs = std::move(diffs).Value()});
  }
  auto result = co_await runtime->Artifacts().PreviewAsync(ref);
  if (!result.Succeeded()) co_return Result::Failure(result.Error());
  CandidatePreview item{.metadata = record.Value().metadata, .message = result.Value().summary};
  if (result.Value().structured && item.metadata.type == DesignArtifactType) {
    auto board = ParseDesign(*result.Value().structured);
    if (!board.Succeeded()) co_return Result::Failure(board.Error());
    item.board = std::make_shared<const DesignBoard>(std::move(board).Value());
  } else if (result.Value().structured) {
    auto outline = ReadPresentationOutline(*result.Value().structured);
    if (outline.Succeeded()) item.deck = std::move(outline).Value();
  }
  co_return Result::Success(std::move(item));
}

View CandidateArtifact(const CandidatePreview& preview, const CandidatePreview* other, std::size_t slide, std::string_view title) {
  std::vector<View> content;
  if (preview.metadata.name != title) content.push_back(Text(preview.metadata.name, TextRole::Title));
  content.push_back(Text(StringVariant::Format(::app::strings::ui_version_0, preview.metadata.ref.version), TextRole::Label));
  if (preview.board) {
    content.push_back(DesignArtboard(preview.board, {}, 204.0F));
    if (other && other->board) {
      std::vector<std::string> differences;
      for (const auto& node : preview.board->nodes) {
        const auto found = std::ranges::find(other->board->nodes, node.id, &DesignNode::id);
        if (found == other->board->nodes.end()) differences.push_back("Only here: " + node.label);
        else if (*found != node) differences.push_back("Different: " + node.label);
      }
      if (preview.board->width != other->board->width || preview.board->height != other->board->height)
        differences.push_back("Different artboard size");
      if (!differences.empty()) {
        content.push_back(Text(::app::strings::ui_differences, TextRole::Label));
        for (auto& line : differences) content.push_back(Text(std::move(line)));
      }
    }
  } else if (preview.deck) {
    if (slide < preview.deck->slides.size()) {
      const auto& shown = preview.deck->slides[slide];
      content.push_back(Text(shown.title, TextRole::Title));
      for (const auto& bullet : shown.bullets) content.push_back(Text(bullet));
      if (other && other->deck && (slide >= other->deck->slides.size() || shown != other->deck->slides[slide]))
        content.push_back(Text(::app::strings::ui_slide_content_differs, TextRole::Label));
    } else {
      content.push_back(Text(::app::strings::ui_no_slide_at_this_position));
    }
  } else if (!preview.code_diffs.empty()) {
    for (const auto& diff : preview.code_diffs) content.push_back(Text(diff));
  } else if (!preview.message.empty()) {
    content.push_back(Text(preview.message));
  }
  return Column(std::move(content)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

struct AdoptionSubmission {
  kit::CommandId command;
  kit::AdoptCandidateRequest request;
};

kit::Async<kit::ToolExecutionResult> ReadPublishedAdoption(std::shared_ptr<kit::AgentRuntime> runtime,
    std::shared_ptr<const kit::SessionSnapshot> session, const kit::ToolCallSnapshot& call) {
  using Result = kit::AgentResult<kit::ToolExecutionResult>;
  const auto unavailable = [] { return Result::Failure({.code = kit::ErrorCode::ResultUnknown,
      .message = "The original publication could not be verified"}); };
  if (!call.issued_targets || call.issued_targets->size() != 1 || !call.issued_targets->front().expected)
    co_return unavailable();
  const auto& target = call.issued_targets->front();
  const bool design = HasDesignTargets(*call.issued_targets);
  bool baseline = false;
  std::optional<kit::ArtifactRef> applied;
  std::optional<kit::ResourceRef> version;
  for (const auto& ref : session->artifacts) {
    const auto resource = design ? DesignTargetBaseline(ref) : PresentationTargetBaseline(ref);
    if (resource.target != target.target) continue;
    auto record = co_await runtime->Artifacts().ReadAsync(ref);
    if (!record.Succeeded() || record.Value().session != session->summary.id ||
        record.Value().metadata.type != (design ? DesignArtifactType : PresentationArtifactType)) co_return unavailable();
    if (resource == target) baseline = true;
    if (record.Value().call == call.id && record.Value().run == call.run) {
      if (applied) co_return unavailable();
      applied = ref;
      version = resource.expected;
    }
  }
  if (!baseline) co_return unavailable();
  kit::ResourceEffect effect{.resource = target.target, .state = kit::ResourceEffectState::Unchanged, .before = target.expected};
  if (!applied) co_return Result::Success({.outcome = kit::ToolOutcome::Failed, .effects = {std::move(effect)},
      .reason = kit::AgentError{.code = kit::ErrorCode::InvalidState, .message = "The selected version was not published"}});
  const auto prior = std::ranges::find(call.effects, target.target, &kit::ResourceEffect::resource);
  if (prior != call.effects.end() && prior->state == kit::ResourceEffectState::Applied && prior->after == version) {
    effect = *prior;
  } else {
    effect.state = kit::ResourceEffectState::Applied;
    effect.after = version;
  }
  co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .existing_artifacts = {*applied}, .effects = {std::move(effect)}});
}

[[huxerui::composable]]
View ComparisonDetail(std::shared_ptr<kit::AgentRuntime> runtime, std::shared_ptr<kit::Conversation> source, ReviewId id,
    ComparisonWorkspaceServices services, SelectReview select, OpenSession open_branch, OpenSession open_approval) {
  const auto tasks = UseTaskScope();
  const auto snapshot = UseState(std::shared_ptr<const kit::ForkReviewSnapshot>{});
  const auto previews = UseState(std::shared_ptr<const Previews>{});
  const auto error = UseState(std::string{});
  const auto operation_error = UseState(std::string{});
  const auto pending = UseState(std::shared_ptr<const AdoptionSubmission>{});
  const auto reconciliation = UseState(std::optional<std::pair<kit::ToolCallId, kit::CommandId>>{});
  const auto reviewing_recovered = UseState(std::optional<kit::ToolCallId>{});
  const auto busy = UseState(false);
  const auto current_slide = UseState(std::size_t{0});
  Lifecycle([tasks, runtime, id, snapshot, error] {
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto load = tasks.Launch([runtime, id, subscription, snapshot, error]() -> Task<void> {
      auto observed = co_await runtime->ForkReviews().ObserveComparisonAsync(id, [snapshot, error](auto result) {
        if (result.Succeeded()) {
          if (!snapshot.Get() || !SameComparison(*snapshot.Get(), *result.Value())) snapshot = std::move(result).Value();
          error = "";
        } else { snapshot = std::shared_ptr<const kit::ForkReviewSnapshot>{}; error = result.Error().message; }
      });
      if (observed.Succeeded()) subscription->emplace(std::move(observed).Value());
      else error = observed.Error().message;
    });
    return [load, subscription] { load.Cancel(); subscription->reset(); };
  }, id);
  std::vector<kit::ArtifactRef> references;
  if (snapshot.Get()) {
    for (const auto& item : snapshot.Get()->candidates) {
      references.insert(references.end(), item.artifacts.begin(), item.artifacts.end());
    }
  }
  Lifecycle([runtime, tasks, previews, references, evidence = services.evidence] {
    const auto retained = previews.Get();
    auto load = tasks.Launch([runtime, previews, references, retained, evidence]() -> Task<void> {
      auto ready = std::make_shared<Previews>();
      for (const auto& ref : references) {
        const auto key = std::pair{ref.id, ref.version};
        if (retained) {
          const auto prior = retained->find(key);
          if (prior != retained->end() && !prior->second.metadata.type.empty()) {
            ready->emplace(key, prior->second);
            continue;
          }
        }
        auto result = co_await LoadPreview(runtime, evidence, ref);
        if (result.Succeeded()) ready->emplace(key, std::move(result).Value());
        else ready->emplace(key, CandidatePreview{.metadata = {.ref = ref, .name = "Artifact unavailable"},
            .message = result.Error().message});
      }
      previews = std::move(ready);
    });
    return [load] { load.Cancel(); };
  }, references);

  const auto appearance = UseTheme();
  const auto& theme = appearance.colors;
  const auto link_style = ComparisonLinkStyle(UseEnvironment<ButtonStyle>(), theme);
  const bool wide = UseViewportClass() == ViewportClass::Expanded;
  if (!snapshot.Get()) return Text(error->empty() ? StringVariant{::app::strings::ui_opening_comparison} : StringVariant{error.Get()});
  const auto& review = *snapshot.Get();
  const auto source_view = source->Snapshot();
  const kit::ToolCallSnapshot* recovered = nullptr;
  if (source_view && review.adoption_call && review.adoption_reconciled) {
    const auto call = std::ranges::find(source_view->tools, *review.adoption_call, &kit::ToolCallSnapshot::id);
    if (call != source_view->tools.end() && call->issued_targets && HasCodeTargets(*call->issued_targets) &&
        std::ranges::any_of(call->effects, [](const auto& effect) {
          return effect.state == kit::ResourceEffectState::Applied && effect.undo && effect.after;
        })) recovered = &*call;
  }
  if (recovered && reviewing_recovered.Get() == recovered->id) {
    auto workspace = source_view->summary.workspace && services.workspaces
        ? services.workspaces->Find(*source_view->summary.workspace) : std::nullopt;
    return Column{
        Row{PreviewBackAction(::app::strings::ui_back_to_comparison, [reviewing_recovered] {
              reviewing_recovered = std::nullopt;
            }, appearance).Key("comparison-return"),
            Text(::app::strings::ui_applied_workspace_changes, TextRole::Title).With(Grow())}
            .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
        CodeRevertReview(runtime, source, *recovered, std::move(workspace), services.evidence).Key(recovered->id.value),
    }.With(Grow(), Spacing(12.0F));
  }
  const auto adopt = [runtime, source, id, snapshot, tasks, pending, busy, operation_error, open_approval, services](
      kit::CandidateId candidate, kit::ArtifactRef ref, kit::AgentId agent) {
    if (busy.Get() || !snapshot.Get()) return;
    busy = true;
    operation_error = "";
    tasks.Launch([runtime, source, id, candidate, ref, agent, revision = snapshot.Get()->revision,
                  pending, busy, operation_error, open_approval, services]() -> Task<void> {
      auto submission = pending.Get();
      if (!submission) {
        const auto target = source->Snapshot();
        if (!target) { busy = false; operation_error = "The target conversation is unavailable."; co_return; }
        std::vector<kit::PartId> parts;
        std::vector<kit::TargetBaseline> targets;
        if (agent == CodeAgent) {
          const auto workspace = target->summary.workspace && services.workspaces
              ? services.workspaces->Find(*target->summary.workspace) : std::nullopt;
          if (!workspace) { busy = false; operation_error = "Open the target workspace to adopt code."; co_return; }
          auto scope = co_await InspectCodeAdoption(runtime, services.evidence, *workspace, ref);
          if (!scope.Succeeded()) { busy = false; operation_error = scope.Error().message; co_return; }
          parts = std::move(scope.Value().parts);
          targets = std::move(scope.Value().targets);
        } else {
          const bool presentation = agent == PptAgent;
          std::optional<kit::ArtifactRef> baseline;
          for (const auto& artifact : std::views::reverse(target->artifacts)) {
            auto record = co_await runtime->Artifacts().ReadAsync(artifact);
            if (record.Succeeded() && record.Value().session == target->summary.id &&
                record.Value().metadata.type == (presentation ? PresentationArtifactType : DesignArtifactType)) {
              baseline = artifact;
              break;
            }
          }
          if (!baseline) {
            busy = false;
            operation_error = presentation ? "Stage a PowerPoint deck in this conversation before adopting a candidate." :
                "Publish a design in this conversation before adopting a candidate.";
            co_return;
          }
          targets.push_back(presentation ? PresentationTargetBaseline(*baseline) : DesignTargetBaseline(*baseline));
        }
        kit::AdoptCandidateRequest request{id, revision, candidate, ref, target->summary.id, std::move(parts), std::move(targets)};
        submission = std::make_shared<AdoptionSubmission>(AdoptionSubmission{kit::NewCommandId(), std::move(request)});
        pending = submission;
      }
      auto receipt = co_await runtime->ForkReviews().PrepareAdoptionAsync(submission->command, submission->request);
      busy = false;
      if (receipt.Succeeded()) {
        pending = std::shared_ptr<const AdoptionSubmission>{};
        open_approval(submission->request.target_session);
      } else {
        if (receipt.Error().code != kit::ErrorCode::CommitUncertain) pending = std::shared_ptr<const AdoptionSubmission>{};
        operation_error = receipt.Error().message;
      }
    });
  };
  const auto close = [runtime, tasks, id, revision = review.revision, operation_error, busy] {
    if (busy.Get()) return;
    busy = true;
    tasks.Launch([runtime, id, revision, operation_error, busy]() -> Task<void> {
      auto result = co_await runtime->ForkReviews().CloseComparisonAsync(kit::NewCommandId(), id, revision);
      busy = false;
      operation_error = result.Succeeded() ? "" : result.Error().message;
    });
  };
  const auto check_adoption = [runtime, source, services, call = review.adoption_call, tasks, busy, operation_error, reconciliation] {
    if (busy.Get() || !call) return;
    auto target = source->Snapshot();
    if (!target) return;
    const auto tool = std::ranges::find(target->tools, *call, &kit::ToolCallSnapshot::id);
    if (tool == target->tools.end() || !tool->issued_targets) {
      operation_error = "The original adoption plan is unavailable.";
      return;
    }
    if (!reconciliation.Get() || reconciliation.Get()->first != *call) reconciliation = std::pair{*call, kit::NewCommandId()};
    busy = true;
    operation_error = "";
    tasks.Launch([runtime, target, call = *tool, services, command = reconciliation.Get()->second, busy, operation_error]() -> Task<void> {
      auto evidence = [&]() -> kit::Async<kit::ToolExecutionResult> {
        if (!HasCodeTargets(*call.issued_targets)) co_return co_await ReadPublishedAdoption(runtime, target, call);
        const auto workspace = target->summary.workspace && services.workspaces
            ? services.workspaces->Find(*target->summary.workspace) : std::nullopt;
        if (!workspace) co_return kit::AgentResult<kit::ToolExecutionResult>::Failure({.code = kit::ErrorCode::PermissionDenied,
            .message = "The original target workspace is unavailable"});
        co_return co_await ReadCodeChangeResultAsync(services.evidence, *workspace, call);
      };
      auto observed = co_await evidence();
      if (!observed.Succeeded()) { busy = false; operation_error = observed.Error().message; co_return; }
      auto receipt = co_await runtime->ReconcileToolAsync(command, target->summary.id, call.id, std::move(observed).Value());
      if (!receipt.Succeeded()) operation_error = receipt.Error().message;
      busy = false;
    });
  };
  const auto artifact_for = [&](const kit::ForkCandidate& candidate) -> const CandidatePreview* {
    if (!previews.Get() || candidate.artifacts.empty()) return nullptr;
    const auto& ref = candidate.artifacts.front();
    const auto found = previews.Get()->find({ref.id, ref.version});
    return found == previews.Get()->end() ? nullptr : &found->second;
  };
  const auto& candidates = review.candidates;
  std::size_t pages = 0;
  for (const auto& candidate : candidates) {
    const auto* preview = artifact_for(candidate);
    if (preview && preview->deck) pages = std::max(pages, preview->deck->slides.size());
  }
  const auto slide = pages ? std::min(current_slide.Get(), pages - 1) : std::size_t{0};
  std::vector<View> panels;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto& candidate = candidates[i];
    const auto& label = review.request.candidates[i].session.title;
    const auto title = label.empty() ? UseString(::app::strings::ui_candidate) : label;
    const auto* preview = artifact_for(candidate);
    const auto other = candidates.size() == 2 ? artifact_for(candidates[1 - i]) : nullptr;
    const auto& agent = review.request.candidates[i].session.agent;
    std::vector<View> links{
        Button(::app::strings::ui_open_conversation).OnClick([open_branch, session = candidate.session] { open_branch(session); })
            .Key("comparison-open/" + candidate.session.value),
    };
    if (CanPrepareAdoption(review) && (agent == PptAgent || agent == DesignAgent || agent == CodeAgent) && source_view &&
        (source_view->summary.agent.id == agent || source_view->summary.agent.id == GeneralAgent)) {
      const auto ref = preview && (preview->deck || preview->board || !preview->code_diffs.empty())
          ? std::optional{preview->metadata.ref} : std::nullopt;
      links.push_back(Button(StringVariant::Format(::app::strings::ui_review_adoption_0, title)).OnClick([adopt, candidate = candidate.id, ref, agent] {
        if (ref) adopt(candidate, *ref, agent);
      }).With(Enabled(ref && candidate.state == kit::CandidateState::Ready && !busy.Get() && !pending.Get()))
          .Key("comparison-adopt/" + candidate.id.value));
    }
    const auto selected = review.selected == candidate.id;
    std::vector<View> panel{
        Text(title, TextRole::Title).Style({Font::System(19.0F).WithWeight(FontWeight::SemiBold), theme.on_surface}),
        Text(selected && review.adoption_state ? AdoptionStatus(*review.adoption_state, review.adoption_reconciled)
            : CandidateStatus(candidate.state), TextRole::Label),
        ProvideEnvironment(link_style, Flow(std::move(links)).With(Spacing(6.0F))),
        Divider(),
    };
    if (preview) panel.push_back(CandidateArtifact(*preview, other, slide, title));
    else if (!candidate.artifacts.empty()) panel.push_back(Text(::app::strings::ui_reading_the_candidate_version));
    if (candidate.reason) panel.push_back(Text(candidate.reason->message));
    if (candidate.validation && candidate.validation->model)
      panel.push_back(Text(candidate.validation->model->value, TextRole::Label));
    const auto tokens = candidate.usage.input_tokens + candidate.usage.output_tokens;
    if (tokens || candidate.usage.tool_calls) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(candidate.elapsed).count();
      panel.push_back(Text(std::to_string(seconds) + " s \xc2\xb7 " + std::to_string(tokens) +
          " tokens \xc2\xb7 " + std::to_string(candidate.usage.tool_calls) + " tool calls", TextRole::Label));
    }
    if (candidate.validation && candidate.validation->evidence) {
      const auto& values = candidate.validation->evidence->values;
      const auto* text = std::get_if<std::string>(&values.value);
      panel.push_back(Text(text ? *text : EncodeJson(values).dump(), TextRole::Label));
    }
    panels.push_back(Column(std::move(panel)).With(Grow(), Spacing(10.0F), Padding(16.0F),
        Background(theme.surface), Border{selected ? theme.on_primary_container : theme.outline},
        CornerRadius(9.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key(candidate.id.value));
  }
  std::vector<View> controls{
      Button(::app::strings::ui_all_comparisons).OnClick([select] { select(std::nullopt); }),
  };
  if (!review.adoption_reconciled &&
      (review.adoption_state == kit::ToolCallState::PartiallyApplied || review.adoption_state == kit::ToolCallState::ResultUnknown))
    controls.push_back(Button(::app::strings::ui_check_adoption_result).OnClick(check_adoption).With(Enabled(!busy.Get())));
  if (recovered) controls.push_back(Button(::app::strings::ui_review_recovered_changes).OnClick([reviewing_recovered, call = recovered->id] {
    reviewing_recovered = std::optional{call};
  }));
  if (review.state != kit::ForkReviewState::Closed)
    controls.push_back(Button(::app::strings::ui_close_comparison).OnClick(close).With(Enabled(!busy.Get()), Tooltip(::app::strings::ui_end_the_current_comparison)));
  std::vector<View> results{
      ProvideEnvironment(link_style, Flow(std::move(controls)).With(Spacing(6.0F))),
      Text(ReviewTitle(review.request.comparison_criteria), TextRole::Title)
          .Style({Font::System(26.0F).WithWeight(FontWeight::SemiBold), theme.on_surface}),
  };
  if (pending.Get() && !busy.Get()) results.push_back(Button(::app::strings::ui_check_adoption).OnClick([adopt] { adopt({}, {}, {}); }));
  if (!operation_error->empty()) results.push_back(Text(operation_error.Get(), TextRole::Label));
  if (pages > 1) {
    results.push_back(ProvideEnvironment(link_style, Row{
        Button(::app::strings::ui_previous_slide).OnClick([current_slide, slide] { current_slide = slide - 1; }).With(Enabled(slide > 0)),
        Text(std::to_string(slide + 1) + " / " + std::to_string(pages), TextRole::Label),
        Button(::app::strings::ui_next_slide).OnClick([current_slide, slide] { current_slide = slide + 1; }).With(Enabled(slide + 1 < pages)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))));
  }
  {
    View body = panels.empty() ? View(Text(::app::strings::ui_no_candidates_are_visible_in_this_comparison)) :
        wide && panels.size() == 2 ? View(Row(std::move(panels)).With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Start))) :
        View(Column(std::move(panels)).With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch)));
    results.push_back(ScrollView(std::move(body)).With(Grow(), ScrollBar()));
  }
  return Column(std::move(results)).With(Grow(), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View ComparisonLibrary(std::shared_ptr<kit::AgentRuntime> runtime, std::shared_ptr<kit::Conversation> source,
    ComparisonWorkspaceServices services, SelectReview select, OpenSession open_approval) {
  const auto tasks = UseTaskScope();
  const auto entries = UseState(std::shared_ptr<const ReviewList>{});
  const auto fresh = UseState(false);
  const auto error = UseState(std::string{});
  const auto snapshot = source->Snapshot();
  const auto source_id = snapshot ? snapshot->summary.id : kit::SessionId{};
  Lifecycle([runtime, tasks, entries, error, source_id] {
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto load = tasks.Launch([runtime, entries, error, source_id, subscription]() -> Task<void> {
      auto observed = co_await runtime->ForkReviews().ObserveComparisonsAsync(source_id, [entries, error](auto result) {
        if (result.Succeeded()) {
          if (!entries.Get() || *entries.Get() != *result.Value()) entries = std::move(result).Value();
          error = "";
        } else { entries = std::shared_ptr<const ReviewList>{}; error = result.Error().message; }
      });
      if (observed.Succeeded()) subscription->emplace(std::move(observed).Value());
      else error = observed.Error().message;
    });
    return [load, subscription] { load.Cancel(); subscription->reset(); };
  }, source_id);
  const auto colors = UseTheme().colors;
  const auto link_style = ComparisonLinkStyle(UseEnvironment<ButtonStyle>(), colors);
  if (!entries.Get()) return Text(error->empty() ? StringVariant{::app::strings::ui_loading_comparisons} : StringVariant{error.Get()});
  if (fresh.Get() || entries.Get()->empty()) {
    std::vector<View> content;
    if (!entries.Get()->empty()) content.push_back(ProvideEnvironment(link_style,
        Row{Button(::app::strings::ui_all_comparisons).OnClick([fresh] { fresh = false; }), Spacer()}));
    content.push_back(NewComparison(runtime, source, std::move(services), select, open_approval, [fresh] {
      if (!fresh.Get()) fresh = true;
    }).Key("new-comparison"));
    return Column(std::move(content)).With(Spacing(14.0F));
  }
  std::vector<View> rows{
      Row{Text(::app::strings::ui_comparisons, TextRole::Title), Spacer(), Button(::app::strings::ui_new_comparison).OnClick([fresh] { fresh = true; })}
          .With(CrossAlign(CrossAxisAlignment::Center)),
  };
  for (const auto& entry : *entries.Get()) {
    rows.push_back(ProvideEnvironment(link_style, Row{
        Button(ReviewTitle(entry.criteria)).OnClick([select, id = entry.id] { select(id); }),
        Spacer(), Text(ReviewStatus(entry.state), TextRole::Label),
    }.With(Spacing(8.0F), Padding(EdgeInsets::Symmetric(8.0F, 4.0F)), CrossAlign(CrossAxisAlignment::Center),
        Background(colors.surface_container_high), Border{colors.outline}, CornerRadius(7.0F))
        .Key(entry.id.value)));
  }
  return Column(std::move(rows)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]]
View ComparisonWorkspace(std::shared_ptr<kit::AgentRuntime> runtime, std::shared_ptr<kit::Conversation> source,
    std::optional<ReviewId> review, ComparisonWorkspaceServices services,
    SelectReview select, std::function<void()> back, OpenSession open_branch, OpenSession open_approval) {
  const auto snapshot = source->Snapshot();
  const auto theme = UseTheme();
  const float gutter = UseViewportClass() == ViewportClass::Compact ? 14.0F : 22.0F;
  auto content = review ? ComparisonDetail(runtime, source, *review, services, select, open_branch, open_approval).Key(review->value) :
      View(ScrollView(ComparisonLibrary(runtime, source, std::move(services), select, open_approval)).With(ScrollBar()));
  return Row{Spacer(), Column{
      Row{PreviewBackAction(::app::strings::ui_back_to_conversation, std::move(back), theme).Key("back-to-conversation"),
          Text(snapshot ? snapshot->summary.title : "", TextRole::Title)
              .Style({Font::System(17.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface})
              .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow())}
          .With(Spacing(8.0F), Frame{.min_height = 44.0F}, CrossAlign(CrossAxisAlignment::Center)),
      Divider(),
      std::move(content).With(Grow(), Padding(EdgeInsets{.top = 14.0F, .bottom = 20.0F})),
  }.With(Frame{.width = 1040.0F}, CrossAlign(CrossAxisAlignment::Stretch)), Spacer()}
      .With(Grow(), Padding(EdgeInsets::Symmetric(gutter, 0.0F)), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace agentkit_preview
