#include <huxerui/agentkit.h>
#include <huxerui/huxerui.h>
#include <huxerui/markdown.h>

#include "chat_control_icons.h"
#include "json_wire.h"
#include "markdown_selection.h"
#include "runtime_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
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

struct SelectionToolbarAnchor {
  ViewHandle view;
  std::optional<Rect> caret;
  bool pointer_selecting = false;
  bool operator==(const SelectionToolbarAnchor&) const = default;
};

struct SelectedText {
  SourceSlice source;
  std::string text;
  std::vector<SourceSlice> additional_sources;
  std::optional<detail::MarkdownRange> range;
  std::optional<SelectionToolbarAnchor> toolbar_anchor;
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

TextStyle ChatBodyStyle(const ThemeSpec& theme, bool bubbles) {
  const auto size = bubbles ? theme.typography.body_large : theme.typography.body_medium;
  return {Font::FromFamily(theme.typography.family, size), theme.colors.on_surface};
}

[[huxerui::composable]]
View SelectableText(SourceRef source, std::variant<BlockId, PartId> leaf, std::string text,
                    State<std::optional<SelectedText>> selection, std::string key,
                    std::optional<ViewHandle> target = {}, bool fit_content = false) {
  const auto local_target = UseViewHandle();
  const auto anchor = target.value_or(local_target);
  const auto theme = UseTheme();
  const auto style = ChatBodyStyle(theme, UseEnvironment<AgentChatLayoutStyle>().chat_bubbles);
  const TextShapingOptions shaping{.locale = std::string(UseEnvironment<Locale>().LanguageTag())};
  Frame frame;
  if (fit_content) {
    const auto metrics = UseTextMeasurer().MeasureText(std::string_view{text}, style, std::numeric_limits<float>::infinity(),
        {.shaping = shaping, .wrap = TextWrap::NoWrap});
    frame.max_width = std::ceil(metrics.size.width);
  }
  const auto owns_selection = [source, leaf](const std::optional<SelectedText>& selected) {
    return selected && selected->source.source == source &&
        selected->source.text_range && selected->source.text_range->leaf == leaf;
  };
  const auto selected = selection.Get();
  auto highlight = theme.colors.primary;
  highlight.alpha = 0.30F;
  return detail::SelectablePlainText(std::move(text), style, shaping, highlight, anchor,
      owns_selection(selected) ? selected->range : std::nullopt,
      [selection, anchor, source = std::move(source), leaf = std::move(leaf), owns_selection](
          std::optional<detail::MarkdownExcerpt> excerpt) {
        if (!excerpt || excerpt->source.source_bytes.empty()) {
          if (owns_selection(selection.Get())) selection = std::nullopt;
          return;
        }
        const auto [begin, end] = excerpt->source.source_bytes.front();
        selection = SelectedText{.source = {source, {}, SourceTextRange{leaf, begin, end}},
            .text = std::move(excerpt->source.text), .range = excerpt->range,
            .toolbar_anchor = SelectionToolbarAnchor{anchor, excerpt->active_caret, excerpt->pointer_selecting}};
      }).With(frame).Key(std::move(key));
}

StringVariant AuthorLabel(MessageAuthor author, std::string_view agent) {
  switch (author) {
  case MessageAuthor::User: return huxerui_agentkit::strings::ui_you;
  case MessageAuthor::Agent: return agent.empty() ? StringVariant{huxerui_agentkit::strings::ui_assistant} : StringVariant{agent};
  case MessageAuthor::Tool: return huxerui_agentkit::strings::ui_tool;
  case MessageAuthor::Collaboration: return huxerui_agentkit::strings::ui_collaborator;
  case MessageAuthor::System: return huxerui_agentkit::strings::ui_system;
  }
  return {};
}

TextFieldStyle ChatFieldStyle(TextFieldStyle style, const TextStyle& text, Color placeholder) {
  style.show_label = false;
  style.standard.border = Color::Transparent();
  style.standard.hovered_border = Color::Transparent();
  style.standard.focused_border = Color::Transparent();
  style.standard.minimum_height = 0.0F;
  style.text_style = text;
  style.placeholder_style = {text.font, placeholder};
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

View ChatBackAction(const ThemeSpec& theme, std::function<void()> back) {
  auto style = IconButtonStyle::Default();
  style.foreground = theme.colors.on_surface_variant;
  style.icon_size = 18.0F;
  style.minimum_interactive_size = 34.0F;
  style.state_layer_size = 32.0F;
  style.corner_radius = 7.0F;
  style.indication = theme.interactions.indication;
  style.indication->hover.reset();
  style.indication->press.reset();
  style.indication->ripple.reset();
  const auto label = huxerui_agentkit::strings::ui_back_to_conversation;
  return ProvideEnvironment(style, IconButton(huxerui_agentkit::images::arrow_left, label)
      .OnClick(std::move(back)).With(PointerCursor(PointerCursorKind::Hand), Tooltip(label)).Key("back-to-conversation"));
}

bool SubmitOnEnter(const KeyEvent& event, const TextEditingValue& draft, const std::function<void()>& submit) {
  if (event.type != KeyEventType::Down || (event.key != Key::Enter && event.key != Key::NumpadEnter) ||
      event.modifiers.control || event.modifiers.alt || event.modifiers.meta) return false;
  if (draft.composition) return true;
  if (event.modifiers.shift) return false;
  if (!event.repeat) submit();
  return true;
}

[[huxerui::composable]]
View ChatComposerInput(State<TextEditingValue> draft, ViewHandle focus, State<bool> focused,
                       StringVariant placeholder, const char* key, std::function<void()> submit,
                       bool enabled = true, bool dense = false) {
  const auto bubbles = UseEnvironment<AgentChatLayoutStyle>().chat_bubbles;
  const auto theme = UseTheme();
  auto style = ChatFieldStyle(UseEnvironment<TextFieldStyle>(), ChatBodyStyle(theme, bubbles), theme.colors.on_surface_variant);
  return ProvideEnvironment(style, Row{
      TextField(draft).Placeholder(std::move(placeholder)).Label(huxerui_agentkit::strings::ui_message)
          .LineLimits(TextFieldLineLimits::MultiLine(dense ? 1 : 2, 5)).Variant(TextFieldVariant::Standard)
          .OnChanged([draft](const TextEditingValue& value) { draft = value; })
          .On<ViewEvents::FocusChanged>([focused](bool value) { focused = value; })
          .On<ViewEvents::KeyIntercept>([draft, submit](const KeyEvent& event) { return SubmitOnEnter(event, draft.Get(), submit); })
          .OnSubmitted(submit).With(focus, Grow(), Enabled(enabled)).Key(key),
  }.With(Frame{.min_height = dense ? 38.0F : bubbles ? 46.0F : 59.0F},
         Padding(EdgeInsets{.top = 4.0F, .right = 4.0F, .left = 4.0F})).Key("message-input"));
}

[[huxerui::composable]]
View ChatComposerPanel(std::vector<View> content, State<bool> focused, bool bubbles, const ColorScheme& colors) {
  return Column(std::move(content)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Padding(bubbles ? 10.0F : 8.0F), CornerRadius(bubbles ? 15.0F : 11.0F),
      Background(bubbles ? colors.surface_container : colors.surface),
      Shadow{.color = Color::Rgb(34, 25, 55, 0.11F), .offset = {0.0F, 3.0F}, .blur_radius = 9.0F, .spread = -5.0F},
      Border{focused.Get() ? colors.on_primary_container : colors.outline}).Key("agent-chat-composer");
}

std::optional<ModelContextUsage> ComposerContextUsage(const SessionSnapshot& snapshot,
    std::optional<ModelProviderRef> selected_model) {
  if (snapshot.summary.active_run) {
    const auto active = std::ranges::find(snapshot.runs, *snapshot.summary.active_run, &RunSnapshot::id);
    return active == snapshot.runs.end() ? std::nullopt : active->context_usage;
  }
  const auto last = std::ranges::find_if(snapshot.runs.rbegin(), snapshot.runs.rend(),
      [](const auto& run) { return !run.parent; });
  if (last == snapshot.runs.rend() || (selected_model && last->model != *selected_model)) return {};
  return last->context_usage;
}

View ComposerContextIndicator(ModelContextUsage usage, const ColorScheme& colors) {
  const auto label = huxerui_agentkit::strings::ui_composer_context;
  const auto ratio = usage.input_budget_tokens ?
      std::min(1.0, static_cast<double>(usage.estimated_input_tokens) / usage.input_budget_tokens) : 1.0;
  const auto percent = static_cast<unsigned>(std::floor(ratio * 100.0));
  const auto details = usage.before_compaction_tokens
      ? StringVariant::Format(huxerui_agentkit::strings::ui_composer_context_compacted,
            percent, usage.estimated_input_tokens, usage.input_budget_tokens, *usage.before_compaction_tokens)
      : StringVariant::Format(huxerui_agentkit::strings::ui_composer_context_usage,
            percent, usage.estimated_input_tokens, usage.input_budget_tokens);
  const auto ink = ratio >= 0.9 ? colors.primary : colors.on_surface_variant;
  return Canvas([ratio, track = colors.outline, ink](PaintContext& context, Size size) {
    constexpr auto circle = 2.0F * std::numbers::pi_v<float>;
    const Point center{size.width / 2, size.height / 2};
    const StrokeStyle stroke{.width = 1.5F, .cap = StrokeCap::Round};
    context.DrawArc(center, 6.5F, 0, circle, track, stroke);
    if (ratio > 0) context.DrawArc(center, 6.5F, -circle / 4, static_cast<float>(ratio) * circle, ink, stroke);
  }).With(Frame{.width = 28.0F, .height = 32.0F}, Focusable(true), Tooltip(details),
      Semantics{.role = SemanticRole::ProgressIndicator, .label = label, .value = details,
          .range = SemanticRange{.maximum = 100, .current = static_cast<double>(percent)}})
      .Key("composer-context");
}

[[huxerui::composable]]
View ComposerSendControl(StringVariant label, const char* key, std::function<void()> send,
    std::function<bool()> send_available, bool bubbles) {
  const auto colors = UseTheme().colors;
  const bool enabled = send_available();
  if (bubbles) return detail::ChatSendAction(label, key, std::move(send), enabled, colors, UseEnvironment<IconButtonStyle>());
  auto style = UseEnvironment<ButtonStyle>();
  style.background = colors.primary;
  style.label_style = {Font::System(13.0F).WithWeight(FontWeight::SemiBold), colors.on_primary};
  style.disabled_background = colors.surface_container_high;
  style.disabled_label = colors.on_surface_variant;
  style.minimum_height = 34.0F;
  style.padding = EdgeInsets::Symmetric(11.0F, 5.0F);
  style.corner_radii = CornerRadii{6.0F};
  return detail::ChatControlWithIcons(Button(label).OnClick(std::move(send)).With(Enabled(enabled)).Key(key),
      style, enabled ? colors.on_primary : colors.on_surface_variant, detail::ChatControlIcon::Send);
}

struct ChatActivityFeed {
  std::shared_ptr<Conversation> conversation;
  Revision access;
  std::map<RunId, RunSnapshot> runs;
  std::map<ToolCallId, ToolCallSnapshot> tools;
};

using ChatActivityState = State<std::shared_ptr<const ChatActivityFeed>>;

std::function<void()> ObserveChatActivity(TaskScope tasks, std::shared_ptr<Conversation> conversation,
                                            Revision access, ChatActivityState feed) {
  auto initial = conversation ? conversation->Snapshot() : nullptr;
  if (!initial || initial->access_revision != access) return [] {};
  auto state = std::make_shared<ChatActivityFeed>(ChatActivityFeed{conversation, access});
  for (const auto& run : initial->runs) state->runs.emplace(run.id, run);
  for (const auto& tool : initial->tools) state->tools.emplace(tool.id, tool);
  feed = std::move(state);
  auto subscription = std::make_shared<std::optional<Subscription>>();
  auto worker = tasks.Launch([conversation, subscription, feed, access, after = initial->event_sequence]() -> Task<void> {
    auto observed = co_await conversation->ObserveEventsAsync(after, [conversation, feed, access](auto result) {
      const auto& current = feed.Get();
      const auto latest = conversation->Snapshot();
      if (!current || current->conversation != conversation || current->access != access ||
          !latest || latest->access_revision != access) return;
      if (!result.Succeeded()) { feed = std::shared_ptr<const ChatActivityFeed>{}; return; }
      std::shared_ptr<ChatActivityFeed> update;
      for (const auto& event : result.Value()->events) {
        const auto* run = std::get_if<RunSnapshot>(&event.body);
        const auto* tool = std::get_if<ToolCallSnapshot>(&event.body);
        if (!run && !tool) continue;
        if (!update) update = std::make_shared<ChatActivityFeed>(*current);
        if (run) update->runs.insert_or_assign(run->id, *run);
        if (tool) update->tools.insert_or_assign(tool->id, *tool);
      }
      if (update) feed = std::move(update);
    });
    if (observed.Succeeded()) *subscription = std::move(observed).Value();
  });
  return [worker, subscription] { worker.Cancel(); subscription->reset(); };
}

StringResource ActivityStateLabel(RunState state) {
  using namespace huxerui_agentkit::strings;
  switch (state) {
    case RunState::WaitingForInput: return ui_waiting_for_input;
    case RunState::WaitingForReply: return ui_waiting_for_reply;
    case RunState::WaitingForDependency: return ui_waiting_for_dependency;
    case RunState::Cancelling: return ui_stopping;
    case RunState::Succeeded: return ui_completed;
    case RunState::Failed: return ui_failed;
    case RunState::Cancelled: return ui_cancelled;
    case RunState::Running: return ui_assistant_working;
  }
  return ui_assistant_working;
}

StringResource ActivityToolStateLabel(ToolCallState state) {
  using namespace huxerui_agentkit::strings;
  switch (state) {
    case ToolCallState::AwaitingApproval: return ui_awaiting_approval;
    case ToolCallState::Reviewing: return ui_reviewing_tool_request;
    case ToolCallState::Succeeded: return ui_completed;
    case ToolCallState::Denied: return ui_tool_denied;
    case ToolCallState::PartiallyApplied: return ui_tool_partially_applied;
    case ToolCallState::ResultUnknown: return ui_tool_result_unknown;
    case ToolCallState::Conflict:
    case ToolCallState::Failed: return ui_failed;
    case ToolCallState::Cancelled: return ui_cancelled;
    case ToolCallState::Ready:
    case ToolCallState::Preparing:
    case ToolCallState::Executing: return ui_running;
  }
  return ui_running;
}

StringVariant FormatRunTime(StringResource label, Duration elapsed) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::max(elapsed, Duration::zero())).count();
  return StringVariant::Format(label, seconds / 60, seconds % 60);
}

std::string ActivityToolLabel(const ToolRef& tool, const std::function<std::string(const ToolRef&)>& resolve) {
  auto label = resolve ? resolve(tool) : std::string{};
  return label.empty() ? tool.id.value : std::move(label);
}

struct ChatWorkLocation {
  std::size_t row;
  ToolCallId group;
  ToolCallId call;
};

struct ChatTurnActivity {
  RunSnapshot run;
  std::vector<RunSnapshot> delegates;
  std::optional<MessageId> last_message;
  std::optional<ChatWorkLocation> first_change;
  bool waits_for_approval = false;
};

struct ChatToolGroup {
  std::vector<ToolCallSnapshot> calls;
};

struct ChatActivityRows {
  std::map<RunId, ChatTurnActivity> turns;
  std::map<MessageId, RunId> footers;
  std::map<MessageId, std::vector<ChatToolGroup>> groups;
  std::map<ToolCallId, const InteractionSnapshot*> approvals;
  std::set<MessageId> outputs;
  std::vector<std::size_t> history;
  std::set<std::variant<MessageId, ToolCallId, RunId>> assistant_starts;
};

bool ShowsToolOutputInHistory(const ContentBlock& block) {
  const auto* text = std::get_if<TextBlock>(&block.body);
  return !text || text->markdown;
}

const ToolApproval* InlineToolApproval(const InteractionSnapshot& interaction) {
  const auto* approval = std::get_if<ToolApproval>(&interaction.body);
  if (!approval || interaction.state != InteractionState::Pending) return nullptr;
  // New destinations are confirmed in the existing creation or export flow.
  return std::ranges::all_of(approval->preview.targets, [](const auto& target) { return target.expected.has_value(); })
      ? approval : nullptr;
}

ChatActivityRows ActivityRows(const SessionSnapshot& snapshot, const ChatActivityFeed* feed, bool group_tools) {
  ChatActivityRows rows;
  std::map<RunId, RunId> roots;
  for (const auto& visible : snapshot.runs) {
    const auto found = feed ? feed->runs.find(visible.id) : std::map<RunId, RunSnapshot>::const_iterator{};
    const auto& run = feed && found != feed->runs.end() ? found->second : visible;
    if (!run.parent) {
      roots.emplace(run.id, run.id);
      rows.turns.emplace(run.id, ChatTurnActivity{run});
    } else if (const auto parent = roots.find(*run.parent); parent != roots.end()) {
      roots.emplace(run.id, parent->second);
      rows.turns.at(parent->second).delegates.push_back(run);
    }
  }
  for (const auto& interaction : snapshot.interactions) {
    if (!interaction.run) continue;
    const auto root = roots.find(*interaction.run);
    if (root != roots.end() && interaction.state == InteractionState::Pending &&
        (std::holds_alternative<ToolApproval>(interaction.body) || std::holds_alternative<OperationApproval>(interaction.body)))
      rows.turns.at(root->second).waits_for_approval = true;
  }
  std::map<RunId, std::size_t> first_rows;
  for (std::size_t index = 0; index < snapshot.messages.size(); ++index) {
    const auto& message = snapshot.messages[index];
    if (!message.run) continue;
    const auto root = roots.find(*message.run);
    if (root != roots.end()) first_rows.try_emplace(root->second, index);
  }
  for (const auto& tool : snapshot.tools) {
    const auto root = roots.find(tool.run);
    if (tool.registered_sequence && tool.output_message && root != roots.end() && first_rows.contains(root->second))
      rows.outputs.insert(tool.output_message->message);
  }
  const auto visible_message = [&](const Message& message) {
    return !rows.outputs.contains(message.ref.message) || std::ranges::any_of(message.blocks, ShowsToolOutputInHistory);
  };
  std::vector<std::size_t> visible_anchors;
  if (group_tools) {
    visible_anchors.reserve(snapshot.messages.size());
    std::size_t anchor = 0;
    for (std::size_t index = 0; index < snapshot.messages.size(); ++index) {
      if (visible_message(snapshot.messages[index])) anchor = index;
      visible_anchors.push_back(anchor);
    }
  }
  std::set<ToolCallId> placed;
  std::vector<const ToolCallSnapshot*> ordered_tools;
  for (const auto& tool : snapshot.tools) if (tool.registered_sequence) ordered_tools.push_back(&tool);
  std::ranges::stable_sort(ordered_tools, {}, [](const auto* tool) { return *tool->registered_sequence; });
  for (const auto* entry : ordered_tools) {
    const auto& visible = *entry;
    const auto root = roots.find(visible.run);
    if (root == roots.end()) continue;
    const auto first = first_rows.find(root->second);
    if (first == first_rows.end()) continue;
    const auto found = feed ? feed->tools.find(visible.id) : std::map<ToolCallId, ToolCallSnapshot>::const_iterator{};
    auto tool = feed && found != feed->tools.end() ? found->second : visible;
    tool.registered_sequence = visible.registered_sequence;
    tool.output_message = visible.output_message;
    auto index = first->second;
    const auto next = std::ranges::upper_bound(snapshot.messages, *tool.registered_sequence, {}, &Message::sequence);
    if (next != snapshot.messages.begin()) index = static_cast<std::size_t>(next - snapshot.messages.begin() - 1);
    if (group_tools && visible_anchors[index] >= first->second) index = visible_anchors[index];
    const auto& anchor = snapshot.messages[index].ref.message;
    auto& groups = rows.groups[anchor];
    if (!group_tools || groups.empty() || groups.back().calls.front().run != tool.run) groups.emplace_back();
    auto& calls = groups.back().calls;
    calls.push_back(tool);
    placed.insert(tool.id);
    if (std::ranges::any_of(tool.effects, [](const auto& effect) { return effect.state == ResourceEffectState::Applied; })) {
      auto& first_change = rows.turns.at(root->second).first_change;
      if (!first_change || index < first_change->row) first_change = ChatWorkLocation{index, calls.front().id, tool.id};
    }
  }
  for (const auto& interaction : snapshot.interactions) {
    if (const auto* approval = InlineToolApproval(interaction); approval && placed.contains(approval->call))
      rows.approvals.emplace(approval->call, &interaction);
  }
  for (std::size_t index = 0; index < snapshot.messages.size(); ++index) {
    const auto& message = snapshot.messages[index];
    if (!visible_message(message) && !rows.groups.contains(message.ref.message)) continue;
    rows.history.push_back(index);
    if (!message.run) continue;
    const auto root = roots.find(*message.run);
    if (root == roots.end()) continue;
    rows.turns.at(root->second).last_message = message.ref.message;
  }
  for (const auto& [id, turn] : rows.turns) {
    if (turn.last_message) rows.footers.emplace(*turn.last_message, id);
  }
  std::optional<RunId> assistant_run;
  const auto begin_assistant = [&](const std::optional<RunId>& run, std::variant<MessageId, ToolCallId, RunId> entry) {
    if (!run || run != assistant_run) rows.assistant_starts.insert(std::move(entry));
    assistant_run = run;
  };
  for (const auto index : rows.history) {
    const auto& message = snapshot.messages[index];
    if (message.author == MessageAuthor::Agent || (message.author == MessageAuthor::Tool && message.run && visible_message(message))) {
      begin_assistant(message.run, message.ref.message);
    } else if (message.author != MessageAuthor::Tool) {
      assistant_run.reset();
    }
    if (const auto groups = rows.groups.find(message.ref.message); groups != rows.groups.end()) {
      for (const auto& group : groups->second) begin_assistant(group.calls.front().run, group.calls.front().id);
    }
    if (const auto footer = rows.footers.find(message.ref.message); footer != rows.footers.end())
      begin_assistant(footer->second, footer->second);
  }
  return rows;
}

std::optional<std::size_t> HistoryIndex(const ChatActivityRows* activity, std::size_t message, ScrollAlignment alignment) {
  if (!activity) return message;
  const auto& history = activity->history;
  if (history.empty()) return {};
  auto position = std::ranges::lower_bound(history, message);
  if (position == history.end() ||
      (alignment == ScrollAlignment::End && *position != message && position != history.begin())) --position;
  return static_cast<std::size_t>(position - history.begin());
}

Indication ActivityDisclosureIndication(const ThemeSpec& theme) {
  auto indication = theme.interactions.indication;
  indication.ripple.reset();
  return indication;
}

[[huxerui::composable]]
View ChatActivityHeader(StringVariant status, std::optional<StringVariant> time = {},
                        bool active = true, std::optional<View> details = {}) {
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto label = Font::FromFamily(theme.typography.family, theme.typography.body_small);
  const auto dot = active ? colors.primary : colors.on_surface_variant;
  std::vector<View> labels{
      Canvas([dot](PaintContext& context, Size size) { context.DrawRect({0, 0, size.width, size.height}, dot); })
          .With(Frame{.width = 6.0F, .height = 6.0F}, CornerRadius(3.0F)),
  };
  if (!UseEnvironment<AgentChatLayoutStyle>().chat_bubbles)
    labels.push_back(Text(huxerui_agentkit::strings::ui_assistant, TextRole::Label)
        .Style({label.WithWeight(FontWeight::SemiBold), colors.on_surface}));
  labels.push_back(Text(std::move(status), TextRole::Label)
      .Style({label, colors.on_surface_variant}).Key("chat-assistant-status"));
  if (time) labels.push_back(Text(std::move(*time), TextRole::Label)
      .Style({label, colors.on_surface_variant}).Key("chat-run-time"));
  if (details) labels.push_back(std::move(*details));
  return Flow(std::move(labels)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View ChatRunActivity(RunSnapshot run, std::vector<RunSnapshot> delegates, bool waits_for_approval,
                    std::function<void()> keep_reading, std::function<void()> show_changes) {
  using namespace huxerui_agentkit::strings;
  const auto expanded = UseState(std::optional<bool>{});
  const auto show_reasoning = UseState(false);
  const auto tasks = UseTaskScope();
  const auto clock = UseState(std::chrono::time_point_cast<Duration>(std::chrono::system_clock::now()));
  const bool finished = detail::Terminal(run.state);
  const bool tick = run.timing && !finished;
  Lifecycle([tasks, clock, tick] {
    if (!tick) return std::function<void()>{[] {}};
    auto timer = tasks.Launch([clock]() -> Task<void> {
      for (;;) {
        clock = std::chrono::time_point_cast<Duration>(std::chrono::system_clock::now());
        co_await Delay(Duration{1000});
      }
    });
    return std::function<void()>{[timer] { timer.Cancel(); }};
  }, tick);
  auto status = ActivityStateLabel(run.state);
  if (run.state == RunState::WaitingForInput && waits_for_approval) status = ui_awaiting_approval;
  if (run.state == RunState::Running && run.model_active) status = ui_generating_reply;
  std::optional<StringVariant> time;
  auto waiting = Duration::zero();
  if (run.timing) {
    const auto end = run.timing->ended_at.value_or(clock.Get());
    time = FormatRunTime(ui_run_elapsed, end - run.timing->started_at);
    waiting = run.timing->waiting_for_input;
    if (run.timing->input_wait_started_at)
      waiting += std::max(Duration::zero(), end - *run.timing->input_wait_started_at);
  }
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto style = ChatActionStyle(UseEnvironment<ButtonStyle>(), theme);
  const auto toggle = [style, &colors, keep_reading, indication = ActivityDisclosureIndication(theme)](
      StringResource title, std::string key, bool open, std::function<void()> change) {
    auto activate = [keep_reading, change = std::move(change)] { keep_reading(); change(); };
    return Row{
        Image(open ? huxerui_agentkit::images::chevron_down : huxerui_agentkit::images::chevron_right)
            .Tint(colors.on_surface_variant).With(Frame{.width = 13.0F, .height = 13.0F}),
        Text(title, TextRole::Label).Style(style.label_style),
    }.OnClick(activate).On<ViewEvents::KeyDown>([activate](const KeyEvent& event) {
      if (event.key != Key::Enter && event.key != Key::Space) return false;
      if (!event.repeat) activate();
      return true;
    }).With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Center), Frame{.min_height = 24.0F},
        Padding(EdgeInsets::Symmetric(4.0F, 0.0F)),
        indication, Focusable{}, Semantics{.role = SemanticRole::Button, .label = title, .expanded = open}).Key(std::move(key));
  };
  const bool show_wait = run.state != RunState::WaitingForInput && waiting >= Duration{1000};
  const bool has_work = !delegates.empty() || !run.reasoning_summary.empty() || show_wait;
  const bool open = expanded.Get().value_or(false);
  std::optional<View> disclosure;
  if (has_work) disclosure = toggle(ui_work_activity, "chat-run-activity", open, [expanded, open] { expanded = !open; });
  if (finished && show_changes) {
    auto changes = ProvideEnvironment(style, Button(ui_view_run_changes).OnClick(std::move(show_changes)));
    disclosure = disclosure ? View{Row {std::move(*disclosure), std::move(changes)}.With(Spacing(5.0F))} : std::move(changes);
  }
  std::vector<View> content{ChatActivityHeader(status, std::move(time), !finished, std::move(disclosure))};
  if (has_work && open) {
    std::vector<View> entries;
    const TextStyle note{Font::System(13.0F), colors.on_surface_variant};
    if (!run.reasoning_summary.empty()) {
      const bool showing = show_reasoning.Get();
      entries.push_back(toggle(ui_reasoning_summary, "chat-reasoning-toggle", showing, [show_reasoning, showing] {
        show_reasoning = !showing;
      }));
      if (showing) entries.push_back(Text(run.reasoning_summary).Style(note)
          .With(Padding(9.0F), CornerRadius(8.0F), Background(colors.surface_container_low)).Key("chat-reasoning-summary"));
    }
    for (const auto& child : delegates)
      entries.push_back(Text(child.agent.id.value + " · " + UseString(ActivityStateLabel(child.state)), TextRole::Label)
          .Style(note).Key(child.id.value));
    if (show_wait) entries.push_back(Text(FormatRunTime(ui_run_input_wait, waiting), TextRole::Label).Style(note));
    content.push_back(Row{
        Canvas([line = colors.outline](PaintContext& context, Size size) {
          context.DrawRect({0, 0, size.width, size.height}, line);
        }).With(Frame{.width = 1.0F}),
        Column(std::move(entries)).With(Grow(), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    }.With(Spacing(11.0F), CrossAlign(CrossAxisAlignment::Stretch), Padding(EdgeInsets{.left = 3.0F})));
  }
  return Column(std::move(content)).With(Spacing(5.0F), Padding(EdgeInsets{.top = 5.0F, .bottom = 8.0F, .left = 3.0F}),
      CrossAlign(CrossAxisAlignment::Stretch)).Key("chat-turn/" + run.id.value);
}

class MarkdownBlockLayout final : public Layout<MarkdownBlockLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    auto& body = node.ChildAt(0);
    // Intrinsic and stretch passes must agree on the width used to measure the same Markdown blocks.
    const auto fixed = constraints.HasBoundedWidth() ? constraints.TightWidth(constraints.max_width) : constraints;
    return LayoutResult{}.SetSize(context.Measure(body, fixed)).Place(body, {});
  }
};

[[huxerui::composable]]
View MarkdownMessageBlock(MessageRef message, BlockId block, std::string source, bool complete, bool can_reference,
                          State<std::optional<SelectedText>> selection,
                          std::shared_ptr<const AgentChatLinkHandler> links, bool fill_width) {
  const auto theme = UseTheme();
  const bool bubbles = UseEnvironment<AgentChatLayoutStyle>().chat_bubbles;
  const auto body_style = ChatBodyStyle(theme, bubbles);
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
    return SelectableText(message, block, source, selection, "markdown-selection/" + block.value, source_focus);
  };
  if (!document || detail::IsPlainMarkdown(*document, source)) {
    if (can_select) return source_field();
    return Text(source).Style(body_style);
  }

  auto style = markdown::MarkdownStyle::FromTheme(theme);
  style.body = body_style;
  if (!bubbles) style.body.font = body_style.font.WithSize(15.0F);
  style.code = {Font::Monospace(13.0F), theme.colors.on_surface};
  style.heading_sizes = bubbles ? std::array{24.0F, 21.0F, 18.0F, 17.0F, 16.0F, 15.0F}
                                : std::array{27.0F, 23.0F, 21.0F, 19.0F, 18.0F, 17.0F};
  style.block_spacing = 12.0F;
  style.list_spacing = bubbles ? 4.0F : 6.0F;
  style.indent = 19.0F;
  style.code_background = theme.colors.surface_container_low;
  style.quote_background = theme.colors.surface_container_low;
  style.table_header_background = theme.colors.surface_container_high;
  style.code_padding = 12.0F;
  style.corner_radius = 9.0F;
  style.blank_line_height = 12.0F;
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
    if (can_select) changed = [message, block, selection, owns_selection, source_focus](std::optional<detail::MarkdownExcerpt> excerpt) {
      if (!excerpt || excerpt->source.source_bytes.empty()) {
        if (owns_selection(selection.Get())) selection = std::nullopt;
        return;
      }
      auto& source_excerpt = excerpt->source;
      const auto [begin, end] = source_excerpt.source_bytes.front();
      SelectedText selected{{message, {}, SourceTextRange{block, begin, end}}, std::move(source_excerpt.text),
                            {}, excerpt->range, SelectionToolbarAnchor{source_focus, excerpt->active_caret, excerpt->pointer_selecting}};
      for (const auto& [first, last] : std::views::drop(source_excerpt.source_bytes, 1)) {
        selected.additional_sources.push_back({message, {}, SourceTextRange{block, first, last}});
      }
      selection = std::move(selected);
    };
    auto highlight = theme.colors.primary;
    highlight.alpha = 0.30F;
    const auto retained = owns_selection(selection.Get()) ? selection.Get()->range : std::nullopt;
    detail::MarkdownImageLoad load;
    if (complete && images && images->load) load = [images, message, block](std::string destination) {
      return images->load({message, block, std::move(destination)});
    };
    content.push_back(detail::SelectableMarkdown(*document, std::move(options), highlight, source_focus, retained, std::move(changed),
        std::move(activate), std::move(load), "markdown/" + message.message.value + "/" + block.value)
        .Key("markdown-selection/" + block.value));
    if (!link_error.Get().empty()) content.push_back(Text(link_error.Get(), TextRole::Label)
        .Style({Font::System(12.0F), theme.colors.error}).Key("markdown-link-error"));
  }
  if (can_select && !detail::HasMarkdownSourceSelection) {
    auto mode = Button(select_source ? huxerui_agentkit::strings::ui_show_formatted_message : huxerui_agentkit::strings::ui_select_source_text)
        .OnClick([source_mode, selection, message, block] {
          if (const auto selected = selection.Get(); selected && selected->source.source == SourceRef{message} &&
              selected->source.text_range && selected->source.text_range->leaf == std::variant<BlockId, PartId>{block}) {
            selection = std::nullopt;
          }
          source_mode = !source_mode.Get();
        }).Key("markdown-source-toggle");
    content.push_back(Row{ProvideEnvironment(action_style, std::move(mode)), Spacer()});
  }
  auto body = Column(std::move(content)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
  if (fill_width) return MarkdownBlockLayout{std::vector<View>{std::move(body)}};
  return body;
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
  const auto indicator = Image(huxerui_agentkit::images::open_review).Tint(colors.on_primary_container)
      .With(Frame{.width = 16.0F, .height = 16.0F});
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
                      .hint = hint, .identifier = "artifact-review/" + key}).Key(std::move(key));
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

constexpr float chat_avatar_size = 26.0F;
constexpr float chat_avatar_gap = 7.0F;

View ChatAvatar(MessageAuthor author, const std::string& agent, const ColorScheme& colors, std::optional<Image> model = {}) {
  const bool user = author == MessageAuthor::User;
  View icon = model
      ? std::move(*model).Key("chat-model-avatar")
      : View{Image(user ? huxerui_agentkit::images::person : huxerui_agentkit::images::assistant)
            .Tint(user ? colors.on_surface_variant : colors.on_primary_container)};
  return Stack{std::move(icon)}.With(Frame{.width = chat_avatar_size, .height = chat_avatar_size},
      Padding(model ? 5.0F : 0.0F), CornerRadius(8.0F),
      Background(user || model ? colors.surface_container_high : colors.primary_container),
      Semantics{.role = SemanticRole::Image, .label = AuthorLabel(author, agent)},
      LayoutPriority(1.0F)).Key(user ? "chat-user-avatar" : "chat-agent-avatar");
}

View AssistantChatLine(View body, std::optional<View> avatar, bool narrow) {
  View leading = avatar ? std::move(*avatar) : View{Spacer().With(Frame{.width = chat_avatar_size}, Grow(0.0F))};
  return Row{std::move(leading), std::move(body).With(Grow())}.With(Spacing(chat_avatar_gap),
      Padding(EdgeInsets{.right = narrow ? 14.0F : 40.0F}));
}

View MessageView(const Message& message, const std::shared_ptr<Conversation>& conversation,
                 const OpenArtifactReview& open_artifact,
                 State<std::optional<SelectedText>> selection, bool selectable, std::optional<SteerState> steer,
                 std::optional<View> recover, std::optional<View> selection_actions, std::string agent,
                 const ThemeSpec& theme,
                 Revision access_revision, const std::shared_ptr<const AgentChatLinkHandler>& links,
                 const std::optional<InlineArtifactSummary>& inline_summary,
                 const decltype(AgentChatOptions::render_block)& render_block, std::optional<View> assistant_avatar,
                 bool bubbles, bool narrow) {
  const auto& colors = theme.colors;
  const bool user = message.author == MessageAuthor::User;
  const bool agent_reply = message.author == MessageAuthor::Agent;
  const bool bubble = bubbles && (user || agent_reply);
  const bool artifact_delivery = bubbles && message.author == MessageAuthor::Tool && !message.blocks.empty() &&
      std::ranges::all_of(message.blocks, [](const ContentBlock& block) { return std::holds_alternative<ArtifactBlock>(block.body); });
  std::vector<View> content;
  if (!bubble && !artifact_delivery) content.push_back(Text(AuthorLabel(message.author, agent))
      .Style({Font::System(12.0F).WithWeight(FontWeight::SemiBold), colors.on_surface_variant}));
  if (steer) {
    auto status = Text(*steer == SteerState::Pending ? huxerui_agentkit::strings::ui_waiting_to_apply :
        *steer == SteerState::Applied ? huxerui_agentkit::strings::ui_applied_to_this_run : huxerui_agentkit::strings::ui_not_applied, TextRole::Label);
    if (recover) content.push_back(Row{std::move(status), std::move(*recover)}
        .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)));
    else content.push_back(std::move(status));
  }
  const auto selected = selection_actions ? selection.Get() : std::nullopt;
  for (const auto& block : message.blocks) {
    if (auto rendered = render_block ? render_block(message, block, conversation) : std::nullopt) {
      content.push_back(std::move(*rendered).Key(block.id.value));
      continue;
    }
    if (const auto* text = std::get_if<TextBlock>(&block.body)) {
      if (text->markdown) {
        content.push_back(MarkdownMessageBlock(message.ref, block.id, text->text, message.state == MessageState::Complete,
            selectable, selection, links, !user).Key(block.id.value));
      } else if (selectable && message.state == MessageState::Complete && !text->text.empty()) {
        content.push_back(SelectableText(message.ref, block.id, text->text, selection, block.id.value, {}, bubble && user));
      } else {
        content.push_back(Text(text->text).Style(ChatBodyStyle(theme, bubbles)).Key(block.id.value));
      }
      if (selected && selected->source.text_range &&
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
      content.push_back(Text(moved->handoff_summary ? StringVariant{*moved->handoff_summary} :
          StringVariant{huxerui_agentkit::strings::ui_assistant_changed}).Key(block.id.value));
    } else if (const auto* custom = std::get_if<CustomBlock>(&block.body)) {
      content.push_back(Text(custom->fallback).Key(block.id.value));
    } else if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body)) {
      content.push_back(Text(conclusion->text, TextRole::Label).Key(block.id.value));
    }
  }
  View body = Column(std::move(content)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
  if (bubbles) body = std::move(body).Key("message-body");
  if (bubble) {
    auto tile = user
        ? std::move(body).With(Padding(EdgeInsets::Symmetric(14.0F, 9.0F)),
            CornerRadius(13.0F), Background(colors.primary_container)).Key("chat-user-bubble")
        : std::move(body).With(Padding(EdgeInsets{.top = 2.0F, .bottom = 3.0F})).Key("chat-agent-bubble");
    if (user) return Row{std::move(tile), ChatAvatar(message.author, agent, colors)}
        .With(MainAlign(MainAxisAlignment::End), Spacing(chat_avatar_gap), Padding(EdgeInsets{.left = narrow ? 30.0F : 64.0F}));
    return AssistantChatLine(std::move(tile), std::move(assistant_avatar), narrow);
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
                     AgentChatApprovalRef approval, StringVariant deny, StringVariant approve,
                     const ButtonStyle& secondary_style, const ColorScheme& colors) {
  auto submit = [tasks, conversation = std::move(conversation), error, approval](ApprovalDecision choice) {
    tasks.Launch([conversation, error, approval, choice]() -> Task<void> {
      auto reply = co_await conversation->SubmitAsync(NewCommandId(), {approval.id, approval.version, choice});
      error = reply.Succeeded() ? "" : reply.Error().message;
    });
  };
  auto secondary = secondary_style;
  secondary.padding = EdgeInsets::Symmetric(12.0F, 5.0F);
  secondary.corner_radii = CornerRadii{7.0F};
  auto primary = secondary;
  primary.background = colors.primary_container;
  primary.label_style.foreground = colors.on_primary_container;
  return Row{Spacer(), Flow{
      ProvideEnvironment(secondary, Button(std::move(deny)).OnClick([submit] { submit(ApprovalDecision::Deny); })
          .With(Border{colors.outline})),
      ProvideEnvironment(primary, Button(std::move(approve)).OnClick([submit] { submit(ApprovalDecision::Approve); })),
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
    details.push_back(ApprovalActions(std::move(conversation), tasks, error, approval,
        huxerui_agentkit::strings::ui_deny_action, huxerui_agentkit::strings::ui_approve_action, action_style, colors)
        .With(LayoutPriority(2.0F)));
  }
  return Column(std::move(details)).With(Spacing(8.0F), Padding(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Background(colors.surface), CornerRadius(8.0F), Border{colors.outline});
}

View ReviewedApproval(const SessionSnapshot& snapshot, ArtifactRef artifact, AgentChatApprovalRef approval,
                      const std::shared_ptr<Conversation>& conversation, TaskScope tasks, State<std::string> error,
                      bool loading, const ButtonStyle& action_style, const ColorScheme& colors) {
  const auto item = std::ranges::find(snapshot.interactions, approval.id, &InteractionSnapshot::id);
  if (item == snapshot.interactions.end()) return Text(huxerui_agentkit::strings::ui_approval_unavailable, TextRole::Label);
  switch (item->state) {
    case InteractionState::Approved: return Text(huxerui_agentkit::strings::ui_change_approved, TextRole::Label);
    case InteractionState::Denied: return Text(huxerui_agentkit::strings::ui_change_denied, TextRole::Label);
    case InteractionState::Expired: return Text(huxerui_agentkit::strings::ui_approval_expired, TextRole::Label);
    case InteractionState::Cancelled: return Text(huxerui_agentkit::strings::ui_approval_cancelled, TextRole::Label);
    case InteractionState::Invalidated: return Text(huxerui_agentkit::strings::ui_approval_invalidated, TextRole::Label);
    case InteractionState::Submitted: return Text(huxerui_agentkit::strings::ui_response_submitted, TextRole::Label);
    case InteractionState::Pending: break;
  }
  const auto* plan = ApprovalDetails(*item);
  if (item->version != approval.version || !plan || std::ranges::find(plan->artifacts, artifact) == plan->artifacts.end()) {
    return Text(huxerui_agentkit::strings::ui_approval_changed_open_the_current_request_to, TextRole::Label);
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
      ApprovalActions(conversation, tasks, error, approval, huxerui_agentkit::strings::ui_deny_reviewed_change,
          huxerui_agentkit::strings::ui_approve_reviewed_change, action_style, colors).With(Enabled(!loading), LayoutPriority(2.0F)),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

struct ChatDisclosureAnchor {
  detail::ChatReadingPosition position;
  float top;
};

using ChatDisclosureRequest = std::shared_ptr<std::optional<ChatDisclosureAnchor>>;

struct ChatDisclosureGeometry {
  class Extension;
  std::shared_ptr<std::optional<float>> top;
};

class ChatDisclosureGeometry::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const ChatDisclosureGeometry& config) : top_(config.top) {}

  void Update(ViewNode&, const ChatDisclosureGeometry& config) { top_ = config.top; }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    const auto bounds = node.PresentationBounds();
    if (bounds.IsEmpty()) top_->reset();
    else *top_ = bounds.y;
    return PaintInvalidation::None;
  }

private:
  std::shared_ptr<std::optional<float>> top_;
};

View WorkDisclosure(View heading, std::string key, std::function<void()> toggle,
                   std::function<void()> keep_reading, ChatDisclosureRequest request) {
  auto top = std::make_shared<std::optional<float>>();
  const detail::ChatReadingPosition anchor{key};
  const auto activate = [request, top, anchor, keep_reading = std::move(keep_reading), toggle = std::move(toggle)] {
    keep_reading();
    if (*top) *request = ChatDisclosureAnchor{anchor, **top};
    toggle();
  };
  return std::move(heading).OnClick(activate).On<ViewEvents::KeyDown>([activate](const KeyEvent& event) {
    if (event.key != Key::Enter && event.key != Key::Space) return false;
    if (!event.repeat) activate();
    return true;
  }).With(ChatDisclosureGeometry{std::move(top)}, Focusable{}, PointerCursor(PointerCursorKind::Hand))
      .LayoutValue<detail::ChatReadingPositionKey>(anchor).Key(std::move(key));
}

[[huxerui::composable]]
View ChatToolStep(ToolCallSnapshot call, std::string label, std::shared_ptr<const Message> anchor,
                  std::shared_ptr<const Message> output,
                  std::shared_ptr<Conversation> conversation, decltype(AgentChatOptions::render_block) render,
                  State<std::set<ToolCallId>> opened, std::function<void()> keep_reading, ChatDisclosureRequest disclosure,
                  std::optional<View> approval, bool grouped) {
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto label_font = Font::FromFamily(theme.typography.family, theme.typography.body_small);
  const auto status = UseString(ActivityToolStateLabel(call.state));
  const auto text = label + " · " + status;
  const bool expanded = opened->contains(call.id);
  const bool can_expand = output || !call.effects.empty() || call.reason || !call.progress.empty();
  const auto toggle = [opened, id = call.id] {
    auto next = opened.Get();
    if (!next.erase(id)) next.insert(id);
    opened = std::move(next);
  };
  const TextBlock* summary = output && !output->blocks.empty() ? std::get_if<TextBlock>(&output->blocks.front().body) : nullptr;
  if (summary && (summary->text.size() > 280 || summary->markdown ||
      (summary->text.find('\n') != std::string::npos && call.state != ToolCallState::PartiallyApplied))) summary = nullptr;
  std::vector<View> header;
  if (can_expand || approval) {
    header.push_back(Image(expanded || approval ? huxerui_agentkit::images::chevron_down : huxerui_agentkit::images::chevron_right)
        .Tint(colors.on_surface_variant).With(Frame{.width = 12.0F, .height = 12.0F}));
  } else {
    header.push_back(Spacer().With(Frame{.width = 12.0F}, Grow(0.0F)));
  }
  header.push_back(Text(text, TextRole::Label).Style({label_font, colors.on_surface_variant}));
  if (summary && !grouped) header.push_back(Text(summary->text, TextRole::Label)
      .Style({label_font, colors.on_surface_variant}).With(Grow()));
  const auto applied = std::ranges::count(call.effects, ResourceEffectState::Applied, &ResourceEffect::state);
  if (applied) header.push_back(Text(StringVariant::Format(huxerui_agentkit::strings::ui_tool_changes_applied, applied),
      TextRole::Label).Style({label_font, colors.on_surface_variant}));
  View heading = Flow(std::move(header)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center),
      Frame{.min_height = 22.0F}, Padding(EdgeInsets::Symmetric(0.0F, 1.0F)));
  const auto key = "chat-tool-toggle/" + call.id.value;
  if (can_expand && !approval) {
    heading = WorkDisclosure(std::move(heading).With(ActivityDisclosureIndication(theme),
        Semantics{.role = SemanticRole::Button, .label = text, .expanded = expanded}), key, toggle,
        std::move(keep_reading), std::move(disclosure));
  }
  std::vector<View> body {std::move(heading).Key(key)};
  if (approval) body.push_back(std::move(*approval));
  if (expanded) {
    std::vector<View> details;
    auto custom = render ? render(output ? *output : *anchor, ContentBlock{{call.id.value}, ToolBlock{call.id}}, conversation)
                         : std::nullopt;
    if (custom) {
      details.push_back(std::move(*custom));
    } else if (output) {
      for (const auto& block : output->blocks) {
        const auto* content = std::get_if<TextBlock>(&block.body);
        if (!content || content->markdown) continue;
        auto view = render ? render(*output, block, conversation) : std::nullopt;
        if (!view) {
          auto text = Text(content->text).Style({Font::System(13.0F), colors.on_surface_variant});
          view = content->text.size() > 600 ? View{ScrollView(std::move(text)).With(Frame{.max_height = 180.0F}, ScrollBar())}
                                           : View{std::move(text)};
        }
        details.push_back(std::move(*view).Key(block.id.value));
      }
    }
    if (call.reason && (!summary || call.reason->message != summary->text))
      details.push_back(Text(call.reason->message, TextRole::Label).Style({label_font, colors.error}));
    else if (!call.progress.empty())
      details.push_back(Text(call.progress, TextRole::Label).Style({label_font, colors.on_surface_variant}));
    if (!details.empty()) body.push_back(Column(std::move(details)).With(Spacing(6.0F),
        Padding(EdgeInsets{.bottom = 6.0F, .left = 22.0F}), CrossAlign(CrossAxisAlignment::Stretch)));
  }
  return Column(std::move(body)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Padding(EdgeInsets{.bottom = 4.0F}))
      .Key("chat-tool/" + call.id.value);
}

[[huxerui::composable]]
View ChatToolGroupView(ChatToolGroup group, std::function<std::string(const ToolRef&)> tool_label,
    std::function<View(const ToolCallSnapshot&)> render_step, std::vector<View> approvals,
    State<std::set<ToolCallId>> opened, std::function<void()> keep_reading, ChatDisclosureRequest disclosure) {
  using namespace huxerui_agentkit::strings;
  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto id = group.calls.front().id;
  const bool expanded = opened->contains(id);
  const auto count = group.calls.size();
  const auto label = count == 1 ? UseString(ui_one_work_operation) : UseString(StringVariant::Format(ui_work_operations, count));
  const auto active = std::ranges::find_if(group.calls.rbegin(), group.calls.rend(), [](const auto& call) {
    return call.state == ToolCallState::Preparing || call.state == ToolCallState::Reviewing ||
           call.state == ToolCallState::Ready || call.state == ToolCallState::Executing;
  });
  const bool running = active != group.calls.rend();
  const bool waiting = std::ranges::any_of(group.calls, [](const auto& call) { return call.state == ToolCallState::AwaitingApproval; });
  const bool needs_check = std::ranges::any_of(group.calls, [](const auto& call) {
    return call.state == ToolCallState::ResultUnknown || call.state == ToolCallState::PartiallyApplied;
  });
  const auto failures = std::ranges::count_if(group.calls, [](const auto& call) {
    return call.state == ToolCallState::Failed || call.state == ToolCallState::Denied || call.state == ToolCallState::Conflict;
  });
  const auto toggle = [opened, id] {
    auto next = opened.Get();
    if (!next.erase(id)) next.insert(id);
    opened = std::move(next);
  };
  const auto font = Font::FromFamily(theme.typography.family, theme.typography.body_small);
  std::vector<View> heading{
      Image(expanded ? huxerui_agentkit::images::chevron_down : huxerui_agentkit::images::chevron_right)
          .Tint(colors.on_surface_variant).With(Frame{.width = 12.0F, .height = 12.0F}),
      Text(label).Style({font.WithWeight(FontWeight::Medium), colors.on_surface_variant}),
  };
  std::string status;
  if (needs_check) status = UseString(ui_tool_result_unknown);
  else if (waiting) status = UseString(ui_awaiting_approval);
  else if (running) status = UseString(StringVariant::Format(ui_running_tool, ActivityToolLabel(active->tool, tool_label)));
  auto accessible_label = label;
  if (!status.empty()) {
    accessible_label += " · " + status;
    heading.push_back(Text("·").Style({font, colors.on_surface_variant}));
    heading.push_back(Text(status).Style({font, needs_check ? colors.error : colors.on_surface_variant})
        .LineLimit(1).Overflow(TextOverflow::Ellipsis));
  }
  if (failures) {
    const auto failed = UseString(StringVariant::Format(ui_work_failures, failures));
    accessible_label += " · " + failed;
    heading.push_back(Text("·").Style({font, colors.on_surface_variant}));
    heading.push_back(Text(failed).Style({font, colors.error}));
  }
  if (running) {
    auto progress = UseEnvironment<ProgressCircleStyle>();
    progress.size = 12.0F;
    progress.stroke_width = 1.5F;
    progress.indicator_color = colors.primary;
    heading.push_back(ProvideEnvironment(progress, ProgressCircle().Key("chat-work-progress")));
  }
  auto header = Row(std::move(heading)).With(Spacing(7.0F), CrossAlign(CrossAxisAlignment::Center),
      Frame{.min_height = 27.0F}, Padding(EdgeInsets::Symmetric(2.0F, 1.0F)), ActivityDisclosureIndication(theme),
      Semantics{.role = SemanticRole::Button, .label = std::move(accessible_label), .expanded = expanded});
  std::vector<View> rows{WorkDisclosure(std::move(header), "chat-work-toggle/" + id.value, toggle,
      std::move(keep_reading), std::move(disclosure))};
  if (expanded) {
    std::vector<View> steps;
    steps.reserve(group.calls.size());
    for (const auto& call : group.calls) steps.push_back(render_step(call));
    rows.push_back(Column(std::move(steps)).With(Padding(EdgeInsets{.left = 19.0F}),
        Spacing(0.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key("chat-work-steps"));
  }
  for (auto& approval : approvals) rows.push_back(std::move(approval));
  return Column(std::move(rows)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Padding(EdgeInsets{.bottom = 6.0F})).Key("chat-work-group/" + id.value);
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
      TextField(response).Label(huxerui_agentkit::strings::ui_json_response).OnChanged([response](const TextEditingValue& value) { response = value; }),
      Row{Button(huxerui_agentkit::strings::ui_dismiss_request).OnClick([submit] { submit(true); }),
          Button(huxerui_agentkit::strings::ui_submit_response).OnClick([submit] { submit(false); }).With(Enabled(!response->text.empty()))}.With(Spacing(8.0F)),
  }.With(Spacing(10.0F));
}

StringVariant SelectionLabel(const SourceSlice& slice, bool multiple) {
  if (const auto* message = std::get_if<MessageRef>(&slice.source)) {
    return StringVariant::Format(multiple ? huxerui_agentkit::strings::ui_message_v_0 :
        huxerui_agentkit::strings::ui_selected_message_v_0, message->version);
  }
  if (const auto* artifact = std::get_if<ArtifactRef>(&slice.source)) {
    const auto* part = slice.text_range ? std::get_if<PartId>(&slice.text_range->leaf) : nullptr;
    if (!part && slice.parts.size() == 1) part = &slice.parts.front();
    if (part) return StringVariant::Format(huxerui_agentkit::strings::ui_artifact_part, artifact->version, part->value);
    return StringVariant::Format(huxerui_agentkit::strings::ui_artifact_v_0, artifact->version);
  }
  return huxerui_agentkit::strings::ui_selected_text;
}

struct SelectionOrigin {
  std::shared_ptr<Conversation> conversation;
  Revision access_revision;

  bool HasAccess() const {
    const auto current = conversation->Snapshot();
    return current && current->access_revision == access_revision;
  }
};

struct SideChatRequest {
  std::shared_ptr<AgentRuntime> runtime;
  AgentId agent;
  SelectionOrigin origin;
  std::vector<SelectionPassage> passages;
  std::shared_ptr<detail::SideChatDraft> side;
  State<AgentChatDraftSources> draft;
  std::function<void()> restore_origin;
  std::function<void()> on_citation_added;
};

struct SideChatReturn {
  std::shared_ptr<const std::function<void(AgentChatCitation)>> action;
  static SideChatReturn Default() { return {}; }

  bool operator==(const SideChatReturn&) const = default;
};

[[huxerui::composable]]
View SideChatContent(std::shared_ptr<const SideChatRequest> request, ViewHandle question_focus,
                     std::function<void()> close, std::function<void()> restore_origin) {
  const auto tasks = UseApplicationTaskScope();
  const auto observation = UseTaskScope();
  const auto source_available = UseState(true);
  const auto& origin = request->origin;
  const auto selection_visible = [origin, source_available] { return source_available.Get() && origin.HasAccess(); };
  const auto& side = request->side;
  const auto question = side->input;
  const auto pending = side->pending;
  const auto error = side->error;
  const auto opened = side->conversation.Get();
  const auto returned = UseState(false);
  const bool compact = UseViewportClass() == ViewportClass::Compact;
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
  Lifecycle([tasks, returned, restore_origin = std::move(restore_origin), on_citation_added = request->on_citation_added] {
    return [tasks, returned, restore_origin, on_citation_added] {
      if (returned.Get() && on_citation_added) tasks.Post(on_citation_added);
      else if (!returned.Get() && restore_origin) restore_origin();
    };
  });
  Lifecycle([opened, question_focus, available = selection_visible()] {
    if (available && opened) question_focus.RequestFocus(true);
  }, static_cast<bool>(opened), source_available.Get());
  const auto submit = [tasks, runtime = request->runtime, agent = request->agent, side, question, pending, error, selection_visible] {
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
      Text(huxerui_agentkit::strings::ui_side_chat, TextRole::Title).Style({Font::System(16.0F).WithWeight(FontWeight::SemiBold), colors.on_surface}),
      Spacer(),
      ProvideEnvironment(close_style, Button(huxerui_agentkit::strings::ui_close).OnClick(close)),
  }.With(CrossAlign(CrossAxisAlignment::Center), Frame{.min_height = 44.0F}, Padding(EdgeInsets::Symmetric(15.0F, 0.0F)));
  const bool available = selection_visible();
  const auto& passages = request->passages;
  std::vector<View> body;
  if (available) {
    std::vector<View> fragments;
    for (const auto& passage : passages) {
      if (passages.size() > 1) fragments.push_back(Text(SelectionLabel(side->sources[passage.first_slice], true))
          .Style({Font::System(12.0F), colors.on_surface_variant}));
      fragments.push_back(Text(passage.text).Style({Font::System(13.0F), colors.on_surface_variant}));
    }
    auto label = passages.size() == 1 ? SelectionLabel(side->sources.front(), false) :
        StringVariant::Format(huxerui_agentkit::strings::ui_0_selected_passages, passages.size());
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
    body.push_back(Text(huxerui_agentkit::strings::ui_selection_access_changed_close_this_side_chat));
  } else if (opened) {
    AgentChatOptions options;
    options.draft = question;
    options.composer_focus = question_focus;
    const auto bring_back = [draft = request->draft, close, returned, selection_visible](AgentChatCitation reply) {
      if (!selection_visible()) return;
      auto sources = draft.Get();
      sources.conclusion = std::move(reply);
      draft = std::move(sources);
      returned = true;
      close();
    };
    auto chat = ProvideEnvironment(SideChatReturn{std::make_shared<const std::function<void(AgentChatCitation)>>(bring_back)},
        AgentChat(opened, std::move(options)).With(Grow()));
    const float height = compact ? 248.0F : 304.0F;
    body.push_back(Column{std::move(chat)}.With(Frame{.height = height, .max_height = height},
        CrossAlign(CrossAxisAlignment::Stretch), ClipChildren{}));
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
        Text(huxerui_agentkit::strings::ui_question).Style({Font::System(12.0F), colors.on_surface_variant}),
        ProvideEnvironment(style, TextField(question).Label(huxerui_agentkit::strings::ui_question).Placeholder(huxerui_agentkit::strings::ui_ask_about_this_selection)
            .LineLimits(TextFieldLineLimits::MultiLine(2, 4)).Variant(TextFieldVariant::Outlined)
            .OnChanged([question](const TextEditingValue& value) { question = value; })
            .On<ViewEvents::KeyIntercept>([question, submit](const KeyEvent& event) {
              return SubmitOnEnter(event, question.Get(), submit);
            })
            .OnSubmitted(submit).With(question_focus, Enabled(!pending.Get())).Key("side-chat-question")),
    }.With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch)));
    if (!error.Get().empty()) body.push_back(Text(error.Get(), TextRole::Label));
    body.push_back(Row{Spacer(),
        Button(huxerui_agentkit::strings::ui_ask).OnClick(submit).With(Enabled(!pending.Get() && !question->text.empty() && !question->composition))}
        .With(CrossAlign(CrossAxisAlignment::Center)));
  }
  View content = Column(std::move(body)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Padding(EdgeInsets{.top = 13.0F, .right = 15.0F, .bottom = 16.0F, .left = 15.0F}));
  auto panel = Column{std::move(header), Divider(), std::move(content)}
      .With(CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface));
  return std::move(panel).With(Frame{.width = 440.0F}, CornerRadius(12.0F), Border{colors.outline}).Key("side-chat-dialog");
}

void OpenSideChat(const DialogHandle& dialogs, std::shared_ptr<AgentChatSelection> service,
                  SelectionOrigin origin, MessageSelection selected, State<AgentChatDraftSources> draft,
                  ViewHandle question_focus, std::function<void()> restore_origin, std::function<void()> on_citation_added) {
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
  SideChatRequest request{service->runtime, service->assistant, std::move(origin), std::move(selected.passages),
      *found, draft, std::move(restore_origin), std::move(on_citation_added)};
  dialogs.Show(DialogFactory{[request = std::make_shared<const SideChatRequest>(std::move(request)), question_focus](DialogContext dialog) {
    return SideChatContent(request, question_focus, [dialog] { dialog.Dismiss(); }, request->restore_origin);
  }}, {.initial_focus = question_focus});
}

[[huxerui::composable]]
View MessageSelectionToolbar(SourceSlice source, bool ranging, State<std::optional<SourceSlice>> focus_request,
                             State<bool> busy, State<std::string> error, std::function<void(bool)> use,
                             std::function<void()> extend) {
  const auto action_focus = UseViewHandle();
  const auto theme = UseTheme();
  const auto style = ChatActionStyle(UseEnvironment<ButtonStyle>(), theme);
  Lifecycle([focus_request, action_focus, source] {
    if (focus_request.Get() != source) return;
    focus_request = std::nullopt;
    action_focus.RequestFocus(true);
  }, focus_request.Get(), source);
  auto row = ProvideEnvironment(style, Row{
      Button(huxerui_agentkit::strings::ui_side_chat).OnClick([use] { use(true); }).With(action_focus, Enabled(!busy.Get())),
      Button(huxerui_agentkit::strings::ui_quote).OnClick([use] { use(false); }).With(Enabled(!busy.Get())),
      Button(ranging ? huxerui_agentkit::strings::ui_cancel_range : huxerui_agentkit::strings::ui_extend_selection).OnClick(std::move(extend)).With(Enabled(!busy.Get())),
  }.With(Spacing(4.0F)));
  View toolbar = error.Get().empty() ? std::move(row) : View(Column{std::move(row), Text(error.Get(), TextRole::Label)}.With(Spacing(3.0F)));
  return std::move(toolbar).Key("message-selection-actions");
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
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
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
                  focus_request, range_anchor, anchor, busy, error, excerpts = std::move(excerpts), visible, ask,
                  on_citation_added]() mutable -> Task<void> {
      auto text = co_await origin.conversation->ReadSelectionsAsync(excerpts.slices);
      busy = false;
      if (!text.Succeeded()) { error = text.Error().message; co_return; }
      if (!visible() || !origin.HasAccess()) { error = "Selection access changed"; co_return; }
      if (ask) {
        OpenSideChat(dialogs, service, std::move(origin), std::move(excerpts), draft, question_focus,
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
  const auto extend = [range_anchor, anchor, endpoint] {
    range_anchor = anchor ? std::nullopt : std::optional{endpoint};
  };
  return MessageSelectionToolbar(selected.source, anchor.has_value(), focus_request, busy, error, use, extend);
}

} // namespace

struct AgentChatTextSelection::Data {
  std::shared_ptr<Conversation> conversation;
  State<std::shared_ptr<const SessionSnapshot>> snapshot;
  std::shared_ptr<const SessionSnapshot> projected;
  Revision access;
  SelectedText text;
  State<std::optional<SelectedText>> selection;
  State<std::optional<SourceSlice>> range;
  std::optional<SourceSlice> start;

  bool IsVisible() const {
    const auto current = snapshot.Get();
    const auto authorized = conversation->Snapshot();
    return current && authorized && current->summary.id == authorized->summary.id &&
        current->access_revision == access && authorized->access_revision == access &&
        std::ranges::any_of(current->messages, [&](const Message& message) {
          return message.state == MessageState::Complete && SourceRef{message.ref} == text.source.source;
        });
  }

  MessageSelection Excerpts() const {
    if (start) return MessageRange(projected->messages, *start, SelectionBounds(text));
    MessageSelection excerpts{{text.source}, {{0, text.text}}};
    excerpts.slices.insert(excerpts.slices.end(), text.additional_sources.begin(), text.additional_sources.end());
    return excerpts;
  }
};

struct detail::ChatTextSelectionAccess {
  static std::shared_ptr<const AgentChatTextSelection> Bind(std::shared_ptr<Conversation> conversation,
      State<std::shared_ptr<const SessionSnapshot>> snapshot, Revision access, SelectedText text,
      State<std::optional<SelectedText>> selection, State<std::optional<SourceSlice>> range) {
    auto data = std::make_shared<AgentChatTextSelection::Data>(AgentChatTextSelection::Data{
        std::move(conversation), snapshot, snapshot.Get(), access, std::move(text), selection, range, range.Get()});
    return std::shared_ptr<const AgentChatTextSelection>(new AgentChatTextSelection(std::move(data)));
  }
};

AgentChatTextSelection::AgentChatTextSelection(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

ViewHandle AgentChatTextSelection::Anchor() const { return data_->text.toolbar_anchor->view; }

std::optional<Rect> AgentChatTextSelection::Caret() const { return data_->text.toolbar_anchor->caret; }

bool AgentChatTextSelection::IsExtending() const { return data_->start.has_value(); }

Async<AgentChatCitation> AgentChatTextSelection::ReadAsync() const {
  return [](std::shared_ptr<const Data> data) -> Async<AgentChatCitation> {
    if (!data->IsVisible()) co_return detail::Fail<AgentChatCitation>(ErrorCode::Conflict, "Selection access changed");
    auto excerpts = data->Excerpts();
    if (excerpts.slices.empty()) co_return detail::Fail<AgentChatCitation>(ErrorCode::InvalidArgument,
        "The selected message range could not be resolved");
    const auto checked = co_await data->conversation->ReadSelectionsAsync(excerpts.slices);
    if (!checked.Succeeded()) co_return AgentResult<AgentChatCitation>::Failure(checked.Error());
    if (!data->IsVisible()) co_return detail::Fail<AgentChatCitation>(ErrorCode::Conflict, "Selection access changed");
    co_return AgentResult<AgentChatCitation>::Success(GatherCitation(std::move(excerpts)));
  }(data_);
}

std::vector<std::string> AgentChatTextSelection::Passages() const {
  std::vector<std::string> result;
  if (data_->IsVisible())
    for (auto& passage : data_->Excerpts().passages) result.push_back(std::move(passage.text));
  return result;
}

void AgentChatTextSelection::Extend() const {
  if (data_->IsVisible())
    data_->range = data_->start ? std::nullopt : std::optional{SelectionBounds(data_->text)};
}

void AgentChatTextSelection::Clear() const {
  if (!data_->IsVisible()) return;
  const auto& selected = data_->selection.Get();
  if (!selected || SelectionBounds(*selected) == SelectionBounds(data_->text)) data_->selection = std::nullopt;
  if (data_->range.Get() == data_->start) data_->range = std::nullopt;
}

void AgentChatTextSelection::Restore() const {
  if (!data_->IsVisible()) return;
  data_->selection = data_->text;
  data_->range = data_->start;
}

[[huxerui::composable]]
View AgentChatAttachmentChips(State<AgentChatDraftSources> draft) {
  const auto theme = UseTheme();
  const auto action_style = ChatActionStyle(UseEnvironment<ButtonStyle>(), theme);
  std::vector<View> chips;
  for (const auto& item : draft->attachments) {
    chips.push_back(Row{
        Text(item->name, TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow()),
        ProvideEnvironment(action_style, Button(huxerui_agentkit::strings::ui_remove).OnClick([draft, item] {
          auto editing = draft.Get();
          std::erase(editing.attachments, item);
          draft = std::move(editing);
        }).With(Semantics{.label = StringVariant::Format(huxerui_agentkit::strings::ui_remove_0, item->name)}).Key("remove-attachment/" + item->id.value)),
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
        OpenSideChat(dialogs, service, origin, {{source}, {{0, std::move(text).Value()}}}, draft, question_focus, {}, cited);
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
        Button(huxerui_agentkit::strings::ui_side_chat).OnClick([use, source = *source] { use(source, true); }).With(Enabled(!busy.Get())),
        Button(huxerui_agentkit::strings::ui_quote).OnClick([use, source = *source] { use(source, false); }).With(Enabled(!busy.Get())),
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
      ScrollView(SelectableText(artifact, part, std::move(text), selection, "artifact-text"))
          .With(Grow(), ScrollBar()),
      // Keep the read alive when an action takes focus from the selected text.
      AgentChatSourceActions(conversation,
          selected && !selected->text.empty() ? std::optional{selected->source} : std::nullopt, draft),
  }.With(Grow(), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

struct ChatHistoryScroll {
  class Extension;
  State<bool> paused;
  ScrollController controller;
  TaskScope tasks;
  SessionId session;
  ChatDisclosureRequest disclosure;
};

struct HistoryRowIndexKey {
  using Value = std::size_t;
};

class ChatHistoryScroll::Extension final : public NodeExtension {
public:
  Extension(ViewNode&, const ChatHistoryScroll& config) : config_(config) {}

  void Update(ViewNode&, const ChatHistoryScroll& config) {
    if (config_.session != config.session || config_.controller != config.controller) {
      reading_.reset();
      width_ = 0.0F;
      pending_->reset();
      reflow_ = Reflow::Idle;
    }
    config_ = config;
  }

  void OnScrollActivity(ViewNode&, const ScrollActivity& activity) override {
    const bool direct = activity.source == ScrollSource::Drag || activity.source == ScrollSource::Wheel ||
        activity.source == ScrollSource::Scrollbar || activity.source == ScrollSource::Momentum ||
        activity.source == ScrollSource::Accessibility;
    if (!direct) return;
    config_.disclosure->reset();
    pending_->reset();
    reflow_ = Reflow::Idle;
    if (activity.delta > 0.0F && activity.metrics.maximum_offset - activity.metrics.offset <= 24.0F && config_.paused.Get())
      config_.paused = false;
  }

  PaintInvalidation PrepareGeometry(ViewNode& node, TextMeasurer&) override {
    const auto viewport = node.PresentationBounds();
    if (viewport.IsEmpty()) return PaintInvalidation::None;
    const auto metrics = config_.controller.Metrics();
    if (const auto request = std::exchange(*config_.disclosure, std::nullopt)) {
      if (const auto position = FindReadingPosition(node, viewport, request->position)) {
        if (MoveTo(metrics, metrics.offset + viewport.y + position->top - request->top))
          return PaintInvalidation::None;
      }
    }
    const bool resized = width_ > 0.0F && std::abs(width_ - viewport.width) > 0.25F;
    width_ = viewport.width;
    if (resized && at_end_) {
      MoveTo(metrics, metrics.maximum_offset);
      return PaintInvalidation::None;
    }
    if (reading_ && (resized || reflow_ == Reflow::MountingRow)) {
      if (const auto position = FindReadingPosition(node, viewport, reading_->position)) {
        reflow_ = Reflow::Idle;
        MoveTo(metrics, metrics.offset + position->top - reading_->top);
        return PaintInvalidation::None;
      }
      if (resized) {
        // Reflow of preceding messages can move the reading row outside the realized range.
        const auto row_offset = VirtualList::ScrollOffsetForItem(node, reading_->row, ScrollAlignment::Start, metrics.viewport_extent);
        if (row_offset && MoveTo(metrics, *row_offset)) reflow_ = Reflow::MountingRow;
      }
      if (reflow_ == Reflow::MountingRow && *pending_) return PaintInvalidation::None;
      reflow_ = Reflow::Idle;
    }
    if (!*pending_) {
      at_end_ = metrics.maximum_offset - metrics.offset <= 1.0F;
      reading_ = at_end_ ? std::nullopt : FindReadingPosition(node, viewport, std::nullopt);
    }
    return PaintInvalidation::None;
  }

private:
  enum class Reflow { Idle, MountingRow };

  struct Reading {
    detail::ChatReadingPosition position;
    float top;
    std::size_t row;
  };

  static std::optional<Reading> FindReadingPosition(const ViewNode& root, Rect viewport,
      const std::optional<detail::ChatReadingPosition>& target) {
    std::optional<Reading> best;
    float distance = std::numeric_limits<float>::infinity();
    const float sample = viewport.y + std::min(viewport.height, 12.0F);
    const auto visit = [&](auto&& self, const ViewNode& node, std::size_t row) -> bool {
      const auto bounds = node.PresentationBounds();
      const auto bottom = bounds.y + bounds.height;
      if (!target && (bottom <= viewport.y || bounds.y > sample + 24.0F)) return false;
      if (!bounds.IsEmpty()) {
        if (const auto* position = node.LayoutValue<detail::ChatReadingPositionKey>()) {
          if (target && *position == *target) {
            best = Reading{*position, bounds.y - viewport.y, row};
            return true;
          }
          const float gap = std::max({bounds.y - sample, sample - bottom, 0.0F});
          // Prefer a source-backed paragraph to the enclosing message.
          const float score = position->owner.starts_with("markdown/") ? gap : gap + 25.0F;
          if (!target && score < distance) {
            best = Reading{*position, bounds.y - viewport.y, row};
            distance = score;
          }
        }
      }
      for (std::size_t i = 0; i < node.ChildCount(); ++i) {
        if (self(self, node.ChildAt(i), row)) return true;
      }
      return false;
    };
    for (std::size_t i = 0; i < root.ChildCount(); ++i) {
      const auto& child = root.ChildAt(i);
      if (const auto* row = child.LayoutValue<HistoryRowIndexKey>(); row && visit(visit, child, *row)) break;
    }
    return best;
  }

  bool MoveTo(const ScrollMetrics& metrics, float target) {
    target = std::clamp(target, 0.0F, metrics.maximum_offset);
    if (std::abs(metrics.offset - target) <= 0.5F) {
      pending_->reset();
      return false;
    }
    const bool scheduled = pending_->has_value();
    *pending_ = std::pair{metrics.offset, target};
    if (scheduled) return true;
    config_.tasks.Post([request = std::weak_ptr(pending_), controller = config_.controller] {
      if (const auto pending = request.lock()) {
        const auto correction = std::exchange(*pending, std::nullopt);
        if (correction && std::abs(controller.Offset() - correction->first) <= 0.5F)
          controller.ScrollTo(correction->second);
      }
    });
    return true;
  }

  ChatHistoryScroll config_;
  std::optional<Reading> reading_;
  std::shared_ptr<std::optional<std::pair<float, float>>> pending_ =
      std::make_shared<std::optional<std::pair<float, float>>>();
  float width_ = 0.0F;
  bool at_end_ = false;
  Reflow reflow_ = Reflow::Idle;
};

static View ChatHistoryViewport(View history, ScrollController controller, std::size_t count, State<bool> follow_paused,
                                State<std::optional<SelectedText>> selection, bool clear_selection,
                                IconButtonStyle style, const ThemeSpec& theme) {
  const auto viewport = controller.Metrics();
  View jump = Spacer();
  if (count && viewport.maximum_offset - viewport.offset > 24.0F) {
    style.foreground = theme.colors.on_surface_variant;
    style.icon_size = 16.0F;
    style.minimum_interactive_size = 34.0F;
    style.state_layer_size = 32.0F;
    jump = Row{ProvideEnvironment(style, IconButton(huxerui_agentkit::images::arrow_down, huxerui_agentkit::strings::ui_jump_to_latest)
        .OnClick([controller, count, follow_paused] {
          follow_paused = false;
          controller.ScrollToItem(count - 1, ScrollAlignment::End);
        })
        .With(Tooltip(huxerui_agentkit::strings::ui_jump_to_latest), CornerRadius(17.0F), Background(theme.colors.surface_container_high),
            Border{theme.colors.outline}).Key("jump-to-latest"))}.With(Padding(EdgeInsets{.bottom = 8.0F}));
  }
  auto view = Stack{std::move(history), std::move(jump)}.With(Grow(), Align(HorizontalAlignment::Center, VerticalAlignment::End));
  if (clear_selection) return std::move(view).On<ViewEvents::PointerIntercept>([selection](const PointerEvent& event) {
    if (event.type == PointerEventType::Down && !event.modifiers.shift && selection.Get()) selection = std::nullopt;
    return false;
  });
  return view;
}

enum class ChatPart { Full, Messages, Composer };

[[huxerui::composable]]
static View ConversationChatView(std::shared_ptr<Conversation> conversation, AgentChatOptions options, ChatPart part) {
  const auto tasks = UseTaskScope();
  const auto snapshot = UseState(conversation ? conversation->Snapshot() : std::shared_ptr<const SessionSnapshot>{});
  const auto activity_feed = UseState(std::shared_ptr<const ChatActivityFeed>{});
  const auto opened_steps = UseState(std::set<ToolCallId>{});
  const auto opened_groups = UseState(std::set<ToolCallId>{});
  const auto disclosure_state = UseState(std::make_shared<std::optional<ChatDisclosureAnchor>>());
  const auto disclosure = disclosure_state.Get();
  const auto history_owner = UseState(conversation);
  const auto history_window = UseState(std::optional<SessionHistoryWindow>{});
  const auto history_loading = UseState(false);
  const auto history_initialized = UseState(false);
  const auto history_controller = UseScrollController();
  struct ScrollRequest {
    Sequence sequence;
    ScrollAlignment alignment;
    std::optional<InteractionId> approval;
    ScrollMetrics before_approval;

    bool operator==(const ScrollRequest&) const = default;
  };
  const auto history_scroll = UseState(std::optional<ScrollRequest>{});
  const auto follow_paused = UseState(false);
  const auto keep_reading = [history_controller, history_scroll, follow_paused] {
    follow_paused = true;
    history_scroll = std::nullopt;
    history_controller.ScrollTo(history_controller.Offset());
  };
  const auto follow_sent_message = UseState(std::optional<Sequence>{});
  const auto local_draft = UseState(TextEditingValue::FromText({}));
  const auto draft = options.draft.value_or(local_draft);
  const auto artifact_review = UseState(std::shared_ptr<const ArtifactPreview>{});
  const auto review_loading = UseState(options.review_only && options.review_request.has_value());
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
  const auto side_service = OptionalChatService<AgentChatSelection>();
  const auto links = OptionalChatService<AgentChatLinkHandler>();
  const auto send_menu = UseMenu();
  const auto send_mode = UseViewHandle();
  const auto composer_focused = UseState(false);
  const auto theme = UseTheme();
  const auto layout_style = UseEnvironment<AgentChatLayoutStyle>();
  const auto avatar_for = layout_style.chat_bubbles ? OptionalChatService<AgentChatModelAvatar>() : nullptr;
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
  const auto observed_quote = draft_sources->quote;
  const auto observed_conclusion = draft_sources->conclusion;
  Lifecycle([close_review, reviewed = artifact_review.Get(), review_only = options.review_only,
      focus_cited_draft, observed_quote, observed_conclusion] {
    if (reviewed && !review_only && (observed_quote || observed_conclusion)) {
      close_review();
      focus_cited_draft();
    }
  }, observed_quote, observed_conclusion);

  Lifecycle([conversation, history_owner, history_window, history_loading, history_initialized, history_scroll, follow_paused, follow_sent_message, snapshot,
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
    follow_paused = false;
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

  const bool review_only = options.review_only;
  const bool track_history = part != ChatPart::Composer;
  const auto requested_history = review_only && options.review_request && options.review_request->history_first
      ? std::optional{SessionHistoryWindow{*options.review_request->history_first}}
      : history_owner.Get() == conversation ? history_window.Get() : std::nullopt;
  const auto activity_access = track_history && layout_style.show_run_activity && !review_only && snapshot.Get()
      ? std::optional{snapshot.Get()->access_revision} : std::nullopt;
  Lifecycle([tasks, conversation, activity_feed, activity_access, opened_steps, opened_groups, disclosure] {
    opened_steps = std::set<ToolCallId>{};
    opened_groups = std::set<ToolCallId>{};
    disclosure->reset();
    if (!activity_access) return std::function<void()>{[] {}};
    return ObserveChatActivity(tasks, conversation, *activity_access, activity_feed);
  }, conversation, activity_access);
  Lifecycle([conversation, tasks, snapshot, error, artifact_review, review_ticket, review_loading, focused_interaction,
      history_owner, history_window, history_loading, history_initialized, history_controller, history_scroll, follow_paused, follow_sent_message,
      requested_history, review_only, track_history, inline_activity = layout_style.show_run_activity] {
    auto subscription = std::make_shared<std::optional<Subscription>>();
    if (!conversation) return std::function<void()>{[] {}};
    auto project = [conversation, snapshot, error, artifact_review, review_ticket, review_loading, focused_interaction,
        history_owner, history_window, history_loading, history_initialized, history_controller, history_scroll, follow_paused, follow_sent_message,
        requested_history, review_only, track_history, inline_activity](auto result) {
      if (history_owner.Get() != conversation || (!review_only && history_window.Get() != requested_history)) return;
      if (result.Succeeded()) {
        auto incoming = std::move(result).Value();
        const auto previous = snapshot.Get();
        const bool was_loading = history_loading.Get();
        const bool first = !history_initialized.Get();
        const auto viewport = history_controller.Metrics();
        history_loading = false;
        history_initialized = true;
        if (track_history && !review_only && !incoming->messages.empty()) {
          const auto pending = follow_sent_message.Get();
          const bool sent = pending && std::ranges::any_of(incoming->messages, [pending](const Message& message) {
            return message.author == MessageAuthor::User && message.sequence > *pending;
          });
          if (sent) {
            follow_sent_message = std::nullopt;
            follow_paused = false;
          }
          if (was_loading && requested_history) {
            const auto start = std::ranges::lower_bound(incoming->messages, requested_history->first, {}, &Message::sequence);
            if (start != incoming->messages.end()) history_scroll = ScrollRequest{start->sequence, ScrollAlignment::Start};
          } else if (!artifact_review.Get() && !review_loading.Get() && !focused_interaction.Get()) {
            const auto approval = std::ranges::find_if(incoming->interactions.rbegin(), incoming->interactions.rend(),
                [&](const auto& item) {
                  return inline_activity && InlineToolApproval(item) &&
                      (!previous || std::ranges::none_of(previous->interactions, [&](const auto& old) {
                        return old.id == item.id && old.version == item.version && old.state == InteractionState::Pending;
                      }));
                });
            if (approval != incoming->interactions.rend()) {
              const auto call = std::ranges::find(incoming->tools, std::get<ToolApproval>(approval->body).call, &ToolCallSnapshot::id);
              if (call != incoming->tools.end() && call->registered_sequence) {
                const auto after = std::ranges::upper_bound(incoming->messages, *call->registered_sequence, {}, &Message::sequence);
                const auto anchor = after == incoming->messages.begin() ? after : std::prev(after);
                history_scroll = ScrollRequest{anchor->sequence, ScrollAlignment::End, approval->id, viewport};
              }
            } else if (first || sent || (!follow_paused.Get() && (!history_scroll.Get() || !history_scroll.Get()->approval) &&
                                        (!previous || previous->through_sequence != incoming->through_sequence) &&
                                        viewport.viewport_extent > 0.0F && viewport.maximum_offset - viewport.offset <= 24.0F)) {
              history_scroll = ScrollRequest{incoming->messages.back().sequence, ScrollAlignment::End};
            }
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
  const bool reset_send_as_new = draft_sources->send_as_new_task && vacant();
  Lifecycle([reset_send_as_new, draft_sources] {
    if (!reset_send_as_new || !draft_sources->send_as_new_task) return;
    auto sources = draft_sources.Get();
    sources.send_as_new_task = false;
    draft_sources = std::move(sources);
  }, reset_send_as_new);
  const auto current = history_owner.Get() == conversation ? snapshot.Get() : std::shared_ptr<const SessionSnapshot>{};
  if (!current && !options.review_only) return Text(error->empty() ? StringVariant{huxerui_agentkit::strings::ui_opening_conversation} : StringVariant{error.Get()});
  const auto detached_status = [&](StringVariant message) -> View {
    return Column{Text(std::move(message), TextRole::Label)}
        .With(Grow(), Padding(24.0F), Spacing(12.0F), Background(theme.colors.surface));
  };
  if (!current) return detached_status(error->empty() ? StringVariant{huxerui_agentkit::strings::ui_opening_conversation} : StringVariant{error.Get()});
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
  const auto send_available = [sending, has_input, draft, recovered] {
    return !sending.Get() && has_input() && !draft->composition &&
        std::ranges::all_of(recovered.Get(), [](const auto& entry) { return entry.reference.has_value(); });
  };
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

  const auto bring_back = UseEnvironment<SideChatReturn>().action;
  const bool selection_enabled = options.selection.has_value() ||
      (part == ChatPart::Full && (side_chat ? static_cast<bool>(bring_back) : static_cast<bool>(side_service)));
  std::optional<SelectedText> selected = selection.Get();
  if (selected && (!selection_enabled || selected->text.empty() ||
      std::ranges::none_of(current->messages, [&](const Message& message) {
        return SourceRef{message.ref} == selected->source.source && message.state == MessageState::Complete &&
            (!side_chat || message.author == MessageAuthor::Agent);
      }))) {
    selected.reset();
  }
  const bool ready_selection = selected && selected->toolbar_anchor && !selected->toolbar_anchor->pointer_selecting &&
      range_anchor.Get() != SelectionBounds(*selected);
  Lifecycle([binding = options.selection, conversation, snapshot, access = current->access_revision, selection, range_anchor,
      presented = ready_selection ? selected : std::nullopt] {
    if (binding) *binding = presented ? detail::ChatTextSelectionAccess::Bind(conversation, snapshot, access, *presented,
        selection, range_anchor) : std::shared_ptr<const AgentChatTextSelection>{};
  }, options.selection.has_value(), conversation, current->access_revision, range_anchor.Get(),
     ready_selection ? std::optional{SelectionBounds(*selected)} : std::nullopt,
     ready_selection ? selected->toolbar_anchor : std::nullopt,
     ready_selection ? selected->range : std::nullopt);
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
            summary_scroll = i;
            break;
          }
        }
        auto summary = options.render_artifact_summary ? options.render_artifact_summary(*reviewed, conversation) : std::nullopt;
        if (!summary) summary = ScrollView(generic_preview()).With(Frame{.max_height = 180.0F}, ScrollBar());
        std::vector<View> header;
        if (!position) header.push_back(Text(reviewed->name + " v" + std::to_string(reviewed->artifact.version), TextRole::Label));
        header.push_back(Spacer());
        header.push_back(ProvideEnvironment(action_style, Button(huxerui_agentkit::strings::ui_close_preview).OnClick(return_to_chat)));
        auto details = Column{Row(std::move(header)), std::move(*summary).Key(
            reviewed->artifact.id.value + "/" + std::to_string(reviewed->artifact.version))}.With(Spacing(4.0F),
            CrossAlign(CrossAxisAlignment::Stretch), Padding(EdgeInsets{.right = 12.0F, .bottom = 10.0F, .left = 12.0F}),
            Border{theme.colors.outline}, CornerRadius(8.0F)).Key("artifact-inline-summary");
        if (position) inline_summary = InlineArtifactSummary{*position, reviewed->artifact, std::move(details)};
        else unanchored_summary = std::move(details);
      }
    }
  }
  const auto feed = activity_feed.Get();
  const auto activity = activity_access ? std::make_shared<const ChatActivityRows>(ActivityRows(*current,
      feed && feed->conversation == conversation && feed->access == *activity_access ? feed.get() : nullptr,
      layout_style.chat_bubbles)) : nullptr;
  const auto count = (activity ? activity->history.size() : current->messages.size()) + earlier;
  const auto request = history_scroll.Get();
  std::optional<std::size_t> scroll_item;
  if (request && !current->messages.empty()) {
    const auto position = std::ranges::lower_bound(current->messages, request->sequence, {}, &Message::sequence);
    const auto message = position == current->messages.end() ? current->messages.size() - 1 :
        static_cast<std::size_t>(position - current->messages.begin());
    scroll_item = HistoryIndex(activity.get(), message, request->alignment);
  }
  const auto scroll_metrics = history_controller.Metrics();
  const bool approval_pending = !request || !request->approval || std::ranges::any_of(current->interactions, [&](const auto& item) {
    return item.id == *request->approval && InlineToolApproval(item);
  });
  const bool waiting_for_approval_layout = request && request->approval &&
      scroll_metrics.content_extent == request->before_approval.content_extent &&
      scroll_metrics.viewport_extent == request->before_approval.viewport_extent;
  Lifecycle([history_scroll, history_controller, request, scroll_item, earlier, approval_pending, waiting_for_approval_layout] {
    if (!request) return;
    if (approval_pending && waiting_for_approval_layout) return;
    history_scroll = std::nullopt;
    if (scroll_item && approval_pending) history_controller.ScrollToItem(*scroll_item + earlier, request->alignment);
  }, request, scroll_item, earlier, approval_pending, waiting_for_approval_layout);
  const auto summary_item = summary_scroll ? HistoryIndex(activity.get(), *summary_scroll, ScrollAlignment::Start) : std::nullopt;
  Lifecycle([history_controller, summary_item, earlier] {
    if (summary_item) history_controller.ScrollToItem(*summary_item + earlier, ScrollAlignment::Start);
  }, review_ticket.Get(), summary_item, earlier);

  auto history = VirtualList(count,
      [conversation, open_artifact, selection, selection_enabled, side_chat, current, snapshot, range_anchor, earlier,
       opened_steps, opened_groups, disclosure, tasks, error, history_controller, keep_reading, focused_request = focused_interaction.Get(),
       attachment_label = options.review_attachment_label,
       history_loading, history_window, loading = history_loading.Get(), theme, action_style, restore,
       vacant, recovering,
       agent_label = options.agent_label, avatar_for, links, selected, draft_sources, selection_focus, focus_cited_draft,
       inline_summary = std::move(inline_summary), render_block = options.render_block,
       activity, host_selection = options.selection.has_value(), tool_label = options.tool_label, bubbles = layout_style.chat_bubbles, narrow = compact || side_chat](std::size_t index) -> View {
    if (earlier && index == 0) {
      return ProvideEnvironment(action_style, Button(loading ? huxerui_agentkit::strings::ui_loading_earlier_messages : huxerui_agentkit::strings::ui_load_earlier_messages)
          .OnClick([history_window, history_loading, boundary = current->messages.front().sequence] {
            if (history_loading.Get()) return;
            history_loading = true;
            history_window = SessionHistoryWindow{boundary, 50};
          }).With(Enabled(!loading)).Key("earlier-messages"));
    }
    const auto& message = current->messages[activity ? activity->history[index - earlier] : index - earlier];
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
              Button(huxerui_agentkit::strings::ui_restore_draft).OnClick([restore, command = *found->recovery] { restore(command); })
                  .With(Enabled(vacant() && !recovering.Get())).Key("restore-supplement"));
        }
        if (agent_label && message.author == MessageAuthor::Agent) agent = agent_label(run->agent);
      }
    }
    std::optional<View> selection_actions;
    if (!host_selection && !side_chat && selected && selected->source.source == SourceRef{message.ref} &&
        range_anchor.Get() != SelectionBounds(*selected) && (!selected->toolbar_anchor || !selected->toolbar_anchor->pointer_selecting)) {
      selection_actions = MessageSelectionActions(conversation, snapshot, *selected, selection, range_anchor, draft_sources, selection_focus,
          focus_cited_draft);
    }
    const auto assistant_avatar = [&](const auto& entry, const std::optional<RunId>& run_id) -> std::optional<View> {
      if (!bubbles || (activity && !activity->assistant_starts.contains(entry))) return std::nullopt;
      const auto run = run_id ? std::ranges::find(current->runs, *run_id, &RunSnapshot::id) : current->runs.end();
      if (run == current->runs.end()) return ChatAvatar(MessageAuthor::Agent, {}, theme.colors);
      return ChatAvatar(MessageAuthor::Agent, agent_label ? agent_label(run->agent) : std::string{}, theme.colors,
          avatar_for ? (*avatar_for)(run->model, theme.colors.on_surface) : std::nullopt);
    };
    const auto make_message = [&](const Message& displayed) {
      return MessageView(displayed, conversation, open_artifact, selection,
        selection_enabled && (!side_chat || message.author == MessageAuthor::Agent), steer, std::move(recover),
        std::move(selection_actions),
        std::move(agent), theme, current->access_revision, links, inline_summary, render_block,
        message.author == MessageAuthor::Agent ? assistant_avatar(message.ref.message, message.run) : std::nullopt,
        bubbles, narrow)
        .Key(message.ref.message.value);
    };
    const auto reading = detail::ChatReadingPosition{"message/" + message.ref.message.value};
    if (!activity) return make_message(message).With(Padding(EdgeInsets{.top = 2.0F, .bottom = 18.0F}))
        .LayoutValue<HistoryRowIndexKey>(index)
        .LayoutValue<detail::ChatReadingPositionKey>(reading);
    std::vector<View> row;
    const auto add_message = [&](const Message& displayed) {
      auto view = make_message(displayed);
      if (bubbles && message.author == MessageAuthor::Tool && message.run)
        view = AssistantChatLine(std::move(view), assistant_avatar(message.ref.message, message.run), narrow);
      row.push_back(Column{std::move(view)}.With(CrossAlign(CrossAxisAlignment::Stretch),
          Padding(EdgeInsets{.top = 2.0F, .bottom = 10.0F})));
    };
    if (activity->outputs.contains(message.ref.message)) {
      auto artifacts = message;
      std::erase_if(artifacts.blocks, [](const auto& block) { return !ShowsToolOutputInHistory(block); });
      if (!artifacts.blocks.empty()) add_message(artifacts);
    } else {
      add_message(message);
    }
    if (const auto groups = activity->groups.find(message.ref.message); groups != activity->groups.end()) {
      const auto render_step = [current, conversation, anchor = std::shared_ptr<const Message>(current, &message), render_block,
                                opened_steps, keep_reading, disclosure, tool_label, bubbles](
          const ToolCallSnapshot& tool, std::optional<View> approval) -> View {
        std::shared_ptr<const Message> output;
        if (tool.output_message) {
          const auto found = std::ranges::find(current->messages, *tool.output_message, &Message::ref);
          if (found != current->messages.end()) output = std::shared_ptr<const Message>(current, &*found);
        }
        return ChatToolStep(tool, ActivityToolLabel(tool.tool, tool_label), anchor, std::move(output), conversation,
            render_block, opened_steps, keep_reading, disclosure, std::move(approval), bubbles);
      };
      const auto approval_view = [&](const ToolCallSnapshot& tool) -> std::optional<View> {
        if (const auto pending = activity->approvals.find(tool.id); !focused_request && pending != activity->approvals.end()) {
          return PendingApprovalView(*pending->second, conversation, tasks, error, *current, open_artifact,
              action_style, theme.colors, attachment_label).Key(pending->second->id.value);
        }
        return {};
      };
      for (const auto& group : groups->second) {
        const auto& first = group.calls.front();
        if (bubbles) {
          std::vector<View> approvals;
          for (const auto& tool : group.calls) {
            if (auto view = approval_view(tool)) approvals.push_back(std::move(*view));
          }
          auto view = ChatToolGroupView(group, tool_label, [render_step](const ToolCallSnapshot& call) {
            return render_step(call, std::nullopt);
          }, std::move(approvals), opened_groups, keep_reading, disclosure);
          row.push_back(AssistantChatLine(std::move(view), assistant_avatar(first.id, first.run), narrow).Key(first.id.value));
        } else {
          row.push_back(render_step(first, approval_view(first)));
        }
      }
    }
    if (const auto footer = activity->footers.find(message.ref.message); footer != activity->footers.end()) {
      const auto& turn = activity->turns.at(footer->second);
      std::function<void()> show_changes;
      if (turn.first_change) {
        if (const auto item = HistoryIndex(activity.get(), turn.first_change->row, ScrollAlignment::Start)) {
          show_changes = [history_controller, keep_reading, opened_steps, opened_groups, location = *turn.first_change, index = *item + earlier] {
            keep_reading();
            auto next = opened_steps.Get();
            next.insert(location.call);
            opened_steps = std::move(next);
            auto groups = opened_groups.Get();
            groups.insert(location.group);
            opened_groups = std::move(groups);
            history_controller.ScrollToItem(index, ScrollAlignment::Start);
          };
        }
      }
      auto details = ChatRunActivity(turn.run, turn.delegates, turn.waits_for_approval, keep_reading, std::move(show_changes));
      row.push_back(bubbles ? AssistantChatLine(std::move(details), assistant_avatar(turn.run.id, turn.run.id), narrow).Key(turn.run.id.value)
                            : std::move(details));
    }
    return Column(std::move(row)).With(Spacing(0.0F), CrossAlign(CrossAxisAlignment::Stretch))
        .LayoutValue<HistoryRowIndexKey>(index)
        .LayoutValue<detail::ChatReadingPositionKey>(reading)
        .Key("chat-history-row/" + message.ref.message.value);
  }).EstimatedItemExtent(76.0F).Controller(history_controller)
      .With(Grow(), ScrollBar(), CrossAlign(CrossAxisAlignment::Stretch),
          ChatHistoryScroll{follow_paused, history_controller, tasks, current->summary.id, disclosure})
      .Key("agent-chat-history");
  auto history_view = ChatHistoryViewport(std::move(history), history_controller, count, follow_paused, selection,
      part == ChatPart::Messages && layout_style.chat_bubbles && !side_chat, UseEnvironment<IconButtonStyle>(), theme);
  std::optional<View> review_page;
  AgentChatPage active_page;
  if (focused_interaction.Get()) active_page = *focused_interaction.Get();
  if (review_body) {
    if (options.page) {
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
      active_page = AgentChatReviewRequest{reviewed->artifact, review_ticket.Get(), approval, first};
    }
    std::vector<View> heading;
    if (!review_only) heading.push_back(ChatBackAction(theme, return_to_chat));
    heading.push_back(Text(reviewed->name.empty() ? StringVariant{huxerui_agentkit::strings::ui_artifact} : StringVariant{reviewed->name},
        TextRole::Title).Style({Font::System(compact ? 21.0F : 28.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface})
        .LineLimit(2).Overflow(TextOverflow::Ellipsis).With(Grow()).Key("artifact-review-title"));
    heading.push_back(Text(StringVariant::Format(huxerui_agentkit::strings::ui_version_0, reviewed->artifact.version), TextRole::Label)
        .Style({Font::System(12.0F), theme.colors.on_surface_variant}).Key("artifact-review-version"));
    std::vector<View> details{Row(std::move(heading)).With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center),
        LayoutPriority(3.0F)).Key("artifact-review-heading")};
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
    if (review_loading.Get()) details.push_back(Text(huxerui_agentkit::strings::ui_opening_attachment, TextRole::Label));
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
  Lifecycle([binding = options.page, active_page] {
    if (binding) *binding = active_page;
  }, active_page);
  if (options.review_only) {
    if (review_page) return std::move(*review_page);
    return detached_status(!error->empty() ? error.Get() : review_loading.Get() ? UseString(huxerui_agentkit::strings::ui_opening_review) : UseString(huxerui_agentkit::strings::ui_artifact_unavailable));
  }
  if (part == ChatPart::Messages) {
    if (unanchored_summary) history_view = Column{std::move(history_view), std::move(*unanchored_summary)}
        .With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
    if (!review_page) return history_view;
    return IndexedPages({std::move(history_view), std::move(*review_page)}, 1U).With(Grow());
  }
  std::vector<View> notices;
  if (const auto anchor = range_anchor.Get(); anchor && (!selected || SelectionBounds(*selected) == *anchor))
    notices.push_back(ProvideEnvironment(action_style, Row{
      Text(huxerui_agentkit::strings::ui_select_the_other_end_of_the_range, TextRole::Label).With(Grow()),
      Button(huxerui_agentkit::strings::ui_cancel_range).OnClick([range_anchor] { range_anchor = std::nullopt; }),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))));
  if (std::ranges::any_of(current->tools, [&](const auto& tool) {
        return active == tool.run && tool.state == ToolCallState::Reviewing;
      })) notices.push_back(Text(huxerui_agentkit::strings::ui_reviewing_tool_request, TextRole::Label));
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
        auto status = huxerui_agentkit::strings::ui_running;
        switch (run->state) {
          case RunState::WaitingForInput: status = huxerui_agentkit::strings::ui_waiting_for_input; break;
          case RunState::WaitingForReply: status = huxerui_agentkit::strings::ui_waiting_for_reply; break;
          case RunState::WaitingForDependency:
            status = run->waits.empty() ? huxerui_agentkit::strings::ui_queued : huxerui_agentkit::strings::ui_waiting_for_dependency;
            break;
          case RunState::Cancelling: status = huxerui_agentkit::strings::ui_stopping; break;
          case RunState::Succeeded: status = huxerui_agentkit::strings::ui_completed; break;
          case RunState::Failed: status = huxerui_agentkit::strings::ui_failed; break;
          case RunState::Cancelled: status = huxerui_agentkit::strings::ui_cancelled; break;
          case RunState::Running: break;
        }
        rows.push_back(Text(role_label(run->agent) + " · " + UseString(status), TextRole::Label)
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
    if (const auto* approval = std::get_if<ToolApproval>(&interaction.body);
        activity && approval && activity->approvals.contains(approval->call)) continue;
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
    const auto& failure = *last_root->failure;
    using namespace huxerui_agentkit::strings;
    StringVariant message(failure.message);
    if (failure.provider_code == "first_response_timeout") message = ui_model_first_response_timeout;
    else if (failure.provider_code == "stream_idle_timeout") message = ui_model_stream_idle_timeout;
    else if (failure.provider_code == "request_deadline" || last_root->end_reason == RunEndReason::Deadline)
      message = ui_model_request_deadline;
    else if (failure.provider_code == "output_token_limit") message = ui_model_output_limit;
    else if (failure.provider_code == "context_window_exceeded") message = ui_model_context_limit;
    else if (failure.provider_code == "stream_interrupted" ||
        (failure.code == ErrorCode::ProviderFailure && failure.message == "Model response body could not be read"))
      message = ui_model_stream_interrupted;
    notices.push_back(Text(std::move(message), TextRole::Label)
        .Style({Font::System(12.0F), theme.colors.error}).Key("agent-chat-run-error"));
    const auto trigger = last_root->trigger;
    const bool retryable = trigger == RunTrigger::UserInput || trigger == RunTrigger::Retry || trigger == RunTrigger::Regenerate;
    if (last_root->state == RunState::Failed && retryable && current->summary.lifecycle == SessionLifecycle::Open) {
      if (const auto blocked = detail::RestartToolError(current->tools, last_run_tree)) {
        notices.push_back(Text(blocked->message, TextRole::Label).Key("agent-chat-retry-blocked"));
      } else {
        notices.push_back(ProvideEnvironment(action_style, Row{
            Button(huxerui_agentkit::strings::ui_retry).OnClick([conversation, tasks, error, sending, history_owner, source = last_root->id] {
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
  if (side_chat && bring_back && selected) conclusion = AgentChatCitation{selected->source, selected->text, selected->additional_sources};
  const bool returning_selection = conclusion.has_value();
  if (!conclusion && side_chat && bring_back) conclusion = LastCompletedReply(current->messages);
  std::vector<View> composer;
  if (!draft_sources->attachments.empty()) composer.push_back(AgentChatAttachmentChips(draft_sources));
  if (const auto quoted = draft_sources.Get().quote) {
    composer.push_back(ProvideEnvironment(action_style, Row{Text(quoted->text, TextRole::Label).LineLimit(2)
        .Overflow(TextOverflow::Ellipsis).With(Grow()),
        Button(huxerui_agentkit::strings::ui_remove_quote).OnClick([draft_sources] {
          auto sources = draft_sources.Get();
          sources.quote.reset();
          draft_sources = std::move(sources);
        })}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center), CornerRadius(5.0F),
            Background(theme.colors.surface_container_high), Padding(EdgeInsets::Symmetric(7.0F, 0.0F))))
        .Key("composer-draft-quote"));
  }
  if (const auto brought = draft_sources.Get().conclusion) {
    composer.push_back(ProvideEnvironment(action_style, Row{Text(brought->text, TextRole::Label).LineLimit(2)
        .Overflow(TextOverflow::Ellipsis).With(Grow()),
        Button(huxerui_agentkit::strings::ui_remove_conclusion).OnClick([draft_sources] {
          auto sources = draft_sources.Get();
          sources.conclusion.reset();
          draft_sources = std::move(sources);
        })}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center), CornerRadius(5.0F),
            Background(theme.colors.surface_container_high), Padding(EdgeInsets::Symmetric(7.0F, 0.0F))))
        .Key("composer-draft-conclusion"));
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
          Button(huxerui_agentkit::strings::ui_remove_reference).OnClick([recovered, entry] {
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
  composer.push_back(ChatComposerInput(draft, composer_focus, composer_focused,
      side_chat ? huxerui_agentkit::strings::ui_ask_a_follow_up : huxerui_agentkit::strings::ui_send_a_message,
      side_chat ? "side-chat-composer" : "composer", send, true, side_chat)
      .Key("chat-composer-editor"));
  std::optional<View> run_controls;
  const bool stable_toolbar = layout_style.chat_bubbles && !side_chat;
  if (options.run_controls && options.run_controls->render) {
    std::optional<RunStartBinding> binding;
    if (active) {
      const auto run = std::ranges::find(current->runs, *active, &RunSnapshot::id);
      if (run != current->runs.end()) binding = RunStartBinding{run->model, run->execution_profile};
    }
    auto controls = options.run_controls->render(current->summary, binding);
    run_controls = stable_toolbar ? ProvideEnvironment(AgentChatComposerAnchor{composer_anchor}, std::move(controls))
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
    const auto& mode = enqueue ? huxerui_agentkit::strings::ui_queue : huxerui_agentkit::strings::ui_send_now;
    const auto choose_mode = [send_menu, send_mode, queue, active, enqueue] {
      send_menu.Show(send_mode, {
          MenuItem(huxerui_agentkit::strings::ui_send_now, [queue] { queue = std::nullopt; }).Checked(!enqueue),
          MenuItem(huxerui_agentkit::strings::ui_queue, [queue, active] { queue = active; }).Checked(enqueue),
      });
    };
    if (layout_style.chat_bubbles) {
      actions.push_back(ProvideEnvironment(compact_action, IconButton(enqueue ? huxerui_agentkit::images::queue : huxerui_agentkit::images::lightning, huxerui_agentkit::strings::ui_send_mode)
          .OnClick(choose_mode).With(send_mode, Tooltip(StringVariant::Format(huxerui_agentkit::strings::ui_send_mode_0, UseString(mode))),
              Semantics{.label = huxerui_agentkit::strings::ui_send_mode, .value = mode}).Key("send-mode")));
    } else {
      actions.push_back(detail::ChatControlWithIcons(Button(mode).OnClick(choose_mode)
          .With(send_mode, Semantics{.label = huxerui_agentkit::strings::ui_send_mode, .value = mode}).Key("send-mode"),
          action_style, theme.colors.on_surface_variant, detail::ChatControlIcon::Chevron));
    }
  } else if (stable_toolbar) {
    actions.push_back(reserved_action());
  }
  if (active) {
    actions.push_back(ProvideEnvironment(compact_action, IconButton(huxerui_agentkit::images::stop, huxerui_agentkit::strings::ui_stop_current_task)
        .OnClick([tasks, conversation, active, error] {
          tasks.Launch([conversation, active, error]() -> Task<void> {
            auto stopped = co_await conversation->CancelAsync(NewCommandId(), *active);
            if (!stopped.Succeeded()) error = stopped.Error().message;
          });
        }).With(Tooltip(huxerui_agentkit::strings::ui_stop_current_task)).Key("agent-chat-stop")));
  } else if (stable_toolbar) {
    actions.push_back(reserved_action());
  }
  if (conclusion) {
    actions.push_back(ProvideEnvironment(action_style, Button(huxerui_agentkit::strings::ui_bring_back_conclusion)
        .OnClick([bring_back, selected = std::move(*conclusion)] {
          (*bring_back)(selected);
        }).Key(returning_selection ? "side-chat-return-selection" : "side-chat-return-reply")));
  }
  const auto selected_model = options.run_controls ? options.run_controls->next->model : std::nullopt;
  if (auto context = ComposerContextUsage(*current, selected_model))
    actions.push_back(ComposerContextIndicator(*context, theme.colors));
  const auto& send_label = enqueue ? huxerui_agentkit::strings::ui_queue_message :
      send_as_new ? huxerui_agentkit::strings::ui_send_as_new_task : huxerui_agentkit::strings::ui_send;
  const auto send_key = enqueue ? "chat-send/Queue message" : send_as_new ? "chat-send/Send as new task" : "chat-send/Send";
  actions.push_back(ComposerSendControl(send_label, send_key, send, send_available, layout_style.chat_bubbles));
  if (compact && run_controls && active && !stable_toolbar) {
    composer.push_back(std::move(*run_controls));
    run_controls.reset();
  }
  composer.push_back(Row{
      run_controls ? View(Column{std::move(*run_controls)}.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch))) : View(Spacer()),
      Row(std::move(actions)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)).Key("agent-chat-toolbar"));
  if (current->summary.queued_inputs)
    notices.push_back(Text(StringVariant::Format(huxerui_agentkit::strings::ui_0_queued, current->summary.queued_inputs), TextRole::Label));
  if (options.run_controls && options.run_controls->describe) {
    std::vector<View> queue_rows;
    for (const auto& item : current->queue) {
      const auto* input = std::get_if<SendInput>(&item.input);
      if (!input || !item.user_start || (item.state != QueueItemState::Queued && item.state != QueueItemState::Rejected)) continue;
      const bool rejected = item.state == QueueItemState::Rejected;
      auto detail = options.run_controls->describe(*item.user_start);
      if (rejected && item.reason) detail += "\n" + item.reason->message;
      auto button = rejected ? Button(huxerui_agentkit::strings::ui_edit_queued_input).OnClick([restore, id = item.id] { restore(id); })
          .With(Enabled(vacant() && !recovering.Get())) : Button(huxerui_agentkit::strings::ui_withdraw).OnClick([tasks, conversation, error, id = item.id] {
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

  const bool has_notices = !notices.empty();
  std::vector<View> content;
  content.push_back(std::move(history_view));
  if (activity_access && sending.Get() && !active)
    content.push_back(ChatActivityHeader(huxerui_agentkit::strings::ui_sending_message).Key("chat-sending-message"));
  if (unanchored_summary) content.push_back(std::move(*unanchored_summary));
  if (has_notices) content.push_back(Column(std::move(notices))
      .With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch), LayoutPriority(2.0F)));
  auto composer_panel = ChatComposerPanel(std::move(composer), composer_focused, layout_style.chat_bubbles, theme.colors)
      .With(composer_anchor);
  if (part == ChatPart::Composer) {
    if (has_notices) return Column{std::move(content.back()), std::move(composer_panel)}
        .With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch));
    return composer_panel;
  }
  if (has_notices) composer_panel = std::move(composer_panel).With(LayoutPriority(3.0F));
  content.push_back(std::move(composer_panel));
  const float gutter = compact ? 14.0F : viewport_class == ViewportClass::Medium ? 21.0F : 36.0F;
  View lane = Column(std::move(content)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
  if (layout_style.content_width > 0.0F)
    lane = Row{Spacer(), std::move(lane).With(Frame{.width = layout_style.content_width}), Spacer()}
        .With(CrossAlign(CrossAxisAlignment::Stretch));
  View chat = std::move(lane).With(Grow(),
      Padding(side_chat ? EdgeInsets{} : EdgeInsets{.top = layout_style.chat_bubbles ? 10.0F : compact ? 20.0F : 28.0F, .right = gutter,
          .bottom = compact ? 14.0F : 25.0F, .left = gutter}));
  if (layout_style.chat_bubbles && !side_chat) {
    chat = std::move(chat).On<ViewEvents::PointerIntercept>([selection](const PointerEvent& event) {
      if (event.type == PointerEventType::Down && !event.modifiers.shift && selection.Get()) selection = std::nullopt;
      return false;
    });
  }
  std::optional<View> request_page;
  if (focused_interaction.Get()) {
    const auto item = std::ranges::find(current->interactions, *focused_interaction.Get(), &InteractionSnapshot::id);
    std::vector<View> request{
        Row{ChatBackAction(theme, [focused_interaction] { focused_interaction = std::nullopt; }),
            Text(current->summary.title, TextRole::Title)
                .Style({Font::System(18.0F).WithWeight(FontWeight::SemiBold), theme.colors.on_surface})
                .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow())}
            .With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
        Divider(),
    };
    if (item == current->interactions.end() || item->state != InteractionState::Pending) {
      request.push_back(Text(huxerui_agentkit::strings::ui_this_request_is_no_longer_pending, TextRole::Label));
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
  if (options.page && options.page->Get() != active_page) {
    review_page.reset();
    request_page.reset();
  }
  const auto page = review_page ? 1U : request_page ? 2U : 0U;
  return IndexedPages({std::move(chat), review_page ? std::move(*review_page) : View{Spacer()},
      request_page ? std::move(*request_page) : View{Spacer()}}, page).With(Grow());
}

View AgentChat(std::shared_ptr<Conversation> conversation, AgentChatOptions options) {
  return ConversationChatView(std::move(conversation), std::move(options), ChatPart::Full);
}

View AgentChatMessages(std::shared_ptr<Conversation> conversation, std::optional<AgentChatTextSelection::Binding> selection) {
  return ConversationChatView(std::move(conversation), {.selection = std::move(selection)}, ChatPart::Messages);
}

View AgentChatComposer(std::shared_ptr<Conversation> conversation, State<TextEditingValue> draft,
                       std::optional<State<AgentChatDraftSources>> sources, std::optional<ViewHandle> focus,
                       std::optional<AgentChatRunControls> run_controls) {
  return ConversationChatView(std::move(conversation), {.draft_sources = std::move(sources), .draft = draft,
      .run_controls = std::move(run_controls), .composer_focus = std::move(focus)}, ChatPart::Composer);
}

} // namespace huxerui::agentkit
