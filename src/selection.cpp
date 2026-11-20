#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

constexpr std::size_t selection_limit = 1 << 20;

bool Boundary(std::string_view text, std::uint64_t offset) {
  return offset == text.size() ||
      (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) != 0x80);
}

AgentResult<std::string> InvalidSelection(std::string path, std::string message) {
  auto error = Error(ErrorCode::InvalidArgument, message);
  error.fields.push_back({std::move(path), "invalid_selection", std::move(message)});
  return AgentResult<std::string>::Failure(std::move(error));
}

AgentResult<std::string> SliceText(std::string text, const std::optional<SourceTextRange>& range) {
  if (text.size() > selection_limit) return Fail<std::string>(ErrorCode::BudgetExceeded, "Selection exceeds its byte limit");
  if (!ValidUtf8(text)) return Fail<std::string>(ErrorCode::InvalidArgument, "Selection text is not UTF-8");
  if (range) {
    if (range->begin_utf8 >= range->end_utf8 || !Boundary(text, range->begin_utf8) || !Boundary(text, range->end_utf8))
      return InvalidSelection("$.text_range", "Selection offsets must span complete UTF-8 characters");
    text = text.substr(range->begin_utf8, range->end_utf8 - range->begin_utf8);
  }
  if (text.empty()) return Fail<std::string>(ErrorCode::Unsupported, "Selection has no readable text");
  return AgentResult<std::string>::Success(std::move(text));
}

const Message* FindMessageVersion(const SessionData& source, const MessageRef& ref) {
  const auto found = std::ranges::find_if(source.messages, [&](const auto& message) {
    return message.ref.session == ref.session && message.ref.message == ref.message;
  });
  return found == source.messages.end() ? nullptr : &*found;
}

} // namespace

std::vector<SideChatConclusion> MessageConclusions(const Message& message) {
  std::vector<SideChatConclusion> result;
  for (const auto& block : message.blocks) {
    if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body)) result.push_back(*conclusion);
  }
  return result;
}

Async<std::string> RuntimeState::ResolveSelection(SessionId id, ActionParty actor, SourceSlice slice,
                                                   std::optional<ModelId> model) {
  using Result = AgentResult<std::string>;
  const auto epoch = access_revision;
  if (slice.text_range && !slice.parts.empty())
    co_return InvalidSelection("$.parts", "A selection uses either parts or a text range");
  auto source = sessions.find(id);
  const auto published = [&](const auto& record) -> const SessionData* {
    return record == sessions.end() || record->second.summary.lifecycle == SessionLifecycle::Deleting
        ? nullptr : Published(record->second);
  };
  if (!published(source)) co_return Fail<std::string>(ErrorCode::NotFound, "Selection source is unavailable");
  auto authorize = [&](const SessionData& target) -> Async<void> {
    auto read = co_await Require(CollaborationAccess(actor, Capability::ReadSession, target));
    if (!read.Succeeded()) co_return read;
    auto policy = CollaborationAccess(actor, Capability::ReadContext, target);
    policy.sources = {slice};
    read = co_await Require(policy);
    if (!read.Succeeded() || !model) co_return read;
    policy.capability = Capability::SendToModel;
    policy.recipient_model = model;
    co_return co_await Require(std::move(policy));
  };
  auto allowed = co_await authorize(source->second);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  std::string text;
  if (const auto* ref = std::get_if<MessageRef>(&slice.source)) {
    if (!ref->version || !slice.parts.empty() ||
        (slice.text_range && !std::holds_alternative<BlockId>(slice.text_range->leaf)))
      co_return InvalidSelection("$.text_range.leaf", "A message range requires a text block and version");
    auto origin = sessions.find(ref->session);
    if (!published(origin)) co_return Fail<std::string>(ErrorCode::NotFound, "Message source is unavailable");
    if (origin != source) {
      allowed = co_await authorize(origin->second);
      if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    }
    const auto* image = published(source);
    const auto* original = published(origin);
    if (!image || !original || !FindMessageVersion(*image, *ref))
      co_return Fail<std::string>(ErrorCode::NotFound, "Message is outside the selected conversation");
    const auto* message = FindMessageVersion(*original, *ref);
    if (!message || message->ref != *ref)
      co_return Fail<std::string>(ErrorCode::Conflict, "The selected message version is unavailable");
    if (message->author == MessageAuthor::Agent && original->origin.kind == SessionOriginKind::SideChat) {
      const auto& side = original->origin;
      if (!side.source_session || !side.creator || side.creator->principal != actor.principal)
        co_return Fail<std::string>(ErrorCode::NotFound, "Side chat reply is unavailable");
      const auto source_session = *side.source_session;
      const auto selected = side.side_chat_selection;
      for (const auto& part : selected) {
        auto readable = co_await ResolveSelection(source_session, actor, part, model);
        if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
      }
      image = published(source);
      original = published(origin);
      message = original ? FindMessageVersion(*original, *ref) : nullptr;
      if (!image || !FindMessageVersion(*image, *ref) || !message || message->ref != *ref)
        co_return Fail<std::string>(ErrorCode::Conflict, "The selected side chat reply changed while checking its source");
    }
    if (message->state != MessageState::Complete)
      co_return Fail<std::string>(ErrorCode::InvalidState, "Wait for the message to finish before referencing it");
    const auto* leaf = slice.text_range ? std::get_if<BlockId>(&slice.text_range->leaf) : nullptr;
    for (const auto& block : message->blocks) {
      if (leaf && block.id != *leaf) continue;
      const auto* body = std::get_if<TextBlock>(&block.body);
      if (!body) continue;
      if (!text.empty()) text.push_back('\n');
      text += body->text;
      if (text.size() > selection_limit)
        co_return Fail<std::string>(ErrorCode::BudgetExceeded, "Selection exceeds its byte limit");
    }
    if (text.empty() && leaf)
      co_return InvalidSelection("$.text_range.leaf", "The message text block is unavailable");
  } else if (const auto* ref = std::get_if<ArtifactRef>(&slice.source)) {
    const auto* image = published(source);
    if (!image) co_return Fail<std::string>(ErrorCode::NotFound, "Selection source is unavailable");
    auto stored = co_await ReadArtifact(*ref, actor);
    if (!stored.Succeeded()) co_return Result::Failure(stored.Error());
    image = published(source);
    if (!image || (stored.Value().record.session != id && !InheritedArtifact(*image, *ref)))
      co_return Fail<std::string>(ErrorCode::NotFound, "Artifact is outside the selected conversation");
    auto parts = slice.parts;
    if (slice.text_range) {
      const auto* leaf = std::get_if<PartId>(&slice.text_range->leaf);
      if (!leaf) co_return InvalidSelection("$.text_range.leaf", "An artifact range requires a part");
      parts.push_back(*leaf);
    }
    if (std::ranges::any_of(parts, [&](const auto& part) {
          return std::ranges::find(stored.Value().record.parts, part, &ArtifactPart::id) == stored.Value().record.parts.end();
        })) co_return InvalidSelection("$.parts", "The artifact part is unavailable");
    if (!options.artifact_store.selection)
      co_return Fail<std::string>(ErrorCode::Unsupported, "The host cannot resolve the selected artifact parts");
    ArtifactSelectionRequest request{{actor, *ref, stored.Value().handle}, std::move(parts), selection_limit};
    if (epoch != access_revision)
      co_return Fail<std::string>(ErrorCode::Conflict, "Selection access changed");
    auto resolved = co_await options.artifact_store.selection(std::move(request));
    if (!resolved.Succeeded()) co_return resolved;
    stored = co_await ReadArtifact(*ref, actor);
    if (!stored.Succeeded()) co_return Result::Failure(stored.Error());
    allowed = co_await authorize(source->second);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    text = std::move(resolved).Value();
  } else {
    co_return Fail<std::string>(ErrorCode::Unsupported, "This selection source requires a host adapter");
  }
  const auto current = User();
  if (!current.Succeeded() || current.Value() != actor.principal)
    co_return Fail<std::string>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (epoch != access_revision || !published(source))
    co_return Fail<std::string>(ErrorCode::Conflict, "Selection access changed");
  co_return SliceText(std::move(text), slice.text_range);
}

Async<void> RuntimeState::PrepareSelectedContext(const SessionData& session, const RunExecution& run,
                                                    ModelTurnRequest& request) {
  const auto actor = Request(run.principal, Capability::SendToModel, &session, &run).actor;
  const auto byte_limit = RunBudget(run).context_bytes.value_or(4 << 20);
  std::size_t bytes = 0;
  std::vector<ModelItem> context;
  if (session.origin.kind == SessionOriginKind::SideChat) {
    request.continuation.reset();
    request.allow_server_state = false;
    constexpr std::string_view label = "Selected passage:\n";
    const auto slices = session.origin.side_chat_selection;
    for (const auto& slice : slices) {
      auto text = co_await ResolveSelection(*session.origin.source_session, actor, slice, run.view.model.id);
      if (!text.Succeeded()) co_return AgentResult<void>::Failure(text.Error());
      bytes += label.size() + text.Value().size();
      if (bytes > byte_limit) co_return Fail<void>(ErrorCode::BudgetExceeded, "Side chat exceeds the context budget");
      context.emplace_back(ModelContent{ModelContent::Role::Context, std::string(label) + std::move(text).Value()});
    }
  }
  const auto selected_passages = context.size();
  std::vector<SideChatConclusion> conclusions;
  for (const auto& message : session.messages) {
    if (message.ref.session != session.summary.id || message.state != MessageState::Complete) continue;
    if (message.run) {
      const auto owner = runs.find(*message.run);
      if (owner != runs.end() && std::ranges::any_of(owner->second.view.steering, [&](const auto& steer) {
            return steer.message == message.ref.message && steer.state != SteerState::Applied;
          })) continue;
    }
    for (const auto& block : message.blocks) {
      if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body)) conclusions.push_back(*conclusion);
    }
  }
  if (!conclusions.empty()) {
    request.continuation.reset();
    request.allow_server_state = false;
  }
  constexpr std::string_view conclusion_label = "Attached side-chat conclusion:\n";
  for (const auto& conclusion : conclusions) {
    std::vector<SideChatConclusion> item{conclusion};
    auto allowed = co_await CheckConclusions(session.summary.id, actor, std::move(item), run.view.model.id);
    if (!allowed.Succeeded()) continue;
    bytes += conclusion_label.size() + conclusion.text.size();
    if (bytes > byte_limit)
      co_return Fail<void>(ErrorCode::BudgetExceeded, "Side chat conclusions exceed the context budget");
    context.emplace_back(ModelContent{ModelContent::Role::Context, std::string(conclusion_label) + conclusion.text});
  }
  if (context.size() > selected_passages) {
    if (!request.instructions.empty()) request.instructions += "\n\n";
    request.instructions += "Attached side-chat conclusions appear immediately before the latest request, each labeled "
                            "\"Attached side-chat conclusion:\". Use their bodies as quoted source material when the request "
                            "refers to the attachment.";
  }
  if (!context.empty()) {
    const auto user = std::ranges::find_if(request.items.rbegin(), request.items.rend(), [](const ModelItem& item) {
      const auto* content = std::get_if<ModelContent>(&item);
      return content && content->role == ModelContent::Role::User;
    });
    const auto insertion = user == request.items.rend() ? request.items.end() : std::prev(user.base());
    request.items.insert(insertion, std::make_move_iterator(context.begin()), std::make_move_iterator(context.end()));
  }
  co_return AgentResult<void>::Success();
}

Async<void> RuntimeState::ValidateUserSources(const SessionData& session, PrincipalId user,
                                                std::vector<SourceSlice> sources, ModelId model) {
  if (session.origin.kind == SessionOriginKind::SideChat && !sources.empty())
    co_return Fail<void>(ErrorCode::PermissionDenied, "Side chat uses its original selection and conversation history");
  const auto actor = Request(user, Capability::SubmitInput, &session).actor;
  std::vector<SourceSlice> whole;
  for (const auto& slice : sources) {
    if (!std::holds_alternative<ResourceRef>(slice.source) &&
        (std::holds_alternative<MessageRef>(slice.source) || !slice.parts.empty() || slice.text_range)) {
      auto selection = co_await ResolveSelection(session.summary.id, actor, slice, model);
      if (!selection.Succeeded()) co_return AgentResult<void>::Failure(selection.Error());
    } else {
      whole.push_back(slice);
    }
  }
  co_return co_await ValidateInputSources(session, actor, whole);
}

Async<void> RuntimeState::CheckConclusions(SessionId id, ActionParty actor, std::vector<SideChatConclusion> conclusions,
                                            std::optional<ModelId> model) {
  for (const auto& conclusion : conclusions) {
    for (const auto& slice : conclusion.sources) {
      auto source = co_await ResolveSelection(id, actor, slice, model);
      if (!source.Succeeded()) co_return AgentResult<void>::Failure(source.Error());
    }
  }
  co_return AgentResult<void>::Success();
}

AgentResult<const QueuedInput*> RuntimeState::RejectedInput(
    const SessionId& id, const PrincipalId& user, const QueueItemId& item) const {
  const auto session = sessions.find(id);
  const auto* image = session == sessions.end() ? nullptr : Published(session->second);
  if (!image || image->summary.lifecycle == SessionLifecycle::Deleting)
    return Fail<const QueuedInput*>(ErrorCode::NotFound, "Queued input is unavailable");
  const auto owner = image->queue_owners.find(item);
  const auto found = std::ranges::find(image->queue, item, &QueuedInput::id);
  if (owner == image->queue_owners.end() || owner->second != user || found == image->queue.end() ||
      !std::holds_alternative<SendInput>(found->input)) {
    return Fail<const QueuedInput*>(ErrorCode::NotFound, "Queued input is unavailable");
  }
  if (found->state != QueueItemState::Rejected)
    return Fail<const QueuedInput*>(ErrorCode::InvalidState, "Only rejected input can be recovered");
  return AgentResult<const QueuedInput*>::Success(&*found);
}

Async<QueuedDraft> RuntimeState::ReadQueuedDraft(SessionId id, PrincipalId user, QueueItemId item) {
  auto access = co_await WithSession(id, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<QueuedDraft>::Failure(access.Error());
  const auto found = RejectedInput(id, user, item);
  if (!found.Succeeded()) co_return AgentResult<QueuedDraft>::Failure(found.Error());
  co_return co_await RecoverDraft(std::move(id), std::move(user), std::get<SendInput>(found.Value()->input),
      found.Value()->conclusions, std::move(item));
}

AgentResult<std::pair<const SendInput*, const Message*>> RuntimeState::UnappliedSteer(
    const SessionId& id, const PrincipalId& user, const CommandId& command) const {
  using Input = std::pair<const SendInput*, const Message*>;
  const auto session = sessions.find(id);
  const auto* image = session == sessions.end() ? nullptr : Published(session->second);
  if (!image || image->summary.lifecycle == SessionLifecycle::Deleting)
    return Fail<Input>(ErrorCode::NotFound, "Supplement is unavailable");
  const CommandRecord* record = nullptr;
  if (image->retention == SessionRetention::Stored) {
    const auto& persisted = saved_sessions.at(id).image.commands;
    const auto found = std::ranges::find_if(persisted, [&](const StoredCommand& candidate) {
      return candidate.principal == user && candidate.record.progress.accepted.command == command;
    });
    if (found != persisted.end()) record = &found->record;
  } else if (const auto found = commands.find({user, command}); found != commands.end()) {
    record = &found->second;
  }
  const auto* steer = record && record->progress.accepted.session == id ? std::get_if<RunSteered>(&record->accepted) : nullptr;
  if (!steer) return Fail<Input>(ErrorCode::NotFound, "Supplement is unavailable");
  if (record->progress.state != CommandState::Failed)
    return Fail<Input>(ErrorCode::InvalidState, "Only unapplied supplements can be recovered");
  const auto message = std::ranges::find_if(image->messages, [&](const Message& candidate) {
    return candidate.ref.session == id && candidate.ref.message == steer->message &&
        candidate.run == steer->run && candidate.author == MessageAuthor::User;
  });
  if (message == image->messages.end()) return Fail<Input>(ErrorCode::NotFound, "Supplement is unavailable");
  const auto* input = record->input.input ? &*record->input.input : nullptr;
  if (record->rendered_steer_input) input = &*record->rendered_steer_input;
  else if (record->plan && record->plan->rendered_action) {
    if (const auto* prepared = std::get_if<SteerRequest>(&record->plan->rendered_action->request)) input = &prepared->input;
  }
  return AgentResult<Input>::Success({input, &*message});
}

Async<RecoveredDraft> RuntimeState::ReadUnappliedSteerDraft(SessionId id, PrincipalId user, CommandId command) {
  using Result = AgentResult<RecoveredDraft>;
  auto access = co_await WithSession(id, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  const auto found = UnappliedSteer(id, user, command);
  if (!found.Succeeded()) co_return Result::Failure(found.Error());
  const auto [original, message] = found.Value();
  SendInput input;
  if (original) input = *original;
  else if (!message->blocks.empty()) {
    if (const auto* text = std::get_if<TextBlock>(&message->blocks.front().body)) input.text = text->text;
  }
  const bool incomplete = !original;
  auto restored = co_await RecoverDraft(std::move(id), std::move(user), std::move(input),
      MessageConclusions(*message), std::move(command));
  if (restored.Succeeded() && incomplete)
    restored.Value().references.push_back({{}, "The original references and Skills were not recorded"});
  co_return restored;
}

Async<RecoveredDraft> RuntimeState::RecoverDraft(SessionId id, PrincipalId user, SendInput input,
    std::vector<SideChatConclusion> conclusions, std::variant<QueueItemId, CommandId> origin) {
  using Result = AgentResult<RecoveredDraft>;
  const auto epoch = access_revision;
  const auto* item = std::get_if<QueueItemId>(&origin);
  const auto label = item ? std::string("Queued") : std::string("Original");
  RecoveredDraft draft{.text = std::move(input.text), .skills = std::move(input.skills)};
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = user};
  for (const auto& ref : input.attachments) {
    std::vector<SourceSlice> sources{{ref}};
    auto allowed = co_await ValidateInputSources(sessions.at(id), actor, std::move(sources));
    if (allowed.Succeeded()) {
      auto read = co_await ReadHistoryArtifact(sessions.at(id), actor, ref);
      if (read.Succeeded()) {
        draft.references.push_back({ref, read.Value().record.metadata.name});
        continue;
      }
    }
    draft.references.push_back({{}, label + " attachment is unavailable"});
  }
  for (const auto& slice : input.context) {
    auto text = co_await ResolveSelection(id, actor, slice);
    if (text.Succeeded()) {
      draft.references.push_back({slice, std::move(text).Value()});
    } else if (text.Error().code == ErrorCode::Unsupported &&
               (std::holds_alternative<ResourceRef>(slice.source) ||
                (!slice.text_range && slice.parts.empty() && !std::holds_alternative<MessageRef>(slice.source)))) {
      std::vector<SourceSlice> whole{slice};
      auto readable = co_await ValidateInputSources(sessions.at(id), actor, std::move(whole));
      draft.references.push_back(readable.Succeeded() ? QueuedDraftReference{slice,
          std::holds_alternative<ArtifactRef>(slice.source) ? "Artifact reference" :
              std::holds_alternative<ResourceRef>(slice.source) ? "Workspace reference" : "Shared project context"}
          : QueuedDraftReference{{}, label + " reference is unavailable"});
    } else {
      draft.references.push_back({{}, label + " reference is unavailable"});
    }
  }
  for (std::size_t index = 0; index < conclusions.size(); ++index) {
    const auto& conclusion = conclusions[index];
    std::vector<SideChatConclusion> one{conclusion};
    auto readable = co_await CheckConclusions(id, actor, std::move(one), std::nullopt);
    if (!readable.Succeeded()) {
      draft.references.push_back({{}, label + " conclusion is unavailable"});
    } else if (item) {
      draft.references.push_back({QueuedConclusionRef{*item, index}, conclusion.text});
    } else {
      draft.references.push_back({UnappliedSteerConclusionRef{std::get<CommandId>(origin), index}, conclusion.text});
    }
  }
  const auto current = User();
  if (!current.Succeeded() || current.Value() != user)
    co_return Fail<RecoveredDraft>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (epoch != access_revision)
    co_return Fail<RecoveredDraft>(ErrorCode::Conflict, "References changed access while being read");
  if (item) {
    const auto found = RejectedInput(id, user, *item);
    if (!found.Succeeded()) co_return Result::Failure(found.Error());
  } else {
    const auto found = UnappliedSteer(id, user, std::get<CommandId>(origin));
    if (!found.Succeeded()) co_return Result::Failure(found.Error());
  }
  co_return Result::Success(std::move(draft));
}

Async<std::vector<SideChatConclusion>> RuntimeState::ResolveConclusions(SessionId id, PrincipalId user,
    const SendInput& send, ModelId model) {
  using Result = AgentResult<std::vector<SideChatConclusion>>;
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = user};
  const auto epoch = access_revision;
  std::vector<SideChatConclusion> conclusions;
  std::size_t bytes = 0;
  for (const auto& slice : send.side_chat_conclusions) {
    const auto* ref = std::get_if<MessageRef>(&slice.source);
    if (!ref) co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::InvalidArgument, "Select a side chat reply");
    const auto child = sessions.find(ref->session);
    const auto* image = child == sessions.end() ? nullptr : Published(child->second);
    if (!image || image->origin.kind != SessionOriginKind::SideChat || image->origin.source_session != id ||
        !image->origin.creator || image->origin.creator->principal != user)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::NotFound, "Side chat reply is unavailable");
    const auto* message = FindMessageVersion(*image, *ref);
    if (!message || message->author != MessageAuthor::Agent)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::InvalidArgument, "Select a completed assistant reply");
    auto sources = image->origin.side_chat_selection;
    auto reply = co_await ResolveSelection(ref->session, actor, slice);
    if (!reply.Succeeded()) co_return Result::Failure(reply.Error());
    bytes += reply.Value().size();
    if (bytes > selection_limit)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::BudgetExceeded, "Selected replies exceed the byte limit");
    conclusions.push_back({std::move(reply).Value(), std::move(sources)});
  }
  std::set<std::pair<QueueItemId, std::uint64_t>> reused;
  for (const auto& ref : send.recovered_conclusions) {
    auto input = RejectedInput(id, user, ref.item);
    if (!input.Succeeded()) co_return Result::Failure(input.Error());
    if (ref.index >= input.Value()->conclusions.size())
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::NotFound, "Queued conclusion is unavailable");
    if (!reused.emplace(ref.item, ref.index).second)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::InvalidArgument, "A recovered conclusion was selected more than once");
    const auto& conclusion = input.Value()->conclusions[static_cast<std::size_t>(ref.index)];
    if (conclusion.text.size() > selection_limit - bytes)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::BudgetExceeded, "Selected replies exceed the byte limit");
    bytes += conclusion.text.size();
    conclusions.push_back(conclusion);
  }
  std::set<std::pair<CommandId, std::uint64_t>> reused_steers;
  for (const auto& ref : send.recovered_steer_conclusions) {
    const auto input = UnappliedSteer(id, user, ref.command);
    if (!input.Succeeded()) co_return Result::Failure(input.Error());
    const auto original = MessageConclusions(*input.Value().second);
    if (ref.index >= original.size())
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::NotFound, "Original conclusion is unavailable");
    if (!reused_steers.emplace(ref.command, ref.index).second)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::InvalidArgument, "A recovered conclusion was selected more than once");
    const auto& conclusion = original[static_cast<std::size_t>(ref.index)];
    if (conclusion.text.size() > selection_limit - bytes)
      co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::BudgetExceeded, "Selected replies exceed the byte limit");
    bytes += conclusion.text.size();
    conclusions.push_back(conclusion);
  }
  auto allowed = co_await CheckConclusions(id, actor, conclusions, model);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  if (!conclusions.empty() && epoch != access_revision)
    co_return Fail<std::vector<SideChatConclusion>>(ErrorCode::Conflict, "Selection access changed before submission");
  co_return Result::Success(std::move(conclusions));
}

Async<std::vector<SourceSlice>> RuntimeState::VisibleSelections(SessionId id, PrincipalId user) {
  const auto* image = Published(sessions.at(id));
  std::vector<SourceSlice> visible;
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = user};
  std::vector<SourceSlice> candidates;
  if (!image) co_return AgentResult<std::vector<SourceSlice>>::Success(std::move(visible));
  const bool side_chat = image->origin.kind == SessionOriginKind::SideChat;
  const auto source_id = side_chat ? *image->origin.source_session : id;
  if (side_chat) candidates = image->origin.side_chat_selection;
  auto collect = [&](const SideChatConclusion& conclusion) {
    for (const auto& slice : conclusion.sources) {
      if (std::ranges::find(candidates, slice) == candidates.end()) candidates.push_back(slice);
    }
  };
  for (const auto& message : image->messages) {
    for (const auto& block : message.blocks) {
      if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body)) collect(*conclusion);
    }
  }
  for (const auto& queued : image->queue) {
    const auto owner = image->queue_owners.find(queued.id);
    if (owner != image->queue_owners.end() && owner->second == user) {
      for (const auto& conclusion : queued.conclusions) collect(conclusion);
    }
  }
  for (const auto& slice : candidates) {
    auto text = co_await ResolveSelection(source_id, actor, slice);
    if (text.Succeeded()) visible.push_back(slice);
  }
  co_return AgentResult<std::vector<SourceSlice>>::Success(std::move(visible));
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

Async<std::vector<std::string>> Conversation::ReadSelectionsAsync(std::vector<SourceSlice> selection) {
  return [](std::shared_ptr<detail::RuntimeState> runtime, SessionId id, PrincipalId user,
            std::vector<SourceSlice> slices) -> Async<std::vector<std::string>> {
    using Result = AgentResult<std::vector<std::string>>;
    if (slices.empty() || slices.size() > 64)
      co_return Result::Failure(detail::Error(ErrorCode::InvalidArgument, "Select between one and 64 text fragments"));
    const auto epoch = runtime->access_revision;
    auto allowed = co_await runtime->WithSession(id, user, Capability::ReadSession);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    const ActionParty actor{.kind = ActionParty::Kind::User, .principal = user};
    std::vector<std::string> text;
    text.reserve(slices.size());
    auto remaining = detail::selection_limit;
    for (auto& slice : slices) {
      auto part = co_await runtime->ResolveSelection(id, actor, std::move(slice));
      if (!part.Succeeded()) co_return Result::Failure(part.Error());
      if (runtime->access_revision != epoch)
        co_return Result::Failure(detail::Error(ErrorCode::Conflict, "Selection access changed"));
      if (part.Value().size() > remaining)
        co_return Result::Failure(detail::Error(ErrorCode::BudgetExceeded, "Selection exceeds its byte limit"));
      remaining -= part.Value().size();
      text.push_back(std::move(part).Value());
    }
    co_return Result::Success(std::move(text));
  }(state_, id_, principal_, std::move(selection));
}

Async<QueuedDraft> Conversation::ReadQueuedDraftAsync(QueueItemId item) {
  return [](std::shared_ptr<detail::RuntimeState> runtime, SessionId id, PrincipalId user, QueueItemId item)
      -> Async<QueuedDraft> {
    co_return co_await runtime->ReadQueuedDraft(std::move(id), std::move(user), std::move(item));
  }(state_, id_, principal_, std::move(item));
}

Async<RecoveredDraft> Conversation::ReadUnappliedSteerDraftAsync(CommandId command) {
  return [](std::shared_ptr<detail::RuntimeState> runtime, SessionId id, PrincipalId user, CommandId command)
      -> Async<RecoveredDraft> {
    co_return co_await runtime->ReadUnappliedSteerDraft(std::move(id), std::move(user), std::move(command));
  }(state_, id_, principal_, std::move(command));
}

} // namespace huxerui::agentkit
