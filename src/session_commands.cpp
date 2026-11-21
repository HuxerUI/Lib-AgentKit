#include "runtime_internal.h"

#include <algorithm>

namespace huxerui::agentkit {
namespace detail {
namespace {

template<class T>
std::optional<AgentResult<Receipt<T>>> Replay(RuntimeState& state, const PrincipalId& user,
                                             const CommandId& command, const CommandInput& input) {
  auto previous = state.Find(user, command, input);
  if (!previous.Succeeded()) return AgentResult<Receipt<T>>::Failure(previous.Error());
  if (previous.Value()) return state.ReceiptFor<T>(user, command);
  return {};
}

AgentResult<void> CheckOpen(const SessionData& session) {
  if (session.summary.lifecycle != SessionLifecycle::Open) {
    return Fail<void>(ErrorCode::InvalidState, "Session is not open");
  }
  return AgentResult<void>::Success();
}

bool ValidCopyPlan(const WorkspaceChoice& choice) {
  return choice.kind == WorkspaceChoice::Kind::CopyFromSource && !choice.workspace.value.empty() && choice.copy_plan &&
      choice.copy_plan->version && !choice.copy_plan->id.value.empty() && choice.copy_plan->id.value.size() <= 128 &&
      ValidUtf8(choice.copy_plan->id.value);
}

SessionId WorkspaceForkSession(const WorkspaceCopyPlanRef& plan) {
  return {"workspace-fork:" + plan.id.value + ":" + std::to_string(plan.version)};
}

struct WorkspaceAllocation {
  std::set<SessionId>& pending;
  const SessionId& id;
  bool held = false;
  ~WorkspaceAllocation() { if (held) pending.erase(id); }
};

} // namespace

AgentResult<void> RuntimeState::CheckInput(const SendInput& input, bool user_selection, bool initial_skills) const {
  if (input.text.size() > (1 << 20) || !ValidUtf8(input.text) ||
      (Title(input.text).empty() && input.attachments.empty() && input.staged_attachments.empty())) {
    return Fail<void>(ErrorCode::InvalidArgument, "Enter UTF-8 input within 1 MiB or attach a file");
  }
  if ((!user_selection && !initial_skills && (!input.attachments.empty() || !input.staged_attachments.empty())) ||
      (!user_selection && (!input.side_chat_conclusions.empty() || !input.recovered_conclusions.empty() ||
                           !input.recovered_steer_conclusions.empty()))) {
    return Fail<void>(ErrorCode::Unsupported, "This runtime does not yet resolve input references");
  }
  if (std::ranges::any_of(input.attachments, [](const ArtifactRef& ref) {
        return !ref.version || ref.id.value.empty() || !ValidUtf8(ref.id.value);
      })) return Fail<void>(ErrorCode::InvalidArgument, "An artifact ID and version are required for each attachment");
  std::set<ArtifactId> staged;
  for (const auto& id : input.staged_attachments) {
    if (id.value.empty() || !ValidUtf8(id.value) || !staged.insert(id).second)
      return Fail<void>(ErrorCode::InvalidArgument, "Each staged attachment needs a distinct artifact ID");
  }
  if (!user_selection && !initial_skills && !input.skills.empty()) {
    return Fail<void>(ErrorCode::PermissionDenied, "Select Skills in the executing session");
  }
  if (input.skills.size() > 64 || !std::ranges::all_of(input.skills, ValidSkillRef)) {
    return Fail<void>(ErrorCode::InvalidArgument, "Invalid Skill selection");
  }
  if (input.recovered_conclusions.size() + input.recovered_steer_conclusions.size() > 64) {
    return Fail<void>(ErrorCode::BudgetExceeded, "Too many recovered conclusions");
  }
  if (std::ranges::any_of(input.context, [user_selection](const auto& slice) {
        if (std::holds_alternative<ResourceRef>(slice.source)) return false;
        if (user_selection && (std::holds_alternative<MessageRef>(slice.source) ||
                               std::holds_alternative<ArtifactRef>(slice.source))) return false;
        return (!std::holds_alternative<SharedContextRef>(slice.source) &&
                !std::holds_alternative<ArtifactRef>(slice.source)) || !slice.parts.empty() || slice.text_range;
      })) {
    return Fail<void>(ErrorCode::Unsupported, "This input requires a source resolver");
  }
  return AgentResult<void>::Success();
}

Async<RunStartBinding> RuntimeState::ResolveStart(const SessionData& session, const PrincipalId& user, UserRunOptions start) {
  auto registered = agents.find(session.summary.agent.id);
  if (registered == agents.end() || registered->second.ref != session.summary.agent) {
    co_return Fail<RunStartBinding>(ErrorCode::Conflict, "The session's agent revision is unavailable");
  }
  const auto& agent = registered->second;
  if (session.origin.kind == SessionOriginKind::SideChat &&
      (start.execution_profile || !agent.tools.empty() || !agent.tool_catalogs.empty() || !agent.context.empty() ||
       !agent.delegates.empty() || !agent.capabilities.empty() || workflows.contains(agent.ref.id))) {
    co_return Fail<RunStartBinding>(ErrorCode::PermissionDenied, "Side chat requires a selection-only assistant");
  }
  const auto model = models.find(start.model ? start.model->id : agent.model);
  if ((start.model && (!start.model->version || start.model->id.value.empty())) ||
      (start.execution_profile && (!start.execution_profile->version || start.execution_profile->id.value.empty()))) {
    co_return Fail<RunStartBinding>(ErrorCode::InvalidArgument, "Model and execution profile require valid revisions");
  }
  if (model == models.end()) co_return Fail<RunStartBinding>(ErrorCode::NotFound, "Model unavailable");
  if (start.model && model->second.ref != *start.model) {
    co_return Fail<RunStartBinding>(ErrorCode::Conflict, "Model registration changed");
  }
  auto required = agent.requires_model;
  required.tool_calls |= (!agent.tools.empty() || !agent.tool_catalogs.empty() || !agent.delegates.empty()) &&
      !workflows.contains(agent.ref.id);
  if (!Supports(model->second.provider->Capabilities(), required)) {
    co_return Fail<RunStartBinding>(ErrorCode::Unsupported, "Model lacks capabilities required by the agent");
  }
  RunStartBinding binding{model->second.ref, start.execution_profile};
  auto request = Request(user, Capability::UseRunConfiguration, &session);
  request.run_configuration = binding;
  request.recipient_model = model->first;
  auto access = co_await Require(std::move(request));
  if (!access.Succeeded()) co_return AgentResult<RunStartBinding>::Failure(access.Error());
  co_return AgentResult<RunStartBinding>::Success(std::move(binding));
}

void RuntimeState::AppendInput(SessionData& session, const RunId& run, const SendInput& input,
                               std::optional<MessageId> message, MessageAuthor author,
                               std::vector<SideChatConclusion> conclusions) {
  session.messages.push_back(Message{
      .ref = {session.summary.id, message.value_or(MessageId{MakeId()}), 1}, .sequence = ++session.sequence,
      .run = run, .author = author, .state = MessageState::Complete,
      .blocks = {}});
  if (!input.text.empty()) session.messages.back().blocks.push_back({{MakeId()}, TextBlock{input.text, false}});
  for (const auto& ref : input.attachments) {
    const auto found = std::ranges::find_if(session.artifacts, [&](const auto& entry) { return entry.record.metadata.ref == ref; });
    auto name = found == session.artifacts.end() ? std::string{} : found->record.metadata.name;
    session.messages.back().blocks.push_back({{MakeId()}, ArtifactBlock{ref, std::move(name)}});
  }
  for (auto& conclusion : conclusions)
    session.messages.back().blocks.push_back({{MakeId()}, std::move(conclusion)});
  session.summary.last_activity = Now();
}

void RuntimeState::FirstTitle(SessionData& session, const SendInput& input) {
  if (session.summary.title_source != SessionTitleSource::Placeholder) return;
  auto title = Title(input.text);
  if (title.empty()) return;
  auto& summary = session.summary;
  summary.title = std::move(title);
  summary.title_source = SessionTitleSource::FirstInput;
  ++summary.metadata_revision;
  if (options.generate_session_title) {
    options.tasks.Launch([self = shared_from_this(), id = summary.id, text = input.text,
                          expected = summary.metadata_revision] { return self->OptimizeTitle(id, text, expected); });
  }
}

Task<void> RuntimeState::OptimizeTitle(SessionId id, std::string text, Revision expected) {
  auto& session = sessions.at(id);
  try {
    auto optimized = co_await options.generate_session_title({id, session.summary.agent, std::move(text), {}});
    if (stage != Stage::Started || !optimized.Succeeded() || !optimized.Value() ||
        session.summary.lifecycle != SessionLifecycle::Open ||
        session.summary.title_source != SessionTitleSource::FirstInput ||
        session.summary.metadata_revision != expected) co_return;
    auto title = Title(*optimized.Value());
    if (title.empty()) co_return;
    session.summary.title = std::move(title);
    session.summary.title_source = SessionTitleSource::Optimized;
    ++session.summary.metadata_revision;
    ++session.sequence;
    Persist(session);
  } catch (...) { ReportException(std::current_exception()); }
}

RunExecution& RuntimeState::RecordRun(SessionData& session, PrincipalId user, SendInput input, RunStartBinding start,
                                      RunTrigger trigger, std::optional<RunId> source,
                                      std::vector<SideChatConclusion> conclusions, std::optional<RunId> reserved) {
  const auto id = reserved.value_or(RunId{MakeId()});
  auto& summary = session.summary;
  RunSnapshot view{.id = id, .session = summary.id, .agent = summary.agent, .model = start.model,
                   .trigger = trigger, .retry_of = source, .state = RunState::Running, .usage = {},
                   .execution_profile = start.execution_profile, .timing = RunTiming{.started_at = Now()}};
  const auto boundary = source && trigger == RunTrigger::Regenerate
      ? std::ranges::find_if(session.messages, [&](const Message& prior) {
          return prior.run == source && prior.author == MessageAuthor::User;
        })
      : session.messages.end();
  std::vector<ModelItem> items;
  for (auto it = session.messages.begin(); it != boundary; ++it) {
    const auto& message = *it;
    if (message.ref.session != summary.id || message.state != MessageState::Complete ||
        (message.author != MessageAuthor::User && message.author != MessageAuthor::Agent)) continue;
    if (message.run && runs.at(*message.run).view.parent) continue;
    if (message.run && (message.author == MessageAuthor::User || message.author == MessageAuthor::Collaboration)) {
      const auto& steers = runs.at(*message.run).view.steering;
      if (std::ranges::any_of(steers, [&](const auto& steer) {
            return steer.message == message.ref.message && steer.state != SteerState::Applied;
          })) continue;
    }
    for (const auto& block : message.blocks) {
      if (const auto* text = std::get_if<TextBlock>(&block.body)) {
        items.push_back(ModelContent{message.author == MessageAuthor::User || message.author == MessageAuthor::Collaboration
            ? ModelContent::Role::User : ModelContent::Role::Assistant, text->text});
      }
    }
  }
  if (!input.text.empty()) items.push_back(ModelContent{ModelContent::Role::User, input.text});
  for (const auto& ref : input.attachments) items.push_back(ModelContent{ModelContent::Role::User, ref});
  const auto author = trigger == RunTrigger::Goal || trigger == RunTrigger::ArtifactChange || trigger == RunTrigger::ForkComparison ? MessageAuthor::System :
      trigger == RunTrigger::Collaboration ? MessageAuthor::Collaboration : MessageAuthor::User;
  AppendInput(session, id, input, {}, author, std::move(conclusions));
  if (author != MessageAuthor::System) FirstTitle(session, input);
  auto deadline = agents.at(summary.agent.id).budget.deadline;
  auto& run = runs.emplace(id, RunExecution{.view = std::move(view), .principal = std::move(user),
      .input = std::move(input), .items = std::move(items),
      .model_boundary = true,
      .deadline = deadline ? std::optional<TimePoint>(Now() + *deadline) : std::nullopt}).first->second;
  summary.active_run = id;
  summary.run_state = RunState::Running;
  session.runs.push_back(id);
  return run;
}

Async<Receipt<SessionCreated>> RuntimeState::Create(std::shared_ptr<RuntimeState> self, CommandId command,
                                                   CreateSessionRequest request, std::optional<RunId> bound,
                                                   std::optional<HistoryBoundary> fork,
                                                   std::optional<OperationPlan> approved,
                                                   std::optional<WorkspaceChoice> copy,
                                                   PreparedForkCandidate* candidate) {
  using Result = AgentResult<Receipt<SessionCreated>>;
  const auto operation = fork ? Capability::ForkSession : Capability::CreateSession;
  auto actor = self->Actor(bound, operation);
  if (!actor.Succeeded()) co_return Result::Failure(actor.Error());
  const auto user = actor.Value().principal;
  const auto& side = request.side_chat;
  const auto* initial = std::get_if<SendInput>(&request.initial);
  if (side && (bound || fork || request.options.retention != SessionRetention::Ephemeral ||
      request.options.project || request.options.workspace || !initial || !initial->context.empty() || !initial->attachments.empty() ||
      !initial->staged_attachments.empty() ||
      side->source.value.empty() || side->selected.empty() || side->selected.size() > 64)) {
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument,
        "Side chat requires a temporary conversation, a question and a source selection");
  }
  if (request.options.retention == SessionRetention::Stored && !self->options.store) {
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::Unsupported, "Stored sessions require a configured session store");
  }
  if (copy && !self->options.workspace)
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::Unsupported, "The host has no workspace copy resolver");
  if (request.options.project && self->options.project) {
    auto project = co_await self->options.project(*request.options.project);
    if (!project.Succeeded()) co_return Result::Failure(project.Error());
    if (project.Value().project != request.options.project) {
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::NotFound, "Project unavailable");
    }
    if (request.options.workspace && std::ranges::find(project.Value().workspaces, *request.options.workspace) ==
                                      project.Value().workspaces.end()) {
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::PermissionDenied, "Workspace is outside the project");
    }
  }
  const auto agent = self->agents.find(request.options.agent);
  if (agent == self->agents.end()) co_return Fail<Receipt<SessionCreated>>(ErrorCode::NotFound, "Unknown agent");
  if (approved && approved->new_agent != agent->second.ref) {
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The approved agent revision changed");
  }
  if (bound && agent->first != actor.Value().agent->id) {
    const auto& targets = self->agents.at(actor.Value().agent->id).collaboration_targets;
    if (std::ranges::find(targets, agent->first) == targets.end()) {
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::PermissionDenied, "Target agent is outside the declared collaboration scope");
    }
  }
  const auto* work = std::get_if<WorkRequest>(&request.initial);
  if (bound && initial && !candidate) co_return Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument, "Use a work request for initial agent input");
  if (work) {
    if (work->task) co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "A claimed task must be sent to its claimant session");
    auto valid = self->CheckWork(*work);
    if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  }
  auto title = Title(request.options.title);
  if (!request.options.title.empty() && title.empty()) {
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument, "Invalid session title");
  }
  CommandInput key{.operation = fork ? Operation::Fork : Operation::Create, .run = bound.value_or(RunId{}),
                   .options = request.options, .start = request.user_start, .fork = fork, .workspace_choice = copy,
                   .side_chat = side};
  if (work) key.work = *work;
  if (initial) {
    if (bound && !initial->staged_attachments.empty())
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::PermissionDenied, "A user must submit staged attachments");
    auto valid = self->CheckInput(*initial, candidate != nullptr, !bound);
    if (!valid.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(valid.Error());
    key.input = *initial;
  }
  auto resolve_replay = [&]() -> Async<std::optional<Receipt<SessionCreated>>> {
    if (approved) co_return AgentResult<std::optional<Receipt<SessionCreated>>>::Success({});
    auto prior = self->Find(user, command, key);
    if (!prior.Succeeded()) co_return AgentResult<std::optional<Receipt<SessionCreated>>>::Failure(prior.Error());
    if (!prior.Value()) co_return AgentResult<std::optional<Receipt<SessionCreated>>>::Success({});
    if (prior.Value()->retired_workspace)
      co_return Fail<std::optional<Receipt<SessionCreated>>>(ErrorCode::AlreadyClosed, "The workspace copy was abandoned");
    const auto id = *prior.Value()->progress.accepted.session;
    auto receipt = co_await self->ReplayReceipt(self->ReceiptFor<SessionCreated>(user, command));
    if (!receipt.Succeeded()) co_return AgentResult<std::optional<Receipt<SessionCreated>>>::Failure(receipt.Error());
    auto access = co_await self->Require(self->CollaborationAccess(actor.Value(), Capability::ReadSession, self->sessions.at(id)));
    if (!access.Succeeded()) co_return AgentResult<std::optional<Receipt<SessionCreated>>>::Failure(access.Error());
    co_return AgentResult<std::optional<Receipt<SessionCreated>>>::Success(std::move(receipt).Value());
  };
  auto prior = co_await resolve_replay();
  if (!prior.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(prior.Error());
  if (prior.Value()) co_return AgentResult<Receipt<SessionCreated>>::Success(*prior.Value());
  if (self->storage_error && request.options.retention == SessionRetention::Stored) {
    co_return AgentResult<Receipt<SessionCreated>>::Failure(*self->storage_error);
  }

  const SessionId id = copy ? WorkspaceForkSession(*copy->copy_plan) :
      candidate && candidate->reserved_session ? *candidate->reserved_session : SessionId{MakeId()};
  if (candidate && candidate->reserved_session && *candidate->reserved_session != id)
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The candidate workspace changed after review");
  if (copy && self->sessions.contains(id))
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The workspace plan already has a session");
  std::vector<Capability> confirmation;
  std::optional<Revision> source_binding;
  SessionData session{
      .summary = {.id = id, .title = title.empty() ? self->options.untitled_session_title : title,
                  .title_source = title.empty() ? SessionTitleSource::Placeholder : SessionTitleSource::Manual,
                  .agent = agent->second.ref, .binding_revision = 1, .metadata_revision = 1,
                  .project = request.options.project, .workspace = request.options.workspace,
                  .lifecycle = SessionLifecycle::Open, .last_activity = self->Now(),
                  .pending_interactions = 0, .queued_inputs = 0},
      .retention = request.options.retention,
      .origin = {.kind = side ? SessionOriginKind::SideChat : fork ? SessionOriginKind::Forked : SessionOriginKind::Created,
                 .creator = actor.Value(), .source_session = side ? std::optional{side->source} :
                     fork ? std::optional{fork->session} : actor.Value().session,
                 .fork_point = fork, .side_chat_selection = side ? side->selected : std::vector<SourceSlice>{}},
      .sequence = 1};
  if (fork) {
    auto source = self->sessions.find(fork->session);
    if (source == self->sessions.end() || !self->Published(source->second)) {
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::NotFound, "Source session unavailable");
    }
    if (source->second.origin.kind == SessionOriginKind::SideChat)
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Unsupported, "A temporary side chat cannot be forked");
    auto allowed = co_await self->Require(self->CollaborationAccess(actor.Value(), Capability::ReadSession, source->second));
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    allowed = co_await self->CheckOperation(
        self->CollaborationAccess(actor.Value(), Capability::ForkSession, source->second), confirmation, approved);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    const auto* published = self->Published(source->second);
    if (!published || published->summary.lifecycle == SessionLifecycle::Deleting ||
        !fork->through_sequence || fork->through_sequence > published->sequence) {
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The history boundary is not committed");
    }
    if (copy && published->summary.workspace != copy->workspace)
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The workspace plan does not match the source session");
    source_binding = published->summary.binding_revision;
    if (approved && approved->target_binding != source_binding) {
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The approved source session changed");
    }
    auto through = fork->through_sequence;
    if (const auto* message = std::get_if<MessageId>(&fork->anchor)) {
      auto anchor = std::ranges::find_if(published->messages, [&](const auto& item) {
        return item.ref.message == *message && item.state == MessageState::Complete && item.sequence <= through;
      });
      if (anchor == published->messages.end()) co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "Cannot fork unsealed history");
      through = anchor->sequence;
    } else {
      const auto id = std::get<RunId>(fork->anchor);
      auto anchor = self->runs.find(id);
      if (anchor == self->runs.end() || anchor->second.view.session != fork->session || !Terminal(anchor->second.view.state)) {
        co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "Cannot fork an active run");
      }
      auto last = std::ranges::find_if(published->messages.rbegin(), published->messages.rend(),
                                       [&](const auto& item) { return item.run == id; });
      if (last == published->messages.rend() || last->sequence > through) {
        co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The run is not within the requested history");
      }
      through = last->sequence;
    }
    for (const auto& message : published->messages) {
      if (message.sequence > through || message.state != MessageState::Complete) continue;
      Message copied = message;
      copied.sequence = ++session.sequence;
      copied.run.reset();
      std::erase_if(copied.blocks, [](const auto& block) {
        return !std::holds_alternative<TextBlock>(block.body) && !std::holds_alternative<ArtifactBlock>(block.body) &&
            !std::holds_alternative<CustomBlock>(block.body);
      });
      if (!copied.blocks.empty()) session.messages.push_back(std::move(copied));
    }
  }
  auto create_access = self->CollaborationAccess(actor.Value(), Capability::CreateSession, session);
  create_access.new_agent = request.options.agent;
  if (copy) {
    create_access.workspace = copy->workspace;
    create_access.operation = JsonValue{copy->copy_plan->id.value};
  }
  auto access = co_await self->CheckOperation(std::move(create_access), confirmation, approved);
  if (!access.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(access.Error());
  std::optional<RunStartBinding> binding;
  const auto selection_epoch = self->access_revision;
  auto start = request.user_start;
  if (approved && approved->run_start) start = {approved->run_start->model, approved->run_start->execution_profile};
  if (initial) {
    access = co_await self->Require(candidate ? self->CollaborationAccess(actor.Value(), Capability::SubmitInput, session)
                                             : Request(user, Capability::SubmitInput, &session));
    if (!access.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(access.Error());
    access = co_await self->ValidateInputSources(session, actor.Value(), initial->context);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    auto resolved = co_await self->ResolveStart(session, user, start);
    if (!resolved.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(resolved.Error());
    binding = std::move(resolved).Value();
    if (initial->staged_attachments.empty()) {
      access = co_await self->CheckModelAttachments(session, user, initial->attachments, binding->model);
      if (!access.Succeeded()) co_return Result::Failure(access.Error());
    }
    if (side) {
      auto source = self->sessions.find(side->source);
      if (source == self->sessions.end() || source->second.origin.kind == SessionOriginKind::SideChat)
        co_return Fail<Receipt<SessionCreated>>(ErrorCode::NotFound, "The source conversation is unavailable");
      for (const auto& slice : side->selected) {
        auto text = co_await self->ResolveSelection(side->source, actor.Value(), slice, binding->model.id);
        if (!text.Succeeded()) co_return Result::Failure(text.Error());
      }
    }
  }
  std::optional<CollaborationMessageId> initial_request;
  std::optional<GoalRef> goal;
  auto prepare_work = [&]() -> Async<void> {
    auto permitted = co_await self->ValidateInputSources(session, actor.Value(), work->input.context);
    if (!permitted.Succeeded()) co_return permitted;
    permitted = co_await self->AuthorizeGoalDispatch(session, actor.Value(), goal);
    if (!permitted.Succeeded()) co_return permitted;
    initial_request = CollaborationMessageId{MakeId()};
    SessionMessageRequest message{.to = id, .body = *work};
    auto decision = co_await self->Receive(actor.Value(), session, *initial_request, message, goal);
    if (!decision.Succeeded()) co_return AgentResult<void>::Failure(decision.Error());
    self->EnqueueMessage(session, {.summary = {.id = *initial_request, .from = actor.Value(), .to = id},
                                  .request = std::move(message)}, std::move(decision).Value(), goal);
    if (session.collaboration.at(*initial_request).auto_start) {
      permitted = co_await self->Require(Request(user, Capability::StartSessionRun, &session));
      if (!permitted.Succeeded()) co_return permitted;
      auto resolved = co_await self->ResolveStart(session, user, start);
      if (!resolved.Succeeded()) co_return AgentResult<void>::Failure(resolved.Error());
      binding = std::move(resolved).Value();
    }
    co_return AgentResult<void>::Success();
  };
  if (work) {
    auto send_access = self->CollaborationAccess(actor.Value(), Capability::SendMessage, session);
    if (copy) send_access.workspace = copy->workspace;
    access = co_await self->CheckOperation(std::move(send_access), confirmation, approved);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    goal = self->ActorGoal(actor.Value());
    if (!copy) {
      auto prepared = co_await prepare_work();
      if (!prepared.Succeeded()) co_return Result::Failure(prepared.Error());
    }
  }
  prior = co_await resolve_replay();
  if (!prior.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(prior.Error());
  if (prior.Value()) co_return AgentResult<Receipt<SessionCreated>>::Success(*prior.Value());
  if (!confirmation.empty() && !(candidate && candidate->inspect)) {
    if (candidate) co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The candidate needs a different approval");
    auto home = actor.Value().session ? actor.Value().session : fork ? std::optional{fork->session} : std::nullopt;
    if (!home) co_return Fail<Receipt<SessionCreated>>(ErrorCode::PermissionDenied, "No session can display the approval");
    auto summary = "Assistant: " + agent->second.display_name;
    if (!request.options.title.empty()) summary += "\nConversation: " + request.options.title;
    if (request.options.workspace) summary += "\nWorkspace: " + request.options.workspace->value;
    if (copy) summary += "\nWorkspace copy plan: " + copy->copy_plan->id.value;
    if (work) summary += "\n\n" + work->input.text;
    if (initial) summary += "\n\n" + initial->text;
    auto staged = co_await self->StageOperation(user, command, key, *home,
        {std::move(confirmation), source_binding, agent->second.ref, binding},
        {.title = fork ? "Approve history fork" : "Approve new conversation", .summary = std::move(summary)}, {});
    if (!staged.Succeeded()) co_return Result::Failure(staged.Error());
    co_return co_await self->ReplayReceipt(self->ReceiptFor<SessionCreated>(user, command));
  }

  WorkspaceAllocation allocation{self->allocating_workspaces, id};
  if (copy && !(candidate && candidate->inspect)) {
    allocation.held = allocation.pending.insert(id).second;
    if (!allocation.held)
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Busy, "The workspace plan is being allocated");
    prior = co_await resolve_replay();
    if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
    if (prior.Value()) co_return Result::Success(*prior.Value());
    if (self->WorkspaceCopyRetired(*copy->copy_plan))
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::AlreadyClosed, "The workspace plan was retired");
    WorkspaceRequest workspace{actor.Value(), command, id, copy, request.options.project, fork,
        candidate ? std::optional{candidate->comparison} : std::nullopt};
    auto allocated = co_await self->options.workspace(std::move(workspace));
    if (!allocated.Succeeded()) co_return Result::Failure(allocated.Error());
    if (!allocated.Value() || allocated.Value()->value.empty() || *allocated.Value() == copy->workspace)
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::InvalidState, "The workspace resolver did not allocate a distinct workspace");
    auto& source = self->sessions.at(fork->session);
    const auto* published = self->Published(source);
    if (!published || published->summary.lifecycle == SessionLifecycle::Deleting ||
        published->summary.binding_revision != source_binding || published->summary.workspace != copy->workspace)
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The source session changed during workspace allocation");
    access = co_await self->Require(self->CollaborationAccess(actor.Value(), Capability::ReadSession, source));
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    access = co_await self->CheckOperation(
        self->CollaborationAccess(actor.Value(), Capability::ForkSession, source), confirmation, approved);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    session.summary.workspace = std::move(*allocated.Value());
    if (session.summary.project && self->options.project) {
      auto project = co_await self->options.project(*session.summary.project);
      if (!project.Succeeded()) co_return Result::Failure(project.Error());
      if (project.Value().project != session.summary.project ||
          std::ranges::find(project.Value().workspaces, *session.summary.workspace) == project.Value().workspaces.end())
        co_return Fail<Receipt<SessionCreated>>(ErrorCode::PermissionDenied, "Allocated workspace is outside the project");
    }
    auto target_policy = self->CollaborationAccess(actor.Value(), Capability::CreateSession, session);
    target_policy.new_agent = request.options.agent;
    target_policy.operation = JsonValue{copy->copy_plan->id.value};
    access = co_await self->CheckOperation(std::move(target_policy), confirmation, approved);
    if (!access.Succeeded()) co_return Result::Failure(access.Error());
    if (work) {
      access = co_await self->CheckOperation(
          self->CollaborationAccess(actor.Value(), Capability::SendMessage, session), confirmation, approved);
      if (!access.Succeeded()) co_return Result::Failure(access.Error());
      auto prepared = co_await prepare_work();
      if (!prepared.Succeeded()) co_return Result::Failure(prepared.Error());
    }
    if (!confirmation.empty())
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "Workspace authorization changed after preparation");
    if (candidate) {
      access = co_await self->Require(self->CollaborationAccess(actor.Value(), Capability::SubmitInput, session));
      if (!access.Succeeded()) co_return Result::Failure(access.Error());
      auto resolved = co_await self->ResolveStart(session, user, start);
      if (!resolved.Succeeded()) co_return Result::Failure(resolved.Error());
      binding = std::move(resolved).Value();
    }
    prior = co_await resolve_replay();
    if (!prior.Succeeded()) co_return AgentResult<Receipt<SessionCreated>>::Failure(prior.Error());
    if (prior.Value()) co_return AgentResult<Receipt<SessionCreated>>::Success(*prior.Value());
  }

  if (fork) {
    auto sources = co_await self->VisibleHistorySources(session, actor.Value());
    if (!sources.Succeeded()) co_return Result::Failure(sources.Error());
    auto versions = co_await self->VisibleHistoryArtifacts(session, actor.Value(), sources.Value());
    if (!versions.Succeeded()) co_return Result::Failure(versions.Error());
    for (auto& message : session.messages) FilterHistoryArtifacts(message, versions.Value(), true);
    std::erase_if(session.messages, [&](const Message& message) {
      return !sources.Value().contains(message.ref.session) || message.blocks.empty();
    });
    prior = co_await resolve_replay();
    if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
    if (prior.Value()) co_return Result::Success(*prior.Value());
  }

  std::vector<StoredArtifact> staged_attachments;
  if (initial && !initial->staged_attachments.empty()) {
    auto attachments = co_await self->PrepareInputAttachments(session, user, *initial, binding->model);
    if (!attachments.Succeeded()) co_return Result::Failure(attachments.Error());
    staged_attachments = std::move(attachments).Value();
    prior = co_await resolve_replay();
    if (!prior.Succeeded()) co_return Result::Failure(prior.Error());
    if (prior.Value()) co_return Result::Success(*prior.Value());
  }
  auto current = self->Actor(bound, operation);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  if (current.Value().principal != user) co_return Fail<Receipt<SessionCreated>>(ErrorCode::PermissionDenied, "The acting principal changed");
  if (side && selection_epoch != self->access_revision)
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "Selection access changed before creating the side chat");
  if (fork) {
    const auto* published = self->Published(self->sessions.at(fork->session));
    if (!published || published->summary.lifecycle == SessionLifecycle::Deleting ||
        published->summary.binding_revision != source_binding || (copy && published->summary.workspace != copy->workspace))
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "The source session changed during the fork");
  }
  auto dispatch = self->CheckGoalDispatch(session, goal);
  if (!dispatch.Succeeded()) co_return Result::Failure(dispatch.Error());
  if (session.summary.workspace && self->WorkspaceRetired(*session.summary.workspace))
    co_return Fail<Receipt<SessionCreated>>(ErrorCode::AlreadyClosed, "The workspace was retired");
  if (candidate) {
    candidate->confirmation = std::move(confirmation);
    candidate->session = std::move(session);
    candidate->start = std::move(*binding);
    co_return Result::Success({{}, SessionCreated{id}});
  }
  auto first = initial ? std::optional{*initial} : std::nullopt;
  if (first) {
    if (!staged_attachments.empty() && selection_epoch != self->access_revision)
      co_return Fail<Receipt<SessionCreated>>(ErrorCode::Conflict, "Attachment authorization changed");
    auto committed = self->CommitInputAttachments(session, *first, std::move(staged_attachments));
    if (!committed.Succeeded()) co_return Result::Failure(committed.Error());
  }
  auto& stored = self->sessions.emplace(id, std::move(session)).first->second;
  RunExecution* run = first ? &self->RecordRun(stored, user, std::move(*first), *binding, RunTrigger::UserInput) : nullptr;
  std::vector<const SessionData*> goal_home;
  if (work) {
    self->FirstTitle(stored, work->input);
    if (binding) {
      run = &self->RecordWork(stored, *initial_request, user, *binding);
      if (auto* home = self->AttachGoalRun(*run, goal)) goal_home.push_back(home);
    }
  }
  auto receipt = co_await self->FinishOperation(stored, user, command, key,
      SessionCreated{id, stored.summary.active_run, initial_request}, true, approved.has_value(), nullptr, std::move(goal_home));
  if (receipt.Succeeded() && run && self->stage == Stage::Started && !run->stop.stop_requested()) self->Launch(*run);
  co_return receipt;
}

bool RuntimeState::WorkspaceRetired(const WorkspaceId& workspace) const {
  return std::ranges::any_of(commands, [&](const auto& entry) {
    const auto& record = entry.second;
    return record.retired_workspace == workspace ||
        std::ranges::find(record.retired_comparison_workspaces, workspace, &RetiredComparisonWorkspace::workspace) !=
            record.retired_comparison_workspaces.end();
  });
}

bool RuntimeState::WorkspaceCopyRetired(const WorkspaceCopyPlanRef& copy) const {
  return std::ranges::any_of(commands, [&](const auto& entry) {
    const auto& record = entry.second;
    return (record.retired_workspace && record.input.workspace_choice && record.input.workspace_choice->copy_plan == copy) ||
        std::ranges::find(record.retired_comparison_workspaces, copy, &RetiredComparisonWorkspace::copy) !=
            record.retired_comparison_workspaces.end();
  });
}

Async<void> RuntimeState::AbandonWorkspaceFork(CommandId command, ForkSessionRequest request, WorkspaceId allocated) {
  if (!request.workspace || !ValidCopyPlan(*request.workspace) || request.options.workspace ||
      allocated.value.empty() || allocated == request.workspace->workspace || request.options.retention != SessionRetention::Stored) {
    co_return Fail<void>(ErrorCode::InvalidArgument, "A stored workspace copy and its allocated workspace are required");
  }
  auto user = User();
  if (!user.Succeeded()) co_return AgentResult<void>::Failure(user.Error());
  auto allowed = co_await WithSession(request.from.session, user.Value(), Capability::UpdateSession);
  if (!allowed.Succeeded()) co_return allowed;
  const auto target = WorkspaceForkSession(*request.workspace->copy_plan);
  WorkspaceAllocation allocation{allocating_workspaces, target};
  allocation.held = allocation.pending.insert(target).second;
  if (!allocation.held || executing_operations.contains({user.Value(), command}))
    co_return Fail<void>(ErrorCode::Busy, "The workspace copy is still being processed");
  auto& source = sessions.at(request.from.session);
  if (!options.store || source.retention != SessionRetention::Stored)
    co_return Fail<void>(ErrorCode::Unsupported, "Abandoning a workspace copy requires durable session storage");
  const auto* published = Published(source);
  if (!published || published->summary.lifecycle == SessionLifecycle::Deleting)
    co_return Fail<void>(ErrorCode::NotFound, "The worktree source session is unavailable");
  if (source.summary.workspace != request.workspace->workspace)
    co_return Fail<void>(ErrorCode::Conflict, "The worktree plan no longer matches its source");
  if (sessions.contains(target) || std::ranges::any_of(sessions, [&](const auto& entry) {
        return entry.second.summary.workspace == allocated;
      })) co_return Fail<void>(ErrorCode::Conflict, "A conversation already uses the allocated workspace");

  CommandInput key{.operation = Operation::Fork, .options = std::move(request.options), .work = std::move(request.initial),
                   .fork = std::move(request.from), .workspace_choice = std::move(request.workspace)};
  auto prior = Find(user.Value(), command, key);
  if (!prior.Succeeded()) co_return AgentResult<void>::Failure(prior.Error());
  if (prior.Value()) {
    const auto state = prior.Value()->progress.state;
    if (state == CommandState::AwaitingApproval || state == CommandState::Scheduled)
      co_return Fail<void>(ErrorCode::Busy, "The workspace copy is still awaiting a decision");
    if (state == CommandState::Applied || prior.Value()->progress.accepted.session != source.summary.id)
      co_return Fail<void>(ErrorCode::Conflict, "The command has already committed a workspace copy");
    if (prior.Value()->retired_workspace) {
      if (prior.Value()->retired_workspace != allocated)
        co_return Fail<void>(ErrorCode::Conflict, "The command retired a different workspace");
      if (storage_error) co_return AgentResult<void>::Failure(*storage_error);
      co_return AgentResult<void>::Success();
    }
  }
  ++source.sequence;
  if (!prior.Value()) {
    Accept(user.Value(), command, std::move(key), source, std::monostate{}, CommandState::Withdrawn);
  }
  auto& record = commands.at({user.Value(), command});
  record.retired_workspace = std::move(allocated);
  // The store revision fences a fork submitted by another runtime before the host removes its checkout.
  co_return co_await WhenSaved(Persist(source, false, prior.Value() ? CommitId{} : record.progress.accepted.commit));
}

Async<Receipt<RunAccepted>> RuntimeState::Send(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
                                              CommandId command, SendInput input, UserRunOptions start,
                                              std::shared_ptr<const RenderedExecution> rendered) {
  auto access = co_await self->WithSession(id, user, rendered ? Capability::ReadSession : Capability::SubmitInput);
  if (!access.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(access.Error());
  auto& session = self->sessions.at(id);
  const CommandInput key = rendered ? rendered->key : CommandInput{
      .operation = Operation::Send, .session = id, .input = input, .start = start};
  const bool approved = rendered && rendered->approved;
  if (!approved) {
    if (auto prior = Replay<RunAccepted>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  }
  auto valid = self->CheckInput(input, true);
  if (!valid.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(valid.Error());
  const auto binding_revision = session.summary.binding_revision;
  auto binding = co_await self->ResolveStart(session, user, start);
  if (!binding.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(binding.Error());
  valid = co_await self->ValidateUserSources(session, user, input.context, binding.Value().model.id);
  if (!valid.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(valid.Error());
  const auto attachment_epoch = self->access_revision;
  auto attachments = co_await self->PrepareInputAttachments(session, user, input, binding.Value().model);
  if (!attachments.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(attachments.Error());
  auto conclusions = co_await self->ResolveConclusions(id, user, input, binding.Value().model.id);
  if (!conclusions.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(conclusions.Error());
  input.side_chat_conclusions.clear();
  input.recovered_conclusions.clear();
  input.recovered_steer_conclusions.clear();
  if (rendered) {
    auto permit = Request(user, Capability::SubmitInput, &session);
    permit.run_configuration = binding.Value();
    auto gate = co_await self->CheckRenderedCommit(*rendered, user, command, std::move(permit), binding.Value());
    if (!gate.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(gate.Error());
    if (!gate.Value()) co_return co_await self->ReplayReceipt(self->ReceiptFor<RunAccepted>(user, command));
  }
  if (!approved) {
    if (auto prior = Replay<RunAccepted>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  }
  auto open = CheckOpen(session);
  if (!open.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(open.Error());
  if (binding_revision != session.summary.binding_revision) {
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Conflict, "Session agent changed");
  }
  if (session.summary.active_run || session.consuming || session.summary.queued_inputs) {
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Busy, "Session has active or queued work");
  }
  if (!attachments.Value().empty() && attachment_epoch != self->access_revision)
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Conflict, "Attachment authorization changed");
  valid = self->CommitInputAttachments(session, input, std::move(attachments).Value());
  if (!valid.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(valid.Error());
  auto& run = self->RecordRun(session, user, std::move(input), binding.Value(), RunTrigger::UserInput,
                              {}, std::move(conclusions).Value());
  if (rendered) run.artifact_action = rendered->prepared.artifact;
  auto receipt = co_await self->FinishOperation(session, user, command, key, RunAccepted{run.view.id}, true, approved);
  if (receipt.Succeeded() && self->stage == Stage::Started && !run.stop.stop_requested()) self->Launch(run);
  co_return receipt;
}

Async<Receipt<InputQueued>> RuntimeState::Queue(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
                                               CommandId command, SendInput input, UserRunOptions start) {
  auto access = co_await self->WithSession(id, user, Capability::SubmitInput);
  if (!access.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(access.Error());
  auto& session = self->sessions.at(id);
  CommandInput key{.operation = Operation::Queue, .session = id, .input = input, .start = start};
  if (auto prior = Replay<InputQueued>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto checked = self->CheckInput(input, true);
  if (!checked.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(checked.Error());
  auto binding = co_await self->ResolveStart(session, user, start);
  if (!binding.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(binding.Error());
  checked = co_await self->ValidateUserSources(session, user, input.context, binding.Value().model.id);
  if (!checked.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(checked.Error());
  const auto attachment_epoch = self->access_revision;
  auto attachments = co_await self->PrepareInputAttachments(session, user, input, binding.Value().model);
  if (!attachments.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(attachments.Error());
  auto conclusions = co_await self->ResolveConclusions(id, user, input, binding.Value().model.id);
  if (!conclusions.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(conclusions.Error());
  input.side_chat_conclusions.clear();
  input.recovered_conclusions.clear();
  input.recovered_steer_conclusions.clear();
  if (auto prior = Replay<InputQueued>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto open = CheckOpen(session);
  if (!open.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(open.Error());
  if (!attachments.Value().empty() && attachment_epoch != self->access_revision)
    co_return Fail<Receipt<InputQueued>>(ErrorCode::Conflict, "Attachment authorization changed");
  checked = self->CommitInputAttachments(session, input, std::move(attachments).Value());
  if (!checked.Succeeded()) co_return AgentResult<Receipt<InputQueued>>::Failure(checked.Error());
  const QueueItemId item{MakeId()};
  session.queue.push_back(QueuedInput{.id = item, .input = std::move(input), .state = QueueItemState::Queued,
                                      .user_start = binding.Value(), .conclusions = std::move(conclusions).Value()});
  session.queue_owners.emplace(item, user);
  ++session.summary.queued_inputs;
  ++session.sequence;
  self->Accept(user, command, std::move(key), session, InputQueued{item}, CommandState::Scheduled);
  auto receipt = co_await self->SaveReceipt<InputQueued>(session, user, command);
  if (receipt.Succeeded()) self->options.tasks.Launch([self, id] { return self->ConsumeQueue(id); });
  co_return receipt;
}

Task<void> RuntimeState::ConsumeQueue(SessionId id) {
  auto& session = sessions.at(id);
  if (session.consuming || session.summary.active_run) co_return;
  session.consuming = true;
  auto reset = std::unique_ptr<bool, void(*)(bool*)>(&session.consuming, [](bool* flag) { *flag = false; });
  while (stage == Stage::Started && session.summary.lifecycle == SessionLifecycle::Open && !session.summary.active_run) {
    auto head = std::ranges::find_if(session.queue, [](const QueuedInput& item) {
      return item.state == QueueItemState::Queued && !std::holds_alternative<CollaborationMessageId>(item.input);
    });
    if (head == session.queue.end()) {
      const auto queued = std::ranges::find_if(session.queue, [&](const auto& item) {
        const auto* id = std::get_if<CollaborationMessageId>(&item.input);
        if (!id || item.state != QueueItemState::Queued || PendingStart(*id)) return false;
        const auto& entry = session.collaboration.at(*id);
        return entry.auto_start && CheckGoalDispatch(session, entry.goal).Succeeded();
      });
      if (queued == session.queue.end()) {
        co_await StartGoalRun(id);
        break;
      }
      const auto message = std::get<CollaborationMessageId>(queued->input);
      auto started = co_await StartMessage({"agentkit.start/" + message.value}, message, true);
      const auto& entry = session.collaboration.at(message);
      if (!started.Succeeded() && entry.auto_start && !PendingStart(message) &&
          CheckGoalDispatch(session, entry.goal).Succeeded()) break;
      continue;
    }
    const auto item = head->id;
    if (const auto* change = std::get_if<CommandId>(&head->input)) {
      co_await StartQueuedArtifactChange(id, item, *change);
      continue;
    }
    const auto profile = *head->user_start;
    const auto user = session.queue_owners.at(item);
    const auto revision_at_start = session.summary.binding_revision;
    auto bound = co_await WithSession(id, user, Capability::SubmitInput);
    if (bound.Succeeded()) {
      const auto queued = std::ranges::find(session.queue, item, &QueuedInput::id);
      const auto sources = std::get<SendInput>(queued->input).context;
      bound = co_await ValidateUserSources(session, user, sources, profile.model.id);
    }
    if (bound.Succeeded()) {
      const auto queued = std::ranges::find(session.queue, item, &QueuedInput::id);
      bound = co_await CheckModelAttachments(session, user, std::get<SendInput>(queued->input).attachments, profile.model);
    }
    if (bound.Succeeded()) {
      const auto queued = std::ranges::find(session.queue, item, &QueuedInput::id);
      bound = co_await CheckConclusions(id, Request(user, Capability::SubmitInput, &session).actor, queued->conclusions, profile.model.id);
    }
    auto binding = bound.Succeeded()
        ? co_await ResolveStart(session, user, UserRunOptions{profile.model, profile.execution_profile})
        : AgentResult<RunStartBinding>::Failure(bound.Error());
    head = std::ranges::find(session.queue, item, &QueuedInput::id);
    if (head->state != QueueItemState::Queued) continue;
    if (stage != Stage::Started || session.summary.lifecycle != SessionLifecycle::Open || session.summary.active_run) break;
    if (revision_at_start != session.summary.binding_revision) continue;
    auto owner = std::ranges::find_if(commands, [&](const auto& entry) {
      const auto* queued = std::get_if<InputQueued>(&entry.second.accepted);
      return queued && queued->item == item;
    });
    --session.summary.queued_inputs;
    ++session.sequence;
    if (binding.Succeeded()) {
      auto& run = RecordRun(session, user, std::get<SendInput>(head->input), binding.Value(), RunTrigger::UserInput,
                            {}, head->conclusions);
      head->state = QueueItemState::Started;
      head->run = run.view.id;
      Resolve(owner->second, session, CommandState::Applied);
      auto committed = co_await WhenSaved(Persist(session, true, owner->second.progress.resolution->commit));
      if (committed.Succeeded() && stage == Stage::Started && !run.stop.stop_requested()) Launch(run);
    } else {
      head->state = QueueItemState::Rejected;
      head->reason = binding.Error();
      Resolve(owner->second, session, CommandState::Failed, head->reason);
      auto committed = co_await WhenSaved(Persist(session, false, owner->second.progress.resolution->commit));
      if (!committed.Succeeded()) co_return;
    }
  }
}

Async<CommandReceipt> RuntimeState::Withdraw(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
                                            CommandId command, QueueItemId item) {
  auto access = co_await self->WithSession(id, user, Capability::SubmitInput);
  if (!access.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(access.Error());
  CommandInput key{.operation = Operation::Withdraw, .session = id, .queue_item = item};
  if (auto prior = Replay<std::monostate>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto& session = self->sessions.at(id);
  auto found = std::ranges::find(session.queue, item, &QueuedInput::id);
  if (found == session.queue.end() || session.queue_owners.at(item) != user ||
      std::holds_alternative<CollaborationMessageId>(found->input)) {
    co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Queue item unavailable");
  }
  if (found->state != QueueItemState::Queued) co_return Fail<CommandReceipt>(ErrorCode::AlreadyClosed, "Queue item closed");
  found->state = QueueItemState::Withdrawn;
  --session.summary.queued_inputs;
  ++session.sequence;
  for (auto& [scope, record] : self->commands) {
    const auto* queued = std::get_if<InputQueued>(&record.accepted);
    const auto* change = std::get_if<ChangePlanAccepted>(&record.accepted);
    if ((queued && queued->item == item) || (change && change->queue_item == item))
      self->Resolve(record, session, CommandState::Withdrawn);
  }
  self->Accept(user, command, std::move(key), session, std::monostate{});
  co_return co_await self->SaveReceipt<std::monostate>(session, user, command);
}

std::optional<AgentError> RestartToolError(std::span<const ToolCallSnapshot> tools, const std::set<RunId>& runs,
                                          bool regenerate) {
  bool applied = false;
  for (const auto& tool : tools) {
    if (!runs.contains(tool.run)) continue;
    if (tool.state == ToolCallState::ResultUnknown || std::ranges::any_of(tool.effects, [](const auto& effect) {
          return effect.state == ResourceEffectState::Unknown;
        })) return Error(ErrorCode::ResultUnknown, "Check the original tool result before retrying.");
    const bool partial = tool.state == ToolCallState::PartiallyApplied;
    const bool succeeded = tool.state == ToolCallState::Succeeded;
    if ((partial || succeeded) && tool.reconciliation) applied = true;
    if (!regenerate && (partial || (succeeded && tool.issued_targets && tool.effects.empty()) ||
        std::ranges::any_of(tool.effects, [](const auto& effect) { return effect.state == ResourceEffectState::Applied; })))
      applied = true;
  }
  if (applied) return Error(ErrorCode::Conflict, "The original tool applied changes. Review them before starting a new request.");
  return {};
}

Async<Receipt<RunAccepted>> RuntimeState::Restart(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
    CommandId command, RunId source, bool regenerate) {
  auto access = co_await self->WithSession(id, user, Capability::SubmitInput);
  if (!access.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(access.Error());
  CommandInput key{.operation = regenerate ? Operation::Regenerate : Operation::Retry, .session = id, .run = source};
  if (auto prior = Replay<RunAccepted>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto& session = self->sessions.at(id);
  const auto original = self->runs.find(source);
  if (original == self->runs.end() || original->second.view.session != id) {
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::NotFound, "Run unavailable");
  }
  if (original->second.view.trigger == RunTrigger::ArtifactChange)
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Unsupported, "Submit a fresh artifact change plan");
  if (original->second.view.parent)
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Unsupported, "Retry the parent task to delegate again");
  const auto effect_error = [&] {
    std::set<RunId> related{source};
    if (!original->second.delegations.empty()) {
      for (const auto& run : session.runs) {
        const auto& parent = self->runs.at(run).view.parent;
        if (parent && related.contains(*parent)) related.insert(run);
      }
    }
    return RestartToolError(session.tools, related, regenerate);
  };
  if (auto blocked = effect_error()) co_return AgentResult<Receipt<RunAccepted>>::Failure(std::move(*blocked));
  if (original->second.artifact_action)
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Unsupported, "Reopen the artifact and submit its action again");
  const auto version = session.summary.binding_revision;
  auto binding = co_await self->ResolveStart(session, user, {});
  if (!binding.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(binding.Error());
  auto input = original->second.input;
  if (!regenerate) {
    for (const auto& steer : original->second.view.steering) {
      if (steer.state != SteerState::Applied) continue;
      const auto message = std::ranges::find_if(session.messages, [&](const Message& entry) {
        return entry.ref.message == steer.message && entry.run == source && entry.author == MessageAuthor::User;
      });
      if (message == session.messages.end()) continue;
      for (const auto& block : message->blocks) {
        if (const auto* artifact = std::get_if<ArtifactBlock>(&block.body)) input.attachments.push_back(artifact->artifact);
      }
    }
  }
  auto attachments = co_await self->CheckModelAttachments(session, user, input.attachments, binding.Value().model);
  if (!attachments.Succeeded()) co_return AgentResult<Receipt<RunAccepted>>::Failure(attachments.Error());
  if (auto prior = Replay<RunAccepted>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  if (session.summary.lifecycle != SessionLifecycle::Open || version != session.summary.binding_revision) {
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Conflict, "Session binding changed");
  }
  if (session.summary.active_run || session.summary.queued_inputs || !Terminal(original->second.view.state)) {
    co_return Fail<Receipt<RunAccepted>>(ErrorCode::Busy, "Session has active or queued work");
  }
  if (auto blocked = effect_error()) co_return AgentResult<Receipt<RunAccepted>>::Failure(std::move(*blocked));
  auto& run = self->RecordRun(session, user, std::move(input), binding.Value(),
                              regenerate ? RunTrigger::Regenerate : RunTrigger::Retry, source);
  self->Accept(user, command, std::move(key), session, RunAccepted{run.view.id});
  auto receipt = co_await self->SaveReceipt<RunAccepted>(session, user, command, true);
  if (receipt.Succeeded() && self->stage == Stage::Started && !run.stop.stop_requested()) self->Launch(run);
  co_return receipt;
}

Async<CommandReceipt> RuntimeState::UpdateMetadata(std::shared_ptr<RuntimeState> self, CommandId command,
    SessionId id, Revision expected, SessionMetadataChange change) {
  auto user = self->User();
  if (!user.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(user.Error());
  auto access = co_await self->WithSession(id, user.Value(), Capability::UpdateSession);
  if (!access.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(access.Error());
  CommandInput key{.operation = Operation::Metadata, .session = id, .metadata = change, .expected = expected};
  if (auto prior = Replay<std::monostate>(*self, user.Value(), command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto& session = self->sessions.at(id);
  auto& summary = session.summary;
  if (!expected) co_return Fail<CommandReceipt>(ErrorCode::InvalidArgument, "Expected metadata revision is required");
  if (summary.metadata_revision != expected) {
    auto error = Error(ErrorCode::Conflict, "Session metadata changed");
    error.current_revision = summary.metadata_revision;
    co_return AgentResult<CommandReceipt>::Failure(std::move(error));
  }
  bool changed_order = false;
  if (const auto* rename = std::get_if<RenameSession>(&change)) {
    auto title = Title(rename->title);
    if (title.empty()) co_return Fail<CommandReceipt>(ErrorCode::InvalidArgument, "Title must be nonempty UTF-8");
    summary.title = std::move(title);
    summary.title_source = SessionTitleSource::Manual;
  } else {
    if (summary.active_run) co_return Fail<CommandReceipt>(ErrorCode::Busy, "Finish the active run before changing lifecycle");
    const auto lifecycle = std::holds_alternative<ArchiveSession>(change) ? SessionLifecycle::Archived
        : std::holds_alternative<UnarchiveSession>(change) ? SessionLifecycle::Open : SessionLifecycle::Deleting;
    changed_order = lifecycle != summary.lifecycle;
    summary.lifecycle = lifecycle;
    if (lifecycle == SessionLifecycle::Deleting) {
      for (auto& input : session.queue) {
        if (input.state != QueueItemState::Queued) continue;
        input.state = QueueItemState::Withdrawn;
        for (auto& [scope, record] : self->commands) {
          auto* queued = std::get_if<InputQueued>(&record.accepted);
          const auto* rework = std::get_if<ChangePlanAccepted>(&record.accepted);
          if ((queued && queued->item == input.id) || (rework && rework->queue_item == input.id))
            self->Resolve(record, session, CommandState::Withdrawn);
        }
      }
      summary.queued_inputs = 0;
    }
  }
  ++summary.metadata_revision;
  ++session.sequence;
  self->Accept(user.Value(), command, std::move(key), session, std::monostate{});
  auto receipt = co_await self->SaveReceipt<std::monostate>(session, user.Value(), command, changed_order);
  if (receipt.Succeeded() && summary.lifecycle == SessionLifecycle::Open && (summary.queued_inputs || session.current_goal)) {
    self->options.tasks.Launch([self, id] { return self->ConsumeQueue(id); });
  }
  co_return receipt;
}

Async<CommandReceipt> RuntimeState::Transfer(std::shared_ptr<RuntimeState> self, SessionId id, PrincipalId user,
                                            CommandId command, TransferRequest request) {
  auto access = co_await self->WithSession(id, user, Capability::ReadSession);
  if (!access.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(access.Error());
  CommandInput key{.operation = Operation::Transfer, .session = id, .transfer = request};
  if (auto prior = Replay<std::monostate>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  auto& session = self->sessions.at(id);
  auto permit = Request(user, Capability::TransferAgent, &session);
  permit.new_agent = request.next;
  access = co_await self->Require(std::move(permit));
  if (!access.Succeeded()) co_return AgentResult<CommandReceipt>::Failure(access.Error());
  if (auto prior = Replay<std::monostate>(*self, user, command, key)) co_return co_await self->ReplayReceipt(std::move(*prior));
  if (!request.expected_binding || request.next.value.empty() ||
      (request.handoff_summary && !ValidUtf8(*request.handoff_summary))) {
    co_return Fail<CommandReceipt>(ErrorCode::InvalidArgument, "Invalid transfer request");
  }
  auto next = self->agents.find(request.next);
  if (next == self->agents.end()) co_return Fail<CommandReceipt>(ErrorCode::NotFound, "Target agent unavailable");
  auto& summary = session.summary;
  if (summary.lifecycle != SessionLifecycle::Open || summary.binding_revision != request.expected_binding) {
    co_return Fail<CommandReceipt>(ErrorCode::Conflict, "Session binding changed");
  }
  if (summary.active_run) co_return Fail<CommandReceipt>(ErrorCode::Busy, "Session has an active run");
  auto from = summary.agent;
  summary.agent = next->second.ref;
  ++summary.binding_revision;
  session.messages.push_back(Message{
      .ref = {id, MessageId{MakeId()}, 1}, .sequence = ++session.sequence,
      .author = MessageAuthor::System, .state = MessageState::Complete,
      .blocks = {{BlockId{MakeId()}, AgentTransferred{from, summary.agent, summary.binding_revision, request.handoff_summary}}}});
  self->Accept(user, command, std::move(key), session, std::monostate{});
  co_return co_await self->SaveReceipt<std::monostate>(session, user, command, true);
}

} // namespace detail

using detail::RuntimeState;

Async<Receipt<SessionCreated>> SessionService::CreateAsync(CommandId command, CreateSessionRequest request) {
  return RuntimeState::Create(state_, std::move(command), std::move(request), actor_);
}

Async<Receipt<SessionCreated>> SessionService::ForkAsync(CommandId command, ForkSessionRequest request) {
  return [](SessionService service, CommandId id, ForkSessionRequest value) -> Async<Receipt<SessionCreated>> {
    if (value.options.workspace)
      co_return detail::Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument, "Choose a fork workspace through WorkspaceChoice");
    std::optional<WorkspaceChoice> copy;
    if (value.workspace) {
      if (value.workspace->workspace.value.empty())
        co_return detail::Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument, "A workspace ID is required");
      if (value.workspace->kind == WorkspaceChoice::Kind::Existing) {
        if (value.workspace->copy_plan)
          co_return detail::Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument, "An existing workspace cannot use a copy plan");
        value.options.workspace = value.workspace->workspace;
      } else {
        if (!detail::ValidCopyPlan(*value.workspace))
          co_return detail::Fail<Receipt<SessionCreated>>(ErrorCode::InvalidArgument, "A valid workspace copy plan is required");
        copy = std::move(value.workspace);
      }
    }
    CreateSessionRequest create{.options = std::move(value.options)};
    if (value.initial) create.initial = std::move(*value.initial);
    co_return co_await RuntimeState::Create(service.state_, std::move(id), std::move(create), service.actor_,
                                           std::move(value.from), {}, std::move(copy));
  }(*this, std::move(command), std::move(request));
}

Async<void> SessionService::AbandonWorkspaceForkAsync(CommandId command, ForkSessionRequest request, WorkspaceId allocated) {
  return [](SessionService service, CommandId id, ForkSessionRequest value, WorkspaceId workspace) -> Async<void> {
    if (service.actor_) co_return detail::Fail<void>(ErrorCode::PermissionDenied, "Workspace cleanup requires a user session service");
    co_return co_await service.state_->AbandonWorkspaceFork(std::move(id), std::move(value), std::move(workspace));
  }(*this, std::move(command), std::move(request), std::move(allocated));
}

Async<CommandReceipt> SessionService::UpdateMetadataAsync(CommandId command, SessionId session,
    Revision expected, SessionMetadataChange change) {
  if (actor_) {
    return [](AgentResult<CommandReceipt> denied) -> Async<CommandReceipt> { co_return denied; }(
        detail::Fail<CommandReceipt>(ErrorCode::PermissionDenied, "Metadata changes require a user session service"));
  }
  return RuntimeState::UpdateMetadata(state_, std::move(command), std::move(session), expected, std::move(change));
}

Async<Receipt<RunAccepted>> Conversation::SendAsync(CommandId command, SendInput input, UserRunOptions start) {
  return RuntimeState::Send(state_, id_, principal_, std::move(command), std::move(input), std::move(start));
}

Async<Receipt<InputQueued>> Conversation::QueueAsync(CommandId command, SendInput input, UserRunOptions start) {
  return RuntimeState::Queue(state_, id_, principal_, std::move(command), std::move(input), std::move(start));
}

Async<CommandReceipt> Conversation::WithdrawQueuedInputAsync(CommandId command, QueueItemId item) {
  return RuntimeState::Withdraw(state_, id_, principal_, std::move(command), std::move(item));
}

Async<Receipt<RunAccepted>> Conversation::RetryAsync(CommandId command, RunId source) {
  return RuntimeState::Restart(state_, id_, principal_, std::move(command), std::move(source), false);
}

Async<Receipt<RunAccepted>> Conversation::RegenerateAsync(CommandId command, RunId source) {
  return RuntimeState::Restart(state_, id_, principal_, std::move(command), std::move(source), true);
}

Async<CommandReceipt> Conversation::TransferAsync(CommandId command, TransferRequest request) {
  return RuntimeState::Transfer(state_, id_, principal_, std::move(command), std::move(request));
}

} // namespace huxerui::agentkit
