#pragma once

/// @file
/// @brief Convenience entry point for AgentRuntime, model adapters and the default conversation UI.
/// Include `huxerui/agentkit.h` and link `HuxerUI::AgentKit`. Create and start AgentRuntime in an application
/// scope. Open a Conversation through SessionService, then pass it to AgentChat() or bind your own views to its snapshots.

#include <huxerui/agentkit/runtime.h>
#include <huxerui/agentkit/providers.h>
#include <huxerui/view.h>

namespace huxerui::agentkit {

/// @brief Application-hook entry point for AgentKit setup.
/// @param context Owning application context passed to the installation hook.
/// @note The hook currently performs no registration. Register host services and create the runtime in the application scope.
void Install(ApplicationContext& context);

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

/// @brief Optional UI integration points for AgentChat().
/// The default UI can render messages and standard interactions. Supply host views only for the artifacts,
/// forms or shell controls that require application-specific presentation.
struct AgentChatOptions {
  /// Returns a custom form for the snapshot, or empty to use the default form.
  std::function<std::optional<View>(const InteractionSnapshot&, std::shared_ptr<Conversation>)> render_form;
  /// Receives a selected or complete side-chat reply and its source, for updating the originating draft.
  std::function<void(SourceSlice, std::string)> bring_back;
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
  /// Opens a detached review while retaining its version, approval and history bindings.
  std::function<void(AgentChatReviewRequest)> open_review_window;
  /// Destination-specific callback for closing a detached review.
  std::function<void()> close_review_window;
  /// Resolves a versioned agent reference to its visible name.
  std::function<std::string(const AgentRef&)> agent_label;
  /// Supplies a label for an artifact attached to an approval preview.
  std::function<std::string(const ApprovalPreview&, const ArtifactRef&)> review_attachment_label;
  /// Lets the host handle opening an artifact and retain optional return focus.
  /// Return true when the host consumes the navigation.
  std::function<bool(const ArtifactPreview&, std::optional<ViewHandle>)> open_artifact_review;
  /// Extra view placed in the conversation header.
  std::optional<View> header_actions;
  /// Optional handle for focusing the composer from the host shell.
  std::optional<ViewHandle> composer_focus;
  /// Returns a custom view for a transcript text block, or empty for the default renderer.
  std::function<std::optional<View>(const Message&, const ContentBlock&)> render_text_block;
};

/// @brief Builds the default conversation view with history, interactions, selection and a composer.
/// @param conversation Open conversation obtained through SessionService::OpenAsync().
/// @param options Optional host view, navigation and composer bindings.
/// @return A composable view that owns its observation for the lifetime of the mounted conversation.
/// @see Conversation::ObserveAsync(), AgentChatSelection, AgentChatOptions
View AgentChat(std::shared_ptr<Conversation> conversation, AgentChatOptions options = {});

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
