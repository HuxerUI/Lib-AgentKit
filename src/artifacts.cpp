#include "runtime_internal.h"

namespace huxerui::agentkit {
namespace detail {
namespace {

const StoredArtifact* FindArtifact(const SessionData& session, const ArtifactRef& ref) {
  auto found = std::ranges::find_if(session.artifacts, [&](const auto& item) {
    return item.record.metadata.ref == ref;
  });
  return found == session.artifacts.end() ? nullptr : &*found;
}

bool IncludesPart(const SourceSlice& slice, const PartId& part) {
  if (slice.text_range) {
    const auto* leaf = std::get_if<PartId>(&slice.text_range->leaf);
    if (leaf && *leaf != part) return false;
  }
  return slice.parts.empty() || std::ranges::find(slice.parts, part) != slice.parts.end();
}

std::optional<bool> MatchSource(const SourceSlice& changed, const SourceSlice& input) {
  if (changed.source != input.source) return {};
  if (!changed.parts.empty() && std::ranges::none_of(changed.parts, [&](const auto& part) {
        return IncludesPart(input, part);
      })) return {};
  if (changed.text_range) {
    if (const auto* part = std::get_if<PartId>(&changed.text_range->leaf); part && !IncludesPart(input, *part)) return {};
  }
  if (input.text_range) {
    if (const auto* part = std::get_if<PartId>(&input.text_range->leaf); part && !IncludesPart(changed, *part)) return {};
  }
  if (changed.text_range && input.text_range) {
    if (changed.text_range->leaf != input.text_range->leaf ||
        changed.text_range->end_utf8 <= input.text_range->begin_utf8 ||
        input.text_range->end_utf8 <= changed.text_range->begin_utf8) return {};
  }
  return (!changed.parts.empty() && input.parts.empty() && !input.text_range) ||
      (changed.text_range && !input.text_range);
}

struct ImpactStep {
  SourceSlice changed;
  std::vector<SourceSlice> path;
  bool approximate = false;
};

using ArtifactKey = std::pair<ArtifactId, Revision>;

bool SupportsAttachment(ModelCapabilities caps, std::string_view type) {
  const auto slash = type.find('/');
  if (slash == type.npos || slash + 1 == type.size() ||
      type.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789!#$&^_.+-", slash + 1) != type.npos) return false;
  const auto category = type.substr(0, slash);
  return category == "image" ? caps.image_input :
      caps.file_input && (category == "application" || category == "text" || category == "audio" || category == "video");
}

AgentResult<void> CheckSourceChange(const SourceChange& change) {
  switch (change.kind) {
  case SourceChangeKind::Withdrawn:
    if (!change.after) return AgentResult<void>::Success();
    return Fail<void>(ErrorCode::InvalidArgument, "A withdrawn source has no replacement");
  case SourceChangeKind::Modified:
  case SourceChangeKind::Reverted:
    if (change.after && *change.after == change.before.source)
      return Fail<void>(ErrorCode::InvalidArgument, "A source change requires a different replacement version");
    return AgentResult<void>::Success();
  }
  return Fail<void>(ErrorCode::InvalidArgument, "Unknown source change kind");
}

} // namespace

Async<StoredArtifact> RuntimeState::ReadArtifact(ArtifactRef ref) {
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(user.Error());
  co_return co_await ReadArtifact(std::move(ref), {.kind = ActionParty::Kind::User, .principal = user.Value()});
}

void CollectArtifactRefs(ArtifactVersions& refs, const ContentBlock& block) {
  if (const auto* artifact = std::get_if<ArtifactBlock>(&block.body))
    refs.emplace(artifact->artifact.id, artifact->artifact.version);
  if (const auto* custom = std::get_if<CustomBlock>(&block.body))
    for (const auto& ref : custom->artifacts) refs.emplace(ref.id, ref.version);
  if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body))
    for (const auto& source : conclusion->sources)
      if (const auto* ref = std::get_if<ArtifactRef>(&source.source)) refs.emplace(ref->id, ref->version);
}

void FilterArtifactRefs(std::vector<ArtifactRef>& refs, const ArtifactVersions& allowed) {
  std::erase_if(refs, [&](const ArtifactRef& ref) { return !allowed.contains({ref.id, ref.version}); });
}

bool VisibleArtifactBlock(const ContentBlock& block, const ArtifactVersions& allowed) {
  const auto visible = [&](const ArtifactRef& ref) { return allowed.contains({ref.id, ref.version}); };
  if (const auto* artifact = std::get_if<ArtifactBlock>(&block.body)) return visible(artifact->artifact);
  if (const auto* custom = std::get_if<CustomBlock>(&block.body)) return std::ranges::all_of(custom->artifacts, visible);
  if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body)) {
    return std::ranges::all_of(conclusion->sources, [&](const auto& source) {
      const auto* artifact = std::get_if<ArtifactRef>(&source.source);
      return !artifact || visible(*artifact);
    });
  }
  return true;
}

Async<StoredArtifact> RuntimeState::ReadArtifact(ArtifactRef ref, ActionParty actor) {
  if (ref.id.value.empty() || !ref.version) {
    co_return Fail<StoredArtifact>(ErrorCode::InvalidArgument, "Artifact ID and version are required");
  }
  const auto access = access_revision;
  if (actor.kind == ActionParty::Kind::Agent) {
    auto user = User();
    if (!user.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(user.Error());
    if (user.Value() != actor.principal)
      co_return Fail<StoredArtifact>(ErrorCode::PermissionDenied, "The acting principal changed");
    auto readable = co_await ReadArtifact(ref, {.kind = ActionParty::Kind::User, .principal = user.Value()});
    if (!readable.Succeeded()) co_return readable;
  }
  for (const auto& [id, session] : sessions) {
    const auto* image = Published(session);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting || !FindArtifact(*image, ref)) continue;
    auto accessible = co_await Require(CollaborationAccess(actor, Capability::ReadSession, *image));
    if (!accessible.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(accessible.Error());
    image = Published(session);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting) {
      co_return Fail<StoredArtifact>(ErrorCode::NotFound, "Artifact source is unavailable");
    }
    auto policy = CollaborationAccess(actor, Capability::ReadArtifact, *image);
    policy.sources.push_back({ref});
    auto permitted = co_await Require(std::move(policy));
    if (!permitted.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(permitted.Error());
    if (access != access_revision)
      co_return Fail<StoredArtifact>(ErrorCode::Conflict, "Artifact access changed while checking the source");
    image = Published(session);
    const auto* item = image && image->summary.lifecycle != SessionLifecycle::Deleting ? FindArtifact(*image, ref) : nullptr;
    if (!item || item->record.availability != ArtifactAvailability::Available) {
      co_return Fail<StoredArtifact>(ErrorCode::NotFound, "Artifact version is unavailable");
    }
    co_return AgentResult<StoredArtifact>::Success(*item);
  }
  co_return Fail<StoredArtifact>(ErrorCode::NotFound, "Artifact version is unavailable");
}

Async<std::vector<StoredArtifact>> RuntimeState::ResolveModelAttachments(const SessionData& session, ActionParty actor,
    std::vector<ArtifactRef> refs, ModelProviderRef model) {
  using Result = AgentResult<std::vector<StoredArtifact>>;
  if (refs.empty()) co_return Result::Success({});
  if (session.origin.kind == SessionOriginKind::SideChat &&
      std::ranges::any_of(refs, [&](const auto& ref) { return !FindArtifact(session, ref); })) {
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::PermissionDenied, "Side chat only accepts its own attachments");
  }
  if (!options.artifact_store.open)
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Unsupported, "Artifact byte stream is unavailable");
  const auto provider = models.find(model.id);
  if (provider == models.end() || provider->second.ref != model)
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Conflict, "The attachment recipient model changed");
  const auto caps = provider->second.provider->Capabilities();
  const auto epoch = access_revision;
  auto read = CollaborationAccess(actor, Capability::ReadContext, session);
  for (const auto& ref : refs) read.sources.push_back({ref});
  const auto sources = read.sources;
  auto allowed = co_await Require(std::move(read));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await ValidateInputSources(session, actor, sources);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto send = CollaborationAccess(actor, Capability::SendToModel, session);
  send.recipient_model = model.id;
  send.sources = sources;
  allowed = co_await Require(std::move(send));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  std::vector<StoredArtifact> attachments;
  for (const auto& ref : refs) {
    auto artifact = co_await ReadArtifact(ref, actor);
    if (!artifact.Succeeded()) co_return Result::Failure(artifact.Error());
    if (!SupportsAttachment(caps, artifact.Value().record.metadata.type))
      co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Unsupported, "The selected model does not accept this attachment type");
    attachments.push_back(std::move(artifact).Value());
  }
  if (epoch != access_revision)
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Conflict, "Attachment authorization changed");
  co_return Result::Success(std::move(attachments));
}

Async<void> RuntimeState::CheckModelAttachments(const SessionData& session, PrincipalId user,
    std::vector<ArtifactRef> refs, ModelProviderRef model) {
  if (refs.empty()) co_return AgentResult<void>::Success();
  auto result = co_await ResolveModelAttachments(session, Request(user, Capability::SubmitInput, &session).actor,
      std::move(refs), std::move(model));
  if (!result.Succeeded()) co_return AgentResult<void>::Failure(result.Error());
  co_return AgentResult<void>::Success();
}

Async<std::vector<StoredArtifact>> RuntimeState::PrepareInputAttachments(const SessionData& session, PrincipalId user,
    const SendInput& input, ModelProviderRef model) {
  using Result = AgentResult<std::vector<StoredArtifact>>;
  auto existing = co_await CheckModelAttachments(session, user, input.attachments, model);
  if (!existing.Succeeded()) co_return Result::Failure(existing.Error());
  if (input.staged_attachments.empty()) co_return Result::Success({});
  if (!options.artifact_store.resolve_staged_attachment || !options.artifact_store.open)
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Unsupported, "The host does not import attachments");
  const auto provider = models.find(model.id);
  if (provider == models.end() || provider->second.ref != model)
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Conflict, "The attachment recipient model changed");
  const auto caps = provider->second.provider->Capabilities();
  if (!caps.image_input && !caps.file_input)
    co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Unsupported, "The selected model does not accept attachments");
  const auto epoch = access_revision;
  auto read = Request(user, Capability::ReadContext, &session);
  for (const auto& id : input.staged_attachments) read.sources.push_back({ArtifactRef{id, 1}});
  const auto sources = read.sources;
  auto allowed = co_await Require(std::move(read));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto send = Request(user, Capability::SendToModel, &session);
  send.recipient_model = model.id;
  send.sources = sources;
  allowed = co_await Require(std::move(send));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = std::move(user)};
  std::vector<StoredArtifact> prepared;
  for (const auto& id : input.staged_attachments) {
    if (epoch != access_revision) co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Conflict, "Attachment authorization changed");
    auto resolved = co_await options.artifact_store.resolve_staged_attachment(actor, session.summary.id, id);
    if (!resolved.Succeeded()) co_return Result::Failure(resolved.Error());
    auto draft = std::move(resolved).Value();
    if (draft.id != id || draft.expected_latest || draft.published_effect || !draft.parts.empty() ||
        !draft.changed_parts.empty() || !draft.dependencies.empty() || draft.name.empty() || !ValidUtf8(draft.name) ||
        draft.storage_handle.empty() || !ValidUtf8(draft.storage_handle) || !draft.size_bytes || !*draft.size_bytes) {
      co_return Fail<std::vector<StoredArtifact>>(ErrorCode::InvalidArgument, "Invalid staged attachment metadata");
    }
    if (!SupportsAttachment(caps, draft.type))
      co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Unsupported, "The selected model does not accept this attachment type");
    prepared.push_back({{.metadata = {{id, 1}, std::move(draft.type), std::move(draft.name), draft.size_bytes},
        .session = session.summary.id, .availability = ArtifactAvailability::Available}, std::move(draft.storage_handle)});
  }
  if (epoch != access_revision) co_return Fail<std::vector<StoredArtifact>>(ErrorCode::Conflict, "Attachment authorization changed");
  co_return Result::Success(std::move(prepared));
}

AgentResult<void> RuntimeState::CommitInputAttachments(SessionData& session, SendInput& input,
    std::vector<StoredArtifact> prepared) {
  for (const auto& attachment : prepared) {
    const auto& id = attachment.record.metadata.ref.id;
    for (const auto& [key, source] : sessions) {
      if (std::ranges::any_of(source.artifacts, [&](const auto& entry) { return entry.record.metadata.ref.id == id; }))
        return Fail<void>(ErrorCode::Conflict, "The staged attachment has already been registered");
    }
  }
  for (auto& attachment : prepared) {
    input.attachments.push_back(attachment.record.metadata.ref);
    session.artifacts.push_back(std::move(attachment));
  }
  input.staged_attachments.clear();
  return AgentResult<void>::Success();
}

Async<void> RuntimeState::PrepareModelAttachments(const SessionData& session, const RunExecution& run,
    ModelTurnRequest& request, std::uint64_t remaining) {
  std::vector<ArtifactRef> refs;
  std::map<ArtifactKey, std::size_t> uses;
  for (const auto& item : request.items) {
    const auto* content = std::get_if<ModelContent>(&item);
    const auto* ref = content ? std::get_if<ArtifactRef>(&content->body) : nullptr;
    if (!ref) continue;
    auto& count = uses[{ref->id, ref->version}];
    if (count == 0) refs.push_back(*ref);
    ++count;
  }
  if (refs.empty()) co_return AgentResult<void>::Success();
  const auto epoch = access_revision;
  const auto actor = Request(run.principal, Capability::SendToModel, &session, &run).actor;
  const auto stop = run.stop.get_token();
  auto attachments = co_await ResolveModelAttachments(session, actor, refs, request.provider);
  if (!attachments.Succeeded()) co_return AgentResult<void>::Failure(attachments.Error());
  for (const auto& artifact : attachments.Value()) {
    const auto& meta = artifact.record.metadata;
    const auto copies = uses.at({meta.ref.id, meta.ref.version});
    if (epoch != access_revision) co_return Fail<void>(ErrorCode::Conflict, "Attachment authorization changed");
    if (stop.stop_requested()) co_return Fail<void>(ErrorCode::AlreadyClosed, "Attachment read was cancelled");
    if (meta.size_bytes && *meta.size_bytes > remaining / copies)
      co_return Fail<void>(ErrorCode::BudgetExceeded, "Attachments exceed the Run context budget");
    auto opened = co_await options.artifact_store.open({actor, meta.ref, artifact.handle});
    if (!opened.Succeeded()) co_return AgentResult<void>::Failure(opened.Error());
    auto stream = std::move(opened).Value();
    auto data = std::make_shared<Bytes>();
    for (;;) {
      if (epoch != access_revision) co_return Fail<void>(ErrorCode::Conflict, "Attachment authorization changed");
      if (stop.stop_requested()) co_return Fail<void>(ErrorCode::AlreadyClosed, "Attachment read was cancelled");
      const auto available = remaining / copies;
      const auto chunk = std::min<std::uint64_t>(available, 32 * 1024 - 1) + 1;
      auto read = co_await stream.ReadAsync(static_cast<std::size_t>(chunk));
      if (!read.Succeeded()) co_return Fail<void>(ErrorCode::ProviderFailure, "The attachment could not be read");
      if (read.Value().empty()) break;
      if (read.Value().size() > available) co_return Fail<void>(ErrorCode::BudgetExceeded, "Attachments exceed the Run context budget");
      remaining -= read.Value().size() * copies;
      data->insert(data->end(), read.Value().begin(), read.Value().end());
    }
    if (data->empty()) co_return Fail<void>(ErrorCode::InvalidArgument, "The attachment is empty");
    if (meta.size_bytes && data->size() != *meta.size_bytes)
      co_return Fail<void>(ErrorCode::Conflict, "The attachment size changed while reading its version");
    request.attachments.push_back({meta.ref, meta.type,
      [state = weak_from_this(), epoch, stop, payload = std::shared_ptr<const Bytes>(std::move(data))]() -> Async<AsyncInputStream> {
        const auto current = state.lock();
        if (stop.stop_requested() || !current) co_return Fail<AsyncInputStream>(ErrorCode::AlreadyClosed, "Attachment read was cancelled");
        if (current->access_revision != epoch) co_return Fail<AsyncInputStream>(ErrorCode::Conflict, "Attachment authorization changed");
        co_return AgentResult<AsyncInputStream>::Success(AsyncInputStream::FromReader(
            [state, epoch, stop, payload, offset = std::size_t{0}](std::stop_token, std::size_t limit) mutable -> Task<IoResult<Bytes>> {
              const auto runtime = state.lock();
              if (stop.stop_requested() || !runtime || runtime->access_revision != epoch)
                co_return IoResult<Bytes>::Failure({IoErrorCode::PermissionDenied, "Attachment authorization changed"});
              const auto length = std::min(limit, payload->size() - offset);
              Bytes chunk(payload->data() + offset, payload->data() + offset + length);
              offset += length;
              co_return IoResult<Bytes>::Success(std::move(chunk));
            }));
      }});
  }
  auto checked = co_await ResolveModelAttachments(session, actor, std::move(refs), request.provider);
  if (!checked.Succeeded()) co_return AgentResult<void>::Failure(checked.Error());
  if (epoch != access_revision) co_return Fail<void>(ErrorCode::Conflict, "Attachment authorization changed");
  if (stop.stop_requested()) co_return Fail<void>(ErrorCode::AlreadyClosed, "Attachment read was cancelled");
  co_return AgentResult<void>::Success();
}

Async<StoredArtifact> RuntimeState::ReadHistoryArtifact(const SessionData& target, ActionParty actor, ArtifactRef ref) {
  auto permission = CollaborationAccess(actor, Capability::ReadContext, target);
  permission.sources.push_back({ref});
  auto allowed = co_await Require(std::move(permission));
  if (!allowed.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(allowed.Error());
  co_return co_await ReadArtifact(std::move(ref), std::move(actor));
}

Async<StoredArtifact> RuntimeState::ReadActionArtifact(SessionId target, PrincipalId principal, ArtifactRef ref) {
  auto access = co_await WithSession(target, principal, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(access.Error());
  const auto* image = Published(sessions.at(target));
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = principal};
  if (FindArtifact(*image, ref)) co_return co_await ReadArtifact(ref, actor);
  if (std::ranges::any_of(image->messages, [&](const Message& message) {
        return message.ref.session == target && message.author == MessageAuthor::User &&
            std::ranges::any_of(message.blocks, [&](const ContentBlock& block) {
              const auto* attachment = std::get_if<ArtifactBlock>(&block.body);
              return attachment && attachment->artifact == ref;
            });
      })) {
    std::vector<SourceSlice> input{{ref}};
    auto checked = co_await ValidateInputSources(sessions.at(target), actor, std::move(input));
    if (!checked.Succeeded()) co_return AgentResult<StoredArtifact>::Failure(checked.Error());
    co_return co_await ReadHistoryArtifact(sessions.at(target), actor, ref);
  }
  if (!InheritedArtifact(*image, ref)) co_return Fail<StoredArtifact>(ErrorCode::NotFound, "Artifact version is outside this conversation");
  co_return co_await ReadHistoryArtifact(*image, actor, ref);
}

Async<std::string> RuntimeState::ReadConversationArtifactName(std::shared_ptr<Conversation> conversation, ArtifactRef ref) {
  auto stored = co_await conversation->state_->ReadActionArtifact(conversation->id_, conversation->principal_, std::move(ref));
  if (!stored.Succeeded()) co_return AgentResult<std::string>::Failure(stored.Error());
  co_return AgentResult<std::string>::Success(std::move(stored).Value().record.metadata.name);
}

Async<StoredArtifact> RuntimeState::ReadApprovalArtifact(SessionId target, PrincipalId principal, ArtifactRef ref) {
  using Result = AgentResult<StoredArtifact>;
  const auto epoch = access_revision;
  auto access = co_await WithSession(target, principal, Capability::ReadSession);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  const auto current_session = [&]() -> const SessionData* {
    const auto session = sessions.find(target);
    return session == sessions.end() ? nullptr : Published(session->second);
  };
  const auto* image = current_session();
  if (!image) co_return Fail<StoredArtifact>(ErrorCode::NotFound, "Approval conversation unavailable");
  const auto available = [&](const InteractionSnapshot& item) {
    const auto* approval = std::get_if<ToolApproval>(&item.body);
    return approval && item.recipient == principal &&
        std::ranges::find(approval->preview.artifacts, ref) != approval->preview.artifacts.end();
  };
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = principal};
  std::vector<std::pair<InteractionId, Revision>> candidates;
  for (const auto& item : image->interactions) {
    if (available(item)) candidates.emplace_back(item.id, item.version);
  }
  for (const auto& [id, version] : candidates) {
    image = current_session();
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting) break;
    const auto item = std::ranges::find(image->interactions, id, &InteractionSnapshot::id);
    if (item == image->interactions.end() || item->version != version || !available(*item)) continue;
    const auto& approval = std::get<ToolApproval>(item->body);
    const auto call = std::ranges::find(image->tools, approval.call, &ToolCallSnapshot::id);
    if (call == image->tools.end()) continue;
    const auto project = image->summary.project;
    const bool adoption = call->tool.id.value == "agentkit/adopt-candidate";
    if (adoption) {
      if (item->state != InteractionState::Pending) continue;
      const auto command = std::ranges::find_if(commands, [&](const auto& entry) {
        const auto& data = entry.second;
        return entry.first.first == principal && data.input.session == target && data.input.adoption &&
            data.input.adoption->candidate_artifact == ref && data.artifact_change && data.artifact_change->call == approval.call;
      });
      if (command == commands.end()) continue;
      const auto request = *command->second.input.adoption;
      auto readable = co_await ReadAdoptionCandidate(request, actor);
      if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
    } else {
      if (!project) continue;
      auto context = CollaborationAccess(actor, Capability::ReadContext, *image);
      context.sources.push_back({ref});
      auto allowed = co_await Require(std::move(context));
      if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    }
    auto artifact = co_await ReadArtifact(ref, actor);
    if (!artifact.Succeeded()) co_return artifact;
    auto user = User();
    if (epoch != access_revision || !user.Succeeded() || user.Value() != principal)
      co_return Fail<StoredArtifact>(ErrorCode::Conflict, "Approval artifact access changed");
    if (!adoption) {
      const auto source = sessions.find(artifact.Value().record.session);
      const auto* owner = source == sessions.end() ? nullptr : Published(source->second);
      if (!owner || owner->summary.project != project)
        co_return Fail<StoredArtifact>(ErrorCode::PermissionDenied, "Approval artifact belongs to a different project");
    }
    image = current_session();
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting || image->summary.project != project) break;
    const auto current = std::ranges::find(image->interactions, id, &InteractionSnapshot::id);
    if (current != image->interactions.end() && current->version == version && available(*current) &&
        (!adoption || current->state == InteractionState::Pending))
      co_return artifact;
  }
  co_return Fail<StoredArtifact>(ErrorCode::NotFound, "Artifact version is outside the review");
}

Async<ArtifactVersions> RuntimeState::VisibleApprovalArtifacts(SessionId target, PrincipalId user) {
  ArtifactVersions candidates;
  const auto session = sessions.find(target);
  const auto* image = session == sessions.end() ? nullptr : Published(session->second);
  if (image && image->summary.lifecycle != SessionLifecycle::Deleting) {
    for (const auto& item : image->interactions) {
      const auto* approval = std::get_if<ToolApproval>(&item.body);
      if (!approval || item.recipient != user) continue;
      for (const auto& ref : approval->preview.artifacts) {
        if (!FindArtifact(*image, ref)) candidates.emplace(ref.id, ref.version);
      }
    }
  }
  ArtifactVersions allowed;
  for (const auto& [id, version] : candidates) {
    auto readable = co_await ReadActionArtifact(target, user, {id, version});
    if (!readable.Succeeded() && readable.Error().code == ErrorCode::NotFound)
      readable = co_await ReadApprovalArtifact(target, user, {id, version});
    if (readable.Succeeded()) allowed.emplace(id, version);
  }
  co_return AgentResult<ArtifactVersions>::Success(std::move(allowed));
}

Async<ArtifactPreview> RuntimeState::PreviewArtifact(ArtifactRef ref, std::optional<SessionId> target, PrincipalId principal) {
  using Result = AgentResult<ArtifactPreview>;
  const ActionParty actor{.kind = ActionParty::Kind::User, .principal = principal};
  auto authorize = [&]() -> Async<StoredArtifact> {
    if (!target) co_return co_await ReadArtifact(ref, actor);
    auto related = co_await ReadActionArtifact(*target, principal, ref);
    if (related.Succeeded() || related.Error().code != ErrorCode::NotFound) co_return related;
    co_return co_await ReadApprovalArtifact(*target, principal, ref);
  };
  auto found = co_await authorize();
  if (!found.Succeeded()) co_return Result::Failure(found.Error());
  if (!options.artifact_store.preview) co_return Fail<ArtifactPreview>(ErrorCode::Unsupported, "Artifact preview is unavailable");
  auto preview = co_await options.artifact_store.preview({actor, ref, found.Value().handle});
  if (!preview.Succeeded()) co_return preview;
  if (preview.Value().artifact != ref) co_return Fail<ArtifactPreview>(ErrorCode::Conflict, "Preview did not resolve the requested version");
  found = co_await authorize();
  if (!found.Succeeded()) co_return Result::Failure(found.Error());
  preview.Value().type = found.Value().record.metadata.type;
  preview.Value().access_revision = access_revision;
  preview.Value().name = found.Value().record.metadata.name;
  co_return preview;
}

Async<ArtifactImpact> RuntimeState::InspectArtifactImpact(ArtifactImpactQuery query, std::optional<RunId> bound) {
  using Result = AgentResult<ArtifactImpact>;
  if ((query.change_command && (!query.changes.empty() || query.change_command->value.empty())) ||
      (!query.change_command && (query.changes.empty() || query.changes.size() > 256))) {
    co_return Fail<ArtifactImpact>(ErrorCode::InvalidArgument, "Choose one recorded change or provide up to 256 source changes");
  }
  auto actor = co_await ProjectAccess(query.project, bound, Capability::ReadProject);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  std::optional<SourceChange> recorded;
  std::optional<SharedContextRef> withdrawn_context;
  if (query.change_command) {
    const auto* command = FindProjectCommand(actor.Value().principal, *query.change_command, true);
    const auto* change = command ? command->SourceChangeResult() : nullptr;
    if (!change || command->progress.accepted.project != query.project || (bound && command->actor != bound))
      co_return Fail<ArtifactImpact>(ErrorCode::NotFound, "Source change is unavailable");
    recorded = *change;
    if (command->source_change && change->kind == SourceChangeKind::Withdrawn) {
      withdrawn_context = std::get<SharedContextRef>(change->before.source);
    }
    query.changes.push_back(*change);
  }
  const auto project_revision = [&] {
    const auto* current = PublishedProject(query.project);
    return current ? current->revision : Revision{0};
  };
  const auto project_sessions = [&] {
    std::map<SessionId, Sequence> revisions;
    for (const auto& [id, session] : sessions) {
      const auto* image = Published(session);
      if (image && image->summary.project == query.project && image->summary.lifecycle != SessionLifecycle::Deleting) {
        revisions.emplace(id, image->sequence);
      }
    }
    return revisions;
  };
  const auto revision = project_revision();
  const auto access = access_revision;
  const auto captured = project_sessions();
  std::vector<ArtifactRecord> records;
  for (const auto& [id, sequence] : captured) {
    for (const auto& artifact : Published(sessions.at(id))->artifacts) {
      if (artifact.record.availability == ArtifactAvailability::Available) records.push_back(artifact.record);
    }
  }
  std::vector<SourceSlice> origins;
  std::vector<ImpactStep> pending;
  for (auto& change : query.changes) {
    auto valid = CheckSourceChange(change);
    if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
    origins.push_back(change.before);
    pending.push_back({std::move(change.before), {origins.back()}});
    if (change.after) origins.push_back({*change.after});
  }
  if (withdrawn_context) {
    auto visible = co_await AuthorizeWithdrawnContextImpact(query.project, actor.Value(), *withdrawn_context);
    if (!visible.Succeeded()) co_return Result::Failure(visible.Error());
  } else {
    auto visible = co_await ValidateProjectSources(query.project, actor.Value(), origins);
    if (!visible.Succeeded()) co_return Result::Failure(visible.Error());
  }

  std::set<std::tuple<ArtifactKey, std::optional<PartId>, bool>> visited;
  std::map<ArtifactKey, ImpactItem> affected;
  for (std::size_t index = 0; index < pending.size(); ++index) {
    const auto step = std::move(pending[index]);
    for (const auto& record : records) {
      const auto& ref = record.metadata.ref;
      const ArtifactKey key{ref.id, ref.version};
      for (const auto& edge : record.dependencies) {
        const auto match = MatchSource(step.changed, edge.input);
        if (!match) continue;
        const bool approximate = step.approximate || *match || (!edge.output_part && !record.parts.empty());
        if (!visited.emplace(key, edge.output_part, approximate).second) continue;
        SourceSlice result{ref};
        if (edge.output_part) result.parts.push_back(*edge.output_part);
        std::vector<SourceSlice> selection{result};
        auto permit = co_await ValidateProjectSources(query.project, actor.Value(), selection);
        auto path = step.path;
        if (!permit.Succeeded()) {
          if (permit.Error().code != ErrorCode::PermissionDenied && permit.Error().code != ErrorCode::NotFound) {
            co_return Result::Failure(permit.Error());
          }
          pending.push_back({std::move(result), std::move(path), true});
          continue;
        }
        path.push_back(result);
        auto [item, inserted] = affected.try_emplace(key, ImpactItem{ref, result.parts, path, approximate});
        if (!inserted) {
          auto& prior = item->second;
          if (prior.approximate && !approximate) prior.path = path;
          prior.approximate = prior.approximate || approximate;
          if (result.parts.empty()) prior.parts.clear();
          else if (!prior.parts.empty() && std::ranges::find(prior.parts, *edge.output_part) == prior.parts.end()) {
            prior.parts.push_back(*edge.output_part);
          }
        }
        pending.push_back({std::move(result), std::move(path), approximate});
      }
    }
  }
  auto refreshed = co_await ProjectAccess(query.project, bound, Capability::ReadProject);
  if (!refreshed.Succeeded()) co_return Result::Failure(refreshed.Error());
  if (refreshed.Value().principal != actor.Value().principal) {
    co_return Fail<ArtifactImpact>(ErrorCode::PermissionDenied, "The acting principal changed");
  }
  if (access_revision != access || project_revision() != revision || project_sessions() != captured) {
    co_return Fail<ArtifactImpact>(ErrorCode::CursorExpired, "Sources or access changed while inspecting the project");
  }
  ArtifactImpact result{.project_revision = revision, .recorded_change = std::move(recorded)};
  for (auto& [key, item] : affected) {
    std::ranges::sort(item.parts);
    result.affected.push_back(std::move(item));
  }
  co_return Result::Success(std::move(result));
}

Async<CommandReceipt> RuntimeState::RecordSourceChange(ProjectId id, CommandId command, SourceChange change,
                                                        std::optional<RunId> bound) {
  using Result = AgentResult<CommandReceipt>;
  auto actor = co_await ProjectAccess(id, bound, Capability::RecordSourceChange);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto valid = CheckSourceChange(change);
  if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  ProjectCommand key{change};
  auto prior = ReplayProject<std::monostate>(actor.Value(), id, command, key);
  if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
  if (prior.Value()) co_return co_await ReplayReceipt(Result::Success(*prior.Value()));

  const auto access = access_revision;
  const auto* captured = PublishedProject(id);
  const auto revision = captured ? captured->revision : Revision{};
  auto scope = ProjectPolicy(actor.Value(), id, Capability::RecordSourceChange);
  scope.sources.push_back(change.before);
  if (change.after) scope.sources.push_back({*change.after});
  scope.operation = JsonValue{static_cast<std::int64_t>(change.kind)};
  auto readable = co_await ValidateProjectSources(id, actor.Value(), scope.sources);
  if (!readable.Succeeded()) co_return Result::Failure(readable.Error());
  auto permitted = co_await Require(std::move(scope));
  if (!permitted.Succeeded()) co_return Result::Failure(permitted.Error());
  auto current = Actor(bound, Capability::RecordSourceChange);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != actor.Value().principal)
    co_return Fail<CommandReceipt>(ErrorCode::PermissionDenied, "The acting principal changed");

  prior = ReplayProject<std::monostate>(actor.Value(), id, command, key);
  if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
  if (prior.Value()) co_return co_await ReplayReceipt(Result::Success(*prior.Value()));
  if (storage_error && options.store) co_return Result::Failure(*storage_error);
  const auto project = projects.find(id);
  const auto latest = project == projects.end() ? Revision{} : project->second.revision;
  if (latest != revision || access_revision != access)
    co_return Fail<CommandReceipt>(ErrorCode::Conflict, "Project or source access changed while recording");
  auto& target = projects.try_emplace(id, ProjectData{.id = id}).first->second;
  ++target.revision;
  co_return co_await CommitProject(target, actor.Value(), std::move(command), std::move(key), std::monostate{});
}

std::optional<Revision> RuntimeState::ProjectSourceRevision(const SessionData& session) const {
  if (!session.summary.project) return std::nullopt;
  const auto* project = PublishedProject(*session.summary.project);
  return project ? project->revision : Revision{};
}

bool RuntimeState::SourcesChanged(const SessionData& session, Revision revision, const std::vector<SourceSlice>& inputs) const {
  if (!session.summary.project || inputs.empty()) return false;
  const auto* project = PublishedProject(*session.summary.project);
  if (!project) return false;
  return std::ranges::any_of(project->commands, [&](const ProjectCommandRecord& event) {
    const auto* change = event.SourceChangeResult();
    return change && (change->kind == SourceChangeKind::Withdrawn || event.progress.accepted.project_revision > revision) &&
        std::ranges::any_of(inputs, [&](const auto& input) { return MatchSource(change->before, input).has_value(); });
  });
}

bool RuntimeState::SourcePlanStale(const SessionData& session, const ToolPlanState& plan) const {
  return plan.prepared && plan.source_revision && SourcesChanged(session, *plan.source_revision, plan.prepared->change.inputs);
}

void RuntimeState::InvalidateSourcePlans(const ProjectId& project) {
  for (auto& [id, run] : runs) {
    if (Terminal(run.view.state)) continue;
    auto& session = sessions.at(run.view.session);
    if (session.summary.project != project) continue;
    bool changed = false;
    for (auto& [call, attempt] : run.tool_attempts) {
      if (attempt.issued || !SourcePlanStale(session, attempt.plan)) continue;
      attempt.InterruptReview();
      if (!attempt.plan.approval) continue;
      auto approval = std::ranges::find(session.interactions, *attempt.plan.approval, &InteractionSnapshot::id);
      if (approval != session.interactions.end() && approval->state == InteractionState::Pending) {
        CloseInteraction(session, *approval, InteractionState::Invalidated);
        changed = true;
      }
    }
    if (changed) Persist(session);
  }
}

Async<std::vector<ArtifactRef>> RuntimeState::PublishArtifacts(
    RunId id, ToolCallId call, std::vector<ArtifactDraft> drafts, std::optional<ActionParty> origin, bool reverting) {
  using Registered = AgentResult<std::vector<ArtifactRef>>;
  if (drafts.empty()) co_return Registered::Success({});
  auto& run = runs.at(id);
  auto& session = sessions.at(run.view.session);
  if (!options.artifact_store.open && !options.artifact_store.preview) {
    co_return Fail<std::vector<ArtifactRef>>(ErrorCode::Unsupported, "No artifact store was configured");
  }

  auto policy = Request(run.principal, Capability::WriteArtifact, &session, &run);
  if (origin) policy.actor = std::move(*origin);
  policy.actor.call = call;
  for (const auto& draft : drafts) {
    for (const auto& dependency : draft.dependencies) policy.sources.push_back(dependency.input);
  }
  const auto publisher = policy.actor.principal;
  auto allowed = co_await Require(std::move(policy));
  if (!allowed.Succeeded()) co_return Registered::Failure(allowed.Error());
  if (Terminal(run.view.state)) co_return Fail<std::vector<ArtifactRef>>(ErrorCode::AlreadyClosed, "Run has ended");

  std::set<ArtifactId> staged_ids;
  std::vector<StoredArtifact> pending;
  std::vector<ArtifactRef> refs;
  std::vector<PendingSourceChange> changes;
  for (auto& draft : drafts) {
    if (draft.type.empty() || draft.name.empty() || draft.storage_handle.empty() ||
        !ValidUtf8(draft.type) || !ValidUtf8(draft.name) || !ValidUtf8(draft.storage_handle) ||
        (draft.expected_latest && (!draft.id || !*draft.expected_latest))) {
      co_return Fail<std::vector<ArtifactRef>>(ErrorCode::InvalidArgument, "Artifact needs a type, name and storage handle");
    }
    const auto artifact_id = draft.id.value_or(ArtifactId{MakeId()});
    if (artifact_id.value.empty() || !staged_ids.insert(artifact_id).second) {
      co_return Fail<std::vector<ArtifactRef>>(ErrorCode::Conflict, "Artifact IDs must be distinct within a tool result");
    }
    std::set<PartId> parts;
    for (const auto& part : draft.parts) {
      if (part.id.value.empty() || part.label.empty() || !ValidUtf8(part.label) || !parts.insert(part.id).second) {
        co_return Fail<std::vector<ArtifactRef>>(ErrorCode::InvalidArgument, "Invalid artifact part");
      }
    }
    if (std::ranges::any_of(draft.dependencies, [&](const auto& dependency) {
        return dependency.output_part && !parts.contains(*dependency.output_part);
      })) co_return Fail<std::vector<ArtifactRef>>(ErrorCode::InvalidArgument, "Dependency refers to an unknown output part");
    std::optional<Revision> latest;
    const ArtifactRecord* previous = nullptr;
    if (draft.id) {
      for (const auto& [candidate_id, candidate] : sessions) {
        for (const auto& item : candidate.artifacts) {
          const auto& ref = item.record.metadata.ref;
          if (ref.id != artifact_id) continue;
          if (candidate_id != session.summary.id) {
            co_return Fail<std::vector<ArtifactRef>>(ErrorCode::Conflict, "Artifact is registered in another session");
          }
          if (!latest || ref.version > *latest) {
            latest = ref.version;
            previous = &item.record;
          }
        }
      }
    }
    if (latest != draft.expected_latest || latest == UINT64_MAX) {
      co_return Fail<std::vector<ArtifactRef>>(ErrorCode::Conflict, "Artifact version changed");
    }
    std::set<PartId> changed_parts;
    for (const auto& part : draft.changed_parts) {
      if (!previous || std::ranges::find(previous->parts, part, &ArtifactPart::id) == previous->parts.end() ||
          !changed_parts.insert(part).second)
        co_return Fail<std::vector<ArtifactRef>>(ErrorCode::InvalidArgument, "Change refers to an unknown or duplicate source part");
    }
    const ArtifactRef ref{artifact_id, latest.value_or(0) + 1};
    std::optional<CommandId> change;
    if (previous && session.summary.project && (!options.store || session.retention == SessionRetention::Stored)) {
      change = CommandId{MakeId()};
      const auto kind = reverting && draft.published_effect ? SourceChangeKind::Reverted : SourceChangeKind::Modified;
      changes.push_back({*change, publisher, id,
          SourceChange{{previous->metadata.ref, std::move(draft.changed_parts)}, SourceRef{ref}, kind}});
    }
    refs.push_back(ref);
    pending.push_back({
        .record = {.metadata = {ref, std::move(draft.type), std::move(draft.name), draft.size_bytes},
                   .session = session.summary.id, .run = id, .call = call,
                   .availability = ArtifactAvailability::Available, .parts = std::move(draft.parts),
                   .dependencies = std::move(draft.dependencies), .source_change = std::move(change)},
        .handle = std::move(draft.storage_handle)});
  }
  if (!changes.empty()) {
    auto& events = pending_source_changes[session.summary.id];
    std::ranges::move(changes, std::back_inserter(events));
  }
  std::ranges::move(pending, std::back_inserter(session.artifacts));
  ++session.sequence;
  co_return Registered::Success(std::move(refs));
}

} // namespace detail

ArtifactChanges::ArtifactChanges(std::shared_ptr<detail::RuntimeState> state, std::optional<RunId> actor)
    : state_(std::move(state)), actor_(std::move(actor)) {}

Async<ArtifactRecord> ArtifactChanges::ReadAsync(ArtifactRef artifact) {
  return [](std::shared_ptr<detail::RuntimeState> state, ArtifactRef ref, std::optional<RunId> bound) -> Async<ArtifactRecord> {
    auto actor = state->Actor(bound, Capability::ReadArtifact);
    if (!actor.Succeeded()) co_return AgentResult<ArtifactRecord>::Failure(actor.Error());
    auto found = co_await state->ReadArtifact(std::move(ref), actor.Value());
    if (!found.Succeeded()) co_return AgentResult<ArtifactRecord>::Failure(found.Error());
    auto record = std::move(found).Value().record;
    if (record.source_change) {
      const auto* command = state->FindProjectCommand(actor.Value().principal, *record.source_change, true);
      if (!command || (bound && command->actor != bound)) record.source_change.reset();
    }
    co_return AgentResult<ArtifactRecord>::Success(std::move(record));
  }(state_, std::move(artifact), actor_);
}

Async<ArtifactImpact> ArtifactChanges::InspectImpactAsync(ArtifactImpactQuery query) {
  return [](std::shared_ptr<detail::RuntimeState> state, ArtifactImpactQuery input, std::optional<RunId> actor) -> Async<ArtifactImpact> {
    co_return co_await state->InspectArtifactImpact(std::move(input), std::move(actor));
  }(state_, std::move(query), actor_);
}

Async<CommandReceipt> ArtifactChanges::RecordSourceChangeAsync(ProjectId project, CommandId command, SourceChange change) {
  return [](std::shared_ptr<detail::RuntimeState> state, ProjectId project, CommandId command, SourceChange input,
            std::optional<RunId> actor) -> Async<CommandReceipt> {
    co_return co_await state->RecordSourceChange(std::move(project), std::move(command), std::move(input), std::move(actor));
  }(state_, std::move(project), std::move(command), std::move(change), actor_);
}

Async<ArtifactPreview> ArtifactChanges::PreviewAsync(ArtifactRef artifact) {
  return [](std::shared_ptr<detail::RuntimeState> state, ArtifactRef ref, std::optional<RunId> bound) -> Async<ArtifactPreview> {
    auto actor = state->Actor(bound, Capability::ReadArtifact);
    if (!actor.Succeeded()) co_return AgentResult<ArtifactPreview>::Failure(actor.Error());
    if (!bound) co_return co_await state->PreviewArtifact(std::move(ref), {}, actor.Value().principal);
    auto found = co_await state->ReadArtifact(ref, actor.Value());
    if (!found.Succeeded()) co_return AgentResult<ArtifactPreview>::Failure(found.Error());
    if (!state->options.artifact_store.preview)
      co_return detail::Fail<ArtifactPreview>(ErrorCode::Unsupported, "Artifact preview is unavailable");
    auto preview = co_await state->options.artifact_store.preview({actor.Value(), ref, found.Value().handle});
    if (!preview.Succeeded()) co_return preview;
    if (preview.Value().artifact != ref)
      co_return detail::Fail<ArtifactPreview>(ErrorCode::Conflict, "Preview did not resolve the requested version");
    auto current = state->Actor(bound, Capability::ReadArtifact);
    if (!current.Succeeded()) co_return AgentResult<ArtifactPreview>::Failure(current.Error());
    found = co_await state->ReadArtifact(ref, current.Value());
    if (!found.Succeeded()) co_return AgentResult<ArtifactPreview>::Failure(found.Error());
    preview.Value().type = found.Value().record.metadata.type;
    preview.Value().access_revision = state->access_revision;
    preview.Value().name = found.Value().record.metadata.name;
    co_return preview;
  }(state_, std::move(artifact), actor_);
}

Async<AsyncInputStream> ArtifactChanges::OpenAsync(ArtifactRef artifact) {
  return [](std::shared_ptr<detail::RuntimeState> state, ArtifactRef ref, std::optional<RunId> bound) -> Async<AsyncInputStream> {
    auto actor = state->Actor(bound, Capability::ReadArtifact);
    if (!actor.Succeeded()) co_return AgentResult<AsyncInputStream>::Failure(actor.Error());
    auto found = co_await state->ReadArtifact(ref, actor.Value());
    if (!found.Succeeded()) co_return AgentResult<AsyncInputStream>::Failure(found.Error());
    if (!state->options.artifact_store.open) {
      co_return detail::Fail<AsyncInputStream>(ErrorCode::Unsupported, "Artifact byte stream is unavailable");
    }
    auto opened = co_await state->options.artifact_store.open({actor.Value(), ref, std::move(found).Value().handle});
    if (!opened.Succeeded()) co_return opened;
    auto current = state->Actor(bound, Capability::ReadArtifact);
    if (!current.Succeeded()) co_return AgentResult<AsyncInputStream>::Failure(current.Error());
    found = co_await state->ReadArtifact(ref, current.Value());
    if (!found.Succeeded()) co_return AgentResult<AsyncInputStream>::Failure(found.Error());
    co_return opened;
  }(state_, std::move(artifact), actor_);
}

} // namespace huxerui::agentkit
