#include <huxerui/agentkit/runtime.h>

#include "runtime_internal.h"

#include <algorithm>
#include <atomic>
#include <random>

namespace huxerui::agentkit {
namespace detail {

AgentError Error(ErrorCode code, std::string message) {
  return AgentError{.code = code, .message = std::move(message)};
}

bool Terminal(RunState state) {
  return state == RunState::Succeeded || state == RunState::Failed || state == RunState::Cancelled;
}

bool ValidUtf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i++]);
    if (lead < 0x80) continue;
    if (lead < 0xc2 || lead > 0xf4) return false;
    const std::size_t tails = lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
    if (tails > text.size() - i) return false;
    const auto next = static_cast<unsigned char>(text[i]);
    if ((lead == 0xe0 && next < 0xa0) || (lead == 0xed && next >= 0xa0) ||
        (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next >= 0x90)) return false;
    for (std::size_t j = 0; j < tails; ++j) {
      if ((static_cast<unsigned char>(text[i++]) & 0xc0) != 0x80) return false;
    }
  }
  return true;
}

std::string Title(std::string_view text) {
  constexpr std::size_t max_title_bytes = 96;
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos || !ValidUtf8(text)) return {};
  text.remove_prefix(first);
  auto end = std::min({text.find_first_of("\r\n"), text.size(), max_title_bytes});
  while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
  text = text.substr(0, end);
  return std::string(text.substr(0, text.find_last_not_of(" \t") + 1));
}

std::string MakeId() {
  static const auto epoch = std::chrono::system_clock::now().time_since_epoch().count();
  static const auto salt = std::random_device{}();
  static std::atomic<std::uint64_t> serial = 0;
  return std::to_string(epoch) + "-" + std::to_string(salt) + "-" + std::to_string(++serial);
}

bool Supports(ModelCapabilities actual, ModelCapabilities required) {
  return (!required.text_stream || actual.text_stream) &&
      (!required.tool_calls || actual.tool_calls) &&
      (!required.structured_output || actual.structured_output) &&
      (!required.image_input || actual.image_input) &&
      (!required.file_input || actual.file_input) &&
      (!required.usage || actual.usage) &&
      (!required.multi_tool_calls || actual.multi_tool_calls) &&
      (required.cancellation != ModelCancellation::ProviderCancel ||
       actual.cancellation == ModelCancellation::ProviderCancel);
}

RuntimeState::RuntimeState(RuntimeOptions value) : options(std::move(value)) {}

TimePoint RuntimeState::Now() const {
  return options.now ? options.now() : std::chrono::time_point_cast<Duration>(std::chrono::system_clock::now());
}

AgentResult<PrincipalId> RuntimeState::User() const {
  if (stage == Stage::Closed) return Fail<PrincipalId>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  if (stage != Stage::Started) return Fail<PrincipalId>(ErrorCode::InvalidState, "Start the agent runtime first");
  auto user = options.principal();
  if (user.Succeeded() && user.Value().value.empty()) {
    return Fail<PrincipalId>(ErrorCode::InvalidArgument, "A principal ID is required");
  }
  return user;
}

PolicyRequest RuntimeState::Request(const PrincipalId& user, Capability capability, const SessionData* session,
                                    const RunExecution* run) {
  PolicyRequest request{.actor = {.kind = ActionParty::Kind::User, .principal = user}, .capability = capability};
  if (!session) return request;
  request.agent = session->summary.agent;
  request.project = session->summary.project;
  request.workspace = session->summary.workspace;
  request.target.session = session->summary.id;
  if (run) {
    request.actor.kind = ActionParty::Kind::Agent;
    request.actor.session = session->summary.id;
    request.actor.run = run->view.id;
    request.actor.agent = run->view.agent;
    request.agent = run->view.agent;
    request.target.run = run->view.id;
    request.goal = run->view.goal;
    if (run->view.goal) request.target.goal = run->view.goal->id;
    request.run_configuration = RunStartBinding{run->view.model, run->view.execution_profile};
  }
  return request;
}

Async<PolicyDecision> RuntimeState::Decide(PolicyRequest request) {
  if (stage != Stage::Started) co_return Fail<PolicyDecision>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  const auto actor = request.actor;
  const auto capability = request.capability;
  bool parent_requires_confirmation = false;
  const auto caller = actor.run ? runs.find(*actor.run) : runs.end();
  if (caller != runs.end() && caller->second.delegate &&
      (capability == Capability::SendToModel || capability == Capability::PrepareTool || capability == Capability::ExecuteTool)) {
    for (const auto* child = &caller->second; child->view.parent; ) {
      const auto& parent = runs.at(*child->view.parent);
      const auto& session = sessions.at(child->view.session);
      auto delegation = Request(parent.principal, Capability::DelegateRun, &session, &parent);
      delegation.new_agent = child->view.agent.id;
      delegation.recipient_model = child->view.model.id;
      delegation.sources = child->input.context;
      for (const auto& ref : child->input.attachments) delegation.sources.push_back({ref});
      auto permission = co_await options.access(std::move(delegation));
      if (!permission.Succeeded()) co_return permission;
      if (stage != Stage::Started || parent.stop.stop_requested())
        co_return Fail<PolicyDecision>(ErrorCode::AlreadyClosed, "Ancestor run closed");
      if (permission.Value().decision != AccessDecision::Allow)
        co_return Fail<PolicyDecision>(ErrorCode::PermissionDenied, "Delegation authorization changed");
      child = &parent;
    }
  }
  if (caller != runs.end() && caller->second.delegate && request.tool &&
      (capability == Capability::DiscoverTool || capability == Capability::PrepareTool || capability == Capability::ExecuteTool)) {
    const auto& child = caller->second;
    if (!AllowsRunTool(child, *request.tool))
      co_return Fail<PolicyDecision>(ErrorCode::PermissionDenied, "The tool is outside the delegation scope");
    for (auto parent = child.view.parent; parent; ) {
      const auto& execution = runs.at(*parent);
      const auto& session = sessions.at(execution.view.session);
      const auto binding = Request(execution.principal, capability, &session, &execution);
      auto inherited = request;
      inherited.actor = binding.actor;
      inherited.actor.call = actor.call;
      inherited.agent = binding.agent;
      inherited.run_configuration = binding.run_configuration;
      if (inherited.invocation) {
        inherited.invocation->actor = inherited.actor;
        inherited.invocation->run = execution.view.id;
      }
      auto permission = co_await options.access(std::move(inherited));
      if (!permission.Succeeded()) co_return permission;
      if (execution.stop.stop_requested()) co_return Fail<PolicyDecision>(ErrorCode::AlreadyClosed, "Ancestor run closed");
      if (permission.Value().decision == AccessDecision::Deny ||
          (capability != Capability::ExecuteTool && permission.Value().decision != AccessDecision::Allow)) co_return permission;
      parent_requires_confirmation |= permission.Value().decision != AccessDecision::Allow;
      parent = execution.view.parent;
    }
  }
  auto decision = co_await options.access(std::move(request));
  if (stage != Stage::Started) co_return Fail<PolicyDecision>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  if (actor.kind == ActionParty::Kind::User) {
    auto current = User();
    if (!current.Succeeded() || current.Value() != actor.principal) {
      co_return Fail<PolicyDecision>(ErrorCode::PermissionDenied, "The acting principal changed");
    }
  }
  if (decision.Succeeded() && decision.Value().decision == AccessDecision::AllowReview && capability != Capability::ExecuteTool) {
    co_return Fail<PolicyDecision>(ErrorCode::InvalidState, "Only prepared tool execution supports delegated review");
  }
  if (parent_requires_confirmation && decision.Succeeded() && decision.Value().decision != AccessDecision::Deny)
    co_return AgentResult<PolicyDecision>::Success({AccessDecision::RequireConfirmation, "The delegating run requires confirmation"});
  co_return decision;
}

Async<void> RuntimeState::Require(PolicyRequest request) {
  auto decision = co_await Decide(std::move(request));
  if (!decision.Succeeded()) co_return AgentResult<void>::Failure(decision.Error());
  if (decision.Value().decision != AccessDecision::Allow) {
    co_return Fail<void>(ErrorCode::PermissionDenied, decision.Value().reason);
  }
  co_return AgentResult<void>::Success();
}

void RuntimeState::ReportException(std::exception_ptr error) const noexcept {
  if (options.on_error) {
    try { options.on_error(std::move(error)); } catch (...) {}
  }
}

void RuntimeState::Changed(bool order) {
  ++revision;
  if (order) ++order_revision;
  ScheduleObservers();
}

void RuntimeState::ScheduleObservers() {
  dirty = true;
  if (notifying || observers.empty() || stage != Stage::Started) return;
  notifying = true;
  options.tasks.Launch([self = shared_from_this()] { return self->Notify(); });
}

Task<void> RuntimeState::Notify() {
  while (dirty && stage == Stage::Started) {
    dirty = false;
    std::erase_if(observers, [](const ActiveObserver& observer) {
      auto active = observer.active.lock();
      return !active || !*active;
    });
    auto pending = observers;
    for (auto& observer : pending) {
      if (stage != Stage::Started) break;
      co_await observer.deliver();
    }
  }
  notifying = false;
}

AgentResult<const CommandRecord*> RuntimeState::Find(const PrincipalId& user, const CommandId& command,
                                                     const CommandInput& input) const {
  if (command.value.empty()) return Fail<const CommandRecord*>(ErrorCode::InvalidArgument, "A command ID is required");
  const auto it = commands.find({user, command});
  if (it == commands.end()) {
    if (FindProjectCommand(user, command, false)) return Fail<const CommandRecord*>(ErrorCode::Conflict, "The command ID is bound to a project operation");
    return AgentResult<const CommandRecord*>::Success(nullptr);
  }
  if (it->second.input != input) {
    return Fail<const CommandRecord*>(ErrorCode::Conflict, "The command ID is bound to a different request");
  }
  return AgentResult<const CommandRecord*>::Success(&it->second);
}

void RuntimeState::Accept(const PrincipalId& user, const CommandId& command, CommandInput input,
                          const SessionData& session, CommandValue value, CommandState status) {
  CommandProgress progress{
      .accepted = {.command = command, .commit = {MakeId()}, .session = session.summary.id,
                   .session_sequence = session.sequence},
      .state = status, .result = value};
  commands.emplace(std::pair(user, command), CommandRecord{std::move(input), std::move(progress), std::move(value)});
}

void RuntimeState::Resolve(CommandRecord& record, SessionData& session, CommandState status,
                           std::optional<AgentError> failure) {
  record.progress.state = status;
  record.progress.failure = std::move(failure);
  record.progress.resolution = CommitStamp{
      .command = record.progress.accepted.command, .commit = {MakeId()},
      .session = session.summary.id, .session_sequence = session.sequence};
}

} // namespace detail

using detail::Fail;
using detail::RuntimeState;

CommandId NewCommandId() { return {detail::MakeId()}; }

Subscription::Subscription(std::shared_ptr<bool> active) : active_(std::move(active)) {}

Subscription::Subscription(Subscription&& other) noexcept = default;

Subscription& Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    Reset();
    active_ = std::move(other.active_);
  }
  return *this;
}

Subscription::~Subscription() { Reset(); }

void Subscription::Reset() noexcept {
  if (active_) *active_ = false;
  active_.reset();
}

AgentRuntime::AgentRuntime(std::shared_ptr<RuntimeState> state) : state_(std::move(state)) {}

AgentResult<std::shared_ptr<AgentRuntime>> AgentRuntime::Create(RuntimeOptions options) {
  if (!options.principal || !options.access || detail::Title(options.untitled_session_title).empty()) {
    return Fail<std::shared_ptr<AgentRuntime>>(ErrorCode::InvalidArgument,
                                               "Principal, access policy and placeholder title are required");
  }
  if (const auto& reviewer = options.tool_reviewer; reviewer &&
      (reviewer->id.empty() || !detail::ValidUtf8(reviewer->id) || !reviewer->version ||
       reviewer->timeout <= Duration::zero() || !reviewer->review)) {
    return Fail<std::shared_ptr<AgentRuntime>>(ErrorCode::InvalidArgument, "Invalid tool reviewer registration");
  }
  if (!options.max_parallel_tools_per_turn)
    return Fail<std::shared_ptr<AgentRuntime>>(ErrorCode::InvalidArgument, "Tool concurrency limit must be at least one");
  if (bool(options.changes.prepare_rework) != bool(options.changes.execute_rework)) {
    return Fail<std::shared_ptr<AgentRuntime>>(ErrorCode::InvalidArgument, "Rework requires both preparation and execution");
  }
  if (bool(options.changes.prepare_revert) != bool(options.changes.execute_revert)) {
    return Fail<std::shared_ptr<AgentRuntime>>(ErrorCode::InvalidArgument, "Revert requires both preparation and execution");
  }
  if (bool(options.changes.prepare_adoption) != bool(options.changes.execute_adoption)) {
    return Fail<std::shared_ptr<AgentRuntime>>(ErrorCode::InvalidArgument, "Adoption requires both preparation and execution");
  }
  auto state = std::make_shared<RuntimeState>(std::move(options));
  state->InstallArtifactChangeTools();
  return AgentResult<std::shared_ptr<AgentRuntime>>::Success(std::shared_ptr<AgentRuntime>(new AgentRuntime(std::move(state))));
}

namespace {
template<class Registry, class Key, class Value, class Version>
AgentResult<void> RegisterVersion(Registry& registry, Key key, Value value, Version version) {
  auto old = registry.find(key);
  if (old != registry.end() && version(old->second) >= version(value)) {
    return Fail<void>(ErrorCode::Conflict, "Registration version must increase");
  }
  registry.insert_or_assign(std::move(key), std::move(value));
  return AgentResult<void>::Success();
}
} // namespace

AgentResult<void> AgentRuntime::RegisterModel(ModelRegistration model) {
  if (state_->stage != RuntimeState::Stage::Started && state_->stage != RuntimeState::Stage::Registering)
    return Fail<void>(ErrorCode::InvalidState, "Registration closed");
  if (model.ref.id.value.empty() || !model.ref.version || !model.provider || model.endpoint.provider.empty() ||
      model.endpoint.model.empty() || model.endpoint.protocol.empty() || model.context_limits.context_window == 0 ||
      model.context_limits.max_input_tokens == 0 || model.context_limits.max_output_tokens == 0) {
    return Fail<void>(ErrorCode::InvalidArgument, "Invalid model registration");
  }
  auto& versions = state_->models[model.ref.id];
  const auto version = model.ref.version;
  if (!versions.empty() && versions.rbegin()->first >= version)
    return Fail<void>(ErrorCode::Conflict, "Registration version must increase");
  versions.emplace(version, std::move(model));
  return AgentResult<void>::Success();
}

const ModelRegistration* RuntimeState::ResolveModel(const ModelProviderRef& ref) const {
  const auto model = models.find(ref.id);
  if (model == models.end()) return nullptr;
  const auto version = model->second.find(ref.version);
  return version == model->second.end() ? nullptr : &version->second;
}

const ModelRegistration* RuntimeState::LatestModel(const ModelId& id) const {
  const auto model = models.find(id);
  return model == models.end() ? nullptr : &model->second.rbegin()->second;
}

AgentResult<void> AgentRuntime::RegisterTool(ToolDefinition tool) {
  if (state_->stage != RuntimeState::Stage::Registering) return Fail<void>(ErrorCode::InvalidState, "Registration closed");
  if (tool.id.value.empty() || !tool.version || !tool.prepare || !tool.execute ||
      (tool.concurrency == ToolConcurrency::ReadOnly && tool.effect != ToolEffect::Read)) {
    return Fail<void>(ErrorCode::InvalidArgument, "Invalid tool registration");
  }
  if (tool.id == detail::DelegationToolId || detail::SkillTool(tool.id.value) || detail::ArtifactChangeTool(tool.id) || state_->ReservedToolId(tool.id))
    return Fail<void>(ErrorCode::Conflict, "This tool ID is reserved");
  auto id = tool.id;
  return RegisterVersion(state_->tools, std::move(id), std::make_shared<const ToolDefinition>(std::move(tool)),
                         [](const auto& item) { return item->version; });
}

AgentResult<void> AgentRuntime::RegisterContextProvider(ContextProviderId id, ContextProvider provider) {
  if (state_->stage != RuntimeState::Stage::Registering) return Fail<void>(ErrorCode::InvalidState, "Registration closed");
  if (id.value.empty() || !provider) return Fail<void>(ErrorCode::InvalidArgument, "Invalid context provider");
  if (!state_->context.emplace(std::move(id), std::move(provider)).second) {
    return Fail<void>(ErrorCode::Conflict, "Context provider ID already registered");
  }
  return AgentResult<void>::Success();
}

AgentResult<void> AgentRuntime::RegisterAgent(AgentRegistration agent) {
  if (state_->stage != RuntimeState::Stage::Registering) return Fail<void>(ErrorCode::InvalidState, "Registration closed");
  if (std::ranges::any_of(agent.definition.tools, detail::ArtifactChangeTool))
    return Fail<void>(ErrorCode::InvalidArgument, "Artifact changes are exposed through the artifact service");
  auto id = agent.definition.ref.id;
  if (id.value.empty() || !agent.definition.ref.version || agent.definition.display_name.empty()) {
    return Fail<void>(ErrorCode::InvalidArgument, "Invalid agent registration");
  }
  if (agent.restore && !agent.run) return Fail<void>(ErrorCode::InvalidArgument, "A restore handler requires a run handler");
  auto registered = RegisterVersion(state_->agents, id, std::move(agent.definition),
                                    [](const AgentDefinition& item) { return item.ref.version; });
  if (registered.Succeeded()) {
    if (agent.run) state_->workflows.insert_or_assign(id, detail::HostWorkflow{std::move(agent.run), std::move(agent.restore)});
    else state_->workflows.erase(id);
  }
  return registered;
}

AgentResult<void> AgentRuntime::ValidateRegistration() const {
  for (const auto& [id, agent] : state_->agents) {
    const auto* model = state_->LatestModel(agent.model);
    if (!model) return Fail<void>(ErrorCode::NotFound, "Unregistered model for agent " + id.value);
    auto required = agent.requires_model;
    required.tool_calls |= (!agent.tools.empty() || !agent.tool_catalogs.empty() || !agent.delegates.empty()) &&
        !state_->workflows.contains(id);
    if (!detail::Supports(model->provider->Capabilities(), required)) {
      return Fail<void>(ErrorCode::Unsupported, "Model lacks required capabilities for agent " + id.value);
    }
    for (const auto& tool : agent.tools) {
      if (!state_->tools.contains(tool)) return Fail<void>(ErrorCode::NotFound, "Unregistered tool " + tool.value);
    }
    for (const auto& catalog : agent.tool_catalogs) {
      if (!state_->tool_catalogs.contains(catalog)) return Fail<void>(ErrorCode::NotFound, "Unregistered tool catalog " + catalog.value);
    }
    for (const auto& source : agent.skill_sources) {
      if (!state_->skill_sources.contains(source)) return Fail<void>(ErrorCode::NotFound, "Unregistered Skill source");
    }
    for (const auto& delegate : agent.delegates) {
      if (!state_->agents.contains(delegate)) return Fail<void>(ErrorCode::NotFound, "Unregistered delegate " + delegate.value);
    }
    for (const auto& provider : agent.context) {
      if (!state_->context.contains(provider)) return Fail<void>(ErrorCode::NotFound, "Unregistered context provider");
    }
    for (const auto& type : agent.content_types) {
      if (!state_->content_components.contains({type.id, type.schema_version})) {
        return Fail<void>(ErrorCode::NotFound, "Unregistered content component " + type.id);
      }
    }
  }
  return AgentResult<void>::Success();
}

Async<void> AgentRuntime::StartAsync() {
  auto state = state_;
  if (state->stage == RuntimeState::Stage::Closed) co_return Fail<void>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  if (state->stage == RuntimeState::Stage::Started) co_return AgentResult<void>::Success();
  if (state->stage == RuntimeState::Stage::Starting) co_return Fail<void>(ErrorCode::Busy, "Agent runtime is loading");
  auto valid = ValidateRegistration();
  if (!valid.Succeeded()) co_return valid;
  try {
    state->options.tasks.Post([] {});
  } catch (const std::logic_error&) {
    co_return Fail<void>(ErrorCode::InvalidArgument, "An application task scope is required");
  }
  state->stage = RuntimeState::Stage::Starting;
  auto loaded = co_await state->LoadStored();
  if (state->stage == RuntimeState::Stage::Closed) co_return Fail<void>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  if (!loaded.Succeeded()) {
    state->stage = RuntimeState::Stage::Registering;
    state->sessions.clear();
    state->runs.clear();
    state->commands.clear();
    state->saved_sessions.clear();
    state->ephemeral_sessions.clear();
    state->session_events.clear();
    state->projects.clear();
    state->saved_projects.clear();
    state->storage_error.reset();
    co_return loaded;
  }
  state->stage = RuntimeState::Stage::Started;
  state->ResumeOperations();
  for (auto& [id, run] : state->runs) {
    if (run.restoring && !run.delegate) state->Launch(run);
  }
  for (const auto& [id, session] : state->sessions) {
    if (session.summary.lifecycle == SessionLifecycle::Open && (session.summary.queued_inputs || session.current_goal)) {
      state->options.tasks.Launch([state, id] { return state->ConsumeQueue(id); });
    }
    if (session.current_goal) state->ScheduleGoalVerification(id, session.goals.at(*session.current_goal));
  }
  co_return AgentResult<void>::Success();
}

Async<void> AgentRuntime::CloseAsync() {
  auto state = state_;
  if (state->stage != RuntimeState::Stage::Closed) {
    state->stage = RuntimeState::Stage::Closed;
    for (auto& [id, run] : state->runs) {
      if (detail::Terminal(run.view.state) || run.view.state == RunState::Cancelling) continue;
      const auto saved = state->saved_sessions.find(run.view.session);
      bool resumable = false;
      const bool waiting_on_model_tools = run.host_model_step && !run.pending_calls.empty();
      if (saved != state->saved_sessions.end() && !run.ExecutingTool() && (!run.host_model_busy || waiting_on_model_tools) && !run.stream &&
          (run.model_boundary || run.tool_turn || !run.tool_attempts.empty() || run.checkpoint)) {
        const auto& image = saved->second.image;
        auto prior = std::ranges::find_if(image.runs, [&](const auto& entry) { return entry.view.id == id; });
        resumable = prior != image.runs.end() && state->CanRestore(*prior, image.data);
      }
      if (resumable) {
        run.suspended = true;
        run.stop.request_stop();
        run.InterruptToolReviews();
        state->CancelToolTurnTasks(run);
        run.task.Cancel();
        run.host_tool_task.Cancel();
        run.timer.Cancel();
      } else {
        state->Stop(run, RunEndReason::Interrupted);
      }
    }
    for (auto& [id, session] : state->sessions) {
      for (auto& [goal_id, goal] : session.goals) goal.verification_stop.request_stop();
      for (auto& [interaction, timer] : session.interaction_timers) timer.Cancel();
      session.interaction_timers.clear();
    }
    auto observers = std::exchange(state->observers, {});
    for (auto& observer : observers) {
      try { observer.close(); } catch (...) { state->ReportException(std::current_exception()); }
    }
    state->visible_sessions.clear();
  }
  while (std::ranges::any_of(state->runs, [](const auto& item) {
    return !detail::Terminal(item.second.view.state) && !item.second.suspended;
  })) {
    co_await Delay(Duration(20));
  }
  if (!state->saves.empty()) {
    auto committed = co_await RuntimeState::WhenSaved(state->saves.back().ticket);
    if (!committed.Succeeded()) co_return committed;
  }
  if (state->storage_error) co_return AgentResult<void>::Failure(*state->storage_error);
  co_return AgentResult<void>::Success();
}

Async<void> AgentRuntime::RefreshAccessAsync() {
  auto state = state_;
  auto user = state->User();
  if (!user.Succeeded()) co_return AgentResult<void>::Failure(user.Error());
  ++state->access_revision;
  for (auto& [id, run] : state->runs) {
    run.InterruptToolReviews();
  }
  state->visible_sessions.clear();
  state->visible_history_artifacts.clear();
  state->checked_session_artifacts.clear();
  state->visible_approval_artifacts.clear();
  state->visible_selections.clear();
  state->Changed(true);
  co_return AgentResult<void>::Success();
}

CommandService AgentRuntime::Commands() { return CommandService(state_); }

SessionService AgentRuntime::Sessions() { return SessionService(state_); }

InteractionService AgentRuntime::Interactions() { return InteractionService(state_); }

ArtifactChanges AgentRuntime::Artifacts() { return ArtifactChanges(state_); }

} // namespace huxerui::agentkit
