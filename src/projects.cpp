#include "runtime_internal.h"
#include "json_wire.h"

#include <nlohmann/json.hpp>
#include <ranges>

namespace huxerui::agentkit::detail {
namespace {

const SharedContextItem* FindContext(const ProjectData* project, const SharedContextRef& ref) {
  if (!project || !ref.version) return nullptr;
  const auto versions = project->context.find(ref.item);
  if (versions == project->context.end() || ref.version > versions->second.size()) return nullptr;
  return &versions->second[ref.version - 1];
}

PolicyRequest ContextPolicy(RuntimeState& runtime, const ActionParty& actor, const SharedContextItem& item) {
  auto policy = runtime.ProjectPolicy(actor, item.ref.project, Capability::ReadContext);
  policy.sources.push_back({item.ref});
  policy.operation = JsonValue{JsonValue::Object{{"access_profile", JsonValue{item.data.access_profile}}}};
  return policy;
}

AgentResult<void> CheckDraft(const SharedContextDraft& draft) {
  if (draft.title.empty() || draft.title.size() > 512 || !ValidUtf8(draft.title) || !ValidUtf8(draft.access_profile)) {
    return Fail<void>(ErrorCode::InvalidArgument, "Shared context requires a valid title and access profile");
  }
  return ValidateJson(draft.content, JsonSchema{JsonValue{true}});
}

} // namespace

PolicyRequest RuntimeState::ProjectPolicy(const ActionParty& actor, ProjectId project, Capability capability) const {
  PolicyRequest policy{.actor = actor, .capability = capability, .project = std::move(project)};
  if (actor.agent) policy.agent = actor.agent;
  if (actor.session) policy.workspace = sessions.at(*actor.session).summary.workspace;
  return policy;
}

Async<ActionParty> RuntimeState::ProjectActor(ProjectId project, std::optional<RunId> bound, Capability capability) {
  auto actor = Actor(bound, capability);
  if (!actor.Succeeded()) co_return actor;
  if (project.value.empty() || !ValidUtf8(project.value)) co_return Fail<ActionParty>(ErrorCode::InvalidArgument, "Project ID is required");
  if (bound && sessions.at(*actor.Value().session).summary.project != project) {
    co_return Fail<ActionParty>(ErrorCode::PermissionDenied, "The run belongs to a different project");
  }
  if (options.project) {
    auto binding = co_await options.project(project);
    if (!binding.Succeeded()) co_return AgentResult<ActionParty>::Failure(binding.Error());
    if (binding.Value().project != project) co_return Fail<ActionParty>(ErrorCode::NotFound, "Project unavailable");
    if (bound) {
      const auto& workspace = sessions.at(*actor.Value().session).summary.workspace;
      if (workspace && std::ranges::find(binding.Value().workspaces, *workspace) == binding.Value().workspaces.end()) {
        co_return Fail<ActionParty>(ErrorCode::PermissionDenied, "Run workspace is outside the project");
      }
    }
  }
  auto current = Actor(bound, capability);
  if (!current.Succeeded()) co_return current;
  if (current.Value().principal != actor.Value().principal ||
      (bound && sessions.at(*current.Value().session).summary.project != project))
    co_return Fail<ActionParty>(ErrorCode::PermissionDenied, "The acting principal or project changed");
  co_return current;
}

Async<ActionParty> RuntimeState::ProjectAccess(ProjectId project, std::optional<RunId> bound, Capability capability) {
  auto actor = co_await ProjectActor(project, bound, capability);
  if (!actor.Succeeded()) co_return actor;
  auto allowed = co_await Require(ProjectPolicy(actor.Value(), project, capability));
  if (!allowed.Succeeded()) co_return AgentResult<ActionParty>::Failure(allowed.Error());
  auto current = Actor(bound, capability);
  if (!current.Succeeded()) co_return current;
  if (current.Value().principal != actor.Value().principal) co_return Fail<ActionParty>(ErrorCode::PermissionDenied, "The acting principal changed");
  co_return current;
}

const ProjectData* RuntimeState::PublishedProject(const ProjectId& project) const {
  if (options.store) {
    auto found = saved_projects.find(project);
    return found == saved_projects.end() ? nullptr : &found->second.image;
  }
  auto found = projects.find(project);
  return found == projects.end() ? nullptr : &found->second;
}

const ProjectCommandRecord* RuntimeState::FindProjectCommand(const PrincipalId& user, const CommandId& command, bool committed) const {
  for (const auto& [id, working] : projects) {
    const auto* project = committed ? PublishedProject(id) : &working;
    if (!project) continue;
    auto found = std::ranges::find_if(project->commands, [&](const auto& record) {
      return record.principal == user && record.progress.accepted.command == command;
    });
    if (found != project->commands.end()) return &*found;
  }
  return nullptr;
}

Async<ProjectSnapshot> RuntimeState::ReadProject(ProjectId id, std::optional<RunId> bound) {
  using Result = AgentResult<ProjectSnapshot>;
  auto actor = co_await ProjectAccess(id, bound, Capability::ReadProject);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  ProjectSnapshot snapshot{.project = id};
  std::vector<SharedContextItem> contexts;
  if (const auto* project = PublishedProject(id)) {
    snapshot.revision = project->revision;
    for (const auto& [task_id, task] : project->tasks) snapshot.tasks.push_back(task.snapshot);
    for (const auto& [item_id, versions] : project->context) {
      for (const auto& item : versions) {
        if (item.state == SharedContextState::Published ||
            (item.publisher == actor.Value().principal && item.state != SharedContextState::Withdrawn)) contexts.push_back(item);
      }
    }
  }
  std::vector<ProjectTaskSnapshot> visible;
  for (auto& task : snapshot.tasks) {
    auto scope = ProjectPolicy(actor.Value(), id, Capability::ReadProject);
    scope.target.task = task.id;
    scope.sources = task.resolved_inputs;
    auto permitted = co_await Require(std::move(scope));
    if (!permitted.Succeeded()) continue;
    auto sources = task.resolved_inputs;
    sources.insert(sources.end(), task.spec.input.context.begin(), task.spec.input.context.end());
    if (task.outcome) {
      for (const auto& artifact : task.outcome->artifacts) sources.push_back({artifact});
      if (task.outcome->evidence) {
        const auto& evidence = task.outcome->evidence->sources;
        sources.insert(sources.end(), evidence.begin(), evidence.end());
      }
    }
    auto readable = co_await ValidateProjectSources(id, actor.Value(), sources);
    if (!readable.Succeeded()) {
      task.resolved_inputs.clear();
      task.spec.input.context.clear();
      if (task.outcome) {
        task.outcome->artifacts.clear();
        task.outcome->evidence.reset();
      }
    }
    visible.push_back(std::move(task));
  }
  snapshot.tasks = std::move(visible);
  for (const auto& item : contexts) {
    auto allowed = co_await Require(ContextPolicy(*this, actor.Value(), item));
    if (allowed.Succeeded()) snapshot.shared_context.push_back(item.ref);
  }
  for (const auto& [session_id, working] : sessions) {
    const auto* image = Published(working);
    if (!image || image->summary.project != id || image->summary.lifecycle == SessionLifecycle::Deleting) continue;
    const auto summary = image->summary;
    auto allowed = co_await Require(CollaborationAccess(actor.Value(), Capability::DiscoverSession, *image));
    if (allowed.Succeeded()) snapshot.sessions.push_back(summary);
  }
  const auto* latest = PublishedProject(id);
  if (snapshot.revision != (latest ? latest->revision : 0)) {
    co_return Fail<ProjectSnapshot>(ErrorCode::CursorExpired, "The project changed while reading");
  }
  auto current = Actor(bound, Capability::ReadProject);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != actor.Value().principal) co_return Fail<ProjectSnapshot>(ErrorCode::PermissionDenied, "The acting principal changed");
  co_return Result::Success(std::move(snapshot));
}

Async<void> RuntimeState::ValidateProjectSources(ProjectId id, ActionParty actor, const std::vector<SourceSlice>& sources) {
  auto pending = sources;
  std::set<std::pair<SharedContextId, Revision>> checked;
  for (std::size_t index = 0; index < pending.size(); ++index) {
    auto slice = pending[index];
    if (slice.text_range && slice.text_range->begin_utf8 >= slice.text_range->end_utf8) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid source range");
    }
    auto policy = ProjectPolicy(actor, id, Capability::ReadContext);
    policy.sources.push_back(slice);
    if (const auto* shared = std::get_if<SharedContextRef>(&slice.source)) {
      if (shared->project != id) co_return Fail<void>(ErrorCode::PermissionDenied, "Source context belongs to a different project");
      const auto* item = FindContext(PublishedProject(id), *shared);
      if (!item || item->state != SharedContextState::Published) {
        co_return Fail<void>(ErrorCode::NotFound, "The published source version is no longer available");
      }
      if (!checked.emplace(shared->item, shared->version).second) continue;
      pending.insert(pending.end(), item->data.sources.begin(), item->data.sources.end());
      policy = ContextPolicy(*this, actor, *item);
    } else if (const auto* artifact = std::get_if<ArtifactRef>(&slice.source)) {
      auto record = co_await ReadArtifact(*artifact, actor);
      if (!record.Succeeded()) co_return AgentResult<void>::Failure(record.Error());
      const auto& session = sessions.at(record.Value().record.session);
      const auto* image = Published(session);
      if (!image || image->summary.project != id) co_return Fail<void>(ErrorCode::PermissionDenied, "Artifact belongs to a different project");
      if (!slice.parts.empty() || slice.text_range) {
        auto read = CollaborationAccess(actor, Capability::ReadArtifact, *image);
        read.sources.push_back(slice);
        auto allowed = co_await Require(std::move(read));
        if (!allowed.Succeeded()) co_return allowed;
      }
      const auto& parts = record.Value().record.parts;
      if (std::ranges::any_of(slice.parts, [&](const auto& part) {
            return std::ranges::find(parts, part, &ArtifactPart::id) == parts.end();
          })) co_return Fail<void>(ErrorCode::NotFound, "Artifact part is unavailable");
    } else if (const auto* message = std::get_if<MessageRef>(&slice.source)) {
      auto session = sessions.find(message->session);
      const auto* image = session == sessions.end() ? nullptr : Published(session->second);
      if (!image || image->summary.project != id || std::ranges::none_of(image->messages, [&](const auto& item) {
            return item.ref == *message && item.state == MessageState::Complete;
          })) co_return Fail<void>(ErrorCode::NotFound, "The message source version is unavailable");
      auto allowed = co_await Require(CollaborationAccess(actor, Capability::ReadSession, *image));
      if (!allowed.Succeeded()) co_return allowed;
    } else {
      const auto& resource = std::get<ResourceRef>(slice.source);
      if (resource.id.value.empty() || resource.version.empty()) co_return Fail<void>(ErrorCode::InvalidArgument, "Source resource version is required");
    }
    auto allowed = co_await Require(std::move(policy));
    if (!allowed.Succeeded()) co_return allowed;
  }
  for (const auto& [id_value, version] : checked) {
    const auto* item = FindContext(PublishedProject(id), {id, id_value, version});
    if (!item || item->state != SharedContextState::Published) {
      co_return Fail<void>(ErrorCode::NotFound, "The published source version changed");
    }
  }
  co_return AgentResult<void>::Success();
}

Async<void> RuntimeState::AuthorizeWithdrawnContextImpact(ProjectId id, ActionParty actor, SharedContextRef source) {
  const auto* item = FindContext(PublishedProject(id), source);
  if (source.project != id || !item || item->state != SharedContextState::Withdrawn) {
    co_return Fail<void>(ErrorCode::NotFound, "The withdrawn source is unavailable");
  }
  auto policy = ContextPolicy(*this, actor, *item);
  const auto dependencies = item->data.sources;
  auto allowed = co_await Require(std::move(policy));
  if (!allowed.Succeeded()) co_return allowed;
  co_return co_await ValidateProjectSources(std::move(id), std::move(actor), dependencies);
}

Async<void> RuntimeState::ValidateInputSources(const SessionData& session, ActionParty actor,
                                               const std::vector<SourceSlice>& sources) {
  if (sources.empty()) co_return AgentResult<void>::Success();
  std::vector<SourceSlice> project_sources;
  for (const auto& source : sources) {
    if (std::holds_alternative<ResourceRef>(source.source) && source.text_range &&
        (!source.parts.empty() || source.text_range->begin_utf8 >= source.text_range->end_utf8)) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid resource context range");
    }
    const auto* ref = std::get_if<ArtifactRef>(&source.source);
    if (ref && InheritedArtifact(session, *ref)) {
      auto inherited = co_await ReadHistoryArtifact(session, actor, *ref);
      if (!inherited.Succeeded()) co_return AgentResult<void>::Failure(inherited.Error());
      if (std::ranges::any_of(source.parts, [&](const auto& id) {
            return std::ranges::find(inherited.Value().record.parts, id, &ArtifactPart::id) == inherited.Value().record.parts.end();
          })) co_return Fail<void>(ErrorCode::NotFound, "Artifact part is unavailable");
      continue;
    }
    if (session.summary.project) {
      project_sources.push_back(source);
      continue;
    }
    auto policy = CollaborationAccess(actor, Capability::ReadContext, session);
    policy.sources.push_back(source);
    if (const auto* message = std::get_if<MessageRef>(&source.source)) {
      const auto* current = Published(session);
      if (message->session != session.summary.id || !current || std::ranges::none_of(current->messages, [&](const auto& entry) {
            return entry.ref == *message && entry.state == MessageState::Complete;
          })) co_return Fail<void>(ErrorCode::NotFound, "Message source is unavailable");
    } else if (ref) {
      auto artifact = co_await ReadArtifact(*ref, actor);
      if (!artifact.Succeeded()) co_return AgentResult<void>::Failure(artifact.Error());
      if (artifact.Value().record.session != session.summary.id) {
        co_return Fail<void>(ErrorCode::PermissionDenied, "Artifact source is outside the conversation");
      }
      continue;
    } else if (const auto* resource = std::get_if<ResourceRef>(&source.source)) {
      if (resource->id.value.empty() || resource->version.empty()) co_return Fail<void>(ErrorCode::InvalidArgument, "A resource version is required");
    } else {
      co_return Fail<void>(ErrorCode::PermissionDenied, "Shared context requires a project");
    }
    auto allowed = co_await Require(std::move(policy));
    if (!allowed.Succeeded()) co_return allowed;
  }
  if (!project_sources.empty())
    co_return co_await ValidateProjectSources(*session.summary.project, std::move(actor), project_sources);
  co_return AgentResult<void>::Success();
}

Async<SharedContextItem> RuntimeState::ReadSharedContext(SharedContextRef ref, std::optional<RunId> bound, bool published_only) {
  using Result = AgentResult<SharedContextItem>;
  auto actor = co_await ProjectAccess(ref.project, bound, Capability::ReadContext);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  auto found = FindContext(PublishedProject(ref.project), ref);
  if (!found || found->state == SharedContextState::Withdrawn ||
      (found->state != SharedContextState::Published && (published_only || found->publisher != actor.Value().principal))) {
    co_return Fail<SharedContextItem>(ErrorCode::NotFound, "Shared context version unavailable");
  }
  auto item = *found;
  auto allowed = co_await Require(ContextPolicy(*this, actor.Value(), item));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await ValidateProjectSources(ref.project, actor.Value(), item.data.sources);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  found = FindContext(PublishedProject(ref.project), ref);
  if (!found || found->state == SharedContextState::Withdrawn) co_return Fail<SharedContextItem>(ErrorCode::NotFound, "Shared context was withdrawn");
  co_return Result::Success(*found);
}

Async<Receipt<SharedContextRef>> RuntimeState::UpdateSharedContext(ProjectId id, CommandId command, SharedContextCommand request,
                                                                 std::optional<RunId> bound) {
  using Result = AgentResult<Receipt<SharedContextRef>>;
  auto actor = co_await ProjectAccess(id, bound, Capability::PublishSharedContext);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  ProjectCommand key{request};
  auto old = ReplayProject<SharedContextRef>(actor.Value(), id, command, key);
  if (!old.Succeeded()) co_return Result::Failure(old.Error());
  if (old.Value()) co_return co_await ReplayReceipt(Result::Success(*old.Value()));
  const auto* proposal = std::get_if<ProposeSharedContext>(&request);
  const auto* change = std::get_if<ChangeSharedContext>(&request);
  if (change && change->expected.project != id) {
    co_return Fail<Receipt<SharedContextRef>>(ErrorCode::InvalidArgument, "Shared context belongs to a different project");
  }
  const auto* edit = change ? std::get_if<EditSharedContext>(&change->change) : nullptr;
  const auto* published = PublishedProject(id);
  const auto* prior = change ? FindContext(published, change->expected) : nullptr;
  auto draft = proposal ? std::optional{proposal->draft} : edit ? std::optional{edit->draft}
             : prior ? std::optional{prior->data} : std::nullopt;
  if (!draft) co_return Fail<Receipt<SharedContextRef>>(ErrorCode::Conflict, "Shared context version is unavailable");
  auto valid = CheckDraft(*draft);
  if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  if (!change || !std::holds_alternative<WithdrawContext>(change->change)) {
    valid = co_await ValidateProjectSources(id, actor.Value(), draft->sources);
    if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  }
  auto permission = ProjectPolicy(actor.Value(), id, Capability::PublishSharedContext);
  permission.sources = draft->sources;
  const auto action = change ? static_cast<std::int64_t>(change->change.index()) : -1;
  permission.operation = JsonValue{JsonValue::Object{
      {"access_profile", JsonValue{draft->access_profile}},
      {"change", JsonValue{action}}}};
  valid = co_await Require(std::move(permission));
  if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  auto current = Actor(bound, Capability::PublishSharedContext);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != actor.Value().principal) {
    co_return Fail<Receipt<SharedContextRef>>(ErrorCode::PermissionDenied, "The acting principal changed");
  }
  old = ReplayProject<SharedContextRef>(actor.Value(), id, command, key);
  if (!old.Succeeded()) co_return Result::Failure(old.Error());
  if (old.Value()) co_return co_await ReplayReceipt(Result::Success(*old.Value()));
  if (storage_error && options.store) co_return Result::Failure(*storage_error);
  auto& project = projects.try_emplace(id, ProjectData{.id = id}).first->second;
  SharedContextRef result;
  std::optional<SourceChange> source_change;
  if (proposal) {
    if (proposal->expected_project != project.revision) co_return Fail<Receipt<SharedContextRef>>(ErrorCode::Conflict, "Project revision changed");
    result = {id, {MakeId()}, 1};
    project.context.emplace(result.item, std::vector{SharedContextItem{
        result, actor.Value().principal, SharedContextState::Draft, std::move(*draft)}});
  } else {
    auto versions = project.context.find(change->expected.item);
    if (versions == project.context.end() || !change->expected.version || change->expected.version > versions->second.size()) {
      co_return Fail<Receipt<SharedContextRef>>(ErrorCode::Conflict, "Shared context version changed");
    }
    auto& item = versions->second[change->expected.version - 1];
    const bool withdraw = std::holds_alternative<WithdrawContext>(change->change);
    const auto latest = std::ranges::find_if(versions->second | std::views::reverse, [](const auto& candidate) {
      return candidate.state != SharedContextState::Withdrawn;
    });
    if (item.state == SharedContextState::Withdrawn || (!withdraw && (latest == versions->second.rend() || latest->ref != change->expected))) {
      co_return Fail<Receipt<SharedContextRef>>(ErrorCode::Conflict, "Shared context version changed");
    }
    result = item.ref;
    if (edit) {
      result.version = versions->second.back().ref.version + 1;
      versions->second.push_back({result, actor.Value().principal, SharedContextState::Draft, std::move(*draft)});
    } else if (withdraw) {
      if (item.state == SharedContextState::Published) {
        source_change = SourceChange{.before = {result}, .kind = SourceChangeKind::Withdrawn};
      }
      item.state = SharedContextState::Withdrawn;
    } else if (std::holds_alternative<PublishContext>(change->change) && item.state == SharedContextState::PendingReview) {
      const auto previous = std::ranges::find(versions->second | std::views::reverse,
          SharedContextState::Published, &SharedContextItem::state);
      if (previous != versions->second.rend()) {
        source_change = SourceChange{.before = {previous->ref}, .after = SourceRef{result}, .kind = SourceChangeKind::Modified};
      }
      item.state = SharedContextState::Published;
    } else if (std::holds_alternative<RequestContextReview>(change->change) && item.state == SharedContextState::Draft) {
      item.state = SharedContextState::PendingReview;
    } else {
      co_return Fail<Receipt<SharedContextRef>>(ErrorCode::InvalidState, "Shared context transition is unavailable");
    }
  }
  ++project.revision;
  RefreshTaskReadiness(project);
  co_return co_await CommitProject(project, actor.Value(), std::move(command), std::move(key), result,
                                   nullptr, nullptr, std::move(source_change));
}

namespace {

Async<std::vector<ContextFragment>> ReadResourceContext(RuntimeState& runtime, const RunExecution& run,
    ActionParty actor, std::vector<SourceSlice> remaining) {
  using Result = AgentResult<std::vector<ContextFragment>>;
  const auto& providers = runtime.agents.at(run.view.agent.id).context;
  if (providers.empty()) co_return Fail<std::vector<ContextFragment>>(ErrorCode::Unsupported, "This resource needs a host context provider");
  auto bytes_left = runtime.RunBudget(run).context_bytes.value_or(4 << 20);
  const auto epoch = runtime.access_revision;
  std::vector<ContextFragment> fragments;
  for (const auto& provider : providers) {
    auto found = co_await runtime.context.at(provider)({actor, run.view.model.id, remaining, bytes_left, run.stop.get_token()});
    if (run.stop.stop_requested()) co_return Fail<std::vector<ContextFragment>>(ErrorCode::AlreadyClosed, "Run closed");
    if (epoch != runtime.access_revision)
      co_return Fail<std::vector<ContextFragment>>(ErrorCode::Conflict, "Resource context access changed");
    if (!found.Succeeded()) co_return Result::Failure(found.Error());
    for (auto& fragment : found.Value().fragments) {
      const auto requested = std::ranges::find(remaining, fragment.source);
      const auto* text = std::get_if<std::string>(&fragment.content.body);
      const auto* json = std::get_if<JsonValue>(&fragment.content.body);
      if (requested == remaining.end() || fragment.content.role != ModelContent::Role::Context || (!text && !json)) {
        co_return Fail<std::vector<ContextFragment>>(ErrorCode::ProtocolError, "Context provider returned an unexpected fragment");
      }
      if (text && !ValidUtf8(*text))
        co_return Fail<std::vector<ContextFragment>>(ErrorCode::ProtocolError, "Context provider returned invalid UTF-8");
      if (json) {
        auto valid = ValidateJson(*json, JsonSchema{JsonValue{true}});
        if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
      }
      const auto size = text ? text->size() : ToWireJson(*json).dump().size();
      if (size > bytes_left) co_return Fail<std::vector<ContextFragment>>(ErrorCode::BudgetExceeded, "Resource context exceeds the context budget");
      bytes_left -= size;
      remaining.erase(requested);
      fragments.push_back(std::move(fragment));
    }
    if (remaining.empty()) co_return Result::Success(std::move(fragments));
  }
  co_return Fail<std::vector<ContextFragment>>(ErrorCode::NotFound, "The requested resource context version is unavailable");
}

} // namespace

Async<std::vector<ModelContent>> RuntimeState::AssembleProjectInput(const RunExecution& run, std::vector<SourceSlice> extra) {
  using Result = AgentResult<std::vector<ModelContent>>;
  if (run.delegate && std::ranges::any_of(extra, [&](const auto& source) {
        return std::ranges::find(run.input.context, source) == run.input.context.end();
      })) co_return Fail<std::vector<ModelContent>>(ErrorCode::PermissionDenied, "The source is outside the delegation");
  for (const auto& source : run.input.context) {
    if (std::ranges::find(extra, source) == extra.end()) extra.push_back(source);
  }
  if (extra.empty()) co_return Result::Success({});
  if (const auto* scope = Scope(run); scope && std::ranges::any_of(extra, [&](const auto& input) {
        return std::ranges::find(scope->inputs, input) == scope->inputs.end();
      })) {
    co_return Fail<std::vector<ModelContent>>(ErrorCode::PermissionDenied, "The source is outside the work request");
  }
  const auto& session = sessions.at(run.view.session);
  const auto authorized_at = access_revision;
  auto request = Request(run.principal, Capability::SendToModel, &session, &run);
  auto actor = request.actor;
  request.sources = extra;
  request.recipient_model = run.view.model.id;
  auto allowed = co_await Require(request);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  auto whole = extra;
  std::erase_if(whole, [](const auto& source) {
    return !std::holds_alternative<ResourceRef>(source.source) &&
        (std::holds_alternative<MessageRef>(source.source) || !source.parts.empty() || source.text_range);
  });
  allowed = co_await ValidateInputSources(session, actor, whole);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  std::vector<SourceSlice> resources;
  for (const auto& source : extra) {
    if (std::holds_alternative<ResourceRef>(source.source) && std::ranges::find(resources, source) == resources.end())
      resources.push_back(source);
  }
  std::vector<ContextFragment> resource_context;
  if (!resources.empty()) {
    if (run.stop.stop_requested()) co_return Fail<std::vector<ModelContent>>(ErrorCode::AlreadyClosed, "Run closed");
    if (authorized_at != access_revision)
      co_return Fail<std::vector<ModelContent>>(ErrorCode::Conflict, "Resource context access changed");
    auto resolved = co_await ReadResourceContext(*this, run, actor, std::move(resources));
    if (!resolved.Succeeded()) co_return Result::Failure(resolved.Error());
    resource_context = std::move(resolved).Value();
  }
  std::vector<ModelContent> content;
  for (const auto& source : extra) {
    if (std::holds_alternative<ResourceRef>(source.source)) {
      auto fragment = std::ranges::find(resource_context, source, &ContextFragment::source);
      if (fragment != resource_context.end()) {
        content.push_back(std::move(fragment->content));
        resource_context.erase(fragment);
      }
      continue;
    }
    if (std::holds_alternative<MessageRef>(source.source) ||
        (std::holds_alternative<ArtifactRef>(source.source) && (!source.parts.empty() || source.text_range))) {
      auto selected = co_await ResolveSelection(session.summary.id, actor, source, run.view.model.id);
      if (!selected.Succeeded()) co_return Result::Failure(selected.Error());
      content.push_back({ModelContent::Role::Context, std::move(selected).Value()});
      continue;
    }
    if (!source.parts.empty() || source.text_range) {
      co_return Fail<std::vector<ModelContent>>(ErrorCode::Unsupported, "Only whole project context versions can be assembled");
    }
    if (const auto* ref = std::get_if<SharedContextRef>(&source.source)) {
      const auto* item = FindContext(PublishedProject(ref->project), *ref);
      if (!item || item->state != SharedContextState::Published) {
        co_return Fail<std::vector<ModelContent>>(ErrorCode::NotFound, "Project context version is unavailable");
      }
      content.push_back({ModelContent::Role::Context, "Project data " + item->data.title +
          " (version " + std::to_string(ref->version) + ")\n" + ToWireJson(item->data.content).dump()});
    } else if (const auto* artifact = std::get_if<ArtifactRef>(&source.source)) {
      auto record = co_await ReadArtifact(*artifact, actor);
      if (!record.Succeeded()) co_return Result::Failure(record.Error());
      const auto& meta = record.Value().record.metadata;
      auto data = "Artifact " + meta.name + " (" + meta.type +
          ", version " + std::to_string(meta.ref.version) + ", ID " + meta.ref.id.value + ")";
      if (options.artifact_store.preview) {
        auto preview = co_await options.artifact_store.preview({actor, *artifact, record.Value().handle});
        if (!preview.Succeeded()) co_return Result::Failure(preview.Error());
        if (preview.Value().artifact != *artifact) {
          co_return Fail<std::vector<ModelContent>>(ErrorCode::Conflict, "Resolved artifact version changed");
        }
        if (preview.Value().structured) {
          auto valid = ValidateJson(*preview.Value().structured, JsonSchema{JsonValue{true}});
          if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
          data += "\n" + ToWireJson(*preview.Value().structured).dump();
        }
      }
      content.push_back({ModelContent::Role::Context, std::move(data)});
    } else {
      co_return Fail<std::vector<ModelContent>>(ErrorCode::Unsupported, "This project source needs a host context provider");
    }
  }
  allowed = co_await ValidateInputSources(session, actor, whole);
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  allowed = co_await Require(std::move(request));
  if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  if (run.stop.stop_requested()) co_return Fail<std::vector<ModelContent>>(ErrorCode::AlreadyClosed, "Run closed");
  co_return Result::Success(std::move(content));
}

Async<std::vector<ModelContent>> RuntimeState::AssembleInheritedHistory(const RunExecution& run) {
  using Result = AgentResult<std::vector<ModelContent>>;
  const auto& session = sessions.at(run.view.session);
  const auto* image = Published(session);
  if (!image) co_return Fail<std::vector<ModelContent>>(ErrorCode::NotFound, "Conversation history is unavailable");
  std::vector<Message> inherited;
  for (const auto& message : image->messages) {
    if (message.ref.session != session.summary.id && message.state == MessageState::Complete) inherited.push_back(message);
  }
  if (inherited.empty()) co_return Result::Success({});

  const auto epoch = access_revision;
  auto outbound = Request(run.principal, Capability::SendToModel, &session, &run);
  outbound.recipient_model = run.view.model.id;
  auto sources = co_await VisibleHistorySources(*image, outbound.actor);
  if (!sources.Succeeded()) co_return Result::Failure(sources.Error());
  const auto in_scope = [&](const SourceSlice& source) {
    const auto* scope = Scope(run);
    return !scope || std::ranges::find(scope->inputs, source) != scope->inputs.end();
  };
  std::vector<ModelContent> content;
  ArtifactVersions added;
  for (const auto& message : inherited) {
    if (message.run) {
      const auto owner = runs.find(*message.run);
      if (owner != runs.end() && owner->second.view.parent) continue;
    }
    if (!sources.Value().contains(message.ref.session)) continue;
    const SourceSlice message_ref{message.ref};
    const bool has_text = (message.author == MessageAuthor::User || message.author == MessageAuthor::Agent) &&
        std::ranges::any_of(message.blocks, [](const auto& block) { return std::holds_alternative<TextBlock>(block.body); });
    if (has_text && in_scope(message_ref)) {
      auto read = CollaborationAccess(outbound.actor, Capability::ReadContext, session);
      read.sources = {message_ref};
      auto permitted = co_await Require(std::move(read));
      if (permitted.Succeeded()) {
        outbound.sources = {message_ref};
        permitted = co_await Require(outbound);
      }
      if (permitted.Succeeded()) {
        const auto role = message.author == MessageAuthor::User ? ModelContent::Role::User : ModelContent::Role::Assistant;
        for (const auto& block : message.blocks) {
          if (const auto* text = std::get_if<TextBlock>(&block.body)) content.push_back({role, text->text});
        }
      }
    }
    for (const auto& block : message.blocks) {
      const auto* ref = std::get_if<ArtifactBlock>(&block.body);
      if (!ref || !in_scope({ref->artifact}) || added.contains({ref->artifact.id, ref->artifact.version}) ||
          std::ranges::find(run.input.context, SourceSlice{ref->artifact}) != run.input.context.end()) continue;
      auto artifact = co_await ReadHistoryArtifact(session, outbound.actor, ref->artifact);
      if (!artifact.Succeeded()) continue;
      outbound.sources = {{ref->artifact}};
      auto permitted = co_await Require(outbound);
      if (!permitted.Succeeded()) continue;
      const auto& meta = artifact.Value().record.metadata;
      content.push_back({ModelContent::Role::Context, "Inherited artifact " + meta.name + " (" + meta.type +
          ", version " + std::to_string(meta.ref.version) + ", ID " + meta.ref.id.value + ")"});
      added.emplace(ref->artifact.id, ref->artifact.version);
    }
  }
  if (run.stop.stop_requested()) co_return Fail<std::vector<ModelContent>>(ErrorCode::AlreadyClosed, "Run closed");
  if (epoch != access_revision) co_return Fail<std::vector<ModelContent>>(ErrorCode::Conflict, "History access changed before model submission");
  co_return Result::Success(std::move(content));
}

} // namespace huxerui::agentkit::detail
