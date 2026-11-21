#include "runtime_internal.h"
#include "json_wire.h"

#include <nlohmann/json.hpp>

namespace huxerui::agentkit::detail {
namespace {

constexpr std::size_t MaxBatchSize = 32;

const RunExecution* Parent(const RuntimeState& state, const RunExecution& run) {
  return run.view.parent ? &state.runs.at(*run.view.parent) : nullptr;
}

std::uint64_t SatAdd(std::uint64_t a, std::uint64_t b) {
  return a + std::min(UINT64_MAX - a, b);
}

void Limit(std::optional<std::uint64_t>& cap, std::optional<std::uint64_t> allocation, std::uint64_t used = 0) {
  if (!allocation) return;
  const auto remaining = *allocation - std::min(*allocation, used);
  cap = std::min(cap.value_or(remaining), remaining);
}

bool InScope(const std::vector<SourceSlice>& scope, const std::vector<SourceSlice>& sources) {
  return std::ranges::all_of(sources, [&](const auto& source) { return std::ranges::find(scope, source) != scope.end(); });
}

} // namespace

bool RuntimeState::AllowsAgentOperation(const RunExecution& run, Capability capability) const {
  for (auto* current = &run; current; current = Parent(*this, *current)) {
    const auto& capabilities = agents.at(current->view.agent.id).capabilities;
    if (std::ranges::find(capabilities, capability) == capabilities.end()) return false;
  }
  return true;
}

bool RuntimeState::AllowsRunTool(const RunExecution& run, const ToolRef& tool) const {
  const auto* scope = Scope(run);
  if (scope && std::ranges::find(scope->tools, tool.id) == scope->tools.end()) return false;
  if (!run.delegate) return true;
  for (auto* current = &run; current; current = Parent(*this, *current)) {
    const auto binding = FindAgentTool(agents.at(current->view.agent.id), tool.id);
    if (!binding || binding->ref != tool) return false;
  }
  return true;
}

Async<std::vector<ResourceAccess>> RuntimeState::ResolveRunToolResources(const RunExecution& run, ToolInvocation invocation) {
  auto granted = co_await options.tool_resources(invocation);
  if (!granted.Succeeded()) co_return granted;
  for (auto* current = Parent(*this, run); current; current = Parent(*this, *current)) {
    const auto& session = sessions.at(run.view.session);
    invocation.actor = Request(current->principal, Capability::PrepareTool, &session, current).actor;
    invocation.actor.call = invocation.call;
    invocation.run = current->view.id;
    auto inherited = co_await options.tool_resources(invocation);
    if (!inherited.Succeeded()) co_return inherited;
    std::erase_if(granted.Value(), [&](const auto& grant) {
      return std::ranges::none_of(inherited.Value(), [&](const auto& permission) {
        return grant.resource == permission.resource && grant.current == permission.current &&
            (grant.mode == ResourceAccessMode::Read || permission.mode == ResourceAccessMode::Write);
      });
    });
  }
  co_return granted;
}

void RuntimeState::ConstrainDelegationBudget(const RunExecution& run, Budget& budget) const {
  if (!run.delegate && run.delegations.empty()) return;
  const auto& session = sessions.at(run.view.session);
  std::map<RunId, Usage> used;
  for (auto* current = &run; current; current = Parent(*this, *current)) used.emplace(current->view.id, Usage{});
  for (const auto& id : session.runs) {
    if (id == run.view.id) continue;
    const auto& other = runs.at(id);
    for (auto* current = &other; current; current = Parent(*this, *current)) {
      auto entry = used.find(current->view.id);
      if (entry == used.end()) continue;
      auto& sum = entry->second;
      const auto& usage = other.view.usage;
      sum.input_tokens = SatAdd(sum.input_tokens, usage.input_tokens);
      sum.output_tokens = SatAdd(sum.output_tokens, usage.output_tokens);
      sum.model_turns = SatAdd(sum.model_turns, usage.model_turns);
      sum.tool_calls = SatAdd(sum.tool_calls, usage.tool_calls);
    }
  }
  for (auto* current = &run; current; current = Parent(*this, *current)) {
    auto cap = agents.at(current->view.agent.id).budget;
    const auto& sum = used.at(current->view.id);
    const auto tokens = SatAdd(sum.input_tokens, sum.output_tokens);
    Limit(budget.tokens, cap.tokens, tokens);
    Limit(budget.model_turns, cap.model_turns.value_or(32), sum.model_turns);
    Limit(budget.tool_calls, cap.tool_calls.value_or(128), sum.tool_calls);
    Limit(budget.context_bytes, cap.context_bytes.value_or(4 << 20));
    const Budget* allocation = current->delegate ? &current->delegate->budget : nullptr;
    if (session.fork_branch && session.fork_branch->run == current->view.id) allocation = &session.fork_branch->budget;
    if (!allocation) continue;
    Limit(budget.tokens, allocation->tokens, tokens);
    Limit(budget.model_turns, allocation->model_turns, sum.model_turns);
    Limit(budget.tool_calls, allocation->tool_calls, sum.tool_calls);
    Limit(budget.context_bytes, allocation->context_bytes);
  }
}

void SetObservedRunState(RunSnapshot& run, RunState state, TimePoint now) {
  if (auto& timing = run.timing) {
    if (state != RunState::WaitingForInput && timing->input_wait_started_at) {
      timing->waiting_for_input += std::max(Duration::zero(), now - *timing->input_wait_started_at);
      timing->input_wait_started_at.reset();
    } else if (state == RunState::WaitingForInput && !timing->input_wait_started_at) {
      timing->input_wait_started_at = now;
    }
    if (Terminal(state)) timing->ended_at = std::max(now, timing->started_at);
  }
  if (Terminal(state)) run.model_active = false;
  run.state = state;
}

void RuntimeState::UpdateRunState(SessionData& session, RunExecution& run, RunState state) {
  SetObservedRunState(run.view, state, Now());
  if (session.summary.active_run == run.view.id) session.summary.run_state = state;
}

bool RuntimeState::HasActiveDelegates(const RunExecution& run) const {
  return std::ranges::any_of(run.delegations, [&](const auto& batch) {
    return std::ranges::any_of(batch.second, [&](const auto& id) { return !Terminal(runs.at(id).view.state); });
  });
}

void RuntimeState::StopDelegates(const RunExecution& run) {
  for (const auto& [command, children] : run.delegations) {
    for (const auto& id : children) {
      auto& child = runs.at(id);
      if (!Terminal(child.view.state) && child.view.state != RunState::Cancelling) Stop(child, RunEndReason::ParentCancelled);
    }
  }
}

void RuntimeState::ScheduleDelegates(SessionData& session) {
  if (stage != Stage::Started || !session.summary.active_run) return;
  auto occupied = std::ranges::count_if(session.runs, [&](const auto& id) {
    const auto& run = runs.at(id);
    return run.delegate_dispatched && !Terminal(run.view.state) &&
        (!run.waiting_for_delegates || !HasActiveDelegates(run));
  });
  std::vector<RunId> ready;
  for (const auto& id : session.runs) {
    if (static_cast<std::size_t>(occupied) >= options.max_parallel_delegates) break;
    auto& child = runs.at(id);
    if (!child.delegate || child.delegate_dispatched || child.stop.stop_requested() || Terminal(child.view.state)) continue;
    const auto& parent = runs.at(*child.view.parent);
    if (!parent.waiting_for_delegates || parent.stop.stop_requested()) continue;
    const auto& batch = parent.delegations.at(*parent.waiting_for_delegates);
    if (std::ranges::find(batch, id) == batch.end()) continue;
    child.delegate_dispatched = true;
    if (!child.delegate->started) {
      child.delegate->started = true;
      child.view.timing = RunTiming{.started_at = Now()};
      UpdateRunState(session, child, RunState::Running);
    }
    ready.push_back(id);
    ++occupied;
  }
  if (ready.empty()) return;
  ++session.sequence;
  auto saved = Persist(session);
  options.tasks.Launch([self = shared_from_this(), ready = std::move(ready), saved]() -> Task<void> {
    auto committed = co_await WhenSaved(saved);
    for (const auto& id : ready) {
      auto& child = self->runs.at(id);
      if (self->stage != Stage::Started || child.stop.stop_requested()) continue;
      if (committed.Succeeded()) self->Launch(child);
      else self->Finish(child, RunState::Failed, RunEndReason::Interrupted, committed.Error());
    }
  });
}

Async<void> RuntimeState::CheckDelegation(const RunExecution& parent, const DelegateRequest& request) {
  if (stage != Stage::Started || parent.stop.stop_requested() || Terminal(parent.view.state)) {
    co_return Fail<void>(ErrorCode::AlreadyClosed, "The delegating run has ended");
  }
  const auto& session = sessions.at(parent.view.session);
  const auto& definition = agents.at(parent.view.agent.id);
  if (std::ranges::find(definition.delegates, request.agent) == definition.delegates.end()) {
    co_return Fail<void>(ErrorCode::PermissionDenied, "The target agent is outside the delegation allowlist");
  }
  std::size_t depth = 1;
  for (auto* current = &parent; current; current = Parent(*this, *current), ++depth) {
    if (current->stop.stop_requested()) co_return Fail<void>(ErrorCode::AlreadyClosed, "An ancestor run has ended");
    const auto* scope = Scope(*current);
    if ((scope && !InScope(scope->inputs, request.input.context)) ||
        (current->delegate && !InScope(current->input.context, request.input.context))) {
      co_return Fail<void>(ErrorCode::PermissionDenied, "The delegated sources are outside the parent scope");
    }
  }
  if (depth - 1 > options.max_delegation_depth || !options.max_parallel_delegates) {
    co_return Fail<void>(ErrorCode::BudgetExceeded, "Run reached its delegation limit");
  }
  if (parent.deadline && *parent.deadline <= Now()) co_return Fail<void>(ErrorCode::DeadlineExceeded, "The delegating run expired");
  auto access = Request(parent.principal, Capability::DelegateRun, &session, &parent);
  access.new_agent = request.agent;
  access.sources = request.input.context;
  auto allowed = co_await Require(access);
  if (!allowed.Succeeded()) co_return allowed;
  co_return co_await ValidateInputSources(session, std::move(access.actor), request.input.context);
}

Async<std::vector<DelegateOutcome>> RuntimeState::Delegate(RunId id, CommandId command, std::vector<DelegateRequest> requests) {
  using Result = AgentResult<std::vector<DelegateOutcome>>;
  auto& parent = runs.at(id);
  auto& session = sessions.at(parent.view.session);
  if (command.value.empty() || requests.empty() || requests.size() > MaxBatchSize) {
    co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::InvalidArgument, "A delegation command requires 1 to 32 requests");
  }
  if (parent.waiting_for_delegates) co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::Busy, "A delegation is already being awaited");
  const auto access_at = access_revision;
  for (const auto& request : requests) {
    auto valid = CheckInput(request.input);
    if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
    if (request.budget.deadline && *request.budget.deadline <= Duration::zero()) {
      co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::InvalidArgument, "A child deadline must be positive");
    }
    if (request.expected_result) {
      valid = ValidateJson(request.expected_result->document, JsonSchema{JsonValue{true}});
      if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
    }
    auto allowed = co_await CheckDelegation(parent, request);
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
  }
  if (access_at != access_revision) co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::Conflict, "Delegation access changed");
  if (parent.stop.stop_requested()) co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::AlreadyClosed, "The delegating run has ended");
  if (parent.waiting_for_delegates) co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::Busy, "A delegation is already being awaited");

  auto prior = parent.delegations.find(command);
  SessionData* goal_owner = nullptr;
  if (prior != parent.delegations.end()) {
    if (prior->second.size() != requests.size()) co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::Conflict, "The delegation command changed");
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const auto& child = runs.at(prior->second[i]);
      if (requests[i] != DelegateRequest{child.view.agent.id, child.input, child.delegate->budget, child.delegate->expected_result}) {
        co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::Conflict, "The delegation command changed");
      }
    }
  } else {
    auto& batch = parent.delegations[command];
    for (auto& request : requests) {
      const auto child_id = RunId{MakeId()};
      const auto& agent = agents.at(request.agent);
      auto deadline = parent.deadline;
      const auto tighten = [&](std::optional<Duration> duration) {
        if (!duration) return;
        const auto now = Now();
        const auto at = now + std::min(*duration, TimePoint::max() - now);
        if (!deadline || at < *deadline) deadline = at;
      };
      tighten(agent.budget.deadline);
      tighten(request.budget.deadline);
      std::vector<ModelItem> items{ModelContent{ModelContent::Role::User, request.input.text}};
      RunSnapshot view{.id = child_id, .session = session.summary.id, .agent = agent.ref, .model = models.at(agent.model).ref,
          .trigger = RunTrigger::Delegation, .parent = id, .goal = parent.view.goal, .state = RunState::WaitingForDependency, .usage = {}};
      auto& child = runs.emplace(child_id, RunExecution{.view = std::move(view), .principal = parent.principal, .input = std::move(request.input),
          .items = std::move(items), .model_boundary = true, .deadline = deadline,
          .delegate = DelegatedRun{std::move(request.budget), std::move(request.expected_result)}}).first->second;
      if (parent.view.goal) goal_owner = AttachGoalRun(child, parent.view.goal);
      batch.push_back(child_id);
      session.runs.push_back(child_id);
    }
    prior = parent.delegations.find(command);
  }

  parent.waiting_for_delegates = command;
  const auto clear_wait = std::unique_ptr<RunExecution, void(*)(RunExecution*)>(&parent,
      [](RunExecution* run) { run->waiting_for_delegates.reset(); run->delegate_waiter.reset(); });
  const auto matches = [&](const RunWait& wait) {
    const auto* batch = std::get_if<CommandId>(&wait.subject);
    return wait.kind == WaitKind::Delegation && batch && *batch == command;
  };
  if (std::ranges::none_of(parent.view.waits, matches)) parent.view.waits.push_back({WaitKind::Delegation, command, parent.deadline});
  UpdateRunState(session, parent, RunState::WaitingForDependency);
  ++session.sequence;
  auto committed = co_await WhenSaved(Persist(session, false, {}, goal_owner));
  if (!committed.Succeeded()) co_return Result::Failure(committed.Error());

  for (const auto& child_id : prior->second) {
    auto& child = runs.at(child_id);
    if (!child.deadline || child.delegate_dispatched || child.stop.stop_requested() || Terminal(child.view.state)) continue;
    child.timer = options.tasks.Launch([self = shared_from_this(), child_id, remaining = *child.deadline - Now()] {
      return self->RunDeadline(child_id, remaining);
    });
  }
  const auto done = [&] {
    return std::ranges::all_of(prior->second, [&](const auto& child) { return Terminal(runs.at(child).view.state); });
  };
  while (!done() && !parent.stop.stop_requested()) {
    ScheduleDelegates(session);
    co_await AwaitCallback<bool>([this, id, &done](CallbackCompletion<bool> complete) {
      auto& run = runs.at(id);
      if (done() || run.stop.stop_requested()) complete.Complete(true);
      else run.delegate_waiter = std::move(complete);
      return [weak = weak_from_this(), id] {
        if (auto state = weak.lock()) state->runs.at(id).delegate_waiter.reset();
      };
    });
  }
  if (parent.stop.stop_requested()) co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::AlreadyClosed, "The delegating run has ended");
  std::erase_if(parent.view.waits, matches);
  UpdateRunState(session, parent, RunState::Running);
  ++session.sequence;
  committed = co_await WhenSaved(Persist(session));
  if (!committed.Succeeded()) co_return Result::Failure(committed.Error());

  const auto delivery_epoch = access_revision;
  std::vector<DelegateOutcome> outcomes;
  for (const auto& child_id : prior->second) {
    const auto& child = runs.at(child_id).view;
    auto result = child.result;
    auto policy = Request(parent.principal, Capability::WaitForResult, &session, &parent);
    policy.target.run = child_id;
    policy.new_agent = child.agent.id;
    auto allowed = co_await Require(std::move(policy));
    if (!allowed.Succeeded()) co_return Result::Failure(allowed.Error());
    if (result) {
      auto actor = Request(parent.principal, Capability::ReadArtifact, &session, &parent).actor;
      std::vector<ArtifactRef> visible;
      for (const auto& ref : result->artifacts) {
        auto readable = co_await ReadArtifact(ref, actor);
        if (readable.Succeeded()) visible.push_back(ref);
      }
      result->artifacts = std::move(visible);
    }
    if (delivery_epoch != access_revision || parent.stop.stop_requested()) {
      co_return Fail<std::vector<DelegateOutcome>>(ErrorCode::Conflict, "Delegation access changed before result delivery");
    }
    outcomes.push_back({child_id, child.state, result, result ? result->structured : std::nullopt, child.failure});
  }
  co_return Result::Success(std::move(outcomes));
}

std::optional<ModelToolSpec> RuntimeState::DelegationTool(const RunExecution& run) const {
  if (!options.max_delegation_depth || !options.max_parallel_delegates) return {};
  const auto& delegates = agents.at(run.view.agent.id).delegates;
  if (delegates.empty()) return {};
  auto schema = nlohmann::json::parse(R"({"type":"object","required":["requests"],"additionalProperties":false,
    "properties":{"requests":{"type":"array","minItems":1,"maxItems":32,"items":{
      "type":"object","required":["agent","text"],"additionalProperties":false,"properties":{
        "agent":{"type":"string"},"text":{"type":"string"},"context":{"type":"array","items":{"type":"integer","minimum":0}}
      }}}}})");
  auto& allowed = schema["properties"]["requests"]["items"]["properties"]["agent"]["enum"];
  for (const auto& id : delegates) allowed.push_back(id.value);
  auto description = std::string("Delegate tasks in parallel and receive their ordered results. Each task receives its text and only the explicitly selected sources.");
  if (!run.input.context.empty()) description += " Use zero-based context indices from the current task's " +
      std::to_string(run.input.context.size()) + " source references.";
  return ModelToolSpec{{DelegationToolId, 1}, DelegationToolId.value, std::move(description), {FromWireJson(schema)}};
}

Async<JsonValue> RuntimeState::ExecuteDelegationTool(RunId id, CommandId command, const JsonValue& arguments) {
  const auto& run = runs.at(id);
  const auto tool = DelegationTool(run);
  if (!tool) co_return Fail<JsonValue>(ErrorCode::PermissionDenied, "Delegation is unavailable");
  auto valid = ValidateJson(arguments, tool->parameters);
  if (!valid.Succeeded()) co_return AgentResult<JsonValue>::Failure(valid.Error());
  const auto wire = ToWireJson(arguments);
  std::vector<DelegateRequest> requests;
  for (const auto& item : wire.at("requests")) {
    SendInput input{.text = item.at("text").get<std::string>()};
    if (auto context = item.find("context"); context != item.end()) {
      for (const auto& index : *context) {
        auto at = index.get<std::uint64_t>();
        if (at >= run.input.context.size()) co_return Fail<JsonValue>(ErrorCode::InvalidArgument, "Delegation context index is unavailable");
        input.context.push_back(run.input.context[static_cast<std::size_t>(at)]);
      }
    }
    requests.push_back({AgentId{item.at("agent").get<std::string>()}, std::move(input)});
  }
  auto result = co_await Delegate(id, std::move(command), std::move(requests));
  if (!result.Succeeded()) co_return AgentResult<JsonValue>::Failure(result.Error());
  auto children = nlohmann::json::array();
  for (const auto& child : result.Value()) {
    auto value = nlohmann::json{{"run", child.run.value}, {"state", child.state == RunState::Succeeded ? "succeeded" :
        child.state == RunState::Cancelled ? "cancelled" : "failed"}};
    if (child.result) {
      const auto& session = sessions.at(run.view.session);
      auto outbound = Request(run.principal, Capability::SendToModel, &session, &run);
      outbound.target.run = child.run;
      outbound.recipient_model = run.view.model.id;
      for (const auto& ref : child.result->artifacts) outbound.sources.push_back({ref});
      auto allowed = co_await Require(std::move(outbound));
      if (!allowed.Succeeded()) co_return AgentResult<JsonValue>::Failure(allowed.Error());
      value["summary"] = child.result->summary;
      if (child.structured) value["structured"] = ToWireJson(*child.structured);
      for (const auto& ref : child.result->artifacts) value["artifacts"].push_back({{"id", ref.id.value}, {"version", ref.version}});
    }
    if (child.failure) value["error"] = child.failure->message;
    children.push_back(std::move(value));
  }
  co_return AgentResult<JsonValue>::Success(FromWireJson(children));
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

Async<std::vector<DelegateOutcome>> RunContext::DelegateAsync(std::vector<DelegateRequest> requests) {
  const auto& execution = runtime_->runs.at(run_);
  if (execution.restoring && !execution.waiting_for_delegates) {
    for (const auto& wait : execution.view.waits) {
      if (const auto* command = std::get_if<CommandId>(&wait.subject); wait.kind == WaitKind::Delegation && command)
        return DelegateAsync(*command, std::move(requests));
    }
  }
  return DelegateAsync(NewCommandId(), std::move(requests));
}

Async<std::vector<DelegateOutcome>> RunContext::DelegateAsync(CommandId command, std::vector<DelegateRequest> requests) {
  return [](RunContext context, CommandId id, std::vector<DelegateRequest> batch) -> Async<std::vector<DelegateOutcome>> {
    co_return co_await context.runtime_->Delegate(context.run_, std::move(id), std::move(batch));
  }(*this, std::move(command), std::move(requests));
}

} // namespace huxerui::agentkit
