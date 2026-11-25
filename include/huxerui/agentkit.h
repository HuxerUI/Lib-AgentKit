#pragma once

/// @file
/// @brief Convenience entry point for AgentRuntime, model adapters and the default conversation UI.
/// Include `huxerui/agentkit.h` and link `HuxerUI::AgentKit`. Create and start AgentRuntime in an application
/// scope. Open a Conversation through SessionService, then pass it to AgentChat() or bind your own views to its snapshots.

#include <huxerui/agentkit/runtime.h>
#include <huxerui/agentkit/providers.h>
#include <huxerui/view.h>

namespace huxerui::agentkit {

namespace detail { struct SideChatDrafts; }

/// @brief Application service enabling quotation and side chat in AgentChat().
/// Register it with ApplicationContext::Provide(). The assistant must be a registered AgentId that can use the selected sources.
struct AgentChatSelection {
  /// Runtime used to create and reopen ephemeral side chats.
  std::shared_ptr<AgentRuntime> runtime;
  /// Agent definition to use for follow-up questions about selected content.
  AgentId assistant;
  /// View-owned cache of side-chat drafts and handles. Leave empty when registering the service.
  std::shared_ptr<detail::SideChatDrafts> side_chats;
};

/// @brief Exact message location and destination passed to a link policy or activation callback.
struct AgentChatLink {
  /// Versioned message containing the link.
  MessageRef message;
  /// Content block in that message version.
  BlockId block;
  /// Parsed Markdown destination as supplied to both callbacks.
  std::string destination;
};

/// @brief Optional application service for explicit link activation in completed replies.
struct AgentChatLinkHandler {
  /// Pure policy check used during rendering and again at activation.
  std::function<bool(const AgentChatLink&)> allows;
  /// Synchronous UI-thread action invoked when an allowed link is activated.
  /// Return an error to show the diagnostic on the originating message.
  std::function<AgentResult<void>(const AgentChatLink&)> open;
};

/// @brief Exact message location and destination of a Markdown image.
struct AgentChatImage {
  /// Versioned message containing the image.
  MessageRef message;
  /// Content block in that message version.
  BlockId block;
  /// Parsed Markdown image source for the host to resolve.
  std::string destination;
};

/// @brief Optional service for asynchronously resolving images in completed messages.
struct AgentChatImageLoader {
  /// Returns a displayable ImageAsset or an empty asset to retain the Markdown alternative text.
  /// The view owns the task lifetime and cancels it when the image is unmounted.
  std::function<Task<ImageAsset>(AgentChatImage)> load;
};

/// @brief Optional application service for model avatars in AgentChat() and AgentChatMessages().
/// Register with ApplicationContext::Provide(). The callback receives the exact model registration
/// bound to the reply's Run and a theme foreground color for images that need tinting.
/// It runs synchronously during rendering. Return an empty result to use the default assistant avatar.
using AgentChatModelAvatar = std::function<std::optional<Image>(const ModelProviderRef&, Color)>;

/// @brief Display text and exact source slices for a composer quote or side-chat conclusion.
struct AgentChatCitation {
  /// First source slice in reading order.
  SourceSlice source;
  /// User-facing excerpt or conclusion text.
  std::string text;
  /// Further slices of the same quotation in reading order.
  std::vector<SourceSlice> additional_sources;
  /// Compares the stored fields for equality.
  bool operator==(const AgentChatCitation&) const = default;
};

namespace detail { struct ChatTextSelectionAccess; }

/// @brief A completed text selection in a mounted message view.
/// Hosts can anchor their own controls to the selection and resolve it before quoting or starting a side chat.
class AgentChatTextSelection {
public:
  /// State updated by AgentChatMessages() or AgentChat(). Empty means no selection is ready to use.
  using Binding = State<std::shared_ptr<const AgentChatTextSelection>>;

  /// Mounted control containing the active end of the selection.
  ViewHandle Anchor() const;

  /// Optional caret rectangle in the anchor's local coordinates.
  std::optional<Rect> Caret() const;

  /// Whether this selection ends a range started in another message or block.
  bool IsExtending() const;

  /// Resolves the text and ordered source slices under current session permissions.
  /// @return Display text and exact original message versions and UTF-8 ranges, or a selection error.
  Async<AgentChatCitation> ReadAsync() const;

  /// Returns display passages in reading order for a selection that has been successfully resolved.
  std::vector<std::string> Passages() const;

  /// Sets the active end as the start of an extended range, or cancels an existing range.
  void Extend() const;

  /// Clears the highlighted text and any extended range.
  void Clear() const;

  /// Restores this selection after temporary host navigation, while its source is still visible.
  void Restore() const;

private:
  struct Data;
  explicit AgentChatTextSelection(std::shared_ptr<const Data> data);

  std::shared_ptr<const Data> data_;
  friend struct detail::ChatTextSelectionAccess;
};

/// @brief Composer entry for an attachment staged by the host.
struct AgentChatAttachment {
  /// Staged attachment ID for SendInput::staged_attachments.
  ArtifactId id;
  /// Visible attachment name.
  std::string name;
};

/// @brief References and composer intent held until the user explicitly sends or queues input.
struct AgentChatDraftSources {
  /// Selected passage to include as a context reference.
  std::optional<AgentChatCitation> quote;
  /// Side-chat conclusion proposed for the main conversation.
  std::optional<AgentChatCitation> conclusion;
  /// Requests a new Run after a previous direct-send target closed.
  bool send_as_new_task = false;
  /// Retained staged attachment entries in display order.
  std::vector<std::shared_ptr<const AgentChatAttachment>> attachments;
  /// Compares the stored fields for equality.
  bool operator==(const AgentChatDraftSources&) const = default;
};

/// @brief Host integration for editing upcoming model and execution-profile choices.
struct AgentChatRunControls {
  /// Editable options for the next new Run or queue item.
  State<UserRunOptions> next;
  /// Renders controls with the session summary and any active or selected Run binding.
  std::function<View(const SessionSummary&, std::optional<RunStartBinding>)> render;
  /// Describes the binding captured by a pending or rejected queue item.
  std::function<std::string(const RunStartBinding&)> describe;
};

/// @brief Versioned approval binding for an independent review view.
struct AgentChatApprovalRef {
  /// Interaction owning the approval.
  InteractionId id;
  /// Version to check before enabling and submitting an approval.
  Revision version;
  /// Compares the stored fields for equality.
  bool operator==(const AgentChatApprovalRef&) const = default;
};

/// @brief Opens a specific artifact version in the corresponding review route.
struct AgentChatReviewRequest {
  /// Exact artifact version to present.
  ArtifactRef artifact;
  /// Host navigation token used to distinguish open requests.
  std::uint64_t ticket;
  /// Original approval binding, when the artifact was opened for approval.
  std::optional<AgentChatApprovalRef> approval;
  /// Optional history anchor for resolving an artifact from an older or inherited message.
  std::optional<Sequence> history_first;
  /// Compares the stored fields for equality.
  bool operator==(const AgentChatReviewRequest&) const = default;
};

/// @brief Navigation request that focuses an existing pending interaction.
struct AgentChatInteractionRequest {
  /// Interaction to display.
  InteractionId interaction;
  /// Host navigation token for this request.
  std::uint64_t ticket;
  /// Compares the stored fields for equality.
  bool operator==(const AgentChatInteractionRequest&) const = default;
};

/// @brief Active surface of the default conversation view.
using AgentChatPage = std::variant<std::monostate, InteractionId, AgentChatReviewRequest>;

/// @brief Optional layout and activity presentation for AgentChat().
/// Provide it with ProvideEnvironment() around a conversation or a group of conversations.
struct AgentChatLayoutStyle {
  /// Preferred conversation width in logical pixels. Use zero to fill the available width in the
  /// conversation, artifact reviews, and focused interaction pages.
  float content_width = 672.0F;
  /// Uses chat bubbles and the compact composer layout.
  bool chat_bubbles = false;
  /// Shows per-Run status, elapsed time, and collapsible activity from session events.
  bool show_run_activity = false;

  /// Returns the default AgentChat layout.
  static constexpr AgentChatLayoutStyle Default() noexcept { return {}; }

  /// Compares all presentation settings.
  bool operator==(const AgentChatLayoutStyle&) const = default;
};

/// @brief Lets an application supply the header for a visible artifact review.
/// Provide around AgentChat() when the app owns review navigation and actions.
struct AgentChatReviewHeader {
  /// Builds the header for the visible version and its review request. The back action is empty in a detached review.
  using Render = std::function<View(const ArtifactPreview&, const AgentChatReviewRequest&, std::function<void()>)>;
  /// Optional application-owned renderer. An empty pointer uses the default layout.
  std::shared_ptr<const Render> render;

  /// Keeps the default review header.
  static AgentChatReviewHeader Default() { return {}; }

  /// Compares the installed header renderers.
  bool operator==(const AgentChatReviewHeader&) const = default;
};

/// @brief Optional mount point for popups opened by host composer controls.
/// Read it with UseEnvironment() from AgentChatRunControls::render descendants. A standalone
/// host composer can provide its own anchor with ProvideEnvironment().
struct AgentChatComposerAnchor {
  /// Mounted composer area. If unavailable, controls may fall back to their own handle.
  std::optional<ViewHandle> view;

  /// Returns an empty anchor.
  static AgentChatComposerAnchor Default() { return {}; }

  /// Compares the anchor handles.
  bool operator==(const AgentChatComposerAnchor&) const = default;
};

/// @brief Optional UI integration points for AgentChat().
/// The default UI can render messages and standard interactions. Supply host views only for the artifacts,
/// forms or shell controls that require application-specific presentation.
struct AgentChatOptions {
  /// Returns a custom form for the snapshot, or empty to use the default form.
  std::function<std::optional<View>(const InteractionSnapshot&, std::shared_ptr<Conversation>)> render_form;
  /// Optional host-owned quotation, conclusion and attachment state shared across views.
  std::optional<State<AgentChatDraftSources>> draft_sources;
  /// Returns a full review for a validated preview and its originating Conversation.
  std::function<std::optional<View>(const ArtifactPreview&, std::shared_ptr<Conversation>,
      State<AgentChatDraftSources>)> render_artifact;
  /// Optional host-owned editor value, including selection and composition.
  std::optional<State<TextEditingValue>> draft;
  /// Optional model and execution-profile editor.
  std::optional<AgentChatRunControls> run_controls;
  /// Optional shared state for references recovered from rejected work.
  std::optional<State<std::vector<QueuedDraftReference>>> recovered_references;
  /// Optional shared state for selected skill versions.
  std::optional<State<std::vector<SkillRef>>> draft_skills;
  /// Optional host-driven request to open a specific artifact review.
  std::optional<AgentChatReviewRequest> review_request;
  /// Returns an inline preview for a validated artifact, or empty for the default behavior.
  std::function<std::optional<View>(const ArtifactPreview&, std::shared_ptr<Conversation>)> render_artifact_summary;
  /// Optional host-driven request to focus a particular pending interaction.
  std::optional<AgentChatInteractionRequest> interaction_request;
  /// Displays the requested review on its own surface.
  bool review_only = false;
  /// Reports the visible chat, interaction, or review so the host can compose its own navigation.
  std::optional<State<AgentChatPage>> page;
  /// Resolves a versioned agent reference to its visible name.
  std::function<std::string(const AgentRef&)> agent_label;
  /// Supplies a label for an artifact attached to an approval preview.
  std::function<std::string(const ApprovalPreview&, const ArtifactRef&)> review_attachment_label;
  /// Lets the host handle opening an artifact and retain optional return focus.
  /// Return true when the host consumes the navigation.
  std::function<bool(const ArtifactPreview&, std::optional<ViewHandle>)> open_artifact_review;
  /// Optional handle for focusing the composer from the host shell.
  std::optional<ViewHandle> composer_focus;
  /// Presents a block from the visible transcript, with an empty result selecting the default rendering.
  /// An expanded tool step also supplies a ToolBlock with its call ID, alongside the output or its history anchor.
  std::function<std::optional<View>(const Message&, const ContentBlock&, std::shared_ptr<Conversation>)> render_block;
  /// Resolves an exact tool version to a display label for Run activity.
  /// An empty callback or label falls back to the tool ID.
  std::function<std::string(const ToolRef&)> tool_label;
  /// When bound, reports selections for host-supplied controls in place of the default selection menu.
  std::optional<AgentChatTextSelection::Binding> selection;
};

/// @brief Builds the default conversation view with history, interactions, selection and a composer.
/// @param conversation Open conversation obtained through SessionService::OpenAsync().
/// @param options Optional host view, navigation and composer bindings.
/// @return A composable view that owns its observation for the lifetime of the mounted conversation.
/// @see Conversation::ObserveAsync(), AgentChatSelection, AgentChatOptions
View AgentChat(std::shared_ptr<Conversation> conversation, AgentChatOptions options = {});

/// @brief Builds a standalone virtualized message view and observes the conversation while it is mounted.
/// @param conversation Open conversation whose history and selection permissions apply.
/// @param selection Optional output for an application-owned selection menu.
View AgentChatMessages(std::shared_ptr<Conversation> conversation,
                       std::optional<AgentChatTextSelection::Binding> selection = {});

/// @brief Builds a standalone composer with send, stop and queue controls.
/// @param conversation Open conversation used to send the draft and observe the current Run.
/// @param draft Host-owned text, selection and IME composition.
/// @param sources Optional host-owned quotations, conclusions and staged attachments.
/// @param focus Optional handle for setting editor focus.
/// @param run_controls Optional host controls for the next model and execution profile.
View AgentChatComposer(std::shared_ptr<Conversation> conversation, State<TextEditingValue> draft,
                       std::optional<State<AgentChatDraftSources>> sources = {}, std::optional<ViewHandle> focus = {},
                       std::optional<AgentChatRunControls> run_controls = {});

/// @brief Builds quotation and side-chat actions for a selected artifact source.
/// @param conversation Open conversation whose permissions apply to the source read.
/// @param source Versioned selection to resolve. Empty hides the actions.
/// @param draft Composer state that receives the quote or side-chat conclusion.
/// @param on_citation_added Optional notification after the composer state is updated.
/// @return The actions view. Side chat needs the application service AgentChatSelection.
/// @see AgentChatSelection, Conversation::ReadSelectionAsync()
View AgentChatSourceActions(std::shared_ptr<Conversation> conversation, std::optional<SourceSlice> source,
                            State<AgentChatDraftSources> draft, std::function<void()> on_citation_added = {});

/// @brief Builds the composer chips and removal actions for staged attachments.
/// @param draft Composer state whose attachment entries the chips display and edit.
/// @return A view with one removable chip per staged attachment.
View AgentChatAttachmentChips(State<AgentChatDraftSources> draft);

/// @brief Builds selectable text for an exact artifact part and shares resulting references with the supplied draft.
/// @param conversation Open conversation whose permissions apply to the source read.
/// @param artifact Version of the artifact being displayed.
/// @param part Part that owns the displayed text.
/// @param text Canonical UTF-8 text of that part, used to derive the source byte offsets.
/// @param draft Composer state that receives a quote or conclusion from the selection.
/// @return A selectable text view that keeps the supplied artifact and part as its citation source.
/// @see AgentChatSourceActions(), Conversation::ReadSelectionAsync()
View AgentChatArtifactText(std::shared_ptr<Conversation> conversation, ArtifactRef artifact, PartId part,
                           std::string text, State<AgentChatDraftSources> draft);

} // namespace huxerui::agentkit
