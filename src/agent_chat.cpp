#include <huxerui/agentkit.h>
#include <huxerui/huxerui.h>
#include <huxerui/markdown.h>

#include "chat_control_icons.h"
#include "chat_layout_style.h"
#include "json_wire.h"
#include "markdown_selection.h"
#include "runtime_internal.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <ranges>
#include <set>
#include <stdexcept>

namespace huxerui::agentkit {
namespace detail {

struct SideChatDraft {
  SessionId session;
  std::vector<SourceSlice> sources;
  State<std::shared_ptr<Conversation>> conversation{std::shared_ptr<Conversation>{}};
  State<TextEditingValue> input{TextEditingValue::FromText({})};
  State<bool> pending{false};
  State<std::string> error{std::string{}};
};

struct SideChatDrafts {
  std::weak_ptr<AgentRuntime> runtime;
  std::vector<std::shared_ptr<SideChatDraft>> entries;
};

} // namespace detail
namespace {

struct SelectedText {
  SourceSlice source;
  std::string text;
  TextSelection selection;
  std::vector<SourceSlice> additional_sources;
  std::optional<detail::MarkdownRange> markdown_range;
};

struct SelectionPassage {
  std::size_t first_slice;
  std::string text;
};

struct MessageSelection {
  std::vector<SourceSlice> slices;
  std::vector<SelectionPassage> passages;
};

struct CitationNavigation {
  std::shared_ptr<const std::function<void(std::uint64_t)>> on_citation_added;
  std::uint64_t review_ticket = 0;

  static CitationNavigation Default() { return {}; }

  bool operator==(const CitationNavigation&) const = default;
};

SourceSlice SelectionBounds(const SelectedText& selected) {
  auto bounds = selected.source;
  if (!selected.additional_sources.empty())
    bounds.text_range->end_utf8 = selected.additional_sources.back().text_range->end_utf8;
  return bounds;
}

MessageSelection MessageRange(const std::vector<Message>& messages, const SourceSlice& anchor,
                              const SourceSlice& endpoint) {
  const auto locate = [&](const SourceSlice& source) -> std::optional<std::pair<std::size_t, std::size_t>> {
    for (std::size_t row = 0; row < messages.size(); ++row) {
      const auto& message = messages[row];
      if (source.source != SourceRef{message.ref} || message.state != MessageState::Complete) continue;
      const auto& block = std::get<BlockId>(source.text_range->leaf);
      const auto found = std::ranges::find(message.blocks, block, &ContentBlock::id);
      if (found != message.blocks.end()) return std::pair{row, static_cast<std::size_t>(found - message.blocks.begin())};
    }
    return {};
  };
  const auto start = locate(anchor);
  const auto end = locate(endpoint);
  if (!start || !end) return {};
  const auto first = std::min(*start, *end);
  const auto last = std::max(*start, *end);
  const auto& tail = *start <= *end ? *endpoint.text_range : *anchor.text_range;
  const auto& head = *start <= *end ? *anchor.text_range : *endpoint.text_range;
  MessageSelection result;
  for (auto row = first.first; row <= last.first; ++row) {
    const auto& message = messages[row];
    if (message.state != MessageState::Complete) return {};
    const auto limit = row == last.first ? last.second + 1 : message.blocks.size();
    for (auto index = row == first.first ? first.second : 0; index < limit; ++index) {
      const auto& block = message.blocks[index];
      const auto* body = std::get_if<TextBlock>(&block.body);
      if (!body || body->text.empty()) continue;
      const auto position = std::pair{row, index};
      const auto begin = position == first ? (first == last ? std::min(head.begin_utf8, tail.begin_utf8) : head.begin_utf8) : 0;
      const auto end = position == last ? (first == last ? std::max(head.end_utf8, tail.end_utf8) : tail.end_utf8) : body->text.size();
      if (begin >= end || end > body->text.size()) return {};
      const auto first_slice = result.slices.size();
      if (body->markdown) {
        auto excerpt = detail::ReadMarkdownSourceRange(body->text, begin, end);
        if (!excerpt) return {};
        if (excerpt->source_bytes.empty()) {
          if (position == first || position == last) return {};
          continue;
        }
        result.passages.push_back({first_slice, std::move(excerpt->text)});
        for (const auto& [lo, hi] : excerpt->source_bytes)
          result.slices.push_back({message.ref, {}, SourceTextRange{block.id, lo, hi}});
      } else {
        result.passages.push_back({first_slice, body->text.substr(begin, end - begin)});
        result.slices.push_back({message.ref, {}, SourceTextRange{block.id, begin, end}});
      }
    }
  }
  return result;
}

AgentChatCitation GatherCitation(MessageSelection selected) {
  AgentChatCitation citation{std::move(selected.slices.front()), std::move(selected.passages.front().text)};
  citation.additional_sources.assign(std::make_move_iterator(std::next(selected.slices.begin())),
      std::make_move_iterator(selected.slices.end()));
  for (const auto& passage : std::views::drop(selected.passages, 1)) citation.text += '\n' + passage.text;
  return citation;
}

template<class Service>
std::shared_ptr<const Service> OptionalChatService() {
  try {
    return UseService<Service>();
  } catch (const std::logic_error&) {
    return {};
  }
}

std::optional<AgentChatCitation> LastCompletedReply(const std::vector<Message>& messages) {
  for (auto message = messages.rbegin(); message != messages.rend(); ++message) {
    if (message->author != MessageAuthor::Agent || message->state != MessageState::Complete) continue;
    std::string text;
    for (const auto& block : message->blocks) {
      if (const auto* content = std::get_if<TextBlock>(&block.body)) {
        if (!text.empty()) text.push_back('\n');
        text += content->text;
      }
    }
    if (!text.empty()) return AgentChatCitation{{message->ref}, std::move(text)};
  }
  return {};
}

View SelectionField(SourceRef source, std::variant<BlockId, PartId> leaf, std::string text,
                    State<std::optional<SelectedText>> selection) {
  TextEditingValue value{.text = text};
  if (const auto selected = selection.Get(); selected && selected->source.source == source &&
      selected->source.text_range->leaf == leaf) value.selection = selected->selection;
  return TextField(std::move(value)).Variant(TextFieldVariant::Standard)
      .LineLimits(TextFieldLineLimits::MultiLine())
      .InputConfiguration({.multiline = true, .autocorrect = false, .keyboard_policy = SoftwareKeyboardPolicy::Manual})
      .OnChanged([selection, source = std::move(source), leaf = std::move(leaf), original = std::move(text)](
                     const TextEditingValue& state) {
        if (state.text != original || state.composition) return;
        const auto range = state.selection.Range();
        const auto start = detail::Utf8Offset(state.text, range.start);
        const auto end = detail::Utf8Offset(state.text, range.end);
        if (!start || !end || *start > *end) return;
        selection = SelectedText{{source, {}, SourceTextRange{leaf, *start, *end}},
            state.text.substr(*start, *end - *start), state.selection};
      }).With(Semantics{.read_only = true});
}

std::string AuthorLabel(MessageAuthor author, std::string_view agent) {
  switch (author) {
  case MessageAuthor::User: return "You";
  case MessageAuthor::Agent: return agent.empty() ? "Assistant" : std::string(agent);
  case MessageAuthor::Tool: return "Tool";
  case MessageAuthor::Collaboration: return "Collaborator";
  case MessageAuthor::System: return "System";
  }
  return {};
}

TextFieldStyle ChatFieldStyle(TextFieldStyle style, const ThemeSpec& theme) {
  style.show_label = false;
  style.standard.border = Color::Transparent();
  style.standard.hovered_border = Color::Transparent();
  style.standard.focused_border = Color::Transparent();
  style.standard.minimum_height = 0.0F;
  style.text_style = {Font::System(14.0F), theme.colors.on_surface};
  style.placeholder_style = {Font::System(14.0F), theme.colors.on_surface_variant};
  style.padding = {};
  return style;
}

ButtonStyle ChatActionStyle(ButtonStyle style, const ThemeSpec& theme) {
  style.background = Color::Transparent();
  style.disabled_background = Color::Transparent();
  style.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), theme.colors.on_surface_variant};
  style.padding = EdgeInsets::Symmetric(7.0F, 4.0F);
  style.minimum_height = 32.0F;
  return style;
}

[[huxerui::composable]]
View MarkdownMessageBlock(MessageRef message, BlockId block, std::string source, bool complete, bool can_reference,
                          State<std::optional<SelectedText>> selection,
                          std::shared_ptr<const AgentChatLinkHandler> links) {
  const auto theme = UseTheme();
  const auto field_style = ChatFieldStyle(UseEnvironment<TextFieldStyle>(), theme);
  const auto parser = UseState(std::make_shared<markdown::MarkdownParseSession>(detail::MarkdownSelectionParseOptions()));
  const auto link_error = UseState(std::string{});
  const auto images = OptionalChatService<AgentChatImageLoader>();
  const auto action_style = ChatActionStyle(UseEnvironment<ButtonStyle>(), theme);
  const auto source_mode = UseState(false);
  const auto source_focus = UseViewHandle();
  const auto select_source = !detail::HasMarkdownSourceSelection && complete && can_reference && source_mode.Get();
  Lifecycle([select_source, source_focus] {
    if (select_source) source_focus.RequestFocus(true);
  }, select_source);

  std::optional<markdown::MarkdownDocument> document;
  try {
    document = parser.Get()->Update(source);
  } catch (const std::logic_error&) {
  } catch (const std::runtime_error&) {
  }
  const bool can_select = complete && can_reference && !source.empty();
  const auto source_field = [&] {
    return ProvideEnvironment(field_style,
        SelectionField(message, block, source, selection).With(source_focus).Key("markdown-selection/" + block.value));
  };
  if (!document || detail::IsPlainMarkdown(*document, source)) {
    if (can_select) return source_field();
    return Text(source).Style({Font::System(14.0F), theme.colors.on_surface});
  }

  auto style = markdown::MarkdownStyle::FromTheme(theme);
  style.body = {Font::System(14.0F), theme.colors.on_surface};
  style.code = {Font::Monospace(13.0F), theme.colors.on_surface};
  style.heading_sizes = {24.0F, 21.0F, 18.0F, 16.0F, 15.0F, 14.0F};
  style.block_spacing = 9.0F;
  const bool link_enabled = complete && links && links->allows && links->open;
  std::vector<View> content;
  if (select_source) {
    content.push_back(source_field());
  } else {
    markdown::MarkdownOptions options{.style = style, .selectable = complete,
        .link_policy = [link_enabled, links, message, block](std::string_view destination) {
          return link_enabled && links->allows({message, block, std::string(destination)});
        }};
    std::function<void(const markdown::MarkdownLinkActivation&)> activate;
    if (link_enabled) {
      activate = [links, message, block, link_error](const markdown::MarkdownLinkActivation& activation) {
        const AgentChatLink link{message, block, activation.destination};
        if (!links->allows(link)) { link_error = "This link is unavailable"; return; }
        auto opened = links->open(link);
        link_error = opened.Succeeded() ? std::string{} : opened.Error().message;
      };
    }
    const auto owns_selection = [message, block](const std::optional<SelectedText>& selected) {
      return selected && selected->source.source == SourceRef{message} &&
          selected->source.text_range && selected->source.text_range->leaf == std::variant<BlockId, PartId>{block};
    };
    std::function<void(std::optional<detail::MarkdownExcerpt>)> changed;
    if (can_select) changed = [message, block, selection, owns_selection](std::optional<detail::MarkdownExcerpt> excerpt) {
      if (!excerpt || excerpt->source.source_bytes.empty()) {
        if (owns_selection(selection.Get())) selection = std::nullopt;
        return;
      }
      auto& source_excerpt = excerpt->source;
      const auto [begin, end] = source_excerpt.source_bytes.front();
      SelectedText selected{{message, {}, SourceTextRange{block, begin, end}}, std::move(source_excerpt.text), {},
                            {}, excerpt->range};
      for (const auto& [first, last] : std::views::drop(source_excerpt.source_bytes, 1)) {
        selected.additional_sources.push_back({message, {}, SourceTextRange{block, first, last}});
      }
      selection = std::move(selected);
    };
    auto highlight = theme.colors.primary;
    highlight.alpha = 0.30F;
    const auto retained = owns_selection(selection.Get()) ? selection.Get()->markdown_range : std::nullopt;
    detail::MarkdownImageLoad load;
    if (complete && images && images->load) load = [images, message, block](std::string destination) {
      return images->load({message, block, std::move(destination)});
    };
    content.push_back(detail::SelectableMarkdown(*document, std::move(options), highlight, source_focus, retained, std::move(changed),
        std::move(activate), std::move(load)).Key("markdown-selection/" + block.value));
    if (!link_error.Get().empty()) content.push_back(Text(link_error.Get(), TextRole::Label)
        .Style({Font::System(12.0F), theme.colors.error}).Key("markdown-link-error"));
  }
  if (can_select && !detail::HasMarkdownSourceSelection) {
    auto mode = Button(select_source ? "Show formatted message" : "Select source text")
        .OnClick([source_mode, selection, message, block] {
          if (const auto selected = selection.Get(); selected && selected->source.source == SourceRef{message} &&
              selected->source.text_range && selected->source.text_range->leaf == std::variant<BlockId, PartId>{block}) {
            selection = std::nullopt;
          }
          source_mode = !source_mode.Get();
        }).Key("markdown-source-toggle");
    content.push_back(Row{ProvideEnvironment(action_style, std::move(mode)), Spacer()});
  }
  return Column(std::move(content)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

const VectorAsset& StopIcon() {
  static const auto icon = VectorAsset::Create(Size{16.0F, 16.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(Path::RoundedRect({3.0F, 3.0F, 10.0F, 10.0F}, CornerRadii{2.0F}),
        Color::White(), StrokeStyle{.width = 1.6F});
  });
  return icon;
}

std::string ShortPreview(std::string_view text) {
  auto end = std::min(text.find('\n'), std::min(text.size(), std::size_t{160}));
  while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
  return std::string(text.substr(0, end)) + (end < text.size() ? "\xe2\x80\xa6" : "");
}

using detail::ShowsArtifact;

using ArtifactMessagePosition = std::pair<MessageId, BlockId>;
using OpenArtifactReview = std::function<void(ArtifactRef, std::optional<AgentChatApprovalRef>,
    std::optional<ViewHandle>, std::optional<ArtifactMessagePosition>)>;

struct InlineArtifactSummary {
  ArtifactMessagePosition position;
  ArtifactRef artifact;
  View view;
};

[[huxerui::composable]]
View ArtifactReviewButton(std::string label, std::string key, ArtifactRef artifact, std::optional<AgentChatApprovalRef> approval,
                          OpenArtifactReview open, std::string title, std::string accessible_label = {},
                          std::optional<ArtifactMessagePosition> message = {}) {
  const auto colors = UseTheme().colors;
  const auto hovered = UseState(false);
  const auto origin = UseViewHandle();
  const auto activate = [open, origin, artifact, approval, message] {
    open(artifact, approval, origin, message);
  };
  const auto indicator = Canvas([ink = colors.on_primary_container](PaintContext& context, Size) {
    const StrokeStyle stroke{.width = 1.35F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
    context.DrawLine({4, 12}, {12, 4}, ink, stroke);
    context.DrawLine({6, 4}, {12, 4}, ink, stroke);
    context.DrawLine({12, 4}, {12, 10}, ink, stroke);
  }).With(Frame{.width = 16.0F, .height = 16.0F});
  const auto group = "artifact-review-action/" + label;
  const auto hint = title + " · Version " + std::to_string(artifact.version);
  auto button = Row{
      Column{
          Text(std::move(title)).Style({Font::System(14.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
              .LineLimit(2).Overflow(TextOverflow::Ellipsis),
          Text("v" + std::to_string(artifact.version)).Style({Font::System(12.0F), colors.on_surface_variant}),
      }.With(Grow(), Spacing(4.0F)),
      indicator,
  }.OnClick(activate).On<ViewEvents::KeyDown>([activate](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    if (!event.repeat) activate();
    return true;
  }).On<ViewEvents::Hover>([hovered](const HoverEvent& event) { hovered = event.type != HoverEventType::Leave; })
      .With(origin, Focusable{}, Frame{.min_height = 62.0F}, Padding(EdgeInsets::Symmetric(12.0F, 8.0F)), Spacing(10.0F),
            CrossAlign(CrossAxisAlignment::Center), CornerRadius(8.0F), Border{colors.outline},
            Background(hovered.Get() ? colors.surface_container_highest : colors.surface_container_high),
            Semantics{.role = SemanticRole::Button, .label = accessible_label.empty() ? std::move(label) : std::move(accessible_label),
                      .hint = hint}).Key(std::move(key));
  return Column{std::move(button)}.With(CrossAlign(CrossAxisAlignment::Stretch)).Key(group);
}

[[huxerui::composable]]
View ConversationArtifactCard(ArtifactBlock block, ArtifactMessagePosition position, std::shared_ptr<Conversation> conversation,
                              Revision access_revision, OpenArtifactReview open) {
  const auto tasks = UseTaskScope();
  const auto resolved = UseState(std::optional<std::pair<Revision, std::string>>{});
  Lifecycle([tasks, resolved, conversation, ref = block.artifact, lookup = block.name.empty(), access_revision] {
    if (!lookup) return std::function<void()>{[] {}};
    auto worker = tasks.Launch([resolved, conversation, ref, access_revision]() -> Task<void> {
      auto name = co_await detail::RuntimeState::ReadConversationArtifactName(conversation, ref);
      if (name.Succeeded()) resolved = std::pair{access_revision, std::move(name).Value()};
    });
    return std::function<void()>{[worker] { worker.Cancel(); }};
  }, conversation, block.artifact, block.name, access_revision);
  const auto name = block.name.empty() && resolved.Get() && resolved.Get()->first == access_revision
      ? resolved.Get()->second : block.name;
  const auto version = " v" + std::to_string(block.artifact.version);
  const auto label = "Review " + (block.name.empty() ? "artifact" : block.name) + version;
  return ArtifactReviewButton(label, position.second.value, block.artifact, {}, std::move(open),
      name.empty() ? "Artifact" : name, name.empty() ? std::string{} : "Review " + name + version, position);
}

View MessageView(const Message& message, const std::shared_ptr<Conversation>& conversation,
                 const OpenArtifactReview& open_artifact,
                 State<std::optional<SelectedText>> selection, bool selectable, std::optional<SteerState> steer,
                 std::optional<View> recover, std::optional<View> selection_actions, std::string agent,
                 const ThemeSpec& theme, const TextFieldStyle& selection_style,
                 Revision access_revision, const std::shared_ptr<const AgentChatLinkHandler>& links,
                 const std::optional<InlineArtifactSummary>& inline_summary,
                 const decltype(AgentChatOptions::render_text_block)& render_text_block, bool bubbles, bool narrow) {
  const auto& colors = theme.colors;
  const bool user = message.author == MessageAuthor::User;
  const bool agent_reply = message.author == MessageAuthor::Agent;
  const bool bubble = bubbles && (user || agent_reply);
  auto author = Text(AuthorLabel(message.author, agent))
      .Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface_variant});
  std::vector<View> content;
  if (!bubble) content.push_back(author);
  if (steer) {
    auto status = Text(*steer == SteerState::Pending ? "Waiting to apply" :
        *steer == SteerState::Applied ? "Applied to this run" : "Not applied", TextRole::Label);
    if (recover) content.push_back(Row{std::move(status), std::move(*recover)}
        .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)));
    else content.push_back(std::move(status));
  }
  const auto selected = selection_actions ? selection.Get() : std::nullopt;
  for (const auto& block : message.blocks) {
    if (const auto* text = std::get_if<TextBlock>(&block.body)) {
      auto rendered = render_text_block ? render_text_block(message, block) : std::nullopt;
      if (rendered) {
        content.push_back(std::move(*rendered).Key(block.id.value));
      } else if (text->markdown) {
        content.push_back(MarkdownMessageBlock(message.ref, block.id, text->text, message.state == MessageState::Complete,
            selectable, selection, links).Key(block.id.value));
      } else if (selectable && message.state == MessageState::Complete && !text->text.empty()) {
        content.push_back(ProvideEnvironment(selection_style,
            SelectionField(message.ref, block.id, text->text, selection).Key(block.id.value)));
      } else {
        content.push_back(Text(text->text).Style({Font::System(14.0F), colors.on_surface}).Key(block.id.value));
      }
      if (!rendered && selected && selected->source.text_range &&
          selected->source.text_range->leaf == std::variant<BlockId, PartId>{block.id}) {
        content.push_back(std::move(*selection_actions));
      }
    } else if (const auto* artifact = std::get_if<ArtifactBlock>(&block.body)) {
      const ArtifactMessagePosition position{message.ref.message, block.id};
      content.push_back(ConversationArtifactCard(*artifact, position, conversation, access_revision, open_artifact)
          .Key("artifact-review/" + block.id.value));
      if (inline_summary && inline_summary->position == position && inline_summary->artifact == artifact->artifact)
        content.push_back(inline_summary->view);
    } else if (const auto* moved = std::get_if<AgentTransferred>(&block.body); moved && moved->to) {
      content.push_back(Text(moved->handoff_summary.value_or("Assistant changed")).Key(block.id.value));
    } else if (const auto* custom = std::get_if<CustomBlock>(&block.body)) {
      auto rendered = conversation->RenderContent({message.ref, block.id}, *custom);
      content.push_back(rendered ? std::move(*rendered).Key(block.id.value) : Text(custom->fallback).Key(block.id.value));
    } else if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body)) {
      content.push_back(Text(conclusion->text, TextRole::Label).Key(block.id.value));
    }
  }
  auto body = Column(std::move(content)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
  if (bubble) {
    auto tile = std::move(body).With(Padding(EdgeInsets::Symmetric(13.0F, 9.0F)),
        CornerRadius(user ? CornerRadii{15.0F, 5.0F, 15.0F, 15.0F} : CornerRadii{5.0F, 15.0F, 15.0F, 15.0F}),
        Background(user ? colors.primary_container : colors.surface_container_low),
        Border{user ? Color::Transparent() : colors.outline}).Key(user ? "chat-user-bubble" : "chat-agent-bubble");
    auto heading = Row{std::move(author)}.With(MainAlign(user ? MainAxisAlignment::End : MainAxisAlignment::Start));
    auto thread = Column{std::move(heading), std::move(tile)}.With(
        Frame{.max_width = user ? 510.0F : 626.0F}, Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
    auto avatar = Canvas([user, ink = user ? colors.on_surface_variant : colors.on_primary_container](PaintContext& context, Size) {
      const StrokeStyle line{.width = 1.3F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
      if (user) {
        context.DrawArc({13.0F, 9.0F}, 3.0F, 0.0F, 6.283185F, ink, line);
        context.DrawArc({13.0F, 20.0F}, 5.3F, 3.141593F, 3.141593F, ink, line);
      } else {
        Path star;
        star.MoveTo({13, 6}).QuadraticTo({14, 12}, {20, 13}).QuadraticTo({14, 14}, {13, 20})
            .QuadraticTo({12, 14}, {6, 13}).QuadraticTo({12, 12}, {13, 6});
        context.StrokePath(std::move(star), ink, line);
      }
    }).With(Frame{.width = 26.0F, .height = 26.0F}, CornerRadius(8.0F),
        Background(user ? colors.surface_container_high : colors.primary_container));
    auto speaker = Column{std::move(avatar)}.With(Padding(EdgeInsets{.top = 21.0F}));
    if (user) return Row{Spacer(), std::move(thread), std::move(speaker)}
        .With(Spacing(7.0F), Padding(EdgeInsets{.left = narrow ? 30.0F : 64.0F}));
    return Row{std::move(speaker), std::move(thread), Spacer()}
        .With(Spacing(7.0F), Padding(EdgeInsets{.right = narrow ? 14.0F : 40.0F}));
  }
  if (!user) return body;
  return Row{
      Canvas([line = colors.outline](PaintContext& context, Size size) {
        context.DrawRect({0, 0, size.width, size.height}, line);
      }).With(Frame{.width = 2.0F}),
      std::move(body).With(Grow()),
  }.With(Spacing(11.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

const ApprovalPreview* ApprovalDetails(const InteractionSnapshot& item) {
  if (const auto* tool = std::get_if<ToolApproval>(&item.body)) return &tool->preview;
  if (const auto* operation = std::get_if<OperationApproval>(&item.body)) return &operation->preview;
  return nullptr;
}

std::vector<ArtifactRef> VisibleApprovalAttachments(const ApprovalPreview& preview, const SessionSnapshot& snapshot) {
  auto visible = preview.artifacts;
  std::set<std::pair<ArtifactId, Revision>> seen;
  std::erase_if(visible, [&](const auto& ref) {
    return !ShowsArtifact(snapshot, ref) || !seen.emplace(ref.id, ref.version).second;
  });
  return visible;
}

View ApprovalActions(std::shared_ptr<Conversation> conversation, TaskScope tasks, State<std::string> error,
                     AgentChatApprovalRef approval, std::string deny, std::string approve,
                     const ButtonStyle& secondary_style, Color outline) {
  auto submit = [tasks, conversation = std::move(conversation), error, approval](ApprovalDecision choice) {
    tasks.Launch([conversation, error, approval, choice]() -> Task<void> {
      auto reply = co_await conversation->SubmitAsync(NewCommandId(), {approval.id, approval.version, choice});
      error = reply.Succeeded() ? "" : reply.Error().message;
    });
  };
  return Row{Spacer(), Flow{
      ProvideEnvironment(secondary_style, Button(std::move(deny)).OnClick([submit] { submit(ApprovalDecision::Deny); })
          .With(Border{outline})),
      Button(std::move(approve)).OnClick([submit] { submit(ApprovalDecision::Approve); }),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))};
}

View PendingApprovalView(const InteractionSnapshot& item, std::shared_ptr<Conversation> conversation,
                         TaskScope tasks, State<std::string> error, const SessionSnapshot& snapshot,
                         const OpenArtifactReview& open_artifact, const ButtonStyle& action_style, const ColorScheme& colors,
                         const std::function<std::string(const ApprovalPreview&, const ArtifactRef&)>& attachment_label) {
  const auto& preview = *ApprovalDetails(item);
  const AgentChatApprovalRef approval{item.id, item.version};
  const auto available = VisibleApprovalAttachments(preview, snapshot);
  std::vector<View> plan;
  const bool target_on_card = available.size() == 1 && preview.targets.size() == 1;
  bool show_targets = !target_on_card;
  for (std::size_t index = 0; index < available.size(); ++index) {
    const auto& ref = available[index];
    const auto action = available.size() == 1 ? std::string("Review change") :
        "Review attachment " + std::to_string(index + 1);
    auto name = attachment_label ? attachment_label(preview, ref) : std::string{};
    auto access_label = name.empty() ? std::string{} : "Review " + name;
    if (name.empty()) name = target_on_card ? preview.targets.front().target.value : action;
    if (target_on_card) show_targets = name != preview.targets.front().target.value;
    const auto key = ref.id.value + "/" + std::to_string(ref.version);
    plan.push_back(ArtifactReviewButton(action, key, ref, approval, open_artifact, std::move(name), std::move(access_label))
        .Key("artifact-review/" + key));
  }
  if (show_targets) {
    for (const auto& baseline : preview.targets) plan.push_back(Text(baseline.target.value, TextRole::Label));
  }
  if (!preview.summary.empty()) plan.emplace_back(Text(preview.summary).Style({Font::System(13.0F), colors.on_surface}));
  std::vector<View> details{
      Text(preview.title, TextRole::Title).Style({Font::System(17.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})
          .With(LayoutPriority(1.0F)),
  };
  if (!plan.empty()) {
    details.push_back(ScrollView(Column(std::move(plan)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)))
        .With(Frame{.max_height = 228.0F}, ScrollBar()));
  }
  if (item.state == InteractionState::Pending) {
    details.push_back(Divider().With(LayoutPriority(1.0F)));
    details.push_back(ApprovalActions(std::move(conversation), tasks, error, approval, "Deny", "Approve", action_style, colors.outline)
        .With(LayoutPriority(2.0F)));
  }
  return Column(std::move(details)).With(Spacing(8.0F), Padding(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Background(colors.surface), CornerRadius(8.0F), Border{colors.outline});
}

View ReviewedApproval(const SessionSnapshot& snapshot, ArtifactRef artifact, AgentChatApprovalRef approval,
                      const std::shared_ptr<Conversation>& conversation, TaskScope tasks, State<std::string> error,
                      bool loading, const ButtonStyle& action_style, const ColorScheme& colors) {
  const auto item = std::ranges::find(snapshot.interactions, approval.id, &InteractionSnapshot::id);
  if (item == snapshot.interactions.end()) return Text("Approval unavailable", TextRole::Label);
  switch (item->state) {
    case InteractionState::Approved: return Text("Change approved", TextRole::Label);
    case InteractionState::Denied: return Text("Change denied", TextRole::Label);
    case InteractionState::Expired: return Text("Approval expired", TextRole::Label);
    case InteractionState::Cancelled: return Text("Approval cancelled", TextRole::Label);
    case InteractionState::Invalidated: return Text("Approval invalidated", TextRole::Label);
    case InteractionState::Submitted: return Text("Response submitted", TextRole::Label);
    case InteractionState::Pending: break;
  }
  const auto* plan = ApprovalDetails(*item);
  if (item->version != approval.version || !plan || std::ranges::find(plan->artifacts, artifact) == plan->artifacts.end()) {
    return Text("Approval changed. Open the current request to review its plan.", TextRole::Label);
  }
  std::vector<View> details;
  for (const auto& target : plan->targets)
    details.push_back(Text(target.target.value, TextRole::Label).Style({Font::System(12.0F), colors.on_surface}));
  if (!plan->summary.empty()) details.push_back(Text(plan->summary).Style({Font::System(12.0F), colors.on_surface_variant}));
  return Column{
      Divider().With(LayoutPriority(1.0F)),
      Text(plan->title, TextRole::Label)
          .Style({Font::System(13.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}).With(LayoutPriority(1.0F)),
      ScrollView(Column(std::move(details)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)))
          .With(Frame{.max_height = 88.0F}, ScrollBar()),
      ApprovalActions(conversation, tasks, error, approval, "Deny reviewed change", "Approve reviewed change",
          action_style, colors.outline).With(Enabled(!loading), LayoutPriority(2.0F)),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View PendingFormView(InteractionSnapshot item, std::shared_ptr<Conversation> conversation, State<std::string> error) {
  const auto tasks = UseTaskScope();
  const auto response = UseState(TextEditingValue::FromText({}));
  const auto& form = std::get<FormRequest>(item.body);
  const auto submit = [tasks, response, conversation, error, id = item.id, version = item.version](bool dismiss) {
    InteractionReply reply{id, version, DismissForm{}};
    if (!dismiss) {
      try { reply.response = detail::FromWireJson(nlohmann::json::parse(response->text)); }
      catch (const nlohmann::json::exception&) { error = "Enter a valid JSON response."; return; }
    }
    tasks.Launch([conversation, error, reply = std::move(reply)]() -> Task<void> {
      auto result = co_await conversation->SubmitAsync(NewCommandId(), reply);
      error = result.Succeeded() ? "" : result.Error().message;
    });
  };
  return Column{
      Text(form.title, TextRole::Title),
      ScrollView(Text(detail::ToWireJson(form.schema.document).dump(2))).With(Frame{.max_height = 180.0F}, ScrollBar()),
      TextField(response).Label("JSON response").OnChanged([response](const TextEditingValue& value) { response = value; }),
      Row{Button("Dismiss request").OnClick([submit] { submit(true); }),
          Button("Submit response").OnClick([submit] { submit(false); }).With(Enabled(!response->text.empty()))}.With(Spacing(8.0F)),
  }.With(Spacing(10.0F));
}

std::string SelectionLabel(const SourceSlice& slice, bool multiple) {
  if (const auto* message = std::get_if<MessageRef>(&slice.source)) {
    return (multiple ? "Message v" : "Selected message \xc2\xb7 v") + std::to_string(message->version);
  }
  if (const auto* artifact = std::get_if<ArtifactRef>(&slice.source)) {
    auto label = "Artifact v" + std::to_string(artifact->version);
    const auto* part = slice.text_range ? std::get_if<PartId>(&slice.text_range->leaf) : nullptr;
    if (!part && slice.parts.size() == 1) part = &slice.parts.front();
    if (part) label += " \xc2\xb7 " + part->value;
    return label;
  }
  return "Selected text";
}

struct SelectionOrigin {
  std::shared_ptr<Conversation> conversation;
  Revision access_revision;

  bool HasAccess() const {
    const auto current = conversation->Snapshot();
    return current && current->access_revision == access_revision;
  }
};

[[huxerui::composable]]
View SideChatDialog(DialogContext dialog, std::shared_ptr<AgentRuntime> runtime, AgentId agent,
                   SelectionOrigin origin, std::vector<SelectionPassage> passages, std::shared_ptr<detail::SideChatDraft> side, ViewHandle question_focus,
                   std::function<void(SourceSlice, std::string)> bring_back, std::function<void()> restore_origin,
                   std::function<void()> on_citation_added) {
  const auto tasks = UseApplicationTaskScope();
  const auto observation = UseTaskScope();
  const auto source_available = UseState(true);
  const auto selection_visible = [origin, source_available] { return source_available.Get() && origin.HasAccess(); };
  const auto question = side->input;
  const auto pending = side->pending;
  const auto error = side->error;
  const auto opened = side->conversation.Get();
  const auto returned = UseState(false);
  const auto compact = UseViewportClass() == ViewportClass::Compact;
  const auto colors = UseTheme().colors;
  Lifecycle([observation, origin, source_available] {
    auto subscription = std::make_shared<std::optional<Subscription>>();
    const auto worker = observation.Launch([origin, source_available, subscription]() -> Task<void> {
      auto observed = co_await origin.conversation->ObserveAsync([access_revision = origin.access_revision, source_available](auto result) {
        if (!result.Succeeded() || result.Value()->access_revision != access_revision) source_available = false;
      });
      if (observed.Succeeded()) *subscription = std::move(observed).Value();
      else source_available = false;
    });
    return [worker, subscription] { worker.Cancel(); subscription->reset(); };
  }, origin.conversation, origin.access_revision);
  Lifecycle([tasks, returned, restore_origin = std::move(restore_origin), on_citation_added = std::move(on_citation_added)] {
    return [tasks, returned, restore_origin, on_citation_added] {
      if (returned.Get() && on_citation_added) tasks.Post(on_citation_added);
      else if (restore_origin) restore_origin();
    };
  });
  Lifecycle([opened, question_focus] { if (opened) question_focus.RequestFocus(true); }, static_cast<bool>(opened));
  const auto submit = [tasks, runtime, agent, side, question, pending, error, selection_visible] {
    if (!selection_visible()) return;
    if (pending.Get() || question->text.empty() || question->composition) return;
    pending = true;
    error = "";
    tasks.Launch([runtime, agent, side, text = question->text, question, pending, error]() -> Task<void> {
      CreateSessionRequest request{
          .options = {.agent = agent, .retention = SessionRetention::Ephemeral},
          .initial = SendInput{.text = text}, .side_chat = SideChatSeed{side->session, side->sources}};
      auto result = co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request));
      if (!result.Succeeded()) { pending = false; error = result.Error().message; co_return; }
      const auto* created = std::get_if<SessionCreated>(&result.Value().outcome);
      if (!created) { pending = false; error = "The side chat is waiting for approval"; co_return; }
      auto conversation = co_await runtime->Sessions().OpenAsync(created->session);
      pending = false;
      if (!conversation.Succeeded()) { error = conversation.Error().message; co_return; }
      question = TextEditingValue::FromText({});
      side->conversation = std::move(conversation).Value();
    });
  };
  auto close_style = UseEnvironment<ButtonStyle>();
  close_style.background = Color::Transparent();
  close_style.label_style = {Font::System(12.0F), colors.on_surface_variant};
  close_style.padding = EdgeInsets::Symmetric(7.0F, 3.0F);
  close_style.minimum_height = 30.0F;
  auto header = Row{
      Text("Side chat", TextRole::Title).Style({Font::System(16.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
      Spacer(),
      ProvideEnvironment(close_style, Button("Close").OnClick([dialog] { dialog.Dismiss(); })),
  }.With(CrossAlign(CrossAxisAlignment::Center), Frame{.min_height = 44.0F}, Padding(EdgeInsets::Symmetric(15.0F, 0.0F)));
  const bool available = selection_visible();
  std::vector<View> body;
  if (available) {
    std::vector<View> fragments;
    for (auto& passage : passages) {
      if (passages.size() > 1) fragments.push_back(Text(SelectionLabel(side->sources[passage.first_slice], true))
          .Style({Font::System(12.0F), colors.on_surface_variant}));
      fragments.push_back(Text(std::move(passage.text)).Style({Font::System(13.0F), colors.on_surface_variant}));
    }
    auto label = passages.size() == 1 ? SelectionLabel(side->sources.front(), false) :
        std::to_string(passages.size()) + " selected passages";
    body.push_back(Column{
        Text(std::move(label)).Style({Font::System(12.0F), colors.on_surface_variant})
            .LineLimit(2).Overflow(TextOverflow::Ellipsis),
        Row{
            Canvas([ink = colors.on_primary_container](PaintContext& context, Size size) {
              context.DrawRect({0, 0, size.width, size.height}, ink);
            }).With(Frame{.width = 2.0F}),
            ScrollView(Column(std::move(fragments)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)))
                .With(Grow(), Frame{.max_height = 78.0F}, ScrollBar()),
        }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key("side-chat-source"));
  }
  if (!available) {
    body.push_back(Text("Selection access changed. Close this side chat and select the text again."));
  } else if (opened) {
    AgentChatOptions options;
    options.draft = question;
    options.composer_focus = question_focus;
    options.bring_back = [bring_back = std::move(bring_back), dialog, returned, selection_visible](SourceSlice ref, std::string text) {
      if (!selection_visible()) return;
      bring_back(std::move(ref), std::move(text));
      returned = true;
      dialog.Dismiss();
    };
    const float height = compact ? 248.0F : 304.0F;
    body.push_back(Column{AgentChat(opened, std::move(options)).With(Grow())}
        .With(Frame{.height = height, .max_height = height}, CrossAlign(CrossAxisAlignment::Stretch), ClipChildren{}));
  } else {
    auto style = UseEnvironment<TextFieldStyle>();
    style.show_label = false;
    style.text_style.foreground = colors.on_surface;
    style.placeholder_style.foreground = colors.on_surface_variant;
    style.caret = colors.on_primary_container;
    style.selection = colors.primary_container;
    style.composition = colors.on_primary_container;
    style.outlined.border = colors.outline;
    style.outlined.focused_border = colors.on_primary_container;
    body.push_back(Column{
        Text("Question").Style({Font::System(12.0F), colors.on_surface_variant}),
        ProvideEnvironment(style, TextField(question).Label("Question").Placeholder("Ask about this selection")
            .LineLimits(TextFieldLineLimits::MultiLine(2, 4)).Variant(TextFieldVariant::Outlined)
            .OnChanged([question](const TextEditingValue& value) { question = value; })
            .OnSubmitted(submit).With(question_focus, Enabled(!pending.Get())).Key("side-chat-question")),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch)));
    if (!error.Get().empty()) body.push_back(Text(error.Get(), TextRole::Label));
    body.push_back(Row{Spacer(),
        Button("Ask").OnClick(submit).With(Enabled(!pending.Get() && !question->text.empty() && !question->composition))}
        .With(CrossAlign(CrossAxisAlignment::Center)));
  }
  return Column{
      std::move(header),
      Canvas([ink = colors.outline](PaintContext& context, Size size) { context.DrawRect({0, 0, size.width, size.height}, ink); })
          .With(Frame{.height = 1.0F}),
      Column(std::move(body)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
          Padding(EdgeInsets{.top = 13.0F, .right = 15.0F, .bottom = 16.0F, .left = 15.0F})),
  }.With(Frame{.width = 440.0F}, CrossAlign(CrossAxisAlignment::Stretch), CornerRadius(12.0F),
      Border{colors.outline}, Background(colors.surface)).Key("side-chat-dialog");
}

void ShowSourceDialog(const DialogHandle& dialogs, std::shared_ptr<AgentChatSelection> service,
                      SelectionOrigin origin, MessageSelection selected,
                      State<AgentChatDraftSources> draft, ViewHandle question_focus, std::function<void()> restore_origin = {},
                      std::function<void()> on_citation_added = {}) {
  if (!service || !service->runtime) return;
  const auto current = origin.conversation->Snapshot();
  if (!current || current->access_revision != origin.access_revision) return;
  const auto& source = current->summary.id;
  if (!service->side_chats || service->side_chats->runtime.lock() != service->runtime) {
    service->side_chats = std::make_shared<detail::SideChatDrafts>();
    service->side_chats->runtime = service->runtime;
  }
  auto& entries = service->side_chats->entries;
  auto found = std::ranges::find_if(entries, [&](const auto& side) {
    return side->session == source && side->sources == selected.slices;
  });
  if (found == entries.end()) {
    entries.push_back(std::make_shared<detail::SideChatDraft>(detail::SideChatDraft{source, std::move(selected.slices)}));
    found = std::prev(entries.end());
  }
  dialogs.Show(DialogFactory{[runtime = service->runtime, agent = service->assistant, side = *found,
      origin = std::move(origin), passages = std::move(selected.passages), draft, question_focus, restore_origin = std::move(restore_origin),
      on_citation_added = std::move(on_citation_added)](DialogContext dialog) {
    return SideChatDialog(dialog, runtime, agent, origin, passages, side, question_focus, [draft](SourceSlice ref, std::string text) {
      auto sources = draft.Get();
      sources.conclusion = AgentChatCitation{std::move(ref), std::move(text)};
      draft = std::move(sources);
    }, restore_origin, on_citation_added);
  }}, {.initial_focus = question_focus});
}

[[huxerui::composable]]
View MessageSelectionActions(std::shared_ptr<Conversation> conversation,
                             State<std::shared_ptr<const SessionSnapshot>> snapshot,
                             SelectedText selected, State<std::optional<SelectedText>> selection,
                             State<std::optional<SourceSlice>> range_anchor,
                             State<AgentChatDraftSources> draft, State<std::optional<SourceSlice>> focus_request,
                             std::function<void()> on_citation_added) {
  const auto service = UseService<AgentChatSelection>();
  const auto tasks = UseTaskScope();
  const auto dialogs = UseDialog();
  const auto question_focus = UseViewHandle();
  const auto action_focus = UseViewHandle();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto style = ChatActionStyle(UseEnvironment<ButtonStyle>(), UseTheme());
  Lifecycle([focus_request, action_focus, source = selected.source] {
    if (focus_request.Get() != source) return;
    focus_request = std::nullopt;
    action_focus.RequestFocus(true);
  }, focus_request.Get(), selected.source);
  const auto anchor = range_anchor.Get();
  const auto endpoint = SelectionBounds(selected);
  auto use = [tasks, dialogs, service, conversation, snapshot, selected, draft, question_focus, selection, endpoint,
              focus_request, range_anchor, anchor, busy, error, on_citation_added](bool ask) {
    const auto current = snapshot.Get();
    if (busy.Get() || !current) return;
    auto excerpts = anchor ? MessageRange(current->messages, *anchor, endpoint) :
        MessageSelection{{selected.source}, {{0, selected.text}}};
    if (!anchor) excerpts.slices.insert(excerpts.slices.end(), selected.additional_sources.begin(), selected.additional_sources.end());
    if (excerpts.slices.empty()) { error = "The selected message range could not be resolved"; return; }
    const auto visible = [snapshot, session = current->summary.id, access = current->access_revision] {
      const auto view = snapshot.Get();
      return view && view->summary.id == session && view->access_revision == access;
    };
    error = "";
    busy = true;
    tasks.Launch([dialogs, service, origin = SelectionOrigin{conversation, current->access_revision}, selected, draft, question_focus, selection,
                  focus_request, range_anchor, anchor, busy, error, excerpts = std::move(excerpts), visible, ask, on_citation_added]() mutable -> Task<void> {
      auto text = co_await origin.conversation->ReadSelectionsAsync(excerpts.slices);
      busy = false;
      if (!text.Succeeded()) { error = text.Error().message; co_return; }
      if (!visible() || !origin.HasAccess()) { error = "Selection access changed"; co_return; }
      if (ask) {
        ShowSourceDialog(dialogs, service, std::move(origin), std::move(excerpts), draft, question_focus,
            [selection, selected, focus_request, range_anchor, anchor, visible] {
              if (!visible()) return;
              selection = selected;
              range_anchor = anchor;
              focus_request = selected.source;
            }, on_citation_added);
      } else {
        auto sources = draft.Get();
        sources.quote = GatherCitation(std::move(excerpts));
        draft = std::move(sources);
        on_citation_added();
      }
    });
  };
  std::vector<View> actions{
      Button("Side chat").OnClick([use] { use(true); }).With(action_focus, Enabled(!busy.Get())),
      Button("Quote").OnClick([use] { use(false); }).With(Enabled(!busy.Get())),
  };
  if (anchor) {
    actions.push_back(Button("Cancel range").OnClick([range_anchor] { range_anchor = std::nullopt; }).With(Enabled(!busy.Get())));
  } else {
    actions.push_back(Button("Extend selection")
        .OnClick([range_anchor, source = std::move(endpoint)] { range_anchor = source; }).With(Enabled(!busy.Get())));
  }
  auto row = ProvideEnvironment(style, Row(std::move(actions)).With(Spacing(8.0F)));
  if (error.Get().empty()) return row;
  return Column{std::move(row), Text(error.Get(), TextRole::Label)}.With(Spacing(3.0F));
}

} // namespace

[[huxerui::composable]]
View AgentChatAttachmentChips(State<AgentChatDraftSources> draft) {
  const auto theme = UseTheme();
  const auto action_style = ChatActionStyle(UseEnvironment<ButtonStyle>(), theme);
  std::vector<View> chips;
  for (const auto& item : draft->attachments) {
    chips.push_back(Row{
        Text(item->name, TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()),
        ProvideEnvironment(action_style, Button("Remove").OnClick([draft, item] {
          auto editing = draft.Get();
          std::erase(editing.attachments, item);
          draft = std::move(editing);
        }).With(Semantics{.label = "Remove " + item->name}).Key("remove-attachment/" + item->id.value)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center), Padding(EdgeInsets::Symmetric(8.0F, 0.0F)),
           Background(theme.colors.surface_container_high), CornerRadius(6.0F)).Key(item->id.value));
  }
  return Column(std::move(chips)).With(Spacing(4.0F));
}

[[huxerui::composable]]
View AgentChatSourceActions(std::shared_ptr<Conversation> conversation, std::optional<SourceSlice> source,
                            State<AgentChatDraftSources> draft, std::function<void()> on_citation_added) {
  const auto service = UseService<AgentChatSelection>();
  const auto tasks = UseTaskScope();
  const auto dialogs = UseDialog();
  const auto question_focus = UseViewHandle();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto navigation = UseEnvironment<CitationNavigation>();
  auto cited = on_citation_added;
  if (!cited && navigation.on_citation_added)
    cited = [navigation] { (*navigation.on_citation_added)(navigation.review_ticket); };
  if (!service || !conversation) return Spacer();
  const auto use = [service, tasks, dialogs, busy, error, conversation, draft, question_focus, cited](SourceSlice source, bool ask) {
    if (busy.Get()) return;
    const auto current = conversation->Snapshot();
    if (!current) { error = "The conversation is unavailable"; return; }
    busy = true;
    error = "";
    tasks.Launch([service, dialogs, busy, error, origin = SelectionOrigin{conversation, current->access_revision},
                  source, draft, question_focus, ask, cited]() -> Task<void> {
      auto text = co_await origin.conversation->ReadSelectionAsync(source);
      busy = false;
      if (!text.Succeeded()) { error = text.Error().message; co_return; }
      if (!origin.HasAccess()) { error = "Selection access changed"; co_return; }
      if (ask) {
        ShowSourceDialog(dialogs, service, origin, {{source}, {{0, std::move(text).Value()}}}, draft, question_focus, {}, cited);
      } else {
        auto sources = draft.Get();
        sources.quote = AgentChatCitation{source, std::move(text).Value()};
        draft = std::move(sources);
        if (cited) cited();
      }
    });
  };
  std::vector<View> content;
  if (source) {
    const auto style = ChatActionStyle(UseEnvironment<ButtonStyle>(), UseTheme());
    content.push_back(ProvideEnvironment(style, Row{
        Button("Side chat").OnClick([use, source = *source] { use(source, true); }).With(Enabled(!busy.Get())),
        Button("Quote").OnClick([use, source = *source] { use(source, false); }).With(Enabled(!busy.Get())),
    }.With(Spacing(8.0F))));
  }
  if (!error.Get().empty()) content.push_back(Text(error.Get(), TextRole::Label).Key("source-selection-error"));
  return Column(std::move(content)).With(Spacing(6.0F));
}

[[huxerui::composable]]
View AgentChatArtifactText(std::shared_ptr<Conversation> conversation, ArtifactRef artifact, PartId part,
                           std::string text, State<AgentChatDraftSources> draft) {
  const auto selection = UseState(std::optional<SelectedText>{});
  const auto selected = selection.Get();
  return Column{
      ScrollView(SelectionField(artifact, part, std::move(text), selection).Key("artifact-text"))
          .With(Grow(), ScrollBar()),
      // Keep the read alive when focusing a button collapses the editor's selection.
      AgentChatSourceActions(conversation,
          selected && !selected->text.empty() ? std::optional{selected->source} : std::nullopt, draft),
  }.With(Grow(), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View AgentChat(std::shared_ptr<Conversation> conversation, AgentChatOptions options) {
  const auto tasks = UseTaskScope();
  const auto snapshot = UseState(conversation ? conversation->Snapshot() : std::shared_ptr<const SessionSnapshot>{});
  const auto history_owner = UseState(conversation);
  const auto history_window = UseState(std::optional<SessionHistoryWindow>{});
  const auto history_loading = UseState(false);
  const auto history_initialized = UseState(false);
  const auto history_controller = UseScrollController();
  using ScrollRequest = std::pair<std::size_t, ScrollAlignment>;
  const auto history_scroll = UseState(std::optional<ScrollRequest>{});
  const auto follow_sent_message = UseState(std::optional<Sequence>{});
  const auto local_draft = UseState(TextEditingValue::FromText({}));
  const auto draft = options.draft.value_or(local_draft);
  const auto artifact_review = UseState(std::shared_ptr<const ArtifactPreview>{});
  const auto review_loading = UseState(static_cast<bool>(options.close_review_window) && options.review_request.has_value());
  const auto review_ticket = UseState(std::uint64_t{0});
  const auto review_approval = UseState(std::optional<AgentChatApprovalRef>{});
  const auto review_origin = UseState(std::optional<ViewHandle>{});
  const auto review_message = UseState(std::optional<ArtifactMessagePosition>{});
  const auto return_focus = UseState(std::optional<ViewHandle>{});
  const auto focus_after_restore = UseState(false);
  const auto local_composer_focus = UseViewHandle();
  const auto composer_focus = options.composer_focus.value_or(local_composer_focus);
  const auto composer_anchor = UseViewHandle();
  const auto handled_review = UseState(std::optional<AgentChatReviewRequest>{});
  const auto focused_interaction = UseState(std::optional<InteractionId>{});
  const auto error = UseState(std::string{});
  const auto sending = UseState(false);
  const auto queue = UseState(std::optional<RunId>{});
  const auto side_service = UseService<AgentChatSelection>();
  const auto links = OptionalChatService<AgentChatLinkHandler>();
  const auto send_menu = UseMenu();
  const auto send_mode = UseViewHandle();
  const auto composer_focused = UseState(false);
  const auto theme = UseTheme();
  const auto layout_style = UseEnvironment<detail::AgentChatLayoutStyle>();
  const auto field_style = ChatFieldStyle(UseEnvironment<TextFieldStyle>(), theme);
  const auto action_style = ChatActionStyle(UseEnvironment<ButtonStyle>(), theme);
  const auto viewport_class = UseViewportClass();
  const bool compact = viewport_class == ViewportClass::Compact;
  const auto selection = UseState(std::optional<SelectedText>{});
  const auto range_anchor = UseState(std::optional<SourceSlice>{});
  const auto selection_focus = UseState(std::optional<SourceSlice>{});
  const auto local_sources = UseState(AgentChatDraftSources{});
  const auto draft_sources = options.draft_sources.value_or(local_sources);
  const auto local_references = UseState(std::vector<QueuedDraftReference>{});
  const auto recovered = options.recovered_references.value_or(local_references);
  const auto local_skills = UseState(std::vector<SkillRef>{});
  const auto draft_skills = options.draft_skills.value_or(local_skills);
  const auto recovering = UseState(false);
  const auto focus_cited_draft = [selection, range_anchor, focus_after_restore] {
    selection = std::nullopt;
    range_anchor = std::nullopt;
    focus_after_restore = true;
  };
  const auto close_review = [artifact_review, review_ticket, review_loading] {
    review_ticket = review_ticket.Get() + 1;
    artifact_review = std::shared_ptr<const ArtifactPreview>{};
    review_loading = false;
  };
  const auto review_citation = UseState(std::make_shared<const std::function<void(std::uint64_t)>>(
      [review_ticket, close_review, focused_interaction, focus_cited_draft](std::uint64_t ticket) {
        if (ticket != review_ticket.Get()) return;
        close_review();
        focused_interaction = std::nullopt;
        focus_cited_draft();
      }));

  Lifecycle([conversation, history_owner, history_window, history_loading, history_initialized, history_scroll, follow_sent_message, snapshot,
      artifact_review, review_ticket, review_loading, review_approval, review_origin, review_message, handled_review,
      selection, range_anchor, selection_focus] {
    if (history_owner.Get() == conversation) return;
    selection = std::nullopt;
    range_anchor = std::nullopt;
    selection_focus = std::nullopt;
    history_owner = conversation;
    history_window = std::nullopt;
    history_loading = false;
    history_initialized = false;
    history_scroll = std::nullopt;
    follow_sent_message = std::nullopt;
    artifact_review = std::shared_ptr<const ArtifactPreview>{};
    review_ticket = review_ticket.Get() + 1;
    review_loading = false;
    review_approval = std::nullopt;
    review_origin = std::nullopt;
    review_message = std::nullopt;
    handled_review = std::nullopt;
    snapshot = conversation ? conversation->Snapshot() : std::shared_ptr<const SessionSnapshot>{};
  }, conversation);

  const bool review_only = static_cast<bool>(options.close_review_window);
  const auto requested_history = review_only && options.review_request && options.review_request->history_first
      ? std::optional{SessionHistoryWindow{*options.review_request->history_first}}
      : history_owner.Get() == conversation ? history_window.Get() : std::nullopt;
  Lifecycle([conversation, tasks, snapshot, error, artifact_review, review_ticket, review_loading, focused_interaction,
      history_owner, history_window, history_loading, history_initialized, history_controller, history_scroll, follow_sent_message,
      requested_history, review_only] {
    auto subscription = std::make_shared<std::optional<Subscription>>();
    if (!conversation) return std::function<void()>{[] {}};
    auto project = [conversation, snapshot, error, artifact_review, review_ticket, review_loading, focused_interaction,
        history_owner, history_window, history_loading, history_initialized, history_controller, history_scroll, follow_sent_message,
        requested_history, review_only](auto result) {
      if (history_owner.Get() != conversation || (!review_only && history_window.Get() != requested_history)) return;
      if (result.Succeeded()) {
        auto incoming = std::move(result).Value();
        const auto previous = snapshot.Get();
        const bool was_loading = history_loading.Get();
        const bool first = !history_initialized.Get();
        const auto viewport = history_controller.Metrics();
        history_loading = false;
        history_initialized = true;
        if (!review_only && !incoming->messages.empty()) {
          const std::size_t earlier = incoming->older_history ? 1 : 0;
          const auto pending = follow_sent_message.Get();
          const bool sent = pending && std::ranges::any_of(incoming->messages, [pending](const Message& message) {
            return message.author == MessageAuthor::User && message.sequence > *pending;
          });
          if (sent) follow_sent_message = std::nullopt;
          if (was_loading && requested_history) {
            const auto start = std::ranges::lower_bound(incoming->messages, requested_history->first, {}, &Message::sequence);
            if (start != incoming->messages.end()) history_scroll = ScrollRequest{
                static_cast<std::size_t>(start - incoming->messages.begin()) + earlier, ScrollAlignment::Start};
          } else if (!artifact_review.Get() && !review_loading.Get() && !focused_interaction.Get() &&
              (first || sent || ((!previous || previous->through_sequence != incoming->through_sequence) &&
                          viewport.viewport_extent > 0.0F && viewport.maximum_offset - viewport.offset <= 24.0F))) {
            history_scroll = ScrollRequest{incoming->messages.size() + earlier - 1, ScrollAlignment::End};
          }
          if (!requested_history) history_window = SessionHistoryWindow{incoming->messages.front().sequence};
        }
        snapshot = std::move(incoming);
      } else {
        history_loading = false;
        history_scroll = std::nullopt;
        snapshot = std::shared_ptr<const SessionSnapshot>{};
        error = result.Error().message;
      }
      const auto& shown = artifact_review.Get();
      if (shown && (!snapshot.Get() || shown->access_revision != snapshot.Get()->access_revision ||
          !ShowsArtifact(*snapshot.Get(), shown->artifact))) {
        review_ticket = review_ticket.Get() + 1;
        artifact_review = std::shared_ptr<const ArtifactPreview>{};
        review_loading = false;
      }
    };
    auto worker = tasks.Launch([conversation, subscription, error, snapshot, history_owner, history_window,
        history_loading, requested_history, review_only, project = std::move(project)]() mutable -> Task<void> {
      auto observed = co_await conversation->ObserveAsync(std::move(project), requested_history);
      if (observed.Succeeded()) *subscription = std::move(observed).Value();
      else if (history_owner.Get() == conversation && (review_only || history_window.Get() == requested_history)) {
        history_loading = false;
        snapshot = std::shared_ptr<const SessionSnapshot>{};
        error = observed.Error().message;
      }
    });
    return std::function<void()>{[worker, subscription] { worker.Cancel(); subscription->reset(); }};
  }, conversation, requested_history, review_only);

  Lifecycle([history_scroll, history_controller] {
    if (const auto request = history_scroll.Get()) {
      history_scroll = std::nullopt;
      history_controller.ScrollToItem(request->first, request->second);
    }
  }, history_scroll.Get());

  const OpenArtifactReview open_artifact = [conversation, tasks, snapshot, error, artifact_review, review_ticket, review_loading,
      review_approval, review_origin, review_message, review_only, custom_review = options.open_artifact_review](
          ArtifactRef ref, std::optional<AgentChatApprovalRef> approval, std::optional<ViewHandle> origin,
          std::optional<ArtifactMessagePosition> message) {
    const auto ticket = review_ticket.Get() + 1;
    review_ticket = ticket;
    const bool switching_attachment = artifact_review.Get() && approval && review_approval.Get() == approval;
    review_approval = std::move(approval);
    review_origin = std::move(origin);
    review_message = std::move(message);
    review_loading = true;
    if (!switching_attachment) artifact_review = std::shared_ptr<const ArtifactPreview>{};
    const bool allow_custom_review = !review_only && !review_approval.Get();
    tasks.Launch([conversation, ref, snapshot, error, artifact_review, review_ticket, review_loading, ticket,
                  allow_custom_review, custom_review, review_origin]() -> Task<void> {
      auto preview = co_await conversation->PreviewArtifactAsync(ref);
      if (ticket != review_ticket.Get()) co_return;
      review_loading = false;
      if (!preview.Succeeded()) { error = preview.Error().message; co_return; }
      if (!snapshot.Get() || preview.Value().access_revision != snapshot.Get()->access_revision ||
          !ShowsArtifact(*snapshot.Get(), ref)) { error = "Artifact is unavailable in this conversation"; co_return; }
      error = "";
      if (allow_custom_review && custom_review && custom_review(preview.Value(), review_origin.Get())) co_return;
      artifact_review = std::make_shared<const ArtifactPreview>(std::move(preview).Value());
    });
  };
  const auto review_request = options.review_request;
  const bool review_history_ready = !review_only || !review_request || !review_request->history_first || history_initialized.Get();
  Lifecycle([conversation, review_request, handled_review, open_artifact, review_history_ready] {
    if (conversation && review_request && review_history_ready && handled_review.Get() != review_request) {
      handled_review = review_request;
      open_artifact(review_request->artifact, review_request->approval, {}, {});
    }
  }, conversation, review_request, review_history_ready);

  const auto interaction_request = options.interaction_request;
  Lifecycle([interaction_request, focused_interaction, artifact_review, review_ticket, review_loading, error] {
    focused_interaction = interaction_request ? std::optional{interaction_request->interaction} : std::nullopt;
    if (interaction_request) {
      review_ticket = review_ticket.Get() + 1;
      artifact_review = std::shared_ptr<const ArtifactPreview>{};
      review_loading = false;
      error = "";
    }
  }, conversation, interaction_request);

  Lifecycle([return_focus, composer_focus, focus_after_restore] {
    if (const auto origin = return_focus.Get()) {
      return_focus = std::nullopt;
      if (!origin->RequestFocus(true)) composer_focus.RequestFocus(true);
    }
    if (focus_after_restore.Get()) {
      focus_after_restore = false;
      composer_focus.RequestFocus(true);
    }
  }, return_focus.Get(), focus_after_restore.Get());

  const auto vacant = [draft, draft_sources, recovered, draft_skills, sending] {
    return !sending.Get() && draft->text.empty() && !draft->composition && !draft_sources->quote &&
        !draft_sources->conclusion && draft_sources->attachments.empty() && recovered.Get().empty() && draft_skills.Get().empty();
  };
  const bool empty_draft = vacant();
  Lifecycle([empty_draft, draft_sources] {
    if (!empty_draft || !draft_sources->send_as_new_task) return;
    auto sources = draft_sources.Get();
    sources.send_as_new_task = false;
    draft_sources = std::move(sources);
  }, empty_draft);
  const auto current = history_owner.Get() == conversation ? snapshot.Get() : std::shared_ptr<const SessionSnapshot>{};
  if (!current && !options.close_review_window) return Text(error->empty() ? "Opening conversation..." : error.Get());
  const auto detached_status = [&](std::string message) -> View {
    return Column{ProvideEnvironment(action_style, Button("Close review").OnClick(options.close_review_window)),
        Text(std::move(message), TextRole::Label)}
        .With(Grow(), Padding(24.0F), Spacing(12.0F), Background(theme.colors.surface));
  };
  if (!current) return detached_status(error->empty() ? "Opening conversation..." : error.Get());
  const bool side_chat = current->origin && current->origin->kind == SessionOriginKind::SideChat;
  const auto active = current->summary.active_run;
  const bool send_as_new = draft_sources->send_as_new_task;
  const bool enqueue = active && (send_as_new || queue.Get() == active);
  const auto restore = [tasks, conversation, vacant, recovering, draft, draft_sources, recovered, draft_skills,
                        focus_after_restore, error](std::variant<QueueItemId, CommandId> origin) {
    if (!vacant() || recovering.Get()) return;
    recovering = true;
    tasks.Launch([conversation, origin = std::move(origin), vacant, recovering, draft, draft_sources, recovered,
                  draft_skills, focus_after_restore, error]() -> Task<void> {
      const auto* item = std::get_if<QueueItemId>(&origin);
      auto pending = item ? conversation->ReadQueuedDraftAsync(*item) :
          conversation->ReadUnappliedSteerDraftAsync(std::get<CommandId>(origin));
      auto restored = co_await std::move(pending);
      recovering = false;
      if (!restored.Succeeded()) { error = restored.Error().message; co_return; }
      if (!vacant()) { error = "Clear the current draft before restoring another input"; co_return; }
      auto content = std::move(restored).Value();
      auto sources = draft_sources.Get();
      sources.send_as_new_task = !item;
      draft_sources = std::move(sources);
      draft = TextEditingValue::FromText(std::move(content.text));
      recovered = std::move(content.references);
      draft_skills = std::move(content.skills);
      error = "";
      focus_after_restore = true;
    });
  };
  const auto has_input = [draft, draft_sources, recovered] {
    return !draft->text.empty() || !draft_sources->attachments.empty() ||
        std::ranges::any_of(recovered.Get(), [](const auto& entry) {
          return entry.reference && std::holds_alternative<ArtifactRef>(*entry.reference);
        });
  };
  const bool can_send = !sending.Get() && has_input() && !draft->composition && std::ranges::all_of(recovered.Get(), [](const auto& entry) {
    return entry.reference.has_value();
  });
  const auto send = [tasks, conversation, draft, error, sending, enqueue, active, draft_sources, recovered, has_input,
                     follow_sent_message, sequence = current->through_sequence, follow = layout_style.chat_bubbles && !side_chat,
                     controls = options.run_controls, draft_skills] {
    if (sending.Get() || !has_input() || draft->composition ||
        std::ranges::any_of(recovered.Get(), [](const auto& entry) { return !entry.reference; })) return;
    const auto input = draft.Get();
    sending = true;
    if (follow && !enqueue) follow_sent_message = sequence;
    tasks.Launch([conversation, input, enqueue, active, draft, error, sending, draft_sources, recovered, draft_skills, follow_sent_message,
                  selected = draft_skills.Get(),
                  restored = recovered.Get(), received = draft_sources.Get(),
                  start = controls ? controls->next.Get() : UserRunOptions{}]() -> Task<void> {
      const auto command = NewCommandId();
      SendInput message{.text = input.text, .skills = selected};
      for (const auto& attachment : received.attachments) message.staged_attachments.push_back(attachment->id);
      for (const auto& entry : restored) {
        if (const auto* source = std::get_if<SourceSlice>(&*entry.reference)) message.context.push_back(*source);
        else if (const auto* queued = std::get_if<QueuedConclusionRef>(&*entry.reference)) message.recovered_conclusions.push_back(*queued);
        else if (const auto* attachment = std::get_if<ArtifactRef>(&*entry.reference)) message.attachments.push_back(*attachment);
        else message.recovered_steer_conclusions.push_back(std::get<UnappliedSteerConclusionRef>(*entry.reference));
      }
      const auto append = [](std::vector<SourceSlice>& destination, const std::optional<AgentChatCitation>& citation) {
        if (!citation) return;
        destination.push_back(citation->source);
        destination.insert(destination.end(), citation->additional_sources.begin(), citation->additional_sources.end());
      };
      append(message.context, received.quote);
      append(message.side_chat_conclusions, received.conclusion);
      auto outcome = AgentResult<void>::Success();
      if (enqueue) {
        auto queued = co_await conversation->QueueAsync(command, std::move(message), start);
        if (!queued.Succeeded()) outcome = AgentResult<void>::Failure(queued.Error());
      } else if (active) {
        auto steered = co_await conversation->SteerAsync(command, *active, std::move(message));
        if (!steered.Succeeded()) outcome = AgentResult<void>::Failure(steered.Error());
      } else {
        auto sent = co_await conversation->SendAsync(command, std::move(message), start);
        if (!sent.Succeeded()) outcome = AgentResult<void>::Failure(sent.Error());
      }
      sending = false;
      if (outcome.Succeeded()) {
        error = "";
        if (draft->text == input.text && !draft->composition) draft = TextEditingValue::FromText({});
        auto sources = draft_sources.Get();
        if (sources.quote == received.quote) sources.quote.reset();
        if (sources.conclusion == received.conclusion) sources.conclusion.reset();
        std::erase_if(sources.attachments, [&](const auto& item) {
          return std::ranges::find(received.attachments, item) != received.attachments.end();
        });
        if (sources.send_as_new_task == received.send_as_new_task) sources.send_as_new_task = false;
        draft_sources = std::move(sources);
        if (recovered.Get() == restored) recovered = std::vector<QueuedDraftReference>{};
        if (draft_skills.Get() == selected) draft_skills = std::vector<SkillRef>{};
      } else {
        follow_sent_message = std::nullopt;
        error = outcome.Error().message;
        if (active && !enqueue) {
          const auto latest = conversation->Snapshot();
          if (outcome.Error().code == ErrorCode::AlreadyClosed || (latest &&
              (latest->summary.active_run != active || latest->summary.run_state == RunState::Cancelling))) {
            auto sources = draft_sources.Get();
            sources.send_as_new_task = true;
            draft_sources = std::move(sources);
          }
        }
      }
    });
  };

  const bool selection_enabled = side_chat ? static_cast<bool>(options.bring_back) : static_cast<bool>(side_service);
  std::optional<SelectedText> selected = selection.Get();
  if (selected && (!selection_enabled || selected->text.empty() ||
      std::ranges::none_of(current->messages, [&](const Message& message) {
        return SourceRef{message.ref} == selected->source.source && message.state == MessageState::Complete &&
            (!side_chat || message.author == MessageAuthor::Agent);
      }))) {
    selected.reset();
  }
  const std::size_t earlier = current->older_history && !current->messages.empty() ? 1 : 0;
  const auto return_to_chat = [close_review, review_origin, return_focus] {
    close_review();
    return_focus = review_origin.Get();
  };
  const auto reviewed = artifact_review.Get();
  const auto approval = review_approval.Get();
  std::optional<View> review_body;
  std::optional<View> unanchored_summary;
  std::optional<InlineArtifactSummary> inline_summary;
  std::optional<std::size_t> summary_scroll;
  if (reviewed && reviewed->access_revision == current->access_revision && ShowsArtifact(*current, reviewed->artifact)) {
    if (options.render_artifact) review_body = options.render_artifact(*reviewed, conversation, draft_sources);
    if (!review_body) review_body = conversation->RenderArtifact(*reviewed);
    if (!review_body) {
      const auto generic_preview = [&] {
        std::vector<View> details;
        if (!reviewed->summary.empty()) details.push_back(Text(reviewed->summary));
        if (reviewed->structured) details.push_back(Text(detail::ToWireJson(*reviewed->structured).dump(2)));
        return Column(std::move(details)).With(Spacing(8.0F));
      };
      if (review_only || approval) {
        review_body = ScrollView(generic_preview()).With(ScrollBar());
      } else {
        auto position = review_message.Get();
        if (!position) {
          for (std::size_t i = 0; i < current->messages.size(); ++i) {
            const auto& message = current->messages[i];
            const auto block = std::ranges::find_if(message.blocks, [&](const ContentBlock& block) {
              const auto* artifact = std::get_if<ArtifactBlock>(&block.body);
              return artifact && artifact->artifact == reviewed->artifact;
            });
            if (block == message.blocks.end()) continue;
            position = ArtifactMessagePosition{message.ref.message, block->id};
            summary_scroll = i + earlier;
            break;
          }
        }
        auto summary = options.render_artifact_summary ? options.render_artifact_summary(*reviewed, conversation) : std::nullopt;
        if (!summary) summary = ScrollView(generic_preview()).With(Frame{.max_height = 180.0F}, ScrollBar());
        std::vector<View> header;
        if (!position) header.push_back(Text(reviewed->name + " v" + std::to_string(reviewed->artifact.version), TextRole::Label));
        header.push_back(Spacer());
        header.push_back(ProvideEnvironment(action_style, Button("Close preview").OnClick(return_to_chat)));
        auto details = Column{Row(std::move(header)), std::move(*summary).Key(
            reviewed->artifact.id.value + "/" + std::to_string(reviewed->artifact.version))}.With(Spacing(4.0F),
            CrossAlign(CrossAxisAlignment::Stretch), Padding(EdgeInsets{.right = 12.0F, .bottom = 10.0F, .left = 12.0F}),
            Border{theme.colors.outline}, CornerRadius(8.0F)).Key("artifact-inline-summary");
        if (position) inline_summary = InlineArtifactSummary{*position, reviewed->artifact, std::move(details)};
        else unanchored_summary = std::move(details);
      }
    }
  }
  Lifecycle([history_controller, summary_scroll] {
    if (summary_scroll) history_controller.ScrollToItem(*summary_scroll, ScrollAlignment::Start);
  }, review_ticket.Get(), summary_scroll);

  auto history = VirtualList(current->messages.size() + earlier,
      [conversation, open_artifact, selection, selection_enabled, side_chat, current, snapshot, range_anchor, earlier,
       history_loading, history_window, loading = history_loading.Get(), theme, field_style, action_style, restore,
       can_restore = empty_draft && !recovering.Get(),
       agent_label = options.agent_label, links, selected, draft_sources, selection_focus, focus_cited_draft,
       inline_summary = std::move(inline_summary), render_text_block = options.render_text_block,
       bubbles = layout_style.chat_bubbles, narrow = compact || side_chat](std::size_t index) -> View {
    if (earlier && index == 0) {
      return ProvideEnvironment(action_style, Button(loading ? "Loading earlier messages..." : "Load earlier messages")
          .OnClick([history_window, history_loading, boundary = current->messages.front().sequence] {
            if (history_loading.Get()) return;
            history_loading = true;
            history_window = SessionHistoryWindow{boundary, 50};
          }).With(Enabled(!loading)).Key("earlier-messages"));
    }
    const auto& message = current->messages[index - earlier];
    std::optional<SteerState> steer;
    std::optional<View> recover;
    std::string agent;
    if (message.run) {
      const auto run = std::ranges::find(current->runs, *message.run, &RunSnapshot::id);
      if (run != current->runs.end()) {
        const auto found = std::ranges::find(run->steering, message.ref.message, &RunSteerSnapshot::message);
        if (found != run->steering.end()) {
          steer = found->state;
          if (found->recovery) recover = ProvideEnvironment(action_style,
              Button("Restore draft").OnClick([restore, command = *found->recovery] { restore(command); })
                  .With(Enabled(can_restore)).Key("restore-supplement"));
        }
        if (agent_label && message.author == MessageAuthor::Agent) agent = agent_label(run->agent);
      }
    }
    std::optional<View> selection_actions;
    if (!side_chat && selected && selected->source.source == SourceRef{message.ref} && range_anchor.Get() != SelectionBounds(*selected)) {
      selection_actions = MessageSelectionActions(conversation, snapshot, *selected, selection, range_anchor, draft_sources, selection_focus,
          focus_cited_draft)
          .Key("message-selection-actions");
    }
    return MessageView(message, conversation, open_artifact, selection,
        selection_enabled && (!side_chat || message.author == MessageAuthor::Agent), steer, std::move(recover),
        std::move(selection_actions),
        std::move(agent), theme, field_style, current->access_revision, links, inline_summary, render_text_block, bubbles, narrow)
        .Key(message.ref.message.value).With(Padding(EdgeInsets{.top = 2.0F, .bottom = 18.0F}));
  }).EstimatedItemExtent(76.0F).Controller(history_controller).With(Grow(), ScrollBar())
      .Key("agent-chat-history");
  std::vector<View> notices;
  if (const auto anchor = range_anchor.Get(); anchor && (!selected || SelectionBounds(*selected) == *anchor))
    notices.push_back(ProvideEnvironment(action_style, Row{
      Text("Select the other end of the range", TextRole::Label).With(Grow()),
      Button("Cancel range").OnClick([range_anchor] { range_anchor = std::nullopt; }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))));
  if (std::ranges::any_of(current->tools, [&](const auto& tool) {
        return active == tool.run && tool.state == ToolCallState::Reviewing;
      })) notices.push_back(Text("Reviewing tool request", TextRole::Label));
  const auto render_request = [&](const InteractionSnapshot& interaction, bool focused) -> std::optional<View> {
    if (std::holds_alternative<ToolApproval>(interaction.body) || std::holds_alternative<OperationApproval>(interaction.body))
      return PendingApprovalView(interaction, conversation, tasks, error, *current, open_artifact, action_style, theme.colors,
          options.review_attachment_label)
          .Key(interaction.id.value);
    if (options.render_form) {
      if (auto form = options.render_form(interaction, conversation)) return std::move(*form).Key(interaction.id.value);
    }
    if (focused) return PendingFormView(interaction, conversation, error).Key(interaction.id.value);
    return {};
  };
  auto last_root = std::ranges::find_if(current->runs.rbegin(), current->runs.rend(),
      [](const auto& run) { return !run.parent; });
  std::set<RunId> last_run_tree;
  const auto role_label = [&](const AgentRef& agent) {
    auto label = options.agent_label ? options.agent_label(agent) : std::string{};
    return label.empty() ? agent.id.value : label;
  };
  if (last_root != current->runs.rend()) {
    last_run_tree.insert(last_root->id);
    std::map<RunId, std::vector<const RunSnapshot*>> children;
    for (const auto& run : current->runs) {
      if (run.parent) children[*run.parent].push_back(&run);
    }
    std::vector<View> rows;
    const auto add_rows = [&](auto&& visit, const RunId& parent, unsigned depth) -> void {
      const auto found = children.find(parent);
      if (found == children.end()) return;
      for (const auto* run : found->second) {
        last_run_tree.insert(run->id);
        const char* status = "Running";
        switch (run->state) {
          case RunState::WaitingForInput: status = "Waiting for input"; break;
          case RunState::WaitingForReply: status = "Waiting for reply"; break;
          case RunState::WaitingForDependency:
            status = run->waits.empty() ? "Queued" : "Waiting for dependency";
            break;
          case RunState::Cancelling: status = "Stopping"; break;
          case RunState::Succeeded: status = "Completed"; break;
          case RunState::Failed: status = "Failed"; break;
          case RunState::Cancelled: status = "Cancelled"; break;
          case RunState::Running: break;
        }
        rows.push_back(Text(role_label(run->agent) + " · " + status, TextRole::Label)
            .With(Padding(EdgeInsets{.left = 12.0F * std::min(depth, 6U)})).Key("delegate/" + run->id.value));
        visit(visit, run->id, depth + 1);
      }
    };
    add_rows(add_rows, last_root->id, 0);
    if (!rows.empty()) notices.push_back(ScrollView(Column(std::move(rows)).With(Spacing(4.0F)))
        .With(Frame{.max_height = 110.0F}, ScrollBar()).Key("delegate-runs"));
  }
  for (const auto& interaction : current->interactions) {
    if (focused_interaction.Get()) break;
    if (interaction.state != InteractionState::Pending) continue;
    const auto child = std::ranges::find_if(current->runs, [&](const auto& run) {
      return run.parent && run.id == interaction.run;
    });
    const bool delegated = child != current->runs.end();
    if (auto view = render_request(interaction, delegated)) {
      if (delegated) {
        notices.push_back(Text(role_label(child->agent), TextRole::Label).Key("delegate-request/" + interaction.id.value));
      }
      notices.push_back(std::move(*view));
    }
  }
  if (!active && last_root != current->runs.rend() && last_root->failure) {
    notices.push_back(Text(last_root->failure->message, TextRole::Label)
        .Style({Font::System(12.0F), theme.colors.error}));
    const auto trigger = last_root->trigger;
    const bool retryable = trigger == RunTrigger::UserInput || trigger == RunTrigger::Retry || trigger == RunTrigger::Regenerate;
    if (last_root->state == RunState::Failed && retryable && current->summary.lifecycle == SessionLifecycle::Open) {
      if (const auto blocked = detail::RestartToolError(current->tools, last_run_tree)) {
        notices.push_back(Text(blocked->message, TextRole::Label).Key("agent-chat-retry-blocked"));
      } else {
        notices.push_back(ProvideEnvironment(action_style, Row{
            Button("Retry").OnClick([conversation, tasks, error, sending, history_owner, source = last_root->id] {
              if (sending.Get()) return;
              sending = true;
              error = "";
              tasks.Launch([conversation, error, sending, history_owner, source]() -> Task<void> {
                auto command = CommandId{"agent-chat:retry/" + source.value};
                const auto result = co_await conversation->RetryAsync(std::move(command), source);
                sending = false;
                if (history_owner.Get() == conversation && !result.Succeeded()) error = result.Error().message;
              });
            }).With(Enabled(!sending.Get() && current->summary.queued_inputs == 0)).Key("agent-chat-retry"),
            Spacer(),
        }));
      }
    }
  }
  const auto last_change = std::ranges::find_if(current->queue.rbegin(), current->queue.rend(), [](const auto& item) {
    return std::holds_alternative<CommandId>(item.input);
  });
  if (last_change != current->queue.rend() && last_change->state == QueueItemState::Rejected) {
    const auto message = last_change->reason ? last_change->reason->message : "The queued change could not start.";
    notices.push_back(Text(message, TextRole::Label).Style({Font::System(12.0F), theme.colors.error})
        .Key("artifact-change-queue-error"));
  }
  if (!error->empty()) notices.push_back(Text(error.Get(), TextRole::Label).Style({Font::System(12.0F), theme.colors.error}));
  std::optional<AgentChatCitation> conclusion;
  if (side_chat && selected) conclusion = AgentChatCitation{selected->source, selected->text};
  const bool returning_selection = conclusion.has_value();
  if (!conclusion && side_chat && options.bring_back) conclusion = LastCompletedReply(current->messages);
  std::vector<View> composer;
  if (!draft_sources->attachments.empty()) composer.push_back(AgentChatAttachmentChips(draft_sources));
  if (const auto quoted = draft_sources.Get().quote) {
    composer.push_back(ProvideEnvironment(action_style, Row{Text(quoted->text, TextRole::Label).LineLimit(2)
        .Overflow(TextOverflow::Ellipsis).With(Grow()),
        Button("Remove quote").OnClick([draft_sources] {
          auto sources = draft_sources.Get();
          sources.quote.reset();
          draft_sources = std::move(sources);
        })}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center), CornerRadius(5.0F),
            Background(theme.colors.surface_container_high), Padding(EdgeInsets::Symmetric(7.0F, 0.0F)))));
  }
  if (const auto brought = draft_sources.Get().conclusion) {
    composer.push_back(ProvideEnvironment(action_style, Row{Text(brought->text, TextRole::Label).LineLimit(2)
        .Overflow(TextOverflow::Ellipsis).With(Grow()),
        Button("Remove conclusion").OnClick([draft_sources] {
          auto sources = draft_sources.Get();
          sources.conclusion.reset();
          draft_sources = std::move(sources);
        })}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center), CornerRadius(5.0F),
            Background(theme.colors.surface_container_high), Padding(EdgeInsets::Symmetric(7.0F, 0.0F)))));
  }
  if (!recovered.Get().empty()) {
    std::vector<View> references;
    for (const auto& entry : recovered.Get()) {
      auto label = entry.text;
      if (entry.reference) {
        const std::string_view origin = send_as_new ? "Recovered " : "Queued ";
        const auto type = std::holds_alternative<SourceSlice>(*entry.reference) ? "reference\n" :
            std::holds_alternative<ArtifactRef>(*entry.reference) ? "attachment\n" : "conclusion\n";
        label = std::string(origin) + type + ShortPreview(entry.text);
      }
      references.push_back(Row{Text(std::move(label), TextRole::Label).With(Grow()),
          Button("Remove reference").OnClick([recovered, entry] {
            auto remaining = recovered.Get();
            const auto found = std::ranges::find(remaining, entry);
            if (found == remaining.end()) return;
            remaining.erase(found);
            recovered = std::move(remaining);
          })}.With(Spacing(8.0F)));
    }
    composer.push_back(ScrollView(Column(std::move(references)).With(Spacing(4.0F)))
        .With(Frame{.max_height = 110.0F}, ScrollBar()));
  }
  auto composer_field_style = field_style;
  if (layout_style.chat_bubbles) {
    composer_field_style.text_style.font = Font::System(15.0F);
    composer_field_style.placeholder_style.font = Font::System(15.0F);
  }
  composer.push_back(ProvideEnvironment(composer_field_style, Row{
      TextField(draft).Placeholder(side_chat ? "Ask a follow-up" : "Send a message").Label("Message")
          .LineLimits(TextFieldLineLimits::MultiLine(side_chat ? 1 : 2, 5))
          .Variant(TextFieldVariant::Standard).OnChanged([draft](const TextEditingValue& value) { draft = value; })
          .On<ViewEvents::FocusChanged>([composer_focused](bool focused) { composer_focused = focused; })
          .OnSubmitted(send).With(composer_focus, Grow()).Key(side_chat ? "side-chat-composer" : "composer"),
  }.With(Frame{.min_height = side_chat ? 38.0F : layout_style.chat_bubbles ? 54.0F : 59.0F},
         Padding(EdgeInsets{.top = 4.0F, .right = 4.0F, .left = 4.0F})).Key("message-input")));
  std::optional<View> run_controls;
  const bool stable_toolbar = layout_style.chat_bubbles && !side_chat;
  if (options.run_controls && options.run_controls->render) {
    std::optional<RunStartBinding> binding;
    if (active) {
      const auto run = std::ranges::find(current->runs, *active, &RunSnapshot::id);
      if (run != current->runs.end()) binding = RunStartBinding{run->model, run->execution_profile};
    }
    auto controls = options.run_controls->render(current->summary, binding);
    run_controls = stable_toolbar ? ProvideEnvironment(detail::ChatComposerAnchor{composer_anchor}, std::move(controls))
                                  : std::move(controls);
  }
  std::vector<View> actions;
  auto compact_action = UseEnvironment<IconButtonStyle>();
  compact_action.foreground = theme.colors.on_surface_variant;
  compact_action.icon_size = 16.0F;
  compact_action.minimum_interactive_size = 34.0F;
  compact_action.state_layer_size = 32.0F;
  const auto reserved_action = [] {
    return Canvas([](PaintContext&, Size) {}).With(Frame{.width = 34.0F, .height = 34.0F});
  };
  if (active && !send_as_new) {
    const char* mode = enqueue ? "Queue" : "Send now";
    const auto choose_mode = [send_menu, send_mode, queue, active, enqueue] {
      send_menu.Show(send_mode, {
          MenuItem("Send now", [queue] { queue = std::nullopt; }).Checked(!enqueue),
          MenuItem("Queue", [queue, active] { queue = active; }).Checked(enqueue),
      });
    };
    if (stable_toolbar) {
      actions.push_back(ProvideEnvironment(compact_action, IconButton(detail::ChatSendModeIcon(enqueue), "Send mode")
          .OnClick(choose_mode).With(send_mode, Tooltip(std::string("Send mode: ") + mode),
              Semantics{.label = "Send mode", .value = mode}).Key("send-mode")));
    } else {
      actions.push_back(detail::ChatControlWithIcons(Button(mode).OnClick(choose_mode)
          .With(send_mode, Semantics{.label = "Send mode", .value = mode}).Key("send-mode"),
          action_style, theme.colors.on_surface_variant, detail::ChatControlIcon::Chevron));
    }
  } else if (stable_toolbar) {
    actions.push_back(reserved_action());
  }
  if (active) {
    actions.push_back(ProvideEnvironment(compact_action, IconButton(StopIcon(), "Stop current task")
        .OnClick([tasks, conversation, active, error] {
          tasks.Launch([conversation, active, error]() -> Task<void> {
            auto stopped = co_await conversation->CancelAsync(NewCommandId(), *active);
            if (!stopped.Succeeded()) error = stopped.Error().message;
          });
        }).With(Tooltip("Stop current task")).Key("agent-chat-stop")));
  } else if (stable_toolbar) {
    actions.push_back(reserved_action());
  }
  if (conclusion) {
    actions.push_back(ProvideEnvironment(action_style, Button("Bring back conclusion")
        .OnClick([bring_back = options.bring_back, selected = std::move(*conclusion)] {
          bring_back(selected.source, selected.text);
        }).Key(returning_selection ? "side-chat-return-selection" : "side-chat-return-reply")));
  }
  auto send_style = UseEnvironment<ButtonStyle>();
  send_style.background = theme.colors.primary;
  send_style.label_style = {Font::System(13.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_primary};
  send_style.disabled_background = theme.colors.surface_container_high;
  send_style.disabled_label = theme.colors.on_surface_variant;
  send_style.minimum_height = 34.0F;
  send_style.padding = EdgeInsets::Symmetric(11.0F, 5.0F);
  send_style.corner_radii = CornerRadii{6.0F};
  const auto send_label = enqueue ? "Queue message" : send_as_new ? "Send as new task" : "Send";
  if (stable_toolbar) {
    actions.push_back(detail::ChatSendAction(send_label, send, can_send, theme.colors, UseEnvironment<IconButtonStyle>()));
  } else {
    actions.push_back(detail::ChatControlWithIcons(
        Button(send_label).OnClick(send).With(Enabled(can_send)).Key(std::string("chat-send/") + send_label), send_style,
        can_send ? theme.colors.on_primary : theme.colors.on_surface_variant, detail::ChatControlIcon::Send));
  }
  if (compact && run_controls && active && !stable_toolbar) {
    composer.push_back(std::move(*run_controls));
    run_controls.reset();
  }
  composer.push_back(Row{
      run_controls ? View(Column{std::move(*run_controls)}.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch))) : View(Spacer()),
      Row(std::move(actions)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)).Key("agent-chat-toolbar"));
  if (current->summary.queued_inputs)
    notices.push_back(Text(std::to_string(current->summary.queued_inputs) + " queued", TextRole::Label));
  if (options.run_controls && options.run_controls->describe) {
    std::vector<View> queue_rows;
    for (const auto& item : current->queue) {
      const auto* input = std::get_if<SendInput>(&item.input);
      if (!input || !item.user_start || (item.state != QueueItemState::Queued && item.state != QueueItemState::Rejected)) continue;
      const bool rejected = item.state == QueueItemState::Rejected;
      auto detail = options.run_controls->describe(*item.user_start);
      if (rejected && item.reason) detail += "\n" + item.reason->message;
      auto button = rejected ? Button("Edit queued input").OnClick([restore, id = item.id] { restore(id); })
          .With(Enabled(empty_draft && !recovering.Get())) : Button("Withdraw").OnClick([tasks, conversation, error, id = item.id] {
        tasks.Launch([conversation, error, id]() -> Task<void> {
          auto withdrawn = co_await conversation->WithdrawQueuedInputAsync(NewCommandId(), id);
          if (!withdrawn.Succeeded()) error = withdrawn.Error().message;
        });
      });
      auto title = input->text.empty() ? std::string("Attachment") : ShortPreview(input->text);
      if (!rejected) title = "Queued \xc2\xb7 " + title;
      queue_rows.push_back(ProvideEnvironment(action_style,
          Column{Row{Column{Text(std::move(title), TextRole::Label), Text(detail, TextRole::Label)}.With(Grow()),
                      std::move(button)}.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)), Divider()}
              .With(Padding(EdgeInsets::Symmetric(4.0F, 1.0F)))).Key(item.id.value));
    }
    if (!queue_rows.empty()) notices.push_back(ScrollView(Column(std::move(queue_rows)).With(Spacing(5.0F)))
        .With(Frame{.max_height = 110.0F}, ScrollBar()));
  }

  std::vector<View> heading;
  if (!side_chat) {
    auto agent_name = options.agent_label ? options.agent_label(current->summary.agent) : std::string{};
    if (agent_name.empty()) agent_name = "Assistant";
    if (current->summary.pending_interactions) agent_name += " · Action required";
    else if (active) agent_name += " · Running";
    View metadata = Text(std::move(agent_name)).Style({Font::System(12.0F), theme.colors.on_surface_variant});
    if (options.header_actions) {
      auto actions = ProvideEnvironment(action_style, std::move(*options.header_actions)).Key("agent-chat-header-actions");
      if (compact) {
        metadata = Column{std::move(metadata), std::move(actions)}.With(Spacing(3.0F));
      } else {
        metadata = Row{std::move(metadata).With(Grow()), std::move(actions)}
            .With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center));
      }
    }
    heading.push_back(Column{
        Text(current->summary.title, TextRole::Title)
            .Style({Font::System(layout_style.heading_size.value_or(
                       compact ? 24.0F : viewport_class == ViewportClass::Medium ? 29.0F : 34.0F))
                        .WithWeight(FontWeight::SemiBold), theme.colors.on_surface})
            .LineLimit(2).Overflow(TextOverflow::Ellipsis).Key("agent-chat-title"),
        std::move(metadata),
    }.With(Grow(), Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch)));
  }
  const auto viewport = history_controller.Metrics();
  if (!current->messages.empty() && viewport.maximum_offset - viewport.offset > 24.0F) {
    heading.push_back(ProvideEnvironment(action_style, Button("Jump to latest").OnClick([history_controller, last = current->messages.size() + earlier - 1] {
      history_controller.ScrollToItem(last, ScrollAlignment::End);
    })));
  }
  const bool has_notices = !notices.empty();
  std::vector<View> content;
  if (!heading.empty()) {
    auto title = Row(std::move(heading)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center),
        Padding(EdgeInsets{.bottom = side_chat ? 3.0F : layout_style.heading_size ? 8.0F : 18.0F}));
    if (has_notices) title = std::move(title).With(LayoutPriority(1.0F), ClipChildren{});
    content.push_back(std::move(title));
  }
  content.push_back(std::move(history));
  if (unanchored_summary) content.push_back(std::move(*unanchored_summary));
  std::optional<View> review_page;
  if (review_body) {
    std::vector<View> toolbar{
        options.close_review_window ? Button("Close review").OnClick(options.close_review_window) :
            Button("Back to conversation").OnClick(return_to_chat),
        Spacer(),
    };
    if (options.open_review_window) {
      std::optional<Sequence> first;
      if (std::ranges::find(current->artifacts, reviewed->artifact) == current->artifacts.end()) {
        const auto source = std::ranges::find_if(current->messages, [&](const Message& message) {
          return std::ranges::any_of(message.blocks, [&](const ContentBlock& block) {
            const auto* artifact = std::get_if<ArtifactBlock>(&block.body);
            return artifact && artifact->artifact == reviewed->artifact;
          });
        });
        if (source != current->messages.end()) first = source->sequence;
      }
      toolbar.push_back(Button("Open in new window").OnClick([open = options.open_review_window, error,
          request = AgentChatReviewRequest{reviewed->artifact, review_ticket.Get(), approval, first}] {
        try { open(request); }
        catch (const std::exception& failure) { error = failure.what(); }
      }).With(Enabled(!review_loading.Get())));
    }
    std::vector<View> details{Column{ProvideEnvironment(action_style, Row(std::move(toolbar))
        .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)))}
        .With(CrossAlign(CrossAxisAlignment::Stretch), LayoutPriority(4.0F)),
        Row{
            Text(reviewed->name.empty() ? "Artifact" : reviewed->name, TextRole::Title)
                .Style({Font::System(compact ? 21.0F : 28.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface})
                .LineLimit(2).Overflow(TextOverflow::Ellipsis).With(Grow()).Key("artifact-review-title"),
            Text("Version " + std::to_string(reviewed->artifact.version), TextRole::Label)
                .Style({Font::System(12.0F), theme.colors.on_surface_variant}).Key("artifact-review-version"),
        }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center), LayoutPriority(3.0F)).Key("artifact-review-heading"),
    };
    if (approval) {
      const auto item = std::ranges::find(current->interactions, approval->id, &InteractionSnapshot::id);
      const auto* plan = item != current->interactions.end() && item->version == approval->version
          ? ApprovalDetails(*item) : nullptr;
      if (plan) {
        auto attachments = VisibleApprovalAttachments(*plan, *current);
        const auto selected = std::ranges::find(attachments, reviewed->artifact);
        if (attachments.size() > 1 && selected != attachments.end()) {
          const auto index = static_cast<std::size_t>(selected - attachments.begin());
          std::vector<TabItem> tabs;
          for (std::size_t i = 0; i < attachments.size(); ++i) {
            auto label = options.review_attachment_label ? options.review_attachment_label(*plan, attachments[i]) : std::string{};
            if (label.empty()) label = "Attachment " + std::to_string(i + 1);
            tabs.push_back(TabItem(std::move(label)).Enabled(!review_loading.Get()));
          }
          details.push_back(ScrollView(Tabs(std::move(tabs), index).OnChanged(
              [attachments = std::move(attachments), index, open_artifact, approval, origin = review_origin.Get()](std::size_t next) {
                if (next < attachments.size() && next != index) open_artifact(attachments[next], approval, origin, {});
              }).Key("review-attachments")).ScrollAxis(Axis::Horizontal).With(LayoutPriority(3.0F)));
        }
      }
    }
    if (review_loading.Get()) details.push_back(Text("Opening attachment...", TextRole::Label));
    auto body = std::move(*review_body).With(Grow());
    if (approval) body = std::move(body).With(Frame{.min_height = 96.0F});
    if (!review_only)
      body = ProvideEnvironment(CitationNavigation{review_citation.Get(), review_ticket.Get()}, std::move(body));
    details.push_back(std::move(body));
    if (approval) {
      details.push_back(ReviewedApproval(*current, reviewed->artifact, *approval, conversation, tasks, error,
          review_loading.Get(), action_style, theme.colors).With(LayoutPriority(2.0F)));
    }
    if (!error->empty()) details.push_back(Text(error.Get(), TextRole::Label));
    review_page = Row{Spacer(), Column(std::move(details)).With(Frame{.width = 804.0F}, Spacing(12.0F),
        CrossAlign(CrossAxisAlignment::Stretch)).Key("artifact-review-content"), Spacer()}
        .With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Padding(compact ? 14.0F : 24.0F),
              Background(theme.colors.surface));
  }
  if (options.close_review_window) {
    if (review_page) return std::move(*review_page);
    return detached_status(!error->empty() ? error.Get() : review_loading.Get() ? "Opening review..." : "Artifact unavailable");
  }
  if (has_notices) content.push_back(Column(std::move(notices))
      .With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch), LayoutPriority(2.0F)));
  auto composer_panel = Column(std::move(composer)).With(composer_anchor, Spacing(3.0F),
      CrossAlign(CrossAxisAlignment::Stretch), Padding(layout_style.chat_bubbles ? 11.0F : 8.0F),
      CornerRadius(layout_style.chat_bubbles ? 17.0F : 11.0F),
      Background(layout_style.chat_bubbles ? theme.colors.surface_container : theme.colors.surface),
      Shadow{.color = Color::Rgb(34, 25, 55, 0.11F), .offset = {0.0F, 3.0F}, .blur_radius = 9.0F, .spread = -5.0F},
      Border{composer_focused.Get() ? theme.colors.on_primary_container : theme.colors.outline}).Key("agent-chat-composer");
  if (has_notices) composer_panel = std::move(composer_panel).With(LayoutPriority(3.0F));
  content.push_back(std::move(composer_panel));
  const float gutter = compact ? 14.0F : viewport_class == ViewportClass::Medium ? 21.0F : 36.0F;
  View chat = Row{
      Spacer(),
      Column(std::move(content)).With(Frame{.width = layout_style.content_width}, Spacing(8.0F),
          CrossAlign(CrossAxisAlignment::Stretch)),
      Spacer(),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch),
      Padding(side_chat ? EdgeInsets{} : EdgeInsets{.top = compact ? 20.0F : 28.0F, .right = gutter,
          .bottom = compact ? 14.0F : 25.0F, .left = gutter}));
  std::optional<View> request_page;
  if (focused_interaction.Get()) {
    const auto item = std::ranges::find(current->interactions, *focused_interaction.Get(), &InteractionSnapshot::id);
    std::vector<View> request{
        Row{ProvideEnvironment(action_style, Button("Back to conversation")
                .OnClick([focused_interaction] { focused_interaction = std::nullopt; })),
            Spacer(), Text(current->summary.title, TextRole::Label)}.With(Spacing(10.0F)), Divider(),
    };
    if (item == current->interactions.end() || item->state != InteractionState::Pending) {
      request.push_back(Text("This request is no longer pending.", TextRole::Label));
    }
    if (item != current->interactions.end()) {
      const auto actor = std::ranges::find_if(current->runs, [&](const auto& run) { return run.parent && run.id == item->run; });
      if (actor != current->runs.end()) request.push_back(Text(role_label(actor->agent), TextRole::Label));
      if (item->state == InteractionState::Pending || !std::holds_alternative<FormRequest>(item->body)) {
        if (auto view = render_request(*item, true)) request.push_back(std::move(*view));
      } else {
        request.push_back(Text(std::get<FormRequest>(item->body).title, TextRole::Title));
      }
    }
    if (!error->empty()) request.push_back(Text(error.Get(), TextRole::Label));
    request_page = Row{Spacer(), Column(std::move(request)).With(Frame{.width = 804.0F}, Spacing(12.0F),
        CrossAlign(CrossAxisAlignment::Stretch)), Spacer()}.With(Grow(), Padding(compact ? 14.0F : 24.0F),
              CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.surface));
  }
  const auto page = review_page ? 1U : request_page ? 2U : 0U;
  return IndexedPages({std::move(chat), review_page ? std::move(*review_page) : View{Spacer()},
      request_page ? std::move(*request_page) : View{Spacer()}}, page).With(Grow());
}

} // namespace huxerui::agentkit
