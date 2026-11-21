#include "sqlite_session_store.h"

#include <huxerui/agentkit/runtime.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace huxerui;
using namespace huxerui::agentkit;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(AgentResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void Take(AgentResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

template<class T>
bool FailedWith(const AgentResult<T>& result, ErrorCode code) {
  return !result.Succeeded() && result.Error().code == code;
}

class Stream final : public ModelTurnStream {
public:
  explicit Stream(std::vector<ModelEvent> events) : events_(std::move(events)) {}

  Async<std::optional<ModelEvent>> ReadAsync() override {
    co_await Delay(2ms);
    if (events_.empty()) co_return AgentResult<std::optional<ModelEvent>>::Success(std::nullopt);
    auto event = std::move(events_.front());
    events_.erase(events_.begin());
    co_return AgentResult<std::optional<ModelEvent>>::Success(std::move(event));
  }

  Async<ModelStopOutcome> StopAsync() override {
    co_return AgentResult<ModelStopOutcome>::Success(ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<ModelEvent> events_;
};

struct Probe {
  int admitted = 0;
  int active_experts = 0;
  int peak = 0;
  int writes = 0;
  int child_model_turns = 0;
  int resumed_parent = 0;
  int resumed_expert = 0;
  bool block_delegation = false;
  bool parent_denies_tool = false;
  bool ask = false;
  bool checked_model_result = false;
  bool lose_tool_result = false;
  std::vector<RunId> completed;
};

class Model final : public ModelProvider {
public:
  explicit Model(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}

  ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true, .structured_output = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    std::vector<ModelEvent> events;
    if (request.instructions == "Coordinator") {
      const bool recover = std::ranges::any_of(request.items, [](const auto& item) {
        const auto* content = std::get_if<ModelContent>(&item);
        const auto* text = content ? std::get_if<std::string>(&content->body) : nullptr;
        return text && *text == "recover model panel";
      });
      const auto result = std::ranges::find_if(request.items, [](const auto& item) { return std::holds_alternative<ModelToolResult>(item); });
      if (result == request.items.end()) {
        Check(std::ranges::any_of(request.tools, [](const auto& tool) { return tool.name == "agentkit_delegate"; }),
            "model did not receive the delegation tool");
        const auto arguments = recover ? R"({"requests":[{"agent":"expert","text":"write"},{"agent":"expert","text":"model"}]})" :
            R"({"requests":[{"agent":"expert","text":"slow"},{"agent":"expert","text":"fast-failure"}]})";
        events = {ModelToolStarted{"experts", "agentkit_delegate"}, ModelToolArguments{"experts", arguments},
            ModelToolFinished{"experts"}, ModelFinished{ModelFinishReason::ToolCalls}};
      } else {
        const auto& response = std::get<ModelToolResult>(*result);
        Check(response.provider_call_id == "experts" && response.value && !response.error, "delegation tool did not return to its call");
        const auto& children = std::get<JsonValue::Array>(response.value->value);
        Check(children.size() == 2, "model did not receive both experts");
        const auto& first = std::get<JsonValue::Object>(children[0].value);
        const auto& second = std::get<JsonValue::Object>(children[1].value);
        Check(first.at("state") == JsonValue{std::string("succeeded")} &&
              second.at("state") == JsonValue{std::string(recover ? "succeeded" : "failed")}, "model results changed request order");
        probe_->checked_model_result = true;
        events = {ModelTextDelta{"Panel synthesized"}, ModelFinished{ModelFinishReason::Completed}};
      }
    } else {
      ++probe_->child_model_turns;
      for (const auto& item : request.items) {
        const auto* content = std::get_if<ModelContent>(&item);
        const auto* text = content ? std::get_if<std::string>(&content->body) : nullptr;
        Check(!text || text->find("private parent prompt") == std::string::npos, "child received parent history");
      }
      events = {ModelTextDelta{"Expert model response"}, ModelUsage{{2, 3, 0, 0}}, ModelFinished{ModelFinishReason::Completed}};
    }
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<Stream>(std::move(events)));
  }

private:
  std::shared_ptr<Probe> probe_;
};

const ToolId shared_tool{"test.shared"};
const ToolId private_tool{"test.private"};
const CommandId batch{"experts"};
const CommandId effect{"expert-effect"};

DelegateRequest Expert(std::string input, Budget budget = {}) {
  return {{"expert"}, {.text = std::move(input)}, budget};
}

Async<RunCompletion> CompletePanel(RunContext context, std::string scenario) {
  std::vector<DelegateRequest> requests;
  if (scenario == "nested" || scenario == "depth") requests.push_back({{"middle"}, {.text = "send to leaf"}});
  else if (scenario == "budget") {
    requests = {Expert("write"), Expert("budget exhausted")};
  } else if (scenario == "cancel") {
    requests = {Expert("cancel pending"), Expert("cancel pending"), Expert("cancel pending")};
  } else if (scenario == "model budget") {
    requests = {Expert("model", {.model_turns = 0}), Expert("model"), Expert("model")};
  } else if (scenario == "token budget") {
    requests = {Expert("model", {.tokens = 4})};
  } else if (scenario == "deadline") {
    requests = {Expert("slow"), Expert("model", {.deadline = 20ms})};
  } else if (scenario == "structured") {
    auto matching = Expert("structured");
    matching.expected_result = JsonSchema{JsonValue::Object{{"type", JsonValue{std::string("boolean")}}}};
    auto invalid = Expert("slow");
    invalid.expected_result = matching.expected_result;
    requests = {std::move(matching), std::move(invalid)};
  } else if (scenario == "revoke") {
    requests = {Expert("write")};
  } else {
    requests = {Expert("write"), Expert("model")};
  }
  auto outcomes = co_await context.DelegateAsync(batch, requests);
  if (!outcomes.Succeeded()) co_return AgentResult<RunCompletion>::Failure(outcomes.Error());
  if (scenario != "revoke") {
    auto replayed = Take(co_await context.DelegateAsync(batch, requests));
    Check(replayed.size() == outcomes.Value().size(), "delegation replay changed batch size");
    for (std::size_t index = 0; index < replayed.size(); ++index)
      Check(replayed[index].run == outcomes.Value()[index].run, "delegation replay created a child");
    requests.front().input.text += " changed";
    Check(FailedWith(co_await context.DelegateAsync(batch, requests), ErrorCode::Conflict), "changed command was accepted");
  }
  JsonValue::Array results;
  for (const auto& child : outcomes.Value()) {
    results.push_back({child.state == RunState::Succeeded ? std::string("okay") :
        child.failure ? child.failure->message : std::string("cancelled")});
  }
  co_return AgentResult<RunCompletion>::Success({"Panel completed", JsonValue{std::move(results)}, {}});
}

Async<RunCompletion> Write(RunContext context) {
  auto blocked = co_await context.CallToolAsync({private_tool, JsonValue::Object{}});
  Check(FailedWith(blocked, ErrorCode::PermissionDenied), "child used a tool unavailable to the parent");
  auto started = co_await context.StartToolAsync(effect, {shared_tool, JsonValue::Object{}});
  if (!started.Succeeded()) co_return AgentResult<RunCompletion>::Failure(started.Error());
  auto result = co_await context.WaitForToolAsync(std::get<ToolCallAccepted>(started.Value().outcome).call);
  if (!result.Succeeded()) co_return AgentResult<RunCompletion>::Failure(result.Error());
  if (result.Value().reason) co_return AgentResult<RunCompletion>::Failure(*result.Value().reason);
  co_return AgentResult<RunCompletion>::Success({"Confirmed expert write"});
}

std::shared_ptr<AgentRuntime> Register(std::shared_ptr<Probe> probe, std::size_t parallel,
                                       std::shared_ptr<SessionStore> store = {}, std::size_t depth = 3) {
  auto runtime = Take(AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"operator"}); },
      .access = [probe](PolicyRequest request) -> Async<PolicyDecision> {
        auto decision = AccessDecision::Allow;
        if (request.capability == Capability::DelegateRun && probe->block_delegation) decision = AccessDecision::Deny;
        if (request.capability == Capability::ExecuteTool && request.tool && request.tool->id == shared_tool) {
          const bool parent = request.actor.agent &&
              (request.actor.agent->id.value == "parent" || request.actor.agent->id.value == "model-parent");
          if (parent && probe->parent_denies_tool) decision = AccessDecision::Deny;
          else if (parent && probe->ask) decision = AccessDecision::RequireConfirmation;
        }
        co_return AgentResult<PolicyDecision>::Success({decision, "Test policy"});
      }, .store = std::move(store), .max_delegation_depth = depth, .max_parallel_delegates = parallel}));
  Take(runtime->RegisterModel({.ref = {{"model"}, 1}, .endpoint = {"test", "delegation", "script", {}},
      .provider = std::make_shared<Model>(probe)}));
  for (const auto& id : {shared_tool, private_tool}) {
    Take(runtime->RegisterTool({.id = id, .version = 1, .description = "Test tool", .input_schema = {JsonValue{true}},
        .effect = probe->lose_tool_result ? ToolEffect::ExternalSubmit : ToolEffect::Read,
        .prepare = [](ToolPrepareContext, ToolRequest) -> Async<PreparedChange> {
          co_return AgentResult<PreparedChange>::Success({.preview = {.title = "Expert work"}});
        }, .execute = [probe](ToolContext, PreparedToolCall) -> Async<ToolExecutionResult> {
          ++probe->writes;
          if (probe->lose_tool_result)
            co_return AgentResult<ToolExecutionResult>::Failure({ErrorCode::ProviderFailure, "Tool result was lost after submission"});
          co_return AgentResult<ToolExecutionResult>::Success({.outcome = ToolOutcome::Succeeded});
        }}));
  }
  Take(runtime->RegisterAgent({.definition = {.ref = {{"model-parent"}, 1}, .display_name = "Model coordinator",
      .instructions = "Coordinator", .model = {"model"}, .tools = {shared_tool}, .delegates = {{"expert"}}}}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"parent"}, 1}, .display_name = "Coordinator", .model = {"model"},
      .tools = {shared_tool}, .delegates = {{"expert"}, {"middle"}}, .budget = {.model_turns = 1, .tool_calls = 1}},
      .run = [](RunContext context, RunInput input) -> Async<RunCompletion> {
        Take(co_await context.SaveCheckpointAsync({"panel", 1, {}}));
        co_return co_await CompletePanel(context, input.input.text);
      },
      .restore = [probe](RunContext context, RunInput input, RunResume resume) -> Async<RunCompletion> {
        ++probe->resumed_parent;
        Check(resume.checkpoint->stage == "panel" && std::ranges::any_of(resume.waits, [](const auto& wait) {
          return wait.kind == WaitKind::Delegation && std::get<CommandId>(wait.subject) == batch;
        }), "parent did not recover the original delegation wait");
        co_return co_await CompletePanel(context, input.input.text);
      }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"middle"}, 1}, .display_name = "Lead expert", .model = {"model"},
      .tools = {shared_tool}, .delegates = {{"expert"}}},
      .run = [](RunContext context, RunInput) -> Async<RunCompletion> {
        std::vector<DelegateRequest> requests{Expert("model")};
        auto children = co_await context.DelegateAsync(std::move(requests));
        if (!children.Succeeded()) co_return AgentResult<RunCompletion>::Failure(children.Error());
        Check(children.Value().size() == 1 && children.Value()[0].state == RunState::Succeeded, "nested delegate did not finish");
        co_return AgentResult<RunCompletion>::Success({"Nested expert completed"});
      }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"expert"}, 1}, .display_name = "Expert", .model = {"model"},
      .tools = {shared_tool, private_tool}},
      .run = [probe](RunContext context, RunInput input) -> Async<RunCompletion> {
        ++probe->admitted;
        if (input.input.text == "structured") co_return AgentResult<RunCompletion>::Success({"Structured result", JsonValue{true}});
        if (input.input.text == "model") {
          auto output = co_await context.CompleteAsync({.instruction = "Inspect the case"});
          if (!output.Succeeded()) co_return AgentResult<RunCompletion>::Failure(output.Error());
          co_return AgentResult<RunCompletion>::Success({std::move(output).Value().text});
        }
        if (input.input.text == "cancel pending") {
          FormRequest request{.title = "Expert question", .schema = {JsonValue{true}}};
          static_cast<void>(co_await context.AskAsync(std::move(request)));
          co_return AgentResult<RunCompletion>::Success({"Expert question ended"});
        }
        if (input.input.text == "slow" || input.input.text == "fast-failure") {
          ++probe->active_experts;
          probe->peak = std::max(probe->peak, probe->active_experts);
          if (input.input.text == "slow") co_await Delay(85ms);
          else co_await Delay(5ms);
          --probe->active_experts;
          probe->completed.push_back(context.Id());
          if (input.input.text == "fast-failure")
            co_return AgentResult<RunCompletion>::Failure({ErrorCode::InvalidState, "Expert failure"});
          co_return AgentResult<RunCompletion>::Success({"First expert"});
        }
        Take(co_await context.SaveCheckpointAsync({"effect", 1, {}}));
        co_return co_await Write(context);
      },
      .restore = [probe](RunContext context, RunInput, RunResume resume) -> Async<RunCompletion> {
        Check(resume.checkpoint->stage == "effect", "expert checkpoint was lost");
        ++probe->resumed_expert;
        co_return co_await Write(context);
      }}));
  return runtime;
}

Async<std::shared_ptr<Conversation>> Start(const std::shared_ptr<AgentRuntime>& runtime, std::string text,
                                           std::string agent = "parent", SessionRetention retention = SessionRetention::Ephemeral) {
  Take(co_await runtime->StartAsync());
  auto created = Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), {
      .options = {.agent = {std::move(agent)}, .retention = retention}, .initial = SendInput{.text = std::move(text)}}));
  co_return co_await runtime->Sessions().OpenAsync(std::get<SessionCreated>(created.outcome).session);
}

Task<std::shared_ptr<const SessionSnapshot>> Wait(std::shared_ptr<Conversation> session, auto condition) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    auto snapshot = session->Snapshot();
    if (snapshot && condition(*snapshot)) co_return snapshot;
    co_await Delay(4ms);
  }
  throw std::runtime_error("Delegation did not reach its expected checkpoint");
}

bool Idle(const SessionSnapshot& snapshot) { return !snapshot.summary.active_run; }

Task<void> VerifyModelPanel() {
  auto probe = std::make_shared<Probe>();
  auto runtime = Register(probe, 2);
  auto session = Take(co_await Start(runtime, "private parent prompt", "model-parent"));
  auto result = co_await Wait(session, Idle);
  Check(result->runs.size() == 3 && result->runs[0].state == RunState::Succeeded && probe->checked_model_result,
      "default model did not complete the expert panel");
  Check(probe->peak == 2 && probe->completed == std::vector{result->runs[2].id, result->runs[1].id},
      "experts did not overlap or completed in unexpected order");
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifyBudgetsAndScope() {
  for (const std::string scenario : {"nested", "depth", "budget", "model budget", "token budget", "deadline", "structured", "revoke", "denied"}) {
    auto probe = std::make_shared<Probe>();
    probe->parent_denies_tool = scenario == "revoke";
    probe->block_delegation = scenario == "denied";
    auto runtime = Register(probe, 1, {}, scenario == "depth" ? 1 : 3);
    auto session = Take(co_await Start(runtime, scenario == "denied" ? "revoke" : scenario));
    auto result = co_await Wait(session, Idle);
    const auto& runs = result->runs;
    if (scenario == "denied") {
      Check(runs.size() == 1 && runs[0].failure && runs[0].failure->code == ErrorCode::PermissionDenied,
          "denied delegation created a child");
    } else {
      Check(!runs.empty() && runs[0].state == RunState::Succeeded, "coordinator did not finish");
      if (scenario == "nested") {
        Check(runs.size() == 3 && runs[1].parent == runs[0].id && runs[2].parent == runs[1].id &&
            probe->child_model_turns == 1, "nested delegation deadlocked on the single slot");
      } else if (scenario == "depth") {
        Check(runs.size() == 2 && runs[1].end_reason == RunEndReason::Budget, "delegation exceeded its depth limit");
      } else if (scenario == "model budget") {
        Check(runs.size() == 4 && runs[1].end_reason == RunEndReason::Budget && runs[2].state == RunState::Succeeded &&
            runs[3].end_reason == RunEndReason::Budget && probe->child_model_turns == 1, "model budget was not shared");
      } else if (scenario == "token budget") {
        Check(runs.size() == 2 && runs[1].end_reason == RunEndReason::Budget && probe->child_model_turns == 1,
            "reported tokens exceeded the child allocation without failing");
      } else if (scenario == "deadline") {
        Check(runs.size() == 3 && runs[2].end_reason == RunEndReason::Deadline && probe->admitted == 1,
            "queued child executed after its deadline");
      } else if (scenario == "structured") {
        Check(runs.size() == 3 && runs[1].result && runs[1].result->structured == JsonValue{true} &&
            runs[2].state == RunState::Failed, "structured results bypassed the expected schema");
      } else if (scenario == "budget") {
        Check(runs.size() == 3 && runs[1].state == RunState::Succeeded && runs[2].end_reason == RunEndReason::Budget &&
            probe->writes == 1, "tool budget was not shared");
      } else {
        Check(runs.size() == 2 && runs[1].state == RunState::Failed && probe->writes == 0, "parent tool denial was bypassed");
      }
    }
    Take(co_await runtime->CloseAsync());
  }
}

std::vector<InteractionSnapshot> PendingInteractions(const SessionSnapshot& snapshot) {
  std::vector<InteractionSnapshot> pending;
  std::ranges::copy_if(snapshot.interactions, std::back_inserter(pending),
      [](const auto& item) { return item.state == huxerui::agentkit::InteractionState::Pending; });
  return pending;
}

Task<void> VerifyCancellation() {
  auto probe = std::make_shared<Probe>();
  probe->ask = true;
  auto runtime = Register(probe, 2);
  auto session = Take(co_await Start(runtime, "cancel"));
  auto waiting = co_await Wait(session, [](const auto& snapshot) { return PendingInteractions(snapshot).size() == 2; });
  Check(waiting->runs.size() == 4 && waiting->summary.active_run == waiting->runs.front().id, "child replaced the root Run");
  Take(co_await session->CancelAsync(NewCommandId(), *waiting->summary.active_run));
  auto stopped = co_await Wait(session, Idle);
  Check(stopped->runs[0].end_reason == RunEndReason::UserCancelled && PendingInteractions(*stopped).empty() && probe->writes == 0,
      "cancelling the parent left a pending effect");
  Check(std::ranges::all_of(stopped->runs | std::views::drop(1), [](const auto& run) {
          return run.end_reason == RunEndReason::ParentCancelled;
        }) && probe->admitted == 2,
      "parent cancellation did not stop active and queued experts");
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifyRevocation() {
  auto probe = std::make_shared<Probe>();
  probe->ask = true;
  auto runtime = Register(probe, 1);
  auto session = Take(co_await Start(runtime, "revoke"));
  auto waiting = co_await Wait(session, [](const auto& snapshot) { return PendingInteractions(snapshot).size() == 1; });
  const auto approval = PendingInteractions(*waiting).front();
  probe->block_delegation = true;
  Take(co_await runtime->RefreshAccessAsync());
  static_cast<void>(co_await session->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve}));
  auto result = co_await Wait(session, Idle);
  Check(result->runs[1].failure && result->runs[1].failure->code == ErrorCode::PermissionDenied &&
        probe->writes == 0, "revoked delegation executed an approved tool");
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifyRetryWithUnknownChildEffect() {
  auto probe = std::make_shared<Probe>();
  probe->lose_tool_result = true;
  auto runtime = Register(probe, 1);
  auto session = Take(co_await Start(runtime, "revoke"));
  const auto original = co_await Wait(session, Idle);
  Check(original->runs.size() == 2 && original->tools.size() == 1 &&
        original->tools.front().state == ToolCallState::ResultUnknown && probe->writes == 1,
        "delegated submission did not leave an unknown effect");
  Check(FailedWith(co_await session->RetryAsync(NewCommandId(), original->runs.front().id), ErrorCode::ResultUnknown),
        "retry accepted the parent of an unreconciled tool");
  Check(session->Snapshot()->runs.size() == 2 && probe->writes == 1, "retry repeated the delegated submission");
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifyRecovery(File file, bool model_driven) {
  auto probe = std::make_shared<Probe>();
  probe->ask = true;
  auto runtime = Register(probe, 1, Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)));
  auto session = Take(co_await Start(runtime, model_driven ? "recover model panel" : "private parent prompt",
      model_driven ? "model-parent" : "parent", SessionRetention::Stored));
  auto waiting = co_await Wait(session, [](const auto& snapshot) { return PendingInteractions(snapshot).size() == 1; });
  const auto approval = PendingInteractions(*waiting).front();
  const auto id = waiting->summary.id;
  Check(waiting->runs.size() == 3 && probe->admitted == 1 && probe->writes == 0, "queued expert started before recovery");
  Take(co_await runtime->CloseAsync());
  session.reset();
  runtime.reset();
  runtime = Register(probe, 1, Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)));
  Take(co_await runtime->StartAsync());
  session = Take(co_await runtime->Sessions().OpenAsync(id));
  auto recovered = co_await Wait(session, [&](const auto& snapshot) {
    return probe->resumed_expert == 1 && PendingInteractions(snapshot).size() == 1;
  });
  Check(recovered->summary.active_run == waiting->summary.active_run && recovered->runs.size() == waiting->runs.size() &&
      PendingInteractions(*recovered).front().id == approval.id && PendingInteractions(*recovered).front().version == approval.version &&
      probe->resumed_parent == (model_driven ? 0 : 1) && probe->writes == 0,
      "recovery changed the parent, queued expert or approval");
  Take(co_await session->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve}));
  auto result = co_await Wait(session, Idle);
  Check(result->runs.size() == 3 && result->runs[0].state == RunState::Succeeded && result->runs[1].state == RunState::Succeeded &&
      result->runs[2].state == RunState::Succeeded && result->runs[2].id == waiting->runs[2].id &&
      probe->writes == 1 && probe->admitted == 2, "recovered batch duplicated work or failed to consume the queued expert");
  if (model_driven) Check(probe->checked_model_result, "model did not receive the restored delegation result");
  Take(co_await runtime->CloseAsync());
}

} // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() / ("agentkit-delegation-" + NewCommandId().value + ".sqlite3");
  bool done = false;
  std::exception_ptr failure;
  Application app{[]() -> View { return Text("AgentKit delegation"); },
      {.application_hooks = {[&](ApplicationContext&) {
        UseApplicationTaskScope().Launch([&]() -> Task<void> {
          try {
            co_await VerifyModelPanel();
            co_await VerifyBudgetsAndScope();
            co_await VerifyCancellation();
            co_await VerifyRevocation();
            co_await VerifyRetryWithUnknownChildEffect();
            co_await VerifyRecovery(File(path.u8string()), false);
            co_await VerifyRecovery(File(path.u8string()), true);
          } catch (...) { failure = std::current_exception(); }
          done = true;
        });
      }}}};
  try {
    testing::UiTest ui(app);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!done && std::chrono::steady_clock::now() < deadline) {
      ui.Pump(2ms);
      std::this_thread::sleep_for(2ms);
    }
    Check(done, "delegation tests timed out");
    if (failure) std::rethrow_exception(failure);
    std::error_code error;
    std::filesystem::remove(path, error);
    std::cout << "AgentKit delegation, parallel experts, budgets, cancellation and SQLite recovery passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
