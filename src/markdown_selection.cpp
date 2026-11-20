#include "markdown_selection.h"

#include <huxerui/huxerui.h>
#include <huxerui/clipboard.h>
#include <huxerui/modifier.h>
#include <huxerui/paint.h>
#include <huxerui/text_input.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>

namespace huxerui::agentkit::detail {

markdown::ParseOptions MarkdownSelectionParseOptions() {
  markdown::ParseOptions options{.soft_break_mode = markdown::SoftBreakMode::LineBreak};
#ifdef HUXERUI_MARKDOWN_SOURCE_MAPPING
  options.track_source_positions = true;
#endif
  return options;
}

bool IsPlainMarkdown(const markdown::MarkdownDocument& document, std::string_view source) {
  const auto& blocks = document.Root().children;
  if (blocks.size() != 1 || blocks.front().kind != markdown::NodeKind::Paragraph) return false;
  const auto& contents = blocks.front().children;
  return contents.size() == 1 && contents.front().kind == markdown::NodeKind::Text && contents.front().text == source;
}

std::optional<std::size_t> Utf8Offset(std::string_view text, TextOffset units) {
  if (units < 0) return {};
  std::size_t offset = 0;
  while (offset < text.size() && units > 0) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    const auto count = byte < 0x80 ? 1U : byte < 0xe0 ? 2U : byte < 0xf0 ? 3U : 4U;
    units -= count == 4 ? 2 : 1;
    offset += count;
  }
  return units == 0 && offset <= text.size() ? std::optional{offset} : std::nullopt;
}

namespace {

struct MarkdownImage {
  std::string destination;
  std::string alt;
};

View RenderMarkdownImage(MarkdownImage image, MarkdownImageLoad load, View fallback,
                         std::function<View(View)> decorate = {}) {
  return Scope([image = std::move(image), load = std::move(load), fallback = std::move(fallback),
                decorate = std::move(decorate)]() -> View {
    const auto asset = UseState(ImageAsset{});
    const auto tasks = UseTaskScope();
    Lifecycle([asset, tasks, image, load] {
      asset = ImageAsset{};
      const auto request = tasks.Launch([asset, image, load]() -> Task<void> {
        try {
          asset = co_await load(image.destination);
        } catch (const std::exception&) {
        }
      });
      return [request] { request.Cancel(); };
    }, image.destination);
    if (!asset.Get().HasValue()) return fallback;
    auto view = Image(asset.Get()).Fit(ImageFit::ScaleDown)
        .With(Frame{.max_height = 260.0F}, Semantics{.role = SemanticRole::Image, .label = image.alt})
        .Key("markdown-image");
    return decorate ? decorate(std::move(view)) : std::move(view);
  });
}

#ifdef HUXERUI_MARKDOWN_SOURCE_MAPPING
struct ParagraphText {
  AttributedText text;
  TextStyle style;
  TextAlign alignment;
  std::vector<markdown::MarkdownSourceSegment> source;
  bool image = false;
};

struct ParagraphKey {
  using Value = std::shared_ptr<const ParagraphText>;
};

void AppendSourceRange(MarkdownSourceExcerpt& excerpt, std::size_t begin, std::size_t end) {
  if (!excerpt.source_bytes.empty() && excerpt.source_bytes.back().second == begin)
    excerpt.source_bytes.back().second = end;
  else excerpt.source_bytes.emplace_back(begin, end);
}

void CollectSourceExcerpt(const markdown::MarkdownNode& node, std::size_t begin, std::size_t end,
                          MarkdownSourceExcerpt& excerpt) {
  using markdown::NodeKind;
  switch (node.kind) {
  case NodeKind::Paragraph:
  case NodeKind::Heading:
  case NodeKind::TableHeaderCell:
  case NodeKind::TableCell:
  case NodeKind::CodeBlock:
    break;
  default:
    for (const auto& child : node.children) CollectSourceExcerpt(child, begin, end, excerpt);
    return;
  }
  auto [rendered, parts] = markdown::PlainTextWithSource(node);
  bool first_part = true;
  for (const auto& part : parts) {
    const auto first = std::max(begin, part.source_begin);
    const auto last = std::min(end, part.source_end);
    if (first >= last) continue;
    const auto text_begin = part.transformed ? part.text_begin : part.text_begin + first - part.source_begin;
    const auto text_end = part.transformed ? part.text_end : part.text_begin + last - part.source_begin;
    if (first_part && !excerpt.text.empty()) excerpt.text += '\n';
    first_part = false;
    excerpt.text.append(rendered, text_begin, text_end - text_begin);
    AppendSourceRange(excerpt, part.transformed ? part.source_begin : first, part.transformed ? part.source_end : last);
  }
}

void CollectImages(const markdown::MarkdownNode& node, std::vector<MarkdownImage>& images) {
  if (node.kind == markdown::NodeKind::Image) {
    images.push_back({node.destination, markdown::PlainText(node)});
    return;
  }
  for (const auto& child : node.children) CollectImages(child, images);
}

View DecorateParagraph(View view, const markdown::MarkdownNode& node, const AttributedText& text, const TextStyle& style,
                        TextAlign alignment, std::span<const markdown::MarkdownSourceSegment> source,
                        bool selectable, const MarkdownImageLoad& load) {
  std::shared_ptr<ParagraphText> content;
  if (selectable) {
    content = std::make_shared<ParagraphText>(ParagraphText{text, style, alignment, {source.begin(), source.end()}});
    view = std::move(view).LayoutValue<ParagraphKey>(content);
  }
  if (!load) return view;
  std::vector<MarkdownImage> images;
  CollectImages(node, images);
  if (images.empty()) return view;
  const auto standalone = node.kind == markdown::NodeKind::Paragraph && node.children.size() == 1 &&
      node.children.front().kind == markdown::NodeKind::Image;
  if (standalone) {
    std::function<View(View)> decorate;
    if (content) {
      auto image_content = std::make_shared<ParagraphText>(*content);
      image_content->image = true;
      decorate = [image_content](View image) { return std::move(image).LayoutValue<ParagraphKey>(image_content); };
    }
    return RenderMarkdownImage(std::move(images.front()), load, std::move(view), std::move(decorate));
  }
  std::vector<View> children{std::move(view)};
  for (auto& image : images) children.push_back(RenderMarkdownImage(std::move(image), load, Column(std::vector<View>{})));
  return Column(std::move(children)).With(CrossAlign(CrossAxisAlignment::Stretch));
}

Rect InOwner(const ViewNode& owner, const ViewNode& node, Rect rect) {
  const auto first = owner.WindowToLocal(node.LocalToWindow({rect.x, rect.y}));
  const auto last = owner.WindowToLocal(node.LocalToWindow({rect.x + rect.width, rect.y + rect.height}));
  if (!first || !last) return {};
  return {first->x, first->y, last->x - first->x, last->y - first->y};
}

struct SelectionBehavior;

struct SelectionModifier {
  using Extension = SelectionBehavior;
  markdown::MarkdownDocument document;
  Color highlight;
  ViewHandle focus;
  std::optional<MarkdownRange> selection;
  std::function<void(std::optional<MarkdownExcerpt>)> changed;
};

struct ParagraphGeometry {
  std::shared_ptr<const ParagraphText> content;
  TextLayout layout;
  Rect bounds;
  Rect clip;
  float width;
};

struct SelectionBehavior final : NodeExtension, TextSelectionClient {
  SelectionBehavior(ViewNode&, const SelectionModifier& spec) : spec_(spec), range_(spec.selection) {}

  void Update(ViewNode&, const SelectionModifier& spec) {
    const bool same_document = spec_.document == spec.document;
    if (!same_document) {
      pressed_ = false;
      range_.reset();
    }
    spec_ = spec;
    const bool caret = range_ && range_->anchor == range_->active && !spec_.selection;
    if ((!caret && range_ != spec_.selection) || painted_color_ != spec_.highlight) {
      range_ = spec_.selection;
      InvalidatePaint();
    }
  }

  PaintInvalidation PrepareGeometry(ViewNode& owner, TextMeasurer& measurer) override {
    std::vector<ParagraphGeometry> next;
    auto collect = [&](auto&& visit, const ViewNode& node, Rect clip) -> void {
      clip = clip.Intersection(InOwner(owner, node, node.Bounds()));
      if (const auto* content = node.LayoutValue<ParagraphKey>()) {
        const auto bounds = node.ContentBounds();
        const auto index = next.size();
        auto previous = index < paragraphs_.size() ? &paragraphs_[index] : nullptr;
        const auto layout = previous && previous->width == bounds.width && previous->content->text == (*content)->text &&
            previous->content->style == (*content)->style && previous->content->alignment == (*content)->alignment
            ? previous->layout : measurer.CreateTextLayout((*content)->text, (*content)->style, bounds.width,
                {.align = (*content)->alignment});
        const auto local = InOwner(owner, node, bounds);
        next.push_back({*content, layout, local, clip.Intersection(local), bounds.width});
        return;
      }
      for (std::size_t i = 0; i < node.ChildCount(); ++i) visit(visit, node.ChildAt(i), clip);
    };
    collect(collect, owner, owner.Bounds());
    paragraphs_ = std::move(next);
    enabled_ = owner.IsEnabled();
    painted_color_ = spec_.highlight;
    return range_ ? PaintInvalidation::Foreground : PaintInvalidation::None;
  }

  TextSelectionClient* GetTextSelectionClient() noexcept override { return this; }

  bool HitTest(ViewNode& node, Point point) const override {
    return node.IsEnabled() && node.Bounds().Contains(point) && !paragraphs_.empty();
  }

  PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
    if (!enabled_ || event.type == PointerEventType::Cancel) {
      pressed_ = false;
      return PointerResult::Ignored;
    }
    if (event.device_kind == PointerDeviceKind::Touch)
      return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
    if (event.type == PointerEventType::Down) {
      if (event.changed_button != PointerButton::None && event.changed_button != PointerButton::Primary)
        return PointerResult::Ignored;
      const auto hit = PositionAt(event.position);
      if (!hit) return PointerResult::Ignored;
      origin_ = event.position;
      pressed_ = true;
      dragging_ = event.modifiers.shift && range_.has_value();
      static_cast<void>(spec_.focus.RequestFocus());
      SetRange({dragging_ ? range_->anchor : *hit, *hit});
      return dragging_ ? PointerResult::CancelTarget : PointerResult::Observe;
    }
    if (!pressed_) return PointerResult::Ignored;
    if (event.type == PointerEventType::Up) pressed_ = false;
    if (!dragging_ && std::hypot(event.position.x - origin_.x, event.position.y - origin_.y) < 6.0F)
      return PointerResult::Observe;
    const auto hit = PositionAt(event.position);
    if (!hit) return PointerResult::Observe;
    dragging_ = true;
    if (range_) SetRange({range_->anchor, *hit});
    return PointerResult::CancelTarget;
  }

  bool OnKey(ViewNode&, const KeyEvent& event) override {
    if (event.type != KeyEventType::Down || !range_ ||
        (event.key != Key::ArrowLeft && event.key != Key::ArrowRight && event.key != Key::Home && event.key != Key::End))
      return false;
    auto next = range_->active;
    if (next.paragraph >= paragraphs_.size()) return false;
    const auto& paragraph = paragraphs_[next.paragraph];
    if (event.key == Key::Home) {
      next.offset = 0;
    } else if (event.key == Key::End) {
      next.offset = paragraph.content->text.Length();
    } else if (event.key == Key::ArrowLeft) {
      if (next.offset == 0 && next.paragraph > 0)
        next = {next.paragraph - 1, paragraphs_[next.paragraph - 1].content->text.Length()};
      else next.offset = paragraph.layout.PreviousCaretOffset(next.offset);
    } else {
      if (next.offset == paragraph.content->text.Length() && next.paragraph + 1 < paragraphs_.size())
        next = {next.paragraph + 1, 0};
      else next.offset = paragraph.layout.NextCaretOffset(next.offset);
    }
    SetRange({event.modifiers.shift ? range_->anchor : next, next});
    return true;
  }

  void PaintAboveContent(const ViewNode&, PaintContext& painter) const override {
    if (!Selection()) return;
    for (std::size_t i = First().paragraph; i <= Last().paragraph; ++i) {
      if (paragraphs_[i].content->image) {
        if (!RangeIn(i).IsCollapsed()) painter.DrawRect(paragraphs_[i].clip, spec_.highlight);
        continue;
      }
      for (auto rect : paragraphs_[i].layout.RangeRects(RangeIn(i))) {
        rect = ToOwner(i, rect);
        if (!rect.IsEmpty()) painter.DrawRect(rect, spec_.highlight);
      }
    }
  }

  TextEditingActionStatus GetTextEditingActionStatus(TextEditingAction action, const Clipboard& clipboard) const override {
    if (action == TextEditingAction::Copy)
      return Selection() && clipboard.SupportsSynchronousText() ? TextEditingActionStatus::Handle : TextEditingActionStatus::Disabled;
    if (action == TextEditingAction::SelectAll)
      return paragraphs_.empty() ? TextEditingActionStatus::Disabled : TextEditingActionStatus::Handle;
    return TextEditingActionStatus::Disabled;
  }

  void PerformTextEditingAction(TextEditingAction action, const Clipboard& clipboard) override {
    if (action == TextEditingAction::SelectAll && !paragraphs_.empty()) {
      SetRange({{0, 0}, {paragraphs_.size() - 1, paragraphs_.back().content->text.Length()}});
    } else if (action == TextEditingAction::Copy && Selection()) {
      static_cast<void>(clipboard.WriteText(Excerpt().source.text));
    }
  }

  bool ClearSelection() override {
    if (!range_) return false;
    range_.reset();
    InvalidatePaint();
    spec_.changed(std::nullopt);
    return true;
  }

  bool SelectWord(Point point) override {
    auto hit = PositionAt(point);
    if (!hit || !enabled_) return false;
    const auto& paragraph = paragraphs_[hit->paragraph];
    auto start = hit->offset;
    if (start == paragraph.content->text.Length()) start = paragraph.layout.PreviousCaretOffset(start);
    auto end = paragraph.layout.NextCaretOffset(start);
    const auto word_boundary = [&](TextOffset from, TextOffset to) {
      const auto scalar = paragraph.content->text.TextInRange({from, to});
      if (scalar.empty()) return true;
      const auto byte = static_cast<unsigned char>(scalar.front());
      return byte < 0x80 && !((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= 'a' && byte <= 'z') || byte == '_');
    };
    if (!word_boundary(start, end)) {
      while (start > 0) {
        const auto previous = paragraph.layout.PreviousCaretOffset(start);
        if (previous == start || word_boundary(previous, start)) break;
        start = previous;
      }
      while (end < paragraph.content->text.Length()) {
        const auto next = paragraph.layout.NextCaretOffset(end);
        if (next == end || word_boundary(end, next)) break;
        end = next;
      }
    }
    SetRange({{hit->paragraph, start}, {hit->paragraph, end}});
    return start != end;
  }

  bool ExtendSelection(Point point, bool start_handle) override {
    const auto hit = PositionAt(point);
    if (!Selection() || !hit || !enabled_) return false;
    SetRange(start_handle ? MarkdownRange{Last(), std::min(Last(), *hit)} : MarkdownRange{First(), std::max(First(), *hit)});
    return true;
  }

  std::optional<TextSelectionGeometry> QuerySelectionGeometry() const override {
    if (!Selection()) return {};
    auto start = Caret(First());
    auto end = Caret(Last());
    return TextSelectionGeometry{start, end, start ? start : end};
  }

  Color SelectionHandleColor() const noexcept override {
    auto color = spec_.highlight;
    color.alpha = 1.0F;
    return color;
  }

private:
  bool Selection() const {
    return range_ && range_->anchor != range_->active && Last().paragraph < paragraphs_.size() &&
        First().offset <= paragraphs_[First().paragraph].content->text.Length() &&
        Last().offset <= paragraphs_[Last().paragraph].content->text.Length();
  }

  MarkdownPosition First() const { return std::min(range_->anchor, range_->active); }

  MarkdownPosition Last() const { return std::max(range_->anchor, range_->active); }

  TextRange RangeIn(std::size_t index) const {
    return {index == First().paragraph ? First().offset : 0,
            index == Last().paragraph ? Last().offset : paragraphs_[index].content->text.Length()};
  }

  Rect ToOwner(std::size_t index, Rect rect) const {
    const auto& paragraph = paragraphs_[index];
    rect.x += paragraph.bounds.x;
    rect.y += paragraph.bounds.y;
    return rect.Intersection(paragraph.clip);
  }

  std::optional<Rect> Caret(MarkdownPosition at) const {
    if (paragraphs_[at.paragraph].content->image) return {};
    auto rect = paragraphs_[at.paragraph].layout.CaretRect({at.offset});
    rect.x = std::min(rect.x, std::max(0.0F, paragraphs_[at.paragraph].bounds.width - rect.width));
    const auto visible = ToOwner(at.paragraph, rect);
    return visible.IsEmpty() ? std::nullopt : std::optional{visible};
  }

  std::optional<MarkdownPosition> PositionAt(Point point) const {
    std::optional<MarkdownPosition> best;
    auto distance = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < paragraphs_.size(); ++i) {
      const auto& paragraph = paragraphs_[i];
      if (paragraph.clip.IsEmpty() || paragraph.content->image || paragraph.content->text.Length() == 0) continue;
      const auto dx = point.x - std::clamp(point.x, paragraph.clip.x, paragraph.clip.x + paragraph.clip.width);
      const auto dy = point.y - std::clamp(point.y, paragraph.clip.y, paragraph.clip.y + paragraph.clip.height);
      if (dx * dx + dy * dy >= distance) continue;
      distance = dx * dx + dy * dy;
      best = MarkdownPosition{i, paragraph.layout.HitTest({point.x - paragraph.bounds.x, point.y - paragraph.bounds.y}).offset};
    }
    return best;
  }

  MarkdownExcerpt Excerpt() const {
    MarkdownExcerpt excerpt{*range_};
    bool mapped = true;
    for (std::size_t i = First().paragraph; i <= Last().paragraph; ++i) {
      const auto& paragraph = *paragraphs_[i].content;
      const auto selected = RangeIn(i);
      if (selected.IsCollapsed()) continue;
      if (!excerpt.source.text.empty()) excerpt.source.text += '\n';
      excerpt.source.text += paragraph.text.TextInRange(selected);
      const auto begin = Utf8Offset(paragraph.text.PlainText(), selected.start);
      const auto end = Utf8Offset(paragraph.text.PlainText(), selected.end);
      const auto ranges = begin && end ? markdown::ResolveMarkdownSourceRanges(
          paragraph.text.PlainText(), paragraph.source, *begin, *end) : std::nullopt;
      if (!ranges) mapped = false;
      else for (const auto& [lo, hi] : *ranges) AppendSourceRange(excerpt.source, lo, hi);
    }
    if (!mapped) excerpt.source.source_bytes.clear();
    return excerpt;
  }

  void SetRange(MarkdownRange value) {
    range_ = value;
    InvalidatePaint();
    if (Selection()) spec_.changed(Excerpt());
    else spec_.changed(std::nullopt);
  }

  SelectionModifier spec_;
  std::vector<ParagraphGeometry> paragraphs_;
  std::optional<MarkdownRange> range_;
  Color painted_color_;
  Point origin_;
  bool enabled_ = true;
  bool pressed_ = false;
  bool dragging_ = false;
};

#endif
} // namespace

std::optional<MarkdownSourceExcerpt> ReadMarkdownSourceRange(std::string_view source, std::size_t begin, std::size_t end) {
  try {
    const auto document = markdown::ParseMarkdown(source, MarkdownSelectionParseOptions());
    if (IsPlainMarkdown(document, source)) return MarkdownSourceExcerpt{std::string(source.substr(begin, end - begin)), {{begin, end}}};
#ifdef HUXERUI_MARKDOWN_SOURCE_MAPPING
    MarkdownSourceExcerpt excerpt;
    CollectSourceExcerpt(document.Root(), begin, end, excerpt);
    return excerpt;
#endif
  } catch (const std::logic_error&) {
  } catch (const std::runtime_error&) {
  }
  return std::nullopt;
}

View SelectableMarkdown(markdown::MarkdownDocument document, markdown::MarkdownOptions options, Color highlight,
                        ViewHandle focus, std::optional<MarkdownRange> selection,
                        std::function<void(std::optional<MarkdownExcerpt>)> changed,
                        std::function<void(const markdown::MarkdownLinkActivation&)> activate, MarkdownImageLoad load) {
#ifdef HUXERUI_MARKDOWN_SOURCE_MAPPING
  if (changed) options.selectable = false;
  if (changed || load) {
    options.decorate_text = [selectable = static_cast<bool>(changed), load = std::move(load)](
        View view, const markdown::MarkdownNode& node, const AttributedText& text, const TextStyle& style, TextAlign alignment,
        std::span<const markdown::MarkdownSourceSegment> source) {
      return DecorateParagraph(std::move(view), node, text, style, alignment, source, selectable, load);
    };
  }
#else
  static_cast<void>(highlight);
  static_cast<void>(focus);
  static_cast<void>(selection);
  static_cast<void>(changed);
  if (load) {
    const auto style = options.style;
    options.standalone_image_renderer = [load = std::move(load), style](const markdown::MarkdownNode& node) {
      auto alt = markdown::PlainText(node);
      return RenderMarkdownImage({node.destination, alt}, load, Text(alt).Style(style.value_or(markdown::MarkdownStyle{}).body));
    };
  }
#endif
  auto body = markdown::Markdown(document, std::move(options));
  if (activate) body = std::move(body).On<markdown::MarkdownEvents::LinkActivated>(std::move(activate));
#ifdef HUXERUI_MARKDOWN_SOURCE_MAPPING
  if (changed) {
    return Column{std::move(body)}.With(CrossAlign(CrossAxisAlignment::Stretch), Focusable{}, focus,
        SelectionModifier{std::move(document), highlight, focus, selection, std::move(changed)});
  }
#endif
  return body;
}

} // namespace huxerui::agentkit::detail
