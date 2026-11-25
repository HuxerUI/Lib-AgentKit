#include "chat_chrome.h"
#include "side_chat.h"
#include <app_resources.h>

#include "design.h"
#include "code_tools.h"
#include "code_workflow.h"
#include "presentation_view.h"
#include "preview_theme.h"
#include "preview_settings.h"
#include "workspace_evidence.h"

#include <huxerui/huxerui.h>
#include <algorithm>
#include <map>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;

struct ReviewedDesign {
  std::shared_ptr<const DesignBoard> board;
  std::optional<kit::AgentChatApprovalRef> review;
  std::optional<kit::ArtifactRef> artifact;
  kit::Revision access_revision = 0;
  bool operator==(const ReviewedDesign&) const = default;
};

using ReviewNodes = std::map<std::pair<kit::ArtifactId, kit::Revision>, std::string>;

struct DesignImpactItem {
  kit::ArtifactRef artifact;
  std::string name;
  bool approximate;
  std::vector<kit::ArtifactPart> parts;
};

struct DesignImpactReview {
  std::shared_ptr<kit::Conversation> owner;
  kit::ArtifactRef artifact;
  std::optional<kit::PartId> part;
  std::optional<std::string> diff;
};

kit::Async<DesignImpactReview> OpenDesignImpact(std::shared_ptr<kit::AgentRuntime> runtime, kit::ArtifactImpactQuery query,
    kit::ArtifactRef artifact, std::optional<kit::PartId> part, bool approximate, std::optional<File> evidence) {
  using Result = kit::AgentResult<DesignImpactReview>;
  auto record = co_await runtime->Artifacts().ReadAsync(artifact);
  if (!record.Succeeded()) co_return Result::Failure(record.Error());
  const auto& metadata = record.Value();
  if (part && std::ranges::find(metadata.parts, *part, &kit::ArtifactPart::id) == metadata.parts.end())
    co_return Result::Failure({.code = kit::ErrorCode::NotFound, .message = "This affected part is unavailable"});
  auto opening = metadata.metadata.type == "presentation/pptx"
      ? OpenPresentationReviewAsync(runtime, {metadata.session, artifact, part})
      : runtime->Sessions().OpenAsync(metadata.session);
  auto opened = co_await std::move(opening);
  if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
  auto owner = std::move(opened).Value();
  const auto snapshot = owner->Snapshot();
  if (!snapshot || snapshot->summary.project != query.project ||
      std::ranges::find(snapshot->artifacts, artifact) == snapshot->artifacts.end()) {
    co_return Result::Failure({.code = kit::ErrorCode::NotFound, .message = "This affected artifact is unavailable"});
  }
  std::optional<std::string> diff;
  if (part && metadata.metadata.type == ImplementationEvidence) {
    if (!evidence) co_return Result::Failure({.code = kit::ErrorCode::Unsupported, .message = "Applied file history is unavailable"});
    auto preview = co_await PreviewAppliedFile(*evidence, metadata, *snapshot, *part);
    if (!preview.Succeeded()) co_return Result::Failure(preview.Error());
    diff = std::move(preview).Value();
  }
  auto latest = co_await runtime->Artifacts().InspectImpactAsync(std::move(query));
  if (!latest.Succeeded()) co_return Result::Failure(latest.Error());
  const auto matched = std::ranges::find(latest.Value().affected, artifact, &kit::ImpactItem::affected);
  if (matched == latest.Value().affected.end() || matched->approximate != approximate ||
      (part ? std::ranges::find(matched->parts, *part) == matched->parts.end() : !matched->parts.empty())) {
    co_return Result::Failure({.code = kit::ErrorCode::Conflict, .message = "The affected scope changed. Inspect the design node again."});
  }
  co_return Result::Success({std::move(owner), std::move(artifact), std::move(part), std::move(diff)});
}

[[huxerui::composable]]
View ReviewDesignImpact(std::shared_ptr<kit::AgentRuntime> runtime, std::shared_ptr<const DesignImpactReview> target,
    std::function<void()> close) {
  PresentationReviewPages initial;
  if (target->part) initial.emplace(std::pair{target->artifact.id, target->artifact.version}, *target->part);
  const auto pages = UseState(std::move(initial));
  kit::AgentChatOptions options{
      .review_request = kit::AgentChatReviewRequest{target->artifact, 1}, .review_only = true};
  ConfigurePreviewArtifactReview(options, runtime, pages);
  if (target->diff) {
    options.render_artifact = [target](const kit::ArtifactPreview& preview, std::shared_ptr<kit::Conversation>,
        State<kit::AgentChatDraftSources>) -> std::optional<View> {
      if (preview.artifact != target->artifact) return {};
      return AppliedFileDiff(target->artifact, *target->part, *target->diff);
    };
  }
  auto chrome = std::make_shared<const ChatHeader>(
      [close = std::move(close)](const kit::AgentChatPage&, std::optional<View>) -> std::optional<View> {
        return Row{Button(::app::strings::ui_close_review).OnClick(close), Spacer()}.With(Padding(14.0F));
      });
  return ProvideEnvironment(ChatChrome{std::move(chrome)}, PreviewChat(target->owner, std::move(options)));
}

struct ArtboardSize { using Value = Size; };
struct ArtboardNodeBounds { using Value = Rect; };

Rect DesignBounds(const DesignNode& node) {
  return {static_cast<float>(node.x), static_cast<float>(node.y), static_cast<float>(node.width), static_cast<float>(node.height)};
}

class ArtboardLayout final : public Layout<ArtboardLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    const auto natural = *node.LayoutValue<ArtboardSize>();
    const float scale = std::min({1.0F, constraints.max_width / natural.width, constraints.max_height / natural.height});
    const Size board{natural.width * scale, natural.height * scale};
    const auto size = constraints.Constrain(board);
    const Point origin{(size.width - board.width) / 2, (size.height - board.height) / 2};
    auto result = LayoutResult{}.SetSize(size);
    for (std::size_t i = 0; i < node.ChildCount(); ++i) {
      auto& child = node.ChildAt(i);
      const auto bounds = child.LayoutValueOr<ArtboardNodeBounds>({0, 0, natural.width, natural.height});
      const auto width = bounds.width * scale;
      const auto height = bounds.height * scale;
      context.Measure(child, {.min_width = width, .max_width = width, .min_height = height, .max_height = height});
      result.Place(child, {origin.x + bounds.x * scale, origin.y + bounds.y * scale});
    }
    return result;
  }
};

std::vector<const DesignNode*> DesignPaintOrder(const DesignBoard& board) {
  std::vector<const DesignNode*> ordered;
  for (const auto& node : board.nodes) ordered.push_back(&node);
  std::ranges::stable_sort(ordered, std::greater{}, [](const auto* node) {
    return node->kind == "card" ? node->width * node->height : 0;
  });
  return ordered;
}

void PaintDesignNode(PaintContext& paint, const DesignNode& node, Rect bounds, float scale, const DesignNode* main_action) {
  const auto ink = Color::Rgb(23, 34, 53);
  const auto muted = Color::Rgb(77, 92, 112);
  const auto accent = Color::Rgb(39, 76, 171);
  const auto edge = Color::Rgb(207, 216, 230);
  const auto white = Color::White();
  const auto corners = CornerRadii{8.0F * scale};
  if (node.kind == "card") {
    const auto radius = CornerRadii{14.0F * scale};
    paint.DrawShadow(bounds, Color::Rgb(38, 54, 86, 0.12F), {0, 6.0F * scale}, 18.0F * scale, 0, radius);
    paint.DrawRect(bounds, white, radius);
    paint.DrawBorder(bounds, edge, {.width = scale}, radius);
    return;
  }
  const bool primary = &node == main_action;
  const bool link = node.kind == "link" || (node.kind == "button" && !primary && main_action &&
      node.height * 5 < main_action->height * 4);
  const bool secondary = node.kind == "secondary_button" || (node.kind == "button" && !primary && !link);
  const bool field = node.kind == "field";
  const bool heading = node.kind == "heading";
  const bool caption = node.kind == "caption";
  const bool toggle = node.kind == "toggle";
  const bool checkbox = node.kind == "checkbox";
  const bool boxed = primary || secondary || field;
  if (boxed) {
    paint.DrawRect(bounds, primary ? accent : field ? Color::Rgb(250, 252, 255) : white, corners);
    if (!primary) paint.DrawBorder(bounds, edge, {.width = scale}, corners);
  }
  float inset = boxed ? std::min(bounds.width * 0.12F, 14.0F * scale) : 0.0F;
  if (toggle || checkbox) {
    const auto size = std::min(bounds.height * 0.65F, 18.0F * scale);
    const Rect indicator{bounds.x, bounds.y + (bounds.height - size) / 2, size * (toggle ? 1.8F : 1.0F), size};
    const auto radius = CornerRadii{toggle ? size / 2 : size * 0.22F};
    paint.DrawRect(indicator, toggle ? Color::Rgb(225, 231, 240) : white, radius);
    paint.DrawBorder(indicator, edge, {.width = scale}, radius);
    if (toggle) paint.DrawCircle({indicator.x + size / 2, indicator.y + size / 2}, size * 0.35F, white);
    inset = indicator.width + 9.0F * scale;
  }
  const Rect text{bounds.x + inset, bounds.y, bounds.width - inset * (boxed ? 2.0F : 1.0F), bounds.height};
  if (text.IsEmpty()) return;
  const auto nominal = (heading ? 32.0F : caption ? 13.0F : 15.0F) * scale;
  const auto font = Font::System(std::min(nominal, text.height / 1.36F)).WithWeight(
      heading ? FontWeight::Bold : caption || primary || secondary || link ? FontWeight::SemiBold : FontWeight::Regular);
  paint.PushClip(bounds);
  paint.DrawText(text, node.label, {font, primary ? white : link || secondary ? accent : heading ? ink : muted},
      {.align = primary || secondary ? TextAlign::Center : TextAlign::Leading, .vertical_align = TextVerticalAlign::Center});
  paint.PopClip();
}

View RenderArtboard(std::shared_ptr<const DesignBoard> board, std::string highlighted, float height,
    std::function<void(std::string)> select) {
  const Size natural{static_cast<float>(board->width), static_cast<float>(board->height)};
  const auto order = DesignPaintOrder(*board);
  std::vector<View> layers{Canvas([board, order, natural, highlighted = std::move(highlighted)](PaintContext& paint, Size size) {
    const float scale = size.width / natural.width;
    if (scale <= 0.0F) return;
    const Rect surface{0, 0, size.width, size.height};
    paint.DrawRect(surface, Color::Rgb(240, 244, 251), CornerRadii{10.0F});
    paint.PushClip(surface, CornerRadii{10.0F});
    const DesignNode* main_action = nullptr;
    for (const auto& node : board->nodes) {
      if (node.kind == "button" && (!main_action || node.width * node.height > main_action->width * main_action->height))
        main_action = &node;
    }
    std::optional<Rect> selected;
    for (const auto* node : order) {
      const auto area = DesignBounds(*node);
      const Rect bounds{area.x * scale, area.y * scale, area.width * scale, area.height * scale};
      PaintDesignNode(paint, *node, bounds, scale, main_action);
      if (node->id == highlighted) selected = bounds;
    }
    if (selected) paint.DrawBorder(*selected, Color::Rgb(91, 122, 207), {.width = 1.5F}, CornerRadii{5.0F});
    paint.PopClip();
  }).Key("design-artboard-paint")};
  if (select) {
    for (const auto* node : order) {
      layers.push_back(Column{}.OnClick([select, id = node->id] { select(id); })
          .With(PointerCursor(PointerCursorKind::Hand), Semantics{.hidden = true})
          .LayoutValue<ArtboardNodeBounds>(DesignBounds(*node)).Key("design-hit/" + node->id));
    }
  }
  return ArtboardLayout(std::move(layers)).LayoutValue<ArtboardSize>(natural)
      .With(Frame{.max_height = height}, Semantics{.label = ::app::strings::ui_design_artboard});
}

} // namespace

View DesignArtboard(std::shared_ptr<const DesignBoard> board, std::string highlighted, float height) {
  return RenderArtboard(std::move(board), std::move(highlighted), height, {});
}

std::optional<View> RenderDesignArtifact(const kit::ArtifactPreview& preview) {
  if (!preview.artifact.id.value.starts_with("preview-design-") || !preview.structured) return {};
  auto verified = ParseDesign(*preview.structured);
  if (!verified.Succeeded()) return Text(verified.Error().message);
  auto board = std::make_shared<const DesignBoard>(std::move(verified).Value());
  return ScrollView(Column{Text(board->title, TextRole::Title), Text(board->brief),
      DesignArtboard(board, {}).Key("design-source-artboard")}.With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Grow(), ScrollBar());
}

namespace {

const kit::JsonValue* DesignForm(const kit::InteractionSnapshot& request) {
  const auto* form = std::get_if<kit::FormRequest>(&request.body);
  const auto* data = form ? std::get_if<kit::JsonValue::Object>(&form->presentation.value) : nullptr;
  if (!data || !data->contains("kind") || !data->contains("design")) return nullptr;
  const auto* kind = std::get_if<std::string>(&data->at("kind").value);
  return kind && *kind == DesignReview ? &data->at("design") : nullptr;
}

[[huxerui::composable]]
View DesignReviewButton(std::function<void(ViewHandle)> open) {
  const auto origin = UseViewHandle();
  return Button(::app::strings::ui_review_design).With(origin).OnClick([open, origin] { open(origin); });
}

[[huxerui::composable]]
View DesignImpactButton(StringVariant label, std::function<void(ViewHandle)> open) {
  const auto origin = UseViewHandle();
  return Button(std::move(label)).With(origin).OnClick([open, origin] { open(origin); });
}

class DesignReviewColumns final : public Layout<DesignReviewColumns> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    auto& artboard = node.ChildAt(0);
    auto& layers = node.ChildAt(1);
    const auto width = std::min(constraints.max_width, 1160.0F);
    const bool split = width >= 680.0F;
    constexpr float gap = 16.0F;
    constexpr float layers_width = 222.0F;
    const float board_width = split ? width - layers_width - gap : width;
    const auto board_size = context.Measure(artboard, constraints.Loose().TightWidth(board_width));
    const auto layers_size = context.Measure(layers, constraints.Loose().TightWidth(split ? layers_width : width));
    const auto height = split ? std::max(board_size.height, layers_size.height) : board_size.height + gap + layers_size.height;
    return LayoutResult{}.SetSize(constraints.Constrain({width, height})).Place(artboard, {})
        .Place(layers, split ? Point{board_width + gap, 0} : Point{0, board_size.height + gap});
  }
};

[[huxerui::composable]]
View DesignDetails(const ReviewedDesign& design, const std::vector<kit::ArtifactRef>& versions, std::string selected,
                   const std::function<void(std::string)>& select, const std::function<void()>& close,
                   const std::function<void(kit::ArtifactRef)>& open,
                   const std::function<void(bool)>& decide, bool pending, bool busy, std::string error,
                   std::function<void()> inspect, std::function<void()> restore, std::vector<View> impact,
                   std::optional<View> source_actions, bool detached, std::function<void()> detach) {
  if (!design.board) return Spacer();
  const auto& board = *design.board;
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  auto quiet = UseEnvironment<ButtonStyle>();
  quiet.background = Color::Transparent();
  quiet.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface_variant};
  quiet.padding = EdgeInsets::Symmetric(6.0F, 4.0F);
  quiet.minimum_height = 30.0F;
  std::vector<View> navigation;
  if (detached) {
    navigation.push_back(Button(::app::strings::ui_close_review).OnClick(close));
    navigation.push_back(Spacer());
  } else {
    navigation.push_back(PreviewBackAction(::app::strings::ui_back_to_conversation, close, theme).Key("back-to-conversation"));
    navigation.push_back(Text(board.title, TextRole::Title)
        .Style({Font::System(19.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
        .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow(), Tooltip(board.title)));
  }
  if (detach) navigation.push_back(PreviewChromeAction(::app::images::open_external, ::app::strings::ui_open_in_new_window,
      std::move(detach), theme).Key("review-open-in-new-window"));
  std::vector<View> revisions;
  std::vector<View> decisions;
  if (design.artifact) {
    const auto current = std::ranges::find(versions, *design.artifact);
    if (current != versions.end() && current != versions.begin()) {
      revisions.push_back(Button(::app::strings::ui_older_design).OnClick([open, prior = *std::prev(current)] { open(prior); }));
    }
    if (current != versions.end() && std::next(current) != versions.end()) {
      revisions.push_back(Button(::app::strings::ui_newer_design).OnClick([open, after = *std::next(current)] { open(after); }));
    }
    if (restore) revisions.push_back(Button(::app::strings::ui_restore_previous_version).OnClick(std::move(restore)).With(Enabled(!busy)));
    navigation.push_back(Text(StringVariant::Format(::app::strings::ui_version_0, design.artifact->version), TextRole::Label)
        .Style({Font::System(12.0F), colors.on_surface_variant}));
  } else if (pending) {
    decisions.push_back(ProvideEnvironment(quiet, Button(::app::strings::ui_dismiss_draft)
        .OnClick([decide] { decide(false); }).With(Enabled(!busy))));
    decisions.push_back(ProvideEnvironment(PreviewPrimaryButton(UseEnvironment<ButtonStyle>(), colors),
        Button(::app::strings::ui_approve_design).OnClick([decide] { decide(true); }).With(Enabled(!busy))));
  } else {
    navigation.push_back(Text(::app::strings::ui_review_closed, TextRole::Label));
  }

  std::vector<View> details;
  if (detached) details.push_back(Text(board.title, TextRole::Title)
      .Style({Font::System(27.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}));
  details.push_back(Text(board.brief).Style({Font::System(13.0F), colors.on_surface_variant}));
  std::vector<View> nodes;
  for (const auto& node : board.nodes) {
    const bool active = node.id == selected;
    const auto activate = [select, id = node.id] { select(id); };
    nodes.push_back(Row{
        Image(node.kind == "card" ? ::app::images::layers : ::app::images::design).Tint(colors.on_surface_variant)
            .With(Frame{.width = 13.0F, .height = 13.0F}),
        Text(node.label).Style({Font::System(12.0F).WithWeight(active ? FontWeight::SemiBold : FontWeight::Regular),
            active ? colors.on_primary_container : colors.on_surface})
            .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()),
    }.OnClick(activate).On<ViewEvents::KeyDown>([activate](const KeyEvent& event) {
      if (event.key != Key::Enter && event.key != Key::Space) return false;
      if (!event.repeat) activate();
      return true;
    }).With(Frame{.min_height = 30.0F}, Padding(EdgeInsets::Symmetric(8.0F, 3.0F)), Spacing(7.0F),
        CrossAlign(CrossAxisAlignment::Center), CornerRadius(6.0F), Focusable{}, PointerCursor(PointerCursorKind::Hand),
        PreviewRowIndication(theme, 6.0F), Background(active ? colors.primary_container : Color::Transparent()),
        Tooltip(node.label), Semantics{.role = SemanticRole::Button, .label = node.label, .selected = active})
        .Key("design-node/" + node.id));
  }
  std::vector<View> inspector{
      Text(::app::strings::ui_design_layers, TextRole::Label)
          .Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface_variant})
          .With(Padding(EdgeInsets::Symmetric(8.0F, 0.0F))),
      ScrollView(Column(std::move(nodes)).With(CrossAlign(CrossAxisAlignment::Stretch)))
          .With(Frame{.max_height = 210.0F}, ScrollBar()).Key("design-layers-list"),
  };
  const auto node = std::ranges::find(board.nodes, selected, &DesignNode::id);
  if (node != board.nodes.end()) {
    inspector.push_back(Divider());
    inspector.push_back(Column{
        Text(node->label).Style({Font::System(13.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
        Text(node->note).Style({Font::System(12.0F), colors.on_surface_variant}),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch), Padding(EdgeInsets::Symmetric(8.0F, 0.0F)))
        .Key("design-selection/" + node->id));
  }
  if (source_actions) inspector.push_back(ProvideEnvironment(quiet, std::move(*source_actions)));
  if (inspect) inspector.push_back(ProvideEnvironment(quiet, Button(::app::strings::ui_inspect_impact).OnClick(std::move(inspect))));
  details.push_back(DesignReviewColumns{
      RenderArtboard(design.board, selected, 420.0F, select).With(Padding(14.0F), CornerRadius(10.0F),
          Border{colors.outline}, Background(colors.surface_container_low)).Key("design-review-artboard"),
      Column(std::move(inspector)).With(Spacing(8.0F), Padding(10.0F), CrossAlign(CrossAxisAlignment::Stretch),
          CornerRadius(10.0F), Border{colors.outline}, Background(colors.surface_container)).Key("design-inspector"),
  });
  if (!revisions.empty()) details.push_back(ProvideEnvironment(quiet, Flow(std::move(revisions)).With(Spacing(7.0F))));
  details.insert(details.end(), impact.begin(), impact.end());
  std::vector<View> page{
      ProvideEnvironment(quiet, Row(std::move(navigation)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))),
      ScrollView(Column(std::move(details)).With(Spacing(13.0F), CrossAlign(CrossAxisAlignment::Stretch)))
          .With(Grow(), ScrollBar()).Key("design-review-scroll"),
  };
  if (!error.empty()) page.push_back(Text(std::move(error), TextRole::Label));
  if (!decisions.empty()) {
    page.push_back(Divider());
    auto actions = Row(std::move(decisions)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
    page.push_back(Row{Spacer(), std::move(actions)});
  }
  return Row{Column(std::move(page)).With(Frame{.max_width = 1160.0F}, Grow(), Spacing(12.0F),
      CrossAlign(CrossAxisAlignment::Stretch)).Key("design-review-content")}
      .With(Grow(), Padding(18.0F), MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]]
View DesignChat(std::shared_ptr<kit::Conversation> conversation, std::shared_ptr<kit::AgentRuntime> runtime,
                kit::AgentChatOptions base_options, std::optional<DesignReviewLocation> window_review,
                std::optional<File> evidence) {
  const auto tasks = UseTaskScope();
  const auto application = UseApplication();
  const auto window = UseWindow();
  const auto snapshot = UseState(conversation->Snapshot());
  const auto review = UseState(ReviewedDesign{});
  const auto selected = UseState(std::string{});
  const auto review_nodes = UseState(ReviewNodes{});
  const auto error = UseState(std::string{});
  const auto impact = UseState(std::shared_ptr<const std::vector<DesignImpactItem>>{});
  const auto impact_message = UseState(std::string{});
  const auto linked_impact = UseState(std::shared_ptr<const DesignImpactReview>{});
  const auto impact_focus = UseState(std::optional<ViewHandle>{});
  const auto restore_impact_focus = UseState(false);
  const auto busy = UseState(false);
  const auto request_id = UseState(std::uint64_t{0});
  const auto source_focus = UseState(std::optional<ViewHandle>{});
  const auto return_focus = UseState(std::optional<ViewHandle>{});
  const auto latest_focus = UseViewHandle();
  const auto local_composer_focus = UseViewHandle();
  const auto composer_focus = base_options.composer_focus.value_or(local_composer_focus);
  const auto local_sources = UseState(kit::AgentChatDraftSources{});
  const auto draft_sources = base_options.draft_sources.value_or(local_sources);
  Lifecycle([conversation, snapshot, error, tasks] {
    auto subscription = std::make_shared<std::optional<kit::Subscription>>();
    auto observe = tasks.Launch([conversation, snapshot, subscription, error]() -> Task<void> {
      auto result = co_await conversation->ObserveAsync([snapshot, error](auto update) {
        if (update.Succeeded()) snapshot = std::move(update).Value();
        else { snapshot = std::shared_ptr<const kit::SessionSnapshot>{}; error = update.Error().message; }
      });
      if (result.Succeeded()) *subscription = std::move(result).Value();
      else { snapshot = std::shared_ptr<const kit::SessionSnapshot>{}; error = result.Error().message; }
    });
    return [observe, subscription] { observe.Cancel(); subscription->reset(); };
  }, conversation);

  const auto close = [review, error, impact, impact_message, request_id, source_focus, return_focus, window,
                      detached = window_review.has_value()] {
    if (detached) { window.Close(); return; }
    request_id += 1;
    review = ReviewedDesign{};
    error = "";
    impact = nullptr;
    impact_message = "";
    return_focus = source_focus.Get();
  };
  Lifecycle([return_focus, latest_focus] {
    if (const auto origin = return_focus.Get()) {
      return_focus = std::nullopt;
      if (!origin->RequestFocus(true)) latest_focus.RequestFocus(true);
    }
  }, return_focus.Get());
  Lifecycle([restore_impact_focus, impact_focus] {
    if (!restore_impact_focus.Get()) return;
    restore_impact_focus = false;
    if (const auto origin = impact_focus.Get()) origin->RequestFocus(true);
    impact_focus = std::nullopt;
  }, restore_impact_focus.Get());
  const auto show_published = [review, selected, review_nodes, error](const kit::ArtifactPreview& item) {
    if (!item.structured) { error = "Design version has no artboard."; return; }
    auto board = ParseDesign(*item.structured);
    if (!board.Succeeded()) { error = board.Error().message; return; }
    auto value = std::make_shared<const DesignBoard>(std::move(board).Value());
    const auto& nodes = review_nodes.Get();
    const auto saved = nodes.find({item.artifact.id, item.artifact.version});
    const auto& node = saved == nodes.end() ? selected.Get() : saved->second;
    selected = std::ranges::find(value->nodes, node, &DesignNode::id) == value->nodes.end()
        ? value->nodes.front().id : node;
    review = ReviewedDesign{.board = std::move(value), .artifact = item.artifact, .access_revision = item.access_revision};
    error = "";
  };
  const auto open = [conversation, tasks, show_published, error, impact, impact_message, request_id](kit::ArtifactRef ref) {
    const auto ticket = request_id.Get() + 1;
    request_id = ticket;
    impact = nullptr;
    impact_message = "";
    tasks.Launch([conversation, show_published, error, request_id, ticket, ref = std::move(ref)]() -> Task<void> {
      auto item = co_await conversation->PreviewArtifactAsync(ref);
      if (ticket != request_id.Get()) co_return;
      if (!item.Succeeded()) { error = item.Error().message; co_return; }
      show_published(item.Value());
    });
  };
  Lifecycle([window_review, snapshot, review, selected, error, open] {
    if (!window_review) return;
    selected = window_review->node;
    if (const auto* artifact = std::get_if<kit::ArtifactRef>(&window_review->source)) { open(*artifact); return; }
    const auto& approval = std::get<kit::AgentChatApprovalRef>(window_review->source);
    const auto current = snapshot.Get();
    if (!current) { error = "Conversation unavailable"; return; }
    const auto found = std::ranges::find(current->interactions, approval.id, &kit::InteractionSnapshot::id);
    const auto* document = found != current->interactions.end() && found->version == approval.version ? DesignForm(*found) : nullptr;
    if (!document) { error = "Design review unavailable"; return; }
    auto parsed = ParseDesign(*document);
    if (!parsed.Succeeded()) { error = parsed.Error().message; return; }
    auto board = std::make_shared<const DesignBoard>(std::move(parsed).Value());
    if (std::ranges::find(board->nodes, selected.Get(), &DesignNode::id) == board->nodes.end()) selected = board->nodes.front().id;
    review = ReviewedDesign{.board = std::move(board), .review = approval};
  }, conversation, window_review);
  const auto current = snapshot.Get();
  std::function<void()> inspect;
  std::vector<View> affected;
  if (current && current->summary.project && review->artifact && !selected->empty()) {
    const kit::SourceSlice source{*review->artifact, {kit::PartId{selected.Get()}}};
    const kit::ArtifactImpactQuery query{*current->summary.project, {{source, {}, kit::SourceChangeKind::Modified}}};
    inspect = [runtime, tasks, impact, impact_message, request_id, query] {
      const auto ticket = request_id.Get() + 1;
      request_id = ticket;
      impact = nullptr;
      impact_message = "Checking affected artifacts";
      tasks.Launch([runtime, impact, impact_message, request_id, ticket, query]() -> Task<void> {
        auto matches = co_await runtime->Artifacts().InspectImpactAsync(query);
        if (request_id.Get() != ticket) co_return;
        if (!matches.Succeeded()) { impact_message = matches.Error().message; co_return; }
        std::vector<DesignImpactItem> rows;
        for (const auto& item : matches.Value().affected) {
          auto record = co_await runtime->Artifacts().ReadAsync(item.affected);
          if (request_id.Get() != ticket) co_return;
          if (!record.Succeeded()) continue;
          DesignImpactItem row{item.affected, record.Value().metadata.name, item.approximate};
          const auto& parts = record.Value().parts;
          for (std::size_t index = 0; index < parts.size(); ++index) {
            const auto& part = parts[index];
            if (std::ranges::find(item.parts, part.id) == item.parts.end()) continue;
            auto label = part.label;
            if (record.Value().metadata.type == "presentation/pptx")
              label = "slide " + std::to_string(index + 1) + " / " + label;
            row.parts.push_back({part.id, std::move(label)});
          }
          if (item.parts.empty() || !row.parts.empty()) rows.push_back(std::move(row));
        }
        impact_message = rows.empty() ? "No visible artifacts depend on this node" : "";
        impact = std::make_shared<const std::vector<DesignImpactItem>>(std::move(rows));
      });
    };
    const auto open_affected = [runtime, tasks, request_id, impact, impact_message, linked_impact, impact_focus, query, evidence](
        kit::ArtifactRef artifact, std::optional<kit::PartId> part, bool approximate, ViewHandle origin) {
      const auto ticket = request_id.Get() + 1;
      request_id = ticket;
      impact_message = "Opening affected artifact";
      tasks.Launch([runtime, query, artifact = std::move(artifact), part = std::move(part), approximate, evidence, origin,
                    request_id, ticket, impact, impact_message, linked_impact, impact_focus]() -> Task<void> {
        auto reviewed = co_await OpenDesignImpact(runtime, query, artifact, part, approximate, evidence);
        if (request_id.Get() != ticket) co_return;
        if (!reviewed.Succeeded()) {
          impact = nullptr;
          impact_message = reviewed.Error().message;
          co_return;
        }
        impact_message = "";
        impact_focus = origin;
        linked_impact = std::make_shared<const DesignImpactReview>(std::move(reviewed).Value());
      });
    };
    if (const auto rows = impact.Get()) {
      for (const auto& row : *rows) {
        auto title = row.name + "  v" + std::to_string(row.artifact.version);
        if (row.approximate) title += " (approximate scope)";
        affected.push_back(Text(std::move(title), TextRole::Label));
        const auto link = [open_affected, artifact = row.artifact, approximate = row.approximate](std::optional<kit::PartId> part) {
          return [open_affected, artifact, approximate, part = std::move(part)](ViewHandle origin) {
            open_affected(artifact, part, approximate, std::move(origin));
          };
        };
        const auto key = "design-impact/" + row.artifact.id.value + "/" + std::to_string(row.artifact.version);
        if (row.parts.empty()) affected.push_back(DesignImpactButton(StringVariant::Format(::app::strings::ui_review_0, row.name), link({})).Key(key));
        for (const auto& part : row.parts) {
          affected.push_back(DesignImpactButton(StringVariant::Format(::app::strings::ui_review_0, part.label), link(part.id)).Key(key + "/" + part.id.value));
        }
      }
    }
    if (!impact_message->empty()) affected.push_back(Text(impact_message.Get(), TextRole::Label));
  }
  const auto* observed_review = static_cast<const kit::InteractionSnapshot*>(nullptr);
  if (current && review->review) {
    auto finding = std::ranges::find(current->interactions, review->review->id, &kit::InteractionSnapshot::id);
    if (finding != current->interactions.end()) observed_review = &*finding;
  }
  const bool pending = observed_review && observed_review->state == kit::InteractionState::Pending &&
      observed_review->version == review->review->version;
  const auto decide = [conversation, tasks, busy, error, close, approval = review->review, pending,
                       detached = window_review.has_value()](bool approve) {
    if (busy.Get() || !approval || !pending) return;
    busy = true;
    tasks.Launch([conversation, busy, error, binding = *approval, approve, close, detached]() -> Task<void> {
      kit::InteractionReply answer{.id = binding.id, .expected_version = binding.version};
      if (approve) answer.response = kit::JsonValue{true};
      else answer.response = kit::DismissForm{};
      auto submitted = co_await conversation->SubmitAsync(kit::NewCommandId(), std::move(answer));
      busy = false;
      if (submitted.Succeeded()) { if (!detached) close(); }
      else error = submitted.Error().message;
    });
  };

  std::vector<kit::ArtifactRef> versions;
  if (current) {
    for (const auto& artifact : current->artifacts) {
      if (!artifact.id.value.starts_with("preview-design-")) continue;
      if (review->artifact && artifact.id != review->artifact->id) continue;
      versions.push_back(artifact);
    }
  }

  auto options = base_options;
  options.render_artifact = [fallback = base_options.render_artifact](const kit::ArtifactPreview& item,
      std::shared_ptr<kit::Conversation> session, State<kit::AgentChatDraftSources> draft) -> std::optional<View> {
    if (auto design = RenderDesignArtifact(item)) return design;
    return fallback ? fallback(item, std::move(session), draft) : std::nullopt;
  };
  if (options.review_only) return PreviewChat(conversation, std::move(options));
  options.open_artifact_review = [show_published, source_focus, request_id, impact, impact_message,
      fallback = base_options.open_artifact_review](const kit::ArtifactPreview& item, std::optional<ViewHandle> origin) {
    if (item.type != DesignArtifactType) return fallback && fallback(item, std::move(origin));
    request_id += 1;
    source_focus = std::move(origin);
    impact = nullptr;
    impact_message = "";
    show_published(item);
    return true;
  };
  options.render_form = [review, selected, error, impact, impact_message, request_id, source_focus](
      const kit::InteractionSnapshot& request, std::shared_ptr<kit::Conversation>) -> std::optional<View> {
    const auto* document = DesignForm(request);
    if (!document) return {};
    auto parsed = ParseDesign(*document);
    if (!parsed.Succeeded()) return Text(parsed.Error().message);
    return DesignReviewButton([review, selected, error, impact, impact_message, request_id, source_focus,
        board = std::make_shared<const DesignBoard>(std::move(parsed).Value()), binding = kit::AgentChatApprovalRef{request.id, request.version}](ViewHandle origin) {
      request_id += 1;
      source_focus = std::move(origin);
      selected = board->nodes.front().id;
      review = ReviewedDesign{.board = board, .review = binding};
      error = "";
      impact = nullptr;
      impact_message = "";
    });
  };
  options.draft_sources = draft_sources;
  options.composer_focus = composer_focus;

  std::vector<View> conversation_page;
  const bool latest_in_history = !versions.empty() && std::ranges::any_of(current->messages, [&](const auto& message) {
    return std::ranges::any_of(message.blocks, [&](const auto& block) {
      const auto* artifact = std::get_if<kit::ArtifactBlock>(&block.body);
      return artifact && artifact->artifact == versions.back();
    });
  });
  if (!versions.empty() && !latest_in_history) conversation_page.push_back(
      Button(::app::strings::ui_open_latest_design).With(latest_focus).OnClick([open, latest_focus, source_focus, current = versions.back()] {
        source_focus = latest_focus;
        open(current);
      }));
  if (!error->empty()) conversation_page.push_back(Text(error.Get(), TextRole::Label));
  conversation_page.push_back(PreviewChat(conversation, std::move(options)).With(Grow()));
  const auto select = [selected, review_nodes, artifact = review->artifact, impact, impact_message, request_id](std::string id) {
    request_id += 1;
    if (artifact) {
      auto nodes = review_nodes.Get();
      nodes.insert_or_assign({artifact->id, artifact->version}, id);
      review_nodes = std::move(nodes);
    }
    selected = std::move(id);
    impact = nullptr;
    impact_message = "";
  };
  std::optional<View> source_actions;
  if (current && review->artifact && !selected->empty()) {
    kit::SourceSlice source{*review->artifact, {kit::PartId{selected.Get()}}};
    std::function<void()> return_to_draft;
    if (!window_review) return_to_draft = [close, return_focus, composer_focus, request_id, ticket = request_id.Get()] {
      if (request_id.Get() != ticket) return;
      close();
      return_focus = composer_focus;
    };
    source_actions = PreviewSourceActions(conversation, std::move(source), draft_sources, std::move(return_to_draft))
        .Key(review->artifact->id.value + "/" + std::to_string(review->artifact->version) + "/" + selected.Get());
  }
  std::function<void()> restore;
  if (current && review->artifact) {
    auto request = DesignRevertRequest(*current, *review->artifact);
    if (request) {
      restore = [runtime, tasks, busy, error, close, request_id, request = std::move(*request)] {
        if (busy.Get()) return;
        busy = true;
        const auto ticket = request_id.Get();
        tasks.Launch([runtime, busy, error, close, request_id, ticket, request]() -> Task<void> {
          auto accepted = co_await runtime->Artifacts().PrepareRevertAsync(kit::NewCommandId(), request);
          busy = false;
          if (request_id.Get() != ticket) co_return;
          if (accepted.Succeeded()) close();
          else error = accepted.Error().message;
        });
      };
    }
  }
  const bool visible = current && review->board && (review->artifact ?
      review->access_revision == current->access_revision : observed_review != nullptr);
  std::function<void()> detach;
  if (visible && !window_review && application.SupportsMultipleWindows()) {
    DesignReviewLocation location;
    if (review->artifact) location.source = *review->artifact;
    else location.source = *review->review;
    location.node = selected.Get();
    detach = [application, conversation, runtime, location, draft_sources, evidence, error, title = review->board->title] {
      try {
        OpenPreviewDesignReview(application, conversation, runtime, location, title, draft_sources, evidence);
      } catch (const std::exception& failure) { error = failure.what(); }
    };
  }
  auto details = visible ? DesignDetails(review.Get(), window_review ? std::vector<kit::ArtifactRef>{} : versions,
      selected.Get(), select, close, open, decide, pending, busy.Get(), error.Get(), std::move(inspect),
      window_review ? std::function<void()>{} : std::move(restore), std::move(affected), std::move(source_actions),
      window_review.has_value(), std::move(detach)) : View{Spacer()};
  if (visible) {
    const auto target = linked_impact.Get();
    auto impact_review = target ? ReviewDesignImpact(runtime, target, [linked_impact, restore_impact_focus] {
      linked_impact = nullptr;
      restore_impact_focus = true;
    }) : View{Spacer()};
    details = IndexedPages({details, impact_review}, target ? 1U : 0U).With(Grow());
  }
  if (window_review) {
    const auto settings = UseService<State<PreviewSettings>>();
    const auto theme = PreviewThemeFor(settings ? settings->Get() : PreviewSettings{}, UseSystemThemeBrightness());
    if (visible) return MaterialTheme(theme, PreviewControlsTheme(std::move(details).With(Background(theme.colors.surface))));
    const auto message = !error->empty() ? error.Get() : review->board ? "Design review unavailable" : "Opening design review...";
    return MaterialTheme(theme, PreviewControlsTheme(
        Column{Button(::app::strings::ui_close_review).OnClick(close), Text(message)}
            .With(Grow(), Padding(24.0F), Spacing(12.0F), Background(theme.colors.surface))));
  }
  return IndexedPages({
      Column(std::move(conversation_page)).With(Grow(), Spacing(8.0F)),
      details,
  }, visible ? 1U : 0U).With(Grow());
}

huxerui::WindowHandle OpenPreviewDesignReview(const huxerui::ApplicationHandle& app,
    std::shared_ptr<kit::Conversation> conversation, std::shared_ptr<kit::AgentRuntime> runtime,
    DesignReviewLocation location, std::string title, std::optional<State<kit::AgentChatDraftSources>> draft_sources,
    std::optional<File> evidence) {
  return app.OpenWindow([conversation = std::move(conversation), runtime = std::move(runtime), location = std::move(location),
                         draft_sources, evidence] {
    return DesignChat(conversation, runtime, {.draft_sources = draft_sources}, location, evidence);
  }, {.title = std::move(title) + " - Design review", .initial_size = {900.0F, 760.0F}});
}

} // namespace agentkit_preview
