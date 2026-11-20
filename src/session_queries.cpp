#include "runtime_internal.h"

#include <algorithm>
#include <charconv>
#include <tuple>

namespace huxerui::agentkit {
namespace detail {
namespace {

std::string CursorPart(std::string_view value) {
  return std::to_string(value.size()) + ":" + std::string(value);
}

std::string CursorKey(const PrincipalId& user, Revision revision, std::string filter) {
  return CursorPart(user.value) + CursorPart(std::to_string(revision)) + std::move(filter) + ":";
}

PageCursor Cursor(std::string_view key, std::uint64_t position) {
  return {std::string(key) + std::to_string(position)};
}

AgentResult<std::uint64_t> ReadCursor(const std::optional<PageCursor>& cursor, const std::string& key) {
  if (!cursor) return AgentResult<std::uint64_t>::Success(0);
  if (!cursor->value.starts_with(key)) return Fail<std::uint64_t>(ErrorCode::CursorExpired, "Refresh the first page");
  std::uint64_t offset = 0;
  auto first = cursor->value.data() + key.size();
  auto last = cursor->value.data() + cursor->value.size();
  auto result = std::from_chars(first, last, offset);
  if (result.ec != std::errc{} || result.ptr != last) {
    return Fail<std::uint64_t>(ErrorCode::InvalidArgument, "Invalid page cursor");
  }
  return AgentResult<std::uint64_t>::Success(offset);
}

std::string HistoryKey(const PrincipalId& user, const SessionId& session, Sequence through) {
  return CursorKey(user, through, CursorPart(session.value));
}

void CollectArtifacts(ArtifactVersions& refs, const std::vector<ArtifactRef>& artifacts) {
  for (const auto& ref : artifacts) refs.emplace(ref.id, ref.version);
}

void CollectArtifacts(ArtifactVersions& refs, const std::vector<SourceSlice>& sources) {
  for (const auto& source : sources)
    if (const auto* ref = std::get_if<ArtifactRef>(&source.source)) refs.emplace(ref->id, ref->version);
}

void FilterQueueArtifacts(QueuedInput& item, const ArtifactVersions& visible) {
  auto* input = std::get_if<SendInput>(&item.input);
  if (!input) return;
  FilterArtifactRefs(input->attachments, visible);
  const auto denied = [&](const SourceSlice& source) {
    const auto* ref = std::get_if<ArtifactRef>(&source.source);
    return ref && !visible.contains({ref->id, ref->version});
  };
  std::erase_if(input->context, denied);
  std::erase_if(input->side_chat_conclusions, denied);
}

void FilterRunArtifacts(RunSnapshot& run, const ArtifactVersions& visible) {
  if (!run.result) return;
  auto& result = *run.result;
  const auto count = result.artifacts.size();
  FilterArtifactRefs(result.artifacts, visible);
  // The structured result can contain references to any of its bound artifacts.
  if (result.artifacts.size() != count) result.structured.reset();
}

} // namespace

bool InheritedArtifact(const SessionData& session, const ArtifactRef& artifact) {
  return std::ranges::any_of(session.messages, [&](const Message& message) {
    return message.ref.session != session.summary.id && message.state == MessageState::Complete &&
        std::ranges::any_of(message.blocks, [&](const ContentBlock& block) {
          const auto* entry = std::get_if<ArtifactBlock>(&block.body);
          if (entry) return entry->artifact == artifact;
          const auto* custom = std::get_if<CustomBlock>(&block.body);
          return custom && std::ranges::find(custom->artifacts, artifact) != custom->artifacts.end();
        });
  });
}

bool FilterHistoryArtifacts(Message& message, const ArtifactVersions& permitted, bool inherited) {
  std::erase_if(message.blocks, [&](const ContentBlock& block) {
    return !std::holds_alternative<SideChatConclusion>(block.body) && !VisibleArtifactBlock(block, permitted);
  });
  if (inherited) {
    for (auto& block : message.blocks)
      if (auto* custom = std::get_if<CustomBlock>(&block.body)) custom->interactions.clear();
  }
  return !message.blocks.empty();
}

void FilterContentInteractions(Message& message, const SessionData& session, const PrincipalId& user) {
  for (auto& entry : message.blocks) {
    auto* custom = std::get_if<CustomBlock>(&entry.body);
    if (!custom) continue;
    std::erase_if(custom->interactions, [&](const auto& id) {
      const auto interaction = std::ranges::find(session.interactions, id, &InteractionSnapshot::id);
      return interaction == session.interactions.end() || interaction->recipient != user;
    });
  }
}

bool VisibleConclusion(const SideChatConclusion& conclusion, const std::vector<SourceSlice>& selections) {
  return std::ranges::all_of(conclusion.sources, [&](const auto& source) {
    return std::ranges::find(selections, source) != selections.end();
  });
}

void FilterApprovalArtifacts(InteractionSnapshot& interaction, const std::vector<ArtifactRef>& local,
                             const ArtifactVersions& visible, const ArtifactVersions& review) {
  if (auto* approval = std::get_if<ToolApproval>(&interaction.body)) {
    std::erase_if(approval->preview.artifacts, [&](const ArtifactRef& artifact) {
      return std::ranges::find(local, artifact) == local.end() && !review.contains({artifact.id, artifact.version});
    });
  }
  if (auto* approval = std::get_if<OperationApproval>(&interaction.body))
    FilterArtifactRefs(approval->preview.artifacts, visible);
}

bool ShowsArtifact(const SessionSnapshot& snapshot, const ArtifactRef& ref) {
  if (std::ranges::find(snapshot.artifacts, ref) != snapshot.artifacts.end()) return true;
  if (std::ranges::any_of(snapshot.interactions, [&](const auto& item) {
        const auto* approval = std::get_if<ToolApproval>(&item.body);
        return approval && std::ranges::find(approval->preview.artifacts, ref) != approval->preview.artifacts.end();
      })) return true;
  return std::ranges::any_of(snapshot.messages, [&](const Message& message) {
    return std::ranges::any_of(message.blocks, [&](const ContentBlock& block) {
      const auto* artifact = std::get_if<ArtifactBlock>(&block.body);
      return artifact && artifact->artifact == ref;
    });
  });
}

std::shared_ptr<const SessionSnapshot> RuntimeState::Snapshot(const SessionData& working,
    const PrincipalId& user, std::optional<SessionHistoryWindow> history) const {
  const auto* published = Published(working);
  if (!published || published->summary.lifecycle == SessionLifecycle::Deleting) return {};
  const auto& session = *published;
  SessionSnapshot result{
      .summary = session.summary, .origin = session.origin, .through_sequence = session.sequence,
      .messages = session.messages, .tools = session.tools, .interactions = session.interactions,
      .queue = session.queue, .access_revision = access_revision, .event_sequence = session.event_sequence};
  const auto history_source_visible = [&](const SessionId& id) {
    const auto source = sessions.find(id);
    const auto* image = source == sessions.end() ? nullptr : Published(source->second);
    return image && image->summary.lifecycle != SessionLifecycle::Deleting && visible_sessions.contains({user, id});
  };
  if (result.origin->source_session && !history_source_visible(*result.origin->source_session)) {
    result.origin->creator.reset();
    result.origin->source_session.reset();
    result.origin->fork_point.reset();
    result.origin->side_chat_selection.clear();
  } else if (result.origin->creator && result.origin->creator->principal != user) {
    result.origin->creator.reset();
  }
  const auto visible = visible_history_artifacts.find({user, session.summary.id});
  const auto selected = visible_selections.find({user, session.summary.id});
  const std::vector<SourceSlice> no_selection;
  const auto& selections = selected == visible_selections.end() ? no_selection : selected->second;
  if (session.origin.kind == SessionOriginKind::SideChat) {
    std::erase_if(result.origin->side_chat_selection, [&](const auto& slice) {
      return std::ranges::find(selections, slice) == selections.end();
    });
    if (result.origin->side_chat_selection.size() != session.origin.side_chat_selection.size()) {
      std::erase_if(result.messages, [](const auto& message) { return message.author == MessageAuthor::Agent; });
    }
  }
  const ArtifactVersions empty;
  const auto& permitted = visible == visible_history_artifacts.end() ? empty : visible->second;
  for (const auto& artifact : session.artifacts) {
    const auto& ref = artifact.record.metadata.ref;
    if (permitted.contains({ref.id, ref.version})) result.artifacts.push_back(ref);
  }
  for (auto& call : result.tools) {
    FilterArtifactRefs(call.artifacts, permitted);
    std::erase_if(call.source_changes, [&](const auto& command) { return !FindProjectCommand(user, command, true); });
  }
  for (auto& message : result.messages) {
    const bool inherited = message.ref.session != session.summary.id;
    FilterHistoryArtifacts(message, permitted, inherited);
    if (!inherited) FilterContentInteractions(message, session, user);
    std::erase_if(message.blocks, [&](const auto& block) {
      const auto* conclusion = std::get_if<SideChatConclusion>(&block.body);
      return conclusion && !VisibleConclusion(*conclusion, selections);
    });
  }
  std::erase_if(result.queue, [&](const auto& item) {
    if (std::holds_alternative<CollaborationMessageId>(item.input)) return false;
    const auto owner = session.queue_owners.find(item.id);
    return owner == session.queue_owners.end() || owner->second != user;
  });
  for (auto& item : result.queue) {
    FilterQueueArtifacts(item, permitted);
    std::erase_if(item.conclusions, [&](const auto& conclusion) { return !VisibleConclusion(conclusion, selections); });
  }
  std::erase_if(result.messages, [&](const Message& message) {
    return message.blocks.empty() || (message.ref.session != session.summary.id && !history_source_visible(message.ref.session));
  });
  for (const auto& [other_id, other] : sessions) {
    const auto* image = Published(other);
    if (!image || (other_id != session.summary.id && !visible_sessions.contains({user, other_id}))) continue;
    for (const auto& [id, entry] : image->collaboration) {
      auto summary = entry.record.summary;
      if (summary.to != session.summary.id || summary.from.principal != user) continue;
      if (summary.from.session && !visible_sessions.contains({user, *summary.from.session})) {
        summary.from.session.reset();
        summary.from.run.reset();
        summary.from.agent.reset();
        summary.from.call.reset();
      }
      result.inbox.push_back(std::move(summary));
    }
  }
  std::erase_if(result.interactions, [&](const auto& item) { return item.recipient != user; });
  const auto approval_access = visible_approval_artifacts.find({user, session.summary.id});
  const auto& approval_artifacts = approval_access == visible_approval_artifacts.end() ? empty : approval_access->second;
  for (auto& interaction : result.interactions) {
    FilterApprovalArtifacts(interaction, result.artifacts, permitted, approval_artifacts);
  }
  result.summary.pending_interactions = std::ranges::count(result.interactions, InteractionState::Pending,
                                                           &InteractionSnapshot::state);
  const auto& committed = session.retention == SessionRetention::Stored
      ? saved_sessions.at(session.summary.id).image : ephemeral_sessions.at(session.summary.id);
  for (const auto& run : committed.runs) FilterRunArtifacts(result.runs.emplace_back(run.view), permitted);
  std::map<MessageId, RunSteerSnapshot*> unapplied;
  for (auto& run : result.runs) {
    for (auto& steer : run.steering) {
      if (steer.state == SteerState::NotApplied) unapplied.emplace(steer.message, &steer);
    }
  }
  if (!unapplied.empty()) {
    const auto bind = [&](const CommandRecord& record) {
      const auto* steer = std::get_if<RunSteered>(&record.accepted);
      if (!steer || record.progress.state != CommandState::Failed ||
          record.progress.accepted.session != session.summary.id) return;
      if (const auto found = unapplied.find(steer->message); found != unapplied.end())
        found->second->recovery = record.progress.accepted.command;
    };
    if (session.retention == SessionRetention::Stored) {
      for (const auto& command : saved_sessions.at(session.summary.id).image.commands) {
        if (command.principal == user) bind(command.record);
      }
    } else {
      for (auto command = commands.lower_bound({user, {}}); command != commands.end() && command->first.first == user; ++command)
        bind(command->second);
    }
  }
  constexpr std::size_t history_page = 50;
  auto first = result.messages.size() - std::min(result.messages.size(), history_page);
  if (history) {
    const auto begin = std::ranges::lower_bound(result.messages, history->first, {}, &Message::sequence);
    const auto index = static_cast<std::size_t>(begin - result.messages.begin());
    first = std::min(first, index - std::min(index, history->preceding));
  }
  if (first) {
    result.messages.erase(result.messages.begin(), result.messages.begin() + first);
    result.older_history = Cursor(HistoryKey(user, session.summary.id, session.sequence), result.messages.front().sequence);
  }
  return std::make_shared<const SessionSnapshot>(std::move(result));
}

Async<void> RuntimeState::WithSession(SessionId id, PrincipalId bound, Capability capability) {
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<void>::Failure(user.Error());
  if (user.Value() != bound) co_return Fail<void>(ErrorCode::PermissionDenied, "The acting principal changed");
  auto it = sessions.find(id);
  const auto* published = it == sessions.end() ? nullptr : Published(it->second);
  if (!published || published->summary.lifecycle == SessionLifecycle::Deleting) {
    co_return Fail<void>(ErrorCode::NotFound, "Session unavailable");
  }
  if (published->origin.kind == SessionOriginKind::SideChat &&
      (!published->origin.creator || published->origin.creator->principal != bound))
    co_return Fail<void>(ErrorCode::NotFound, "Session unavailable");
  const auto epoch = access_revision;
  auto access = co_await Require(Request(bound, Capability::ReadSession, published));
  if (!access.Succeeded()) {
    visible_sessions.erase({bound, id});
    co_return access;
  }
  if (epoch != access_revision) co_return Fail<void>(ErrorCode::Conflict, "Conversation access changed while checking permissions");
  published = Published(it->second);
  if (!published || published->summary.lifecycle == SessionLifecycle::Deleting) {
    co_return Fail<void>(ErrorCode::NotFound, "Session unavailable");
  }
  visible_sessions.insert({bound, id});
  if (capability != Capability::ReadSession) {
    if (storage_error && published->retention == SessionRetention::Stored) {
      co_return AgentResult<void>::Failure(*storage_error);
    }
    co_return co_await Require(Request(bound, capability, &it->second));
  }
  co_return AgentResult<void>::Success();
}

Async<std::shared_ptr<const SessionSnapshot>> RuntimeState::ReadSession(SessionId id, PrincipalId bound,
    std::optional<SessionHistoryWindow> history) {
  using Result = AgentResult<std::shared_ptr<const SessionSnapshot>>;
  const auto epoch = access_revision;
  auto access = co_await WithSession(id, bound, Capability::ReadSession);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  const auto& session = sessions.at(id);
  auto selections = co_await VisibleSelections(id, bound);
  if (!selections.Succeeded()) co_return Result::Failure(selections.Error());
  if (epoch != access_revision)
    co_return Fail<std::shared_ptr<const SessionSnapshot>>(ErrorCode::Conflict, "Selection access changed");
  visible_selections.insert_or_assign({bound, id}, std::move(selections).Value());
  const auto* image = Published(session);
  if (image) {
    const ActionParty actor{.kind = ActionParty::Kind::User, .principal = bound};
    auto sources = co_await VisibleHistorySources(*image, actor);
    if (!sources.Succeeded()) co_return Result::Failure(sources.Error());
    image = Published(session);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting)
      co_return Fail<std::shared_ptr<const SessionSnapshot>>(ErrorCode::NotFound, "Session unavailable");
    const auto sequence = image->sequence;
    const auto& committed = session.retention == SessionRetention::Stored
        ? saved_sessions.at(id).image : ephemeral_sessions.at(id);
    ArtifactVersions related;
    for (const auto& run : committed.runs)
      if (run.view.result) CollectArtifacts(related, run.view.result->artifacts);
    for (const auto& call : image->tools) CollectArtifacts(related, call.artifacts);
    for (const auto& entry : image->queue) {
      const auto owner = image->queue_owners.find(entry.id);
      if (owner == image->queue_owners.end() || owner->second != bound) continue;
      if (const auto* input = std::get_if<SendInput>(&entry.input)) {
        CollectArtifacts(related, input->attachments);
        CollectArtifacts(related, input->context);
        CollectArtifacts(related, input->side_chat_conclusions);
      }
    }
    for (const auto& interaction : image->interactions)
      if (const auto* approval = std::get_if<OperationApproval>(&interaction.body); approval && interaction.recipient == bound)
        CollectArtifacts(related, approval->preview.artifacts);
    auto versions = co_await VisibleHistoryArtifacts(*image, actor, sources.Value(), std::move(related));
    if (!versions.Succeeded()) co_return Result::Failure(versions.Error());
    if (epoch != access_revision) co_return Fail<std::shared_ptr<const SessionSnapshot>>(ErrorCode::Conflict, "Conversation access changed while reading history");
    auto& checked = checked_session_artifacts[{bound, id}];
    if (sequence >= checked) {
      checked = sequence;
      visible_history_artifacts.insert_or_assign({bound, id}, std::move(versions).Value());
    }
  }
  auto review_artifacts = co_await VisibleApprovalArtifacts(id, bound);
  if (!review_artifacts.Succeeded()) co_return Result::Failure(review_artifacts.Error());
  if (epoch != access_revision)
    co_return Fail<std::shared_ptr<const SessionSnapshot>>(ErrorCode::Conflict, "Conversation access changed while checking approval artifacts");
  auto user = User();
  if (!user.Succeeded()) co_return Result::Failure(user.Error());
  if (user.Value() != bound)
    co_return Fail<std::shared_ptr<const SessionSnapshot>>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (review_artifacts.Value().empty()) visible_approval_artifacts.erase({bound, id});
  else visible_approval_artifacts.insert_or_assign({bound, id}, std::move(review_artifacts).Value());
  auto current = Snapshot(session, bound, history);
  if (!current) co_return Fail<std::shared_ptr<const SessionSnapshot>>(ErrorCode::NotFound, "Session unavailable");
  co_return Result::Success(std::move(current));
}

void RuntimeState::ScheduleSessionArtifactCheck(const SessionId& id, const PrincipalId& user) {
  const auto* image = Published(sessions.at(id));
  const auto key = std::pair{user, id};
  const auto checked = checked_session_artifacts.find(key);
  if (stage != Stage::Started || !image ||
      (checked != checked_session_artifacts.end() && checked->second == image->sequence) ||
      !checking_session_artifacts.insert(key).second) return;

  // Check committed references on the task scope so Snapshot() can remain a synchronous read.
  options.tasks.Launch([self = shared_from_this(), key]() -> Task<void> {
    bool refreshed = false;
    try {
      auto snapshot = co_await self->ReadSession(key.second, key.first);
      refreshed = snapshot.Succeeded();
    } catch (...) {
      self->ReportException(std::current_exception());
    }
    self->checking_session_artifacts.erase(key);
    if (refreshed) self->ScheduleSessionArtifactCheck(key.second, key.first);
  });
}

Async<std::optional<SessionEventBatch>> RuntimeState::ReadSessionEvents(SessionId id, PrincipalId user, Sequence after,
    std::optional<Revision> previous_access) {
  using Batch = std::optional<SessionEventBatch>;
  const auto epoch = access_revision;
  auto current = co_await ReadSession(id, user, SessionHistoryWindow{0, 0});
  if (!current.Succeeded()) {
    if (current.Error().code == ErrorCode::Conflict && access_revision != epoch)
      co_return Fail<Batch>(ErrorCode::CursorExpired, "Event access changed. Rebuild from a session snapshot");
    co_return AgentResult<Batch>::Failure(current.Error());
  }
  auto principal = User();
  if (!principal.Succeeded()) co_return AgentResult<Batch>::Failure(principal.Error());
  if (principal.Value() != user) co_return Fail<Batch>(ErrorCode::PermissionDenied, "The observing principal changed");
  const auto& snapshot = *current.Value();
  if (previous_access && *previous_access != snapshot.access_revision)
    co_return Fail<Batch>(ErrorCode::CursorExpired, "Event access changed. Rebuild from a session snapshot");
  const auto through = snapshot.event_sequence;
  if (after == through) co_return AgentResult<Batch>::Success({});
  if (after > through) co_return Fail<Batch>(ErrorCode::CursorExpired, "The event cursor is beyond the committed stream");
  const auto journal = session_events.find(id);
  if (journal == session_events.end())
    co_return Fail<Batch>(ErrorCode::CursorExpired, "The event journal is incomplete. Rebuild from a session snapshot");

  const ArtifactVersions empty;
  const auto versions = visible_history_artifacts.find({user, id});
  const auto approvals = visible_approval_artifacts.find({user, id});
  const auto selections = visible_selections.find({user, id});
  const auto& permitted = versions == visible_history_artifacts.end() ? empty : versions->second;
  const auto find_message = [&](const MessageRef& ref) {
    return std::ranges::find_if(snapshot.messages, [&](const Message& message) {
      return message.ref.session == ref.session && message.ref.message == ref.message;
    });
  };
  const auto visible = [&](auto& body) {
    using T = std::remove_cvref_t<decltype(body)>;
    if constexpr (std::is_same_v<T, SessionSummary>) {
      if (body.lifecycle == SessionLifecycle::Deleting) return false;
    } else if constexpr (std::is_same_v<T, SessionOrigin>) {
      if (!snapshot.origin) return false;
      body = *snapshot.origin;
    } else if constexpr (std::is_same_v<T, Message>) {
      const auto shown = find_message(body.ref);
      if (shown == snapshot.messages.end()) return false;
      std::erase_if(body.blocks, [&](const ContentBlock& block) {
        const auto allowed = std::ranges::find(shown->blocks, block.id, &ContentBlock::id);
        if (allowed == shown->blocks.end() || allowed->body.index() != block.body.index()) return true;
        if (const auto* conclusion = std::get_if<SideChatConclusion>(&block.body))
          return selections == visible_selections.end() || !VisibleConclusion(*conclusion, selections->second);
        return !VisibleArtifactBlock(block, permitted);
      });
      for (auto& block : body.blocks) {
        if (auto* custom = std::get_if<CustomBlock>(&block.body)) {
          const auto allowed = std::ranges::find(shown->blocks, block.id, &ContentBlock::id);
          const auto& current_custom = std::get<CustomBlock>(allowed->body);
          std::erase_if(custom->interactions, [&](const InteractionId& interaction) {
            return std::ranges::find(current_custom.interactions, interaction) == current_custom.interactions.end();
          });
        }
      }
      return !body.blocks.empty();
    } else if constexpr (std::is_same_v<T, MessageDelta>) {
      const auto shown = find_message(body.message);
      return shown != snapshot.messages.end() && std::ranges::find(shown->blocks, body.block, &ContentBlock::id) !=
          shown->blocks.end();
    } else if constexpr (std::is_same_v<T, RunSnapshot>) {
      if (std::ranges::find(snapshot.runs, body.id, &RunSnapshot::id) == snapshot.runs.end()) return false;
      FilterRunArtifacts(body, permitted);
    } else if constexpr (std::is_same_v<T, InteractionSnapshot>) {
      if (body.recipient != user || std::ranges::find(snapshot.interactions, body.id, &InteractionSnapshot::id) ==
          snapshot.interactions.end()) return false;
      FilterApprovalArtifacts(body, snapshot.artifacts, permitted,
          approvals == visible_approval_artifacts.end() ? empty : approvals->second);
    } else if constexpr (std::is_same_v<T, ToolCallSnapshot>) {
      const auto shown = std::ranges::find(snapshot.tools, body.id, &ToolCallSnapshot::id);
      if (shown == snapshot.tools.end()) return false;
      FilterArtifactRefs(body.artifacts, permitted);
      std::erase_if(body.source_changes, [&](const CommandId& command) {
        return std::ranges::find(shown->source_changes, command) == shown->source_changes.end();
      });
    } else if constexpr (std::is_same_v<T, QueuedInput>) {
      const auto shown = std::ranges::find(snapshot.queue, body.id, &QueuedInput::id);
      if (shown == snapshot.queue.end()) return false;
      FilterQueueArtifacts(body, permitted);
      std::erase_if(body.conclusions, [&](const auto& conclusion) {
        return selections == visible_selections.end() || !VisibleConclusion(conclusion, selections->second);
      });
    } else if constexpr (std::is_same_v<T, CollaborationSummary>) {
      const auto shown = std::ranges::find(snapshot.inbox, body.id, &CollaborationSummary::id);
      if (shown == snapshot.inbox.end()) return false;
      body.from = shown->from;
    } else if constexpr (std::is_same_v<T, ArtifactRegistered>) {
      return std::ranges::find(snapshot.artifacts, body.artifact) != snapshot.artifacts.end();
    }
    return true;
  };

  const auto sequence = [](const StoredSessionEvent& item) { return item.event.sequence; };
  const auto& events = journal->second;
  auto first = std::ranges::upper_bound(events, after, {}, sequence);
  constexpr Sequence batch_size = 256;
  SessionEventBatch batch{.through_sequence = after + std::min(batch_size, through - after)};
  for (auto next = after; next < batch.through_sequence; ++first) {
    if (first == events.end() || first->event.sequence != ++next)
      co_return Fail<Batch>(ErrorCode::CursorExpired, "The event journal has a gap. Rebuild from a session snapshot");
    auto event = first->event;
    if (!std::visit(visible, event.body)) continue;
    if (auto* summary = std::get_if<SessionSummary>(&event.body))
      summary->pending_interactions = std::ranges::count(first->pending_recipients, user);
    if (first->command_owner != user) event.command.reset();
    batch.events.push_back(std::move(event));
  }
  if (batch.through_sequence < through) ScheduleObservers();
  co_return AgentResult<Batch>::Success(std::move(batch));
}

Async<std::set<SessionId>> RuntimeState::VisibleHistorySources(const SessionData& image, ActionParty actor) {
  const auto access = access_revision;
  std::set<SessionId> candidates;
  if (image.origin.source_session) candidates.insert(*image.origin.source_session);
  for (const auto& message : image.messages) {
    if (message.ref.session != image.summary.id) candidates.insert(message.ref.session);
  }
  std::set<SessionId> visible;
  for (const auto& source : candidates) {
    auto session = sessions.find(source);
    const auto* source_image = session == sessions.end() ? nullptr : Published(session->second);
    auto allowed = source_image && source_image->summary.lifecycle != SessionLifecycle::Deleting
        ? co_await Require(CollaborationAccess(actor, Capability::ReadSession, *source_image))
        : Fail<void>(ErrorCode::NotFound, "History source unavailable");
    if (access != access_revision)
      co_return Fail<std::set<SessionId>>(ErrorCode::Conflict, "History access changed while checking sources");
    if (allowed.Succeeded()) visible.insert(source);
    if (actor.kind == ActionParty::Kind::User) {
      if (allowed.Succeeded()) visible_sessions.insert({actor.principal, source});
      else visible_sessions.erase({actor.principal, source});
    }
  }
  auto current = User();
  if (!current.Succeeded() || current.Value() != actor.principal || stage != Stage::Started) {
    co_return Fail<std::set<SessionId>>(ErrorCode::PermissionDenied, "The acting principal changed");
  }
  co_return AgentResult<std::set<SessionId>>::Success(std::move(visible));
}

Async<ArtifactVersions> RuntimeState::VisibleHistoryArtifacts(const SessionData& image, ActionParty actor,
    const std::set<SessionId>& history_sources, ArtifactVersions related) {
  const auto access = access_revision;
  const auto target = image.summary.id;
  SessionData pending{.summary = image.summary};
  ArtifactVersions candidates;
  ArtifactVersions owned;
  ArtifactVersions permitted;
  for (const auto& artifact : image.artifacts) {
    const auto& ref = artifact.record.metadata.ref;
    owned.emplace(ref.id, ref.version);
  }
  for (const auto& message : image.messages) {
    const bool inherited = message.ref.session != target;
    if (inherited && !history_sources.contains(message.ref.session)) continue;
    for (const auto& block : message.blocks)
      CollectArtifactRefs(inherited ? candidates : related, block);
  }
  for (const auto& ref : related)
    if (!owned.contains(ref)) candidates.insert(ref);
  for (const auto& [id, version] : candidates) {
    auto artifact = co_await ReadHistoryArtifact(pending, actor, {id, version});
    if (access != access_revision)
      co_return Fail<ArtifactVersions>(ErrorCode::Conflict, "History access changed while checking artifacts");
    if (artifact.Succeeded()) permitted.emplace(id, version);
  }
  for (const auto& [id, version] : owned) {
    auto artifact = co_await ReadArtifact({id, version}, actor);
    if (access != access_revision)
      co_return Fail<ArtifactVersions>(ErrorCode::Conflict, "Content access changed while checking artifacts");
    if (artifact.Succeeded()) permitted.emplace(id, version);
  }
  co_return AgentResult<ArtifactVersions>::Success(std::move(permitted));
}

Async<void> RuntimeState::CheckInheritedHistory(const RunExecution& run) {
  const auto& session = sessions.at(run.view.session);
  std::set<SessionId> sources;
  for (const auto& message : session.messages) {
    if (message.ref.session != session.summary.id) sources.insert(message.ref.session);
  }
  auto actor = Request(run.principal, Capability::ReadSession, &session, &run).actor;
  for (const auto& id : sources) {
    auto source = sessions.find(id);
    const auto* image = source == sessions.end() ? nullptr : Published(source->second);
    if (!image || image->summary.lifecycle == SessionLifecycle::Deleting) {
      co_return Fail<void>(ErrorCode::NotFound, "A history source is no longer available");
    }
    auto allowed = co_await Require(CollaborationAccess(actor, Capability::ReadSession, *image));
    if (!allowed.Succeeded()) co_return allowed;
  }
  co_return AgentResult<void>::Success();
}

Async<std::shared_ptr<Conversation>> RuntimeState::Open(SessionId id) {
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<std::shared_ptr<Conversation>>::Failure(user.Error());
  auto access = co_await ReadSession(id, user.Value());
  if (!access.Succeeded()) co_return AgentResult<std::shared_ptr<Conversation>>::Failure(access.Error());
  co_return AgentResult<std::shared_ptr<Conversation>>::Success(
      std::shared_ptr<Conversation>(new Conversation(shared_from_this(), std::move(id), user.Value())));
}

Async<SessionPage> RuntimeState::List(SessionFilter filter, std::optional<PageCursor> cursor, std::optional<RunId> bound) {
  auto actor = Actor(bound, Capability::DiscoverSession);
  if (!actor.Succeeded()) co_return AgentResult<SessionPage>::Failure(actor.Error());
  const auto user = actor.Value().principal;
  if (!filter.page_size || filter.page_size > 500) {
    co_return Fail<SessionPage>(ErrorCode::InvalidArgument, "Use a page size from 1 to 500");
  }
  const auto key = CursorKey(user, order_revision, CursorPart(bound ? bound->value : "") +
      CursorPart(filter.project ? filter.project->value : "") + CursorPart(filter.agent ? filter.agent->value : "") +
      std::to_string(filter.include_archived) + std::to_string(filter.include_ephemeral) + std::to_string(filter.page_size));
  auto offset = ReadCursor(cursor, key);
  if (!offset.Succeeded()) co_return AgentResult<SessionPage>::Failure(offset.Error());
  const auto ordering = order_revision;
  SessionPage page{.revision = revision, .order_revision = ordering};
  for (const auto& [id, session] : sessions) {
    const auto* published = Published(session);
    if (!published) continue;
    const auto summary = published->summary;
    if (published->origin.kind == SessionOriginKind::SideChat &&
        (bound || !published->origin.creator || published->origin.creator->principal != user)) continue;
    if (summary.lifecycle == SessionLifecycle::Deleting ||
        (!filter.include_archived && summary.lifecycle == SessionLifecycle::Archived) ||
        (!filter.include_ephemeral && session.retention == SessionRetention::Ephemeral) ||
        (filter.project && filter.project != summary.project) ||
        (filter.agent && *filter.agent != summary.agent.id)) continue;
    if (bound && actor.Value().session != id) {
      const auto& permitted = agents.at(actor.Value().agent->id).collaboration_targets;
      if (std::ranges::find(permitted, summary.agent.id) == permitted.end()) continue;
    }
    auto access = co_await Require(CollaborationAccess(actor.Value(), Capability::DiscoverSession, *published));
    auto still_allowed = Actor(bound, Capability::DiscoverSession);
    if (!still_allowed.Succeeded()) co_return AgentResult<SessionPage>::Failure(still_allowed.Error());
    if (order_revision != ordering || stage != Stage::Started) {
      co_return Fail<SessionPage>(stage == Stage::Started ? ErrorCode::CursorExpired : ErrorCode::RuntimeClosed,
                                 "Session directory changed while reading");
    }
    if (access.Succeeded()) {
      auto projection = summary;
      projection.pending_interactions = std::ranges::count_if(Published(session)->interactions, [&](const auto& item) {
        return item.state == InteractionState::Pending && item.recipient == user;
      });
      page.sessions.push_back(std::move(projection));
    }
  }
  std::ranges::sort(page.sessions, [](const auto& a, const auto& b) {
    if (a.last_activity != b.last_activity) return a.last_activity > b.last_activity;
    return a.id < b.id;
  });
  const auto start = std::min<std::uint64_t>(offset.Value(), page.sessions.size());
  const auto end = std::min<std::uint64_t>(start + filter.page_size, page.sessions.size());
  if (end < page.sessions.size()) page.next = Cursor(key, end);
  page.sessions.erase(page.sessions.begin() + end, page.sessions.end());
  page.sessions.erase(page.sessions.begin(), page.sessions.begin() + start);
  co_return AgentResult<SessionPage>::Success(std::move(page));
}

Async<HistoryPage> RuntimeState::ReadHistory(SessionId id, HistoryQuery query, std::optional<RunId> bound) {
  const auto epoch = access_revision;
  auto actor = Actor(bound, Capability::ReadSession);
  if (!actor.Succeeded()) co_return AgentResult<HistoryPage>::Failure(actor.Error());
  auto found = sessions.find(id);
  if (found == sessions.end() || !Published(found->second) ||
      found->second.summary.lifecycle == SessionLifecycle::Deleting) {
    co_return Fail<HistoryPage>(ErrorCode::NotFound, "Session unavailable");
  }
  const auto& origin = found->second.origin;
  if (origin.kind == SessionOriginKind::SideChat &&
      (bound || !origin.creator || origin.creator->principal != actor.Value().principal))
    co_return Fail<HistoryPage>(ErrorCode::NotFound, "Session unavailable");
  if (bound && actor.Value().session != id) {
    const auto& allowed = agents.at(actor.Value().agent->id).collaboration_targets;
    if (std::ranges::find(allowed, found->second.summary.agent.id) == allowed.end()) {
      co_return Fail<HistoryPage>(ErrorCode::PermissionDenied, "History is outside the declared collaboration scope");
    }
  }
  auto readable = co_await Require(CollaborationAccess(actor.Value(), Capability::ReadSession, found->second));
  if (!readable.Succeeded()) co_return AgentResult<HistoryPage>::Failure(readable.Error());
  auto selections = co_await VisibleSelections(id, actor.Value().principal);
  if (!selections.Succeeded()) co_return AgentResult<HistoryPage>::Failure(selections.Error());
  const bool selection_visible = origin.kind != SessionOriginKind::SideChat ||
      selections.Value().size() == origin.side_chat_selection.size();
  auto sources = co_await VisibleHistorySources(*Published(found->second), actor.Value());
  if (!sources.Succeeded()) co_return AgentResult<HistoryPage>::Failure(sources.Error());
  auto versions = co_await VisibleHistoryArtifacts(*Published(found->second), actor.Value(), sources.Value());
  if (!versions.Succeeded()) co_return AgentResult<HistoryPage>::Failure(versions.Error());
  auto still_allowed = Actor(bound, Capability::ReadSession);
  if (!still_allowed.Succeeded()) co_return AgentResult<HistoryPage>::Failure(still_allowed.Error());
  const auto* current = Published(found->second);
  if (!current || current->summary.lifecycle == SessionLifecycle::Deleting)
    co_return Fail<HistoryPage>(ErrorCode::NotFound, "Session unavailable");
  if (epoch != access_revision) co_return Fail<HistoryPage>(ErrorCode::Conflict, "History access changed while reading the page");
  const auto& session = *current;
  if (!query.page_size || query.page_size > 500 || (query.before && !query.through_sequence)) {
    co_return Fail<HistoryPage>(ErrorCode::InvalidArgument, "Invalid history page size or missing history boundary");
  }
  const auto through = query.through_sequence.value_or(session.sequence);
  if (through > session.sequence) co_return Fail<HistoryPage>(ErrorCode::InvalidArgument, "History boundary is uncommitted");
  auto key = HistoryKey(actor.Value().principal, id, through);
  if (bound) key += CursorPart(bound->value) + ":";
  auto before = ReadCursor(query.before, key);
  if (!before.Succeeded()) co_return AgentResult<HistoryPage>::Failure(before.Error());
  HistoryPage page{.through_sequence = through};
  for (auto it = session.messages.rbegin(); it != session.messages.rend(); ++it) {
    if (it->sequence > through || (before.Value() && it->sequence >= before.Value())) continue;
    auto message = *it;
    if (!selection_visible && message.author == MessageAuthor::Agent) continue;
    std::erase_if(message.blocks, [&](const auto& block) {
      const auto* conclusion = std::get_if<SideChatConclusion>(&block.body);
      return conclusion && !VisibleConclusion(*conclusion, selections.Value());
    });
    const bool inherited = message.ref.session != id;
    if ((inherited && !sources.Value().contains(message.ref.session)) ||
        !FilterHistoryArtifacts(message, versions.Value(), inherited)) continue;
    if (!inherited) FilterContentInteractions(message, session, actor.Value().principal);
    if (page.messages.size() == query.page_size) {
      page.older = Cursor(key, page.messages.back().sequence);
      break;
    }
    page.messages.push_back(std::move(message));
  }
  std::ranges::reverse(page.messages);
  co_return AgentResult<HistoryPage>::Success(std::move(page));
}

Async<PendingInteractionPage> RuntimeState::ListPending(InteractionFilter filter, std::optional<PageCursor> cursor) {
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<PendingInteractionPage>::Failure(user.Error());
  if (!filter.page_size || filter.page_size > 500) {
    co_return Fail<PendingInteractionPage>(ErrorCode::InvalidArgument, "Invalid page size");
  }
  PendingInteractionPage page{.revision = revision};
  const auto key = CursorKey(user.Value(), page.revision,
      CursorPart(filter.session ? filter.session->value : "") + CursorPart(filter.project ? filter.project->value : "") +
      std::to_string(filter.page_size));
  auto offset = ReadCursor(cursor, key);
  if (!offset.Succeeded()) co_return AgentResult<PendingInteractionPage>::Failure(offset.Error());
  for (const auto& [id, session] : sessions) {
    const auto* published = Published(session);
    if (!published || (filter.session && *filter.session != id) ||
        (filter.project && filter.project != published->summary.project)) continue;
    auto access = co_await WithSession(id, user.Value(), Capability::ReadSession);
    if (!access.Succeeded()) continue;
    access = co_await Require(Request(user.Value(), Capability::SubmitInteraction, Published(session)));
    if (!access.Succeeded()) continue;
    published = Published(session);
    for (const auto& item : published->interactions) {
      if (item.state == InteractionState::Pending && item.recipient == user.Value()) {
        page.items.push_back({published->summary, item});
      }
    }
  }
  if (page.revision != revision) co_return Fail<PendingInteractionPage>(ErrorCode::CursorExpired, "Pending interactions changed");
  std::ranges::sort(page.items, [](const auto& a, const auto& b) {
    return std::tuple(a.interaction.expires_at.value_or(TimePoint::max()), a.session.id, a.interaction.id) <
           std::tuple(b.interaction.expires_at.value_or(TimePoint::max()), b.session.id, b.interaction.id);
  });
  const auto start = std::min<std::uint64_t>(offset.Value(), page.items.size());
  const auto end = std::min<std::uint64_t>(start + filter.page_size, page.items.size());
  if (end < page.items.size()) page.next = Cursor(key, end);
  page.items.erase(page.items.begin() + end, page.items.end());
  page.items.erase(page.items.begin(), page.items.begin() + start);
  for (auto& entry : page.items) {
    auto* tool = std::get_if<ToolApproval>(&entry.interaction.body);
    auto* operation = std::get_if<OperationApproval>(&entry.interaction.body);
    auto* preview = tool ? &tool->preview : operation ? &operation->preview : nullptr;
    if (!preview) continue;
    std::vector<ArtifactRef> attachments;
    const ActionParty actor{.kind = ActionParty::Kind::User, .principal = user.Value()};
    for (const auto& ref : preview->artifacts) {
      auto access = co_await ReadActionArtifact(entry.session.id, user.Value(), ref);
      if (!access.Succeeded() && access.Error().code == ErrorCode::NotFound) {
        access = tool ? co_await ReadApprovalArtifact(entry.session.id, user.Value(), ref)
                      : co_await ReadHistoryArtifact(sessions.at(entry.session.id), actor, ref);
      }
      if (access.Succeeded()) attachments.push_back(ref);
    }
    preview->artifacts = std::move(attachments);
  }
  if (page.revision != revision) co_return Fail<PendingInteractionPage>(ErrorCode::CursorExpired, "Pending interactions changed");
  co_return AgentResult<PendingInteractionPage>::Success(std::move(page));
}

Async<CommandProgress> RuntimeState::ReadCommand(CommandLookup lookup, std::optional<RunId> bound) {
  using Result = AgentResult<CommandProgress>;
  auto user = User();
  if (!user.Succeeded()) co_return Result::Failure(user.Error());
  ActionParty actor{.kind = ActionParty::Kind::User, .principal = user.Value()};
  if (bound) {
    auto run = runs.find(*bound);
    if (run == runs.end() || run->second.principal != user.Value() || Terminal(run->second.view.state) ||
        run->second.stop.stop_requested()) co_return Fail<CommandProgress>(ErrorCode::AlreadyClosed, "The acting run ended");
    actor = Request(user.Value(), Capability::ReadSession, &sessions.at(run->second.view.session), &run->second).actor;
  }
  CommandId id;
  if (const auto* direct = std::get_if<CommandId>(&lookup)) {
    id = *direct;
  } else {
    if (bound) co_return Fail<CommandProgress>(ErrorCode::PermissionDenied, "Only a reader with approval access can look up an interaction");
    const auto& reference = std::get<PendingApproval>(lookup);
    auto access = co_await WithSession(reference.session, user.Value(), Capability::SubmitInteraction);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    const auto* image = Published(sessions.at(reference.session));
    auto found = std::ranges::find(image->interactions, reference.interaction, &InteractionSnapshot::id);
    if (found == image->interactions.end() || found->recipient != user.Value()) {
      co_return Fail<CommandProgress>(ErrorCode::NotFound, "No matching operation approval");
    }
    const auto* approval = std::get_if<OperationApproval>(&found->body);
    if (!approval) co_return Fail<CommandProgress>(ErrorCode::NotFound, "No matching operation approval");
    id = approval->pending_command;
    auto command = commands.find({user.Value(), id});
    if (command == commands.end() || !command->second.progress.approval ||
        (reference.version != found->version && reference.version != command->second.progress.approval->version)) {
      co_return Fail<CommandProgress>(ErrorCode::Conflict, "Approval version changed");
    }
  }
  const auto it = commands.find({user.Value(), id});
  if (it == commands.end()) {
    auto project_command = FindProjectCommand(user.Value(), id, true);
    if (!project_command || (bound && project_command->actor != bound)) {
      co_return Fail<CommandProgress>(ErrorCode::NotFound, "Command not found");
    }
    auto project = *project_command->progress.accepted.project;
    auto permission = co_await ProjectAccess(project, bound, Capability::ReadProject);
    if (!permission.Succeeded()) co_return Result::Failure(permission.Error());
    project_command = FindProjectCommand(user.Value(), id, true);
    if (!project_command) co_return Fail<CommandProgress>(ErrorCode::NotFound, "Command not found");
    auto progress = project_command->progress;
    if (auto* comparison = progress.result ? std::get_if<ForkReviewCreated>(&*progress.result) : nullptr) {
      auto visible = co_await FilterComparisonResult(*comparison, bound);
      if (!visible.Succeeded()) co_return Result::Failure(visible.Error());
    }
    co_return Result::Success(std::move(progress));
  }
  if (bound && it->second.input.run != *bound) co_return Fail<CommandProgress>(ErrorCode::NotFound, "Command is outside the acting run");
  const auto& session = sessions.at(*it->second.progress.accepted.session);
  auto access = co_await WithSession(session.summary.id, user.Value(), Capability::ReadSession);
  if (!access.Succeeded()) co_return Result::Failure(access.Error());
  if (bound) {
    access = co_await Require(CollaborationAccess(actor, Capability::ReadSession, session));
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
  }
  if (it->second.input.adoption) {
    auto candidate = co_await ReadAdoptionCandidate(*it->second.input.adoption, actor);
    if (!candidate.Succeeded()) co_return Result::Failure(candidate.Error());
  }
  auto projection = it->second.progress;
  if (session.retention == SessionRetention::Stored) {
    const auto& entries = saved_sessions.at(session.summary.id).image.commands;
    const auto found = std::ranges::find_if(entries, [&](const auto& item) {
      return item.principal == user.Value() && item.record.progress.accepted.command == id;
    });
    if (found == entries.end()) co_return Fail<CommandProgress>(ErrorCode::NotFound, "Command has no committed receipt");
    projection = found->record.progress;
  }
  if (auto* comparison = projection.result ? std::get_if<ForkReviewCreated>(&*projection.result) : nullptr) {
    auto visible = co_await FilterComparisonResult(*comparison, bound);
    if (!visible.Succeeded()) co_return Result::Failure(visible.Error());
  }
  if (projection.approval && projection.result) {
    std::optional<SessionId> result_session;
    if (const auto* created = std::get_if<SessionCreated>(&*projection.result)) result_session = created->session;
    if (const auto* message = std::get_if<MessageAccepted>(&*projection.result)) {
      auto delivered = FindMessage(message->message);
      if (delivered) result_session = delivered->first;
      else projection.result.reset();
    }
    if (result_session && *result_session != session.summary.id) {
      auto target = sessions.find(*result_session);
      if (target == sessions.end() || !Published(target->second) ||
          !(co_await Require(CollaborationAccess(actor, Capability::ReadSession, target->second))).Succeeded()) {
        projection.result.reset();
      }
    }
  }
  co_return Result::Success(std::move(projection));
}

} // namespace detail

using detail::RuntimeState;

SessionService::SessionService(std::shared_ptr<RuntimeState> state, std::optional<RunId> actor)
    : state_(std::move(state)), actor_(std::move(actor)) {}

CommandService::CommandService(std::shared_ptr<RuntimeState> state, std::optional<RunId> actor)
    : state_(std::move(state)), actor_(std::move(actor)) {}

InteractionService::InteractionService(std::shared_ptr<RuntimeState> state) : state_(std::move(state)) {}

Conversation::Conversation(std::shared_ptr<RuntimeState> state, SessionId id, PrincipalId principal)
    : state_(std::move(state)), id_(std::move(id)), principal_(std::move(principal)) {}

std::shared_ptr<const SessionSnapshot> Conversation::Snapshot() const {
  auto user = state_->User();
  if (!user.Succeeded() || user.Value() != principal_ || !state_->visible_sessions.contains({principal_, id_})) return {};
  const auto& session = state_->sessions.at(id_);
  return state_->Snapshot(session, principal_);
}

Async<ArtifactPreview> Conversation::PreviewArtifactAsync(ArtifactRef artifact) {
  return [](std::shared_ptr<RuntimeState> state, ArtifactRef ref, SessionId id, PrincipalId principal)
      -> Async<ArtifactPreview> {
    co_return co_await state->PreviewArtifact(std::move(ref), std::move(id), std::move(principal));
  }(state_, std::move(artifact), id_, principal_);
}

Async<std::string> Conversation::ReadSelectionAsync(SourceSlice selection) {
  return [](std::shared_ptr<detail::RuntimeState> state, SessionId id, PrincipalId principal, SourceSlice slice)
      -> Async<std::string> {
    auto access = co_await state->WithSession(id, principal, Capability::ReadSession);
    if (!access.Succeeded()) co_return AgentResult<std::string>::Failure(access.Error());
    const ActionParty actor{.kind = ActionParty::Kind::User, .principal = principal};
    co_return co_await state->ResolveSelection(std::move(id), actor, std::move(slice));
  }(state_, id_, principal_, std::move(selection));
}

Async<Subscription> Conversation::ObserveAsync(Observer<SessionSnapshot> observer) {
  return ObserveAsync(std::move(observer), std::nullopt);
}

Async<Subscription> Conversation::ObserveAsync(Observer<SessionSnapshot> observer, std::optional<SessionHistoryWindow> history) {
  return RuntimeState::Observe(state_, std::move(observer),
      [id = id_, user = principal_, history](RuntimeState& state) -> Async<SessionSnapshot> {
        const auto epoch = state.access_revision;
        auto result = co_await state.ReadSession(id, user, history);
        if (!result.Succeeded() && result.Error().code == ErrorCode::Conflict && state.access_revision != epoch)
          co_return detail::Fail<SessionSnapshot>(ErrorCode::CursorExpired, "Snapshot access changed");
        if (!result.Succeeded()) co_return AgentResult<SessionSnapshot>::Failure(result.Error());
        co_return AgentResult<SessionSnapshot>::Success(*result.Value());
      });
}

Async<Subscription> Conversation::ObserveEventsAsync(Sequence after, Observer<SessionEventBatch> observer) {
  using Batch = std::optional<SessionEventBatch>;
  using Result = AgentResult<std::shared_ptr<const SessionEventBatch>>;
  Observer<Batch> deliver;
  if (observer) {
    deliver = [observer = std::move(observer)](AgentResult<std::shared_ptr<const Batch>> result) {
      if (!result.Succeeded()) observer(Result::Failure(result.Error()));
      else if (const auto& value = result.Value(); value->has_value())
        observer(Result::Success(std::shared_ptr<const SessionEventBatch>(value, &value->value())));
    };
  }
  auto cursor = std::make_shared<std::pair<Sequence, std::optional<Revision>>>(after, std::nullopt);
  return RuntimeState::Observe(state_, std::move(deliver),
      [id = id_, user = principal_, cursor = std::move(cursor)](RuntimeState& state) -> Async<Batch> {
        auto& [after, access] = *cursor;
        auto result = co_await state.ReadSessionEvents(id, user, after, access);
        if (result.Succeeded()) {
          access = state.access_revision;
          if (result.Value()) after = result.Value()->through_sequence;
        }
        co_return result;
      }, false);
}

Async<std::shared_ptr<Conversation>> SessionService::OpenAsync(SessionId id) {
  return [](std::shared_ptr<RuntimeState> state, SessionId session, bool bound) -> Async<std::shared_ptr<Conversation>> {
    if (bound) co_return detail::Fail<std::shared_ptr<Conversation>>(
        ErrorCode::PermissionDenied, "Interactive conversations require a user session service");
    co_return co_await state->Open(std::move(session));
  }(state_, std::move(id), actor_.has_value());
}

Async<SessionPage> SessionService::ListAsync(SessionFilter filter, std::optional<PageCursor> cursor) {
  return [](std::shared_ptr<RuntimeState> state, SessionFilter query, std::optional<PageCursor> after, std::optional<RunId> bound)
      -> Async<SessionPage> {
    co_return co_await state->List(std::move(query), std::move(after), bound);
  }(state_, std::move(filter), std::move(cursor), actor_);
}

Async<HistoryPage> SessionService::ReadHistoryAsync(SessionId id, HistoryQuery query) {
  return [](std::shared_ptr<RuntimeState> state, SessionId session, HistoryQuery page, std::optional<RunId> bound)
      -> Async<HistoryPage> {
    co_return co_await state->ReadHistory(std::move(session), std::move(page), bound);
  }(state_, std::move(id), std::move(query), actor_);
}

Async<Subscription> SessionService::ObserveListAsync(SessionFilter filter, Observer<SessionPage> observer) {
  return RuntimeState::Observe(state_, std::move(observer),
      [query = std::move(filter), bound = actor_](RuntimeState& state) { return state.List(query, {}, bound); });
}

Async<PendingInteractionPage> InteractionService::ListPendingAsync(InteractionFilter filter, std::optional<PageCursor> cursor) {
  return [](std::shared_ptr<RuntimeState> state, InteractionFilter query, std::optional<PageCursor> after)
      -> Async<PendingInteractionPage> { co_return co_await state->ListPending(std::move(query), std::move(after)); }
      (state_, std::move(filter), std::move(cursor));
}

Async<Subscription> InteractionService::ObservePendingAsync(InteractionFilter filter, Observer<PendingInteractionPage> observer) {
  return RuntimeState::Observe(state_, std::move(observer),
      [query = std::move(filter)](RuntimeState& state) { return state.ListPending(query, {}); });
}

Async<CommandProgress> CommandService::ReadAsync(CommandLookup lookup) {
  return [](std::shared_ptr<RuntimeState> state, CommandLookup ref, std::optional<RunId> bound) -> Async<CommandProgress> {
    co_return co_await state->ReadCommand(std::move(ref), bound);
  }(state_, std::move(lookup), actor_);
}

Async<Subscription> CommandService::ObserveAsync(CommandLookup lookup, Observer<CommandProgress> observer) {
  return RuntimeState::Observe(state_, std::move(observer),
      [ref = std::move(lookup), bound = actor_](RuntimeState& state) { return state.ReadCommand(ref, bound); });
}

} // namespace huxerui::agentkit
