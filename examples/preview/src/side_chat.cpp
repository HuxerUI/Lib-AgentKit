#include "side_chat.h"
#include "chat_chrome.h"
#include "preview_theme.h"

#include <app_resources.h>
#include <huxerui/huxerui.h>

#include <algorithm>

namespace agentkit_preview {
using namespace huxerui;
namespace kit = huxerui::agentkit;

struct PreviewSideChats::Draft {
  kit::SessionId source;
  std::vector<kit::SourceSlice> slices;
  State<std::shared_ptr<kit::Conversation>> conversation{std::shared_ptr<kit::Conversation>{}};
  State<TextEditingValue> question{TextEditingValue::FromText({})};
  State<kit::AgentChatDraftSources> sources{kit::AgentChatDraftSources{}};
  State<kit::UserRunOptions> next{kit::UserRunOptions{}};
  State<bool> pending{false};
  std::optional<kit::SessionId> created;
  State<std::string> error{std::string{}};
};

namespace {

struct SideChatRequest {
  std::shared_ptr<kit::Conversation> source;
  kit::Revision access;
  std::vector<std::string> passages;
  State<kit::AgentChatDraftSources> main_draft;
  std::function<void()> restore;
  std::function<void()> cited;
  std::shared_ptr<PreviewSideChats::Draft> draft;
  std::uint64_t generation = 0;

  bool HasAccess() const {
    auto current = source->Snapshot();
    return current && current->access_revision == access;
  }
};

struct SideChatHost {
  std::shared_ptr<const std::function<void(SideChatRequest, const kit::AgentChatCitation&)>> open;
  static SideChatHost Default() { return {}; }

  bool operator==(const SideChatHost&) const = default;
};

ButtonStyle SelectionActionStyle(ButtonStyle style, const ColorScheme& colors) {
  style.background = Color::Transparent();
  style.disabled_background = Color::Transparent();
  style.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface_variant};
  style.padding = EdgeInsets::Symmetric(7.0F, 4.0F);
  style.minimum_height = 32.0F;
  return style;
}

std::optional<kit::AgentChatCitation> LatestReply(const kit::SessionSnapshot& snapshot) {
  for (auto message = snapshot.messages.rbegin(); message != snapshot.messages.rend(); ++message) {
    if (message->author != kit::MessageAuthor::Agent || message->state != kit::MessageState::Complete) continue;
    std::string text;
    for (const auto& block : message->blocks) {
      if (const auto* part = std::get_if<kit::TextBlock>(&block.body)) {
        if (!text.empty()) text.push_back('\n');
        text += part->text;
      }
    }
    if (!text.empty()) return kit::AgentChatCitation{{message->ref}, std::move(text)};
  }
  return {};
}

[[huxerui::composable]]
View SideChatPane(std::shared_ptr<const SideChatRequest> request, ViewHandle focus, std::function<void()> close,
                  std::function<void()> restore) {
  const auto service = UseService<PreviewSideChats>();
  const auto tasks = UseTaskScope();
  const auto background = UseApplicationTaskScope();
  const auto allowed = UseState(true);
  const auto returned = UseState(false);
  const auto reply = UseState(std::optional<kit::AgentChatCitation>{});
  const auto selection = UseState(std::shared_ptr<const kit::AgentChatTextSelection>{});
  const auto follow_up_focus = UseViewHandle();
  const auto first_focused = UseState(false);
  const auto draft = request->draft;
  auto controls = service && service->composer_controls
      ? std::optional{service->composer_controls(draft->next, draft->sources)} : std::nullopt;
  const auto conversation = draft->conversation.Get();
  const auto scope = request->access;
  const auto visible = [request, allowed] { return allowed.Get() && request->HasAccess(); };
  Lifecycle([tasks, request, scope, allowed, conversation, reply] {
    auto subscriptions = std::make_shared<std::vector<kit::Subscription>>();
    auto worker = tasks.Launch([subscriptions, request, scope, allowed, conversation, reply]() -> Task<void> {
      auto source = co_await request->source->ObserveAsync([allowed, scope](auto result) {
        if (!result.Succeeded() || result.Value()->access_revision != scope) allowed = false;
      });
      if (source.Succeeded()) subscriptions->push_back(std::move(source).Value());
      else allowed = false;
      if (!conversation) co_return;
      auto side = co_await conversation->ObserveAsync([reply](auto result) {
        reply = result.Succeeded() ? LatestReply(*result.Value()) : std::nullopt;
      });
      if (side.Succeeded()) subscriptions->push_back(std::move(side).Value());
    });
    return [worker, subscriptions] { worker.Cancel(); subscriptions->clear(); };
  }, request, conversation);
  Lifecycle([background, returned, restore, cited = request->cited] {
    return [background, returned, restore, cited] {
      if (returned.Get()) { if (cited) background.Post(cited); }
      else if (restore) restore();
    };
  });
  Lifecycle([tasks, target = conversation ? follow_up_focus : focus, available = visible()] {
    if (available) tasks.Post([target] { target.RequestFocus(true); });
  }, static_cast<bool>(conversation), allowed.Get());

  const auto theme = UseTheme();
  const auto& colors = theme.colors;
  const auto buttons = SelectionActionStyle(UseEnvironment<ButtonStyle>(), colors);
  std::vector<View> body;
  if (visible()) {
    std::vector<View> passages;
    for (const auto& text : request->passages)
      passages.push_back(Text(text).Style({Font::System(13.0F), colors.on_surface}));
    StringVariant label = request->passages.size() > 1
        ? StringVariant::Format(::app::strings::ui_0_selected_passages, request->passages.size())
        : StringVariant{::app::strings::ui_selected_text};
    body.push_back(Column{Text(std::move(label), TextRole::Label)
            .Style({Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_primary_container}),
        ScrollView(Column(std::move(passages)).With(Spacing(5.0F), CrossAlign(CrossAxisAlignment::Stretch)))
            .With(Frame{.max_height = 88.0F}, ScrollBar())}
        .With(Padding(10.0F), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch),
            Background(colors.surface_container_low), Border{colors.outline}, CornerRadius(8.0F)).Key("side-chat-source"));
    if (conversation) {
      body.push_back(kit::AgentChatMessages(conversation, selection).With(Grow()).Key("side-chat-messages"));
      if (selection.Get() || reply.Get()) {
        const auto bring_back = [selection, reply, tasks, visible, request, returned, close] {
          tasks.Launch([selected = selection.Get(), whole = reply.Get(), visible, request, returned, close]() mutable -> Task<void> {
            if (!visible()) co_return;
            if (selected) {
              auto cited = co_await selected->ReadAsync();
              if (!cited.Succeeded()) { request->draft->error = cited.Error().message; co_return; }
              whole = std::move(cited).Value();
            }
            if (!whole || !visible()) co_return;
            auto draft = request->main_draft.Get();
            draft.conclusion = std::move(whole);
            request->main_draft = std::move(draft);
            returned = true;
            close();
          });
        };
        std::vector<View> actions{Button(::app::strings::ui_bring_back_conclusion).OnClick(bring_back)
            .Key(selection.Get() ? "side-chat-return-selection" : "side-chat-return-reply")};
        if (const auto selected = selection.Get()) {
          actions.push_back(Button(::app::strings::ui_quote).OnClick([selected, draft, tasks, visible, follow_up_focus] {
            tasks.Launch([selected, draft, visible, follow_up_focus]() -> Task<void> {
              auto cited = co_await selected->ReadAsync();
              if (!visible()) co_return;
              if (!cited.Succeeded()) { draft->error = cited.Error().message; co_return; }
              auto sources = draft->sources.Get();
              sources.quote = std::move(cited).Value();
              draft->sources = std::move(sources);
              follow_up_focus.RequestFocus(true);
            });
          }).Key("side-chat-quote"));
        }
        body.push_back(ProvideEnvironment(buttons, Flow(std::move(actions)).With(Spacing(5.0F))));
      }
      body.push_back(kit::AgentChatComposer(conversation, draft->question, draft->sources, follow_up_focus,
          std::move(controls)).Key("side-chat-input"));
    } else {
      if (draft->pending.Get()) body.push_back(Text(::app::strings::ui_sending_message, TextRole::Label).Key("chat-sending-message"));
      const auto can_send = [draft] {
        return !draft->pending.Get() && !draft->question->composition &&
            (!draft->question->text.empty() || !draft->sources->attachments.empty());
      };
      const auto submit = [background, service, draft, visible, can_send] {
        if (!service || !visible() || !can_send()) return;
        kit::SendInput input{.text = draft->question->text};
        for (const auto& attachment : draft->sources->attachments) input.staged_attachments.push_back(attachment->id);
        kit::CreateSessionRequest start{.options = {.agent = service->assistant, .retention = kit::SessionRetention::Ephemeral},
            .initial = std::move(input), .side_chat = kit::SideChatSeed{draft->source, draft->slices},
            .user_start = draft->next.Get()};
        draft->pending = true;
        draft->error = "";
        background.Launch([draft, runtime = service->runtime, start = std::move(start)]() mutable -> Task<void> {
          if (!draft->created) {
            auto result = co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(start));
            if (!result.Succeeded()) { draft->pending = false; draft->error = result.Error().message; co_return; }
            const auto* created = std::get_if<kit::SessionCreated>(&result.Value().outcome);
            if (!created) { draft->pending = false; draft->error = "The side chat is waiting for approval"; co_return; }
            draft->created = created->session;
          }
          auto opened = co_await runtime->Sessions().OpenAsync(*draft->created);
          draft->pending = false;
          if (!opened.Succeeded()) { draft->error = opened.Error().message; co_return; }
          draft->question = TextEditingValue::FromText({});
          draft->sources = kit::AgentChatDraftSources{};
          draft->conversation = std::move(opened).Value();
        });
      };
      std::optional<View> toolbar;
      if (controls) toolbar = controls->render({}, {});
      std::vector<View> extras;
      if (!draft->sources->attachments.empty()) extras.push_back(kit::AgentChatAttachmentChips(draft->sources));
      body.push_back(PreviewDraftComposer(draft->question, focus, first_focused, ::app::strings::ui_ask_about_this_selection,
          "side-chat-question", std::move(extras), std::move(toolbar), !draft->pending.Get(), can_send, submit, true));
    }
    if (!draft->error->empty()) body.push_back(Text(draft->error.Get(), TextRole::Label));
  } else {
    body.push_back(Text(::app::strings::ui_selection_access_changed_close_this_side_chat));
  }
  return Column{
      PreviewPaneHeader(::app::images::chat, ::app::strings::ui_side_chat, ::app::strings::ui_close, "side-chat-close", close, theme),
      ProvideEnvironment(DividerStyle{colors.outline}, Divider()),
      Column(std::move(body)).With(Grow(), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch), Padding(14.0F)),
  }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch), Background(colors.surface)).Key("side-chat-pane");
}

[[huxerui::composable]]
View SelectionMenu(std::shared_ptr<kit::Conversation> conversation, kit::AgentChatTextSelection::Binding selection,
                   State<kit::AgentChatDraftSources> draft, ViewHandle composer, State<bool> restore_focus, bool hidden) {
  const auto popup = UsePopup();
  const auto host = UseEnvironment<SideChatHost>();
  const auto tasks = UseTaskScope();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto layer = UseState(std::optional<LayerId>{});
  const auto current = hidden ? nullptr : selection.Get();
  const auto anchor = current ? std::optional{current->Anchor()} : std::nullopt;
  const auto caret = current ? current->Caret() : std::nullopt;
  Lifecycle([anchor, caret, selection, conversation, draft, composer, restore_focus, popup, host, tasks, busy, error, layer] {
    if (!anchor) return std::function<void()>{[] {}};
    error = "";
    const auto use = [selection, conversation, draft, composer, restore_focus, host, tasks, busy, error](PopupContext menu, bool ask) {
      const auto current = selection.Get();
      const auto snapshot = conversation->Snapshot();
      if (!current || !snapshot || busy.Get()) return;
      error = "";
      busy = true;
      tasks.Launch([current, conversation, draft, composer, restore_focus, host, tasks, busy, error, menu, ask,
                    access = snapshot->access_revision]() -> Task<void> {
        auto resolved = co_await current->ReadAsync();
        busy = false;
        if (!resolved.Succeeded()) { error = resolved.Error().message; co_return; }
        const auto cited = [current, composer] {
          current->Clear();
          composer.RequestFocus(true);
        };
        menu.Dismiss();
        if (ask && host.open) {
          current->Clear();
          (*host.open)({conversation, access, current->Passages(), draft,
              [current, restore_focus] { current->Restore(); restore_focus = true; }, cited}, resolved.Value());
        } else if (!ask) {
          auto sources = draft.Get();
          sources.quote = std::move(resolved).Value();
          draft = std::move(sources);
          cited();
        }
      });
    };
    PopupFactory actions = [selection, use, restore_focus, busy, error](PopupContext menu) {
      const auto focus = UseViewHandle();
      const auto colors = UseTheme().colors;
      const auto style = SelectionActionStyle(UseEnvironment<ButtonStyle>(), colors);
      const auto selected = selection.Get();
      Lifecycle([focus, restore_focus] {
        if (restore_focus.Get()) { restore_focus = false; focus.RequestFocus(true); }
      }, restore_focus.Get());
      std::vector<View> items{ProvideEnvironment(style, Row{
          Button(::app::strings::ui_side_chat).OnClick([use, menu] { use(menu, true); }).With(focus, Enabled(!busy.Get())),
          Button(::app::strings::ui_quote).OnClick([use, menu] { use(menu, false); }).With(Enabled(!busy.Get())),
          Button(selected && selected->IsExtending() ? ::app::strings::ui_cancel_range : ::app::strings::ui_extend_selection)
              .OnClick([selection] { if (const auto current = selection.Get()) current->Extend(); }).With(Enabled(!busy.Get())),
      }.With(Spacing(4.0F)))};
      if (!error->empty()) items.push_back(Text(error.Get(), TextRole::Label));
      return Column(std::move(items)).With(Spacing(3.0F), Padding(4.0F), CornerRadius(9.0F),
          Border{colors.outline}, Background(colors.surface_container_high)).Key("message-selection-actions");
    };
    PopupOptions options{.placement = {AnchorSide::Above, AnchorAlignment::Center}, .gap = 6.0F,
        .dismiss_on_outside_press = false, .retain_anchor_focus = true,
        .on_dismiss_request = [selection] { if (const auto current = selection.Get()) current->Clear(); }};
    try {
      const auto id = caret ? popup.ShowAtAnchor(*anchor, *caret, std::move(actions), std::move(options))
                            : popup.Show(*anchor, std::move(actions), std::move(options));
      layer = id;
      return std::function<void()>{[popup, id, layer] {
        popup.Dismiss(id);
        if (layer.Get() == id) layer = std::nullopt;
      }};
    } catch (const std::logic_error&) {
      return std::function<void()>{[] {}};
    }
  }, anchor, caret.has_value(), conversation);
  Lifecycle([popup, id = layer.Get(), caret] {
    if (id && caret) popup.UpdateAnchor(*id, *caret);
  }, layer.Get(), caret);
  return Spacer().With(Frame{.height = 0.0F, .max_height = 0.0F});
}

} // namespace

[[huxerui::composable]]
View PreviewChat(std::shared_ptr<kit::Conversation> conversation, kit::AgentChatOptions options, std::optional<View> actions) {
  const auto chrome = UseEnvironment<ChatChrome>();
  if (!chrome.header) return kit::AgentChat(conversation, options);
  const auto local_page = UseState(kit::AgentChatPage{});
  const auto page = options.page.value_or(local_page);
  auto body = options;
  body.page = page;
  auto header = (*chrome.header)(page.Get(), actions);
  return Column{header ? std::move(*header) : View{Spacer().With(Grow(0.0F), Frame{.height = 0.0F, .max_height = 0.0F})},
      kit::AgentChat(conversation, std::move(body)).With(Grow()).Key("preview-chat-body")}
      .With(Grow(), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View PreviewDraftComposer(State<TextEditingValue> draft, ViewHandle focus, State<bool> focused,
    StringVariant placeholder, const char* input_key, std::vector<View> extras, std::optional<View> controls,
    bool editing, std::function<bool()> send_available, std::function<void()> submit, bool dense) {
  auto content = std::move(extras);
  const auto colors = UseTheme().colors;
  const bool can_send = editing && send_available();
  auto field = UseEnvironment<TextFieldStyle>();
  field.show_label = false;
  field.standard.border = field.standard.hovered_border = field.standard.focused_border = Color::Transparent();
  field.standard.minimum_height = 0.0F;
  field.text_style = {Font::System(15.0F), colors.on_surface};
  field.placeholder_style = {Font::System(15.0F), colors.on_surface_variant};
  field.caret = colors.on_primary_container;
  field.padding = {};
  content.push_back(ProvideEnvironment(field, Row {
    TextField(draft).Label(::app::strings::ui_message).Placeholder(placeholder)
        .Variant(TextFieldVariant::Standard).LineLimits(TextFieldLineLimits::MultiLine(dense ? 1 : 2, 5))
        .On<ViewEvents::KeyIntercept>([draft, submit](const KeyEvent& event) {
          if (event.type != KeyEventType::Down || (event.key != Key::Enter && event.key != Key::NumpadEnter) ||
              event.modifiers.control || event.modifiers.alt || event.modifiers.meta) return false;
          if (draft->composition) return true;
          if (event.modifiers.shift) return false;
          if (!event.repeat) submit();
          return true;
        })
        .OnChanged([draft](const TextEditingValue& value) { draft = value; })
        .On<ViewEvents::FocusChanged>([focused](bool value) { focused = value; }).OnSubmitted(submit)
        .With(focus, Grow(), Enabled(editing)).Key(input_key),
  }.With(Frame{.min_height = dense ? 38.0F : 54.0F},
      Padding(EdgeInsets{.top = 4.0F, .right = 4.0F, .left = 4.0F}))).Key("message-input"));
  auto send = UseEnvironment<IconButtonStyle>();
  send.icon_size = 20.0F;
  send.minimum_interactive_size = send.state_layer_size = 38.0F;
  send.corner_radius = 11.0F;
  send.foreground = colors.on_primary;
  send.disabled_foreground = colors.on_surface_variant;
  content.push_back(Row {
    controls ? View(Column {*controls}.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch))) : View(Spacer()),
    ProvideEnvironment(send, IconButton(::app::images::send, ::app::strings::ui_send).OnClick(submit)
        .With(Enabled(can_send), Background(can_send ? colors.primary : colors.surface_container_high),
              CornerRadius(11.0F), Tooltip(::app::strings::ui_send)).Key("chat-send/Send")),
  }.With(CrossAlign(CrossAxisAlignment::Center)).Key("agent-chat-toolbar"));
  return Column(std::move(content)).With(Spacing(3.0F), CrossAlign(CrossAxisAlignment::Stretch), Padding(11.0F),
      CornerRadius(17.0F), Background(colors.surface_container),
      Border{focused.Get() ? colors.on_primary_container : colors.outline}, Enabled(editing)).Key("agent-chat-composer");
}

[[huxerui::composable]]
View PreviewSideChatWorkspace(View content, std::shared_ptr<kit::Conversation> conversation,
    kit::AgentChatTextSelection::Binding selection, State<kit::AgentChatDraftSources> draft, ViewHandle composer) {
  const auto service = UseService<PreviewSideChats>();
  using Active = std::shared_ptr<const SideChatRequest>;
  const auto active = UseState(Active{});
  const auto focus = UseViewHandle();
  const auto restore_focus = UseState(false);
  const auto open = UseState(std::make_shared<const std::function<void(SideChatRequest, const kit::AgentChatCitation&)>>(
      [active, service](SideChatRequest request, const kit::AgentChatCitation& citation) {
        if (!service || !service->runtime || !request.HasAccess()) return;
        const auto source = request.source->Snapshot()->summary.id;
        auto slices = citation.additional_sources;
        slices.insert(slices.begin(), citation.source);
        auto found = std::ranges::find_if(service->drafts, [&](const auto& draft) {
          return draft->source == source && draft->slices == slices;
        });
        if (found == service->drafts.end()) {
          service->drafts.push_back(std::make_shared<PreviewSideChats::Draft>(PreviewSideChats::Draft{source, std::move(slices)}));
          found = std::prev(service->drafts.end());
        }
        request.draft = *found;
        request.generation = active.Get() ? active.Get()->generation + 1 : 1;
        active = std::make_shared<const SideChatRequest>(std::move(request));
      }));
  const auto split_tree = UseState(DockTree::Split(Axis::Horizontal, {
      DockTree::Tabs({"source"}).With(Frame{.min_width = 316.0F}, Grow()),
      DockTree::Tabs({"side-chat"}).With(Frame{.width = 336.0F, .min_width = 276.0F, .max_width = 520.0F}, Grow(0.0F)),
  }));
  const auto request = active.Get();
  const bool split = request && UseViewportClass() == ViewportClass::Expanded;
  std::vector<DockItem> panes;
  panes.push_back(DockItem("source", ::app::strings::ui_back_to_conversation, ProvideEnvironment(SideChatHost{open.Get()},
      Stack{View(content).With(Grow()), SelectionMenu(conversation, selection, draft, composer, restore_focus, bool(request))
          .With(Frame{.width = 0.0F, .height = 0.0F})}
          .With(Grow(), Align(HorizontalAlignment::Stretch, VerticalAlignment::Center)))).Closable(false));
  if (request) {
    panes.push_back(DockItem("side-chat", ::app::strings::ui_side_chat, SideChatPane(request, focus, [active, request] {
      if (active.Get() == request) active = Active{};
    }, [active, request] {
      if (!active.Get() && request->restore) request->restore();
    }).Key(request->generation)).Closable(false));
  }
  return DockLayout(std::move(panes), split ? split_tree.Get() : DockTree::Tabs({request ? "side-chat" : "source"}))
      .TabBar([](const DockTabBarContext&) { return View{}; })
      .OnChanged([split_tree](const DockTree& tree) { split_tree = tree; }).With(Grow());
}

[[huxerui::composable]]
View PreviewSourceActions(std::shared_ptr<kit::Conversation> conversation, std::optional<kit::SourceSlice> source,
    State<kit::AgentChatDraftSources> draft, std::function<void()> cited) {
  const auto host = UseEnvironment<SideChatHost>();
  const auto tasks = UseTaskScope();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  std::vector<View> entries;
  if (source) {
    const auto use = [conversation, source = *source, draft, cited, host, tasks, busy, error](bool ask) {
      const auto snapshot = conversation->Snapshot();
      if (!snapshot || busy.Get()) return;
      busy = true;
      error = "";
      tasks.Launch([conversation, source, draft, cited, host, busy, error, access = snapshot->access_revision, ask]() -> Task<void> {
        auto read = co_await conversation->ReadSelectionAsync(source);
        busy = false;
        if (!read.Succeeded()) { error = read.Error().message; co_return; }
        const auto latest = conversation->Snapshot();
        if (!latest || latest->access_revision != access) { error = "Selection access changed"; co_return; }
        kit::AgentChatCitation citation{source, std::move(read).Value()};
        if (ask && host.open) (*host.open)({conversation, access, {citation.text}, draft, {}, cited}, citation);
        else if (!ask) {
          auto sources = draft.Get();
          sources.quote = std::move(citation);
          draft = std::move(sources);
          if (cited) cited();
        }
      });
    };
    const auto style = SelectionActionStyle(UseEnvironment<ButtonStyle>(), UseTheme().colors);
    entries.push_back(ProvideEnvironment(style, Row{
        Button(::app::strings::ui_side_chat).OnClick([use] { use(true); }).With(Enabled(!busy.Get() && bool(host.open))),
        Button(::app::strings::ui_quote).OnClick([use] { use(false); }).With(Enabled(!busy.Get())),
    }.With(Spacing(8.0F))));
  }
  if (!error.Get().empty()) entries.push_back(Text(error.Get(), TextRole::Label).Key("source-selection-error"));
  return Column(std::move(entries)).With(Spacing(6.0F));
}

} // namespace agentkit_preview
