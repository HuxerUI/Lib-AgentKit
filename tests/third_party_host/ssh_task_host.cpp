#include <huxerui/agentkit.h>
#include <huxerui/huxerui.h>
#include <huxerui/terminal.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {

using namespace huxerui;
using namespace huxerui::agentkit;
using namespace std::chrono_literals;
namespace term = huxerui::terminal;

const ProjectId project{"remote.build"};
const AgentId assistant{"remote.verification"};
const ModelId model_id{"remote.fixture"};
const ToolId ssh_task{"remote.verify_build"};
const ResourceRef destination{{"remote/verification"}, "v1"};
const JsonValue verify_arguments{JsonValue::Object{{"task", JsonValue{std::string("verify-build")}}}};
constexpr auto remote_command = "agentkit-verify-build";

struct SshConfig {
  std::filesystem::path executable;
  std::string configuration;
  std::string host;
};

struct HostState {
  bool authorized = true;
  int launches = 0;
};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(AgentResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

template<class CheckReady>
Task<void> WaitUntil(CheckReady ready, const char* message) {
  const auto end = std::chrono::steady_clock::now() + 18s;
  while (!ready() && std::chrono::steady_clock::now() < end) co_await Delay(20ms);
  Check(ready(), message);
}

class Stream final : public ModelTurnStream {
public:
  explicit Stream(std::vector<ModelEvent> events) : events_(std::move(events)) {}

  Async<std::optional<ModelEvent>> ReadAsync() override {
    if (next_ == events_.size()) co_return AgentResult<std::optional<ModelEvent>>::Success(std::nullopt);
    co_return AgentResult<std::optional<ModelEvent>>::Success(std::move(events_[next_++]));
  }

  Async<ModelStopOutcome> StopAsync() override {
    next_ = events_.size();
    co_return AgentResult<ModelStopOutcome>::Success(ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<ModelEvent> events_;
  std::size_t next_ = 0;
};

class LocalModel final : public ModelProvider {
public:
  ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    const bool answered = std::ranges::any_of(request.items, [](const auto& item) {
      return std::holds_alternative<ModelToolResult>(item);
    });
    std::vector<ModelEvent> events;
    if (answered) {
      events = {ModelTextDelta{"Remote verification result recorded."}, ModelFinished{ModelFinishReason::Completed, {}}};
    } else {
      Check(request.tools.size() == 1 && request.tools.front().tool.id == ssh_task, "The SSH tool was unavailable");
      events = {ModelToolStarted{"verify-build", request.tools.front().name},
          ModelToolArguments{"verify-build", R"({"task":"verify-build"})"},
          ModelToolFinished{"verify-build"}, ModelFinished{ModelFinishReason::ToolCalls, {}}};
    }
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<Stream>(std::move(events)));
  }
};

AgentResult<ToolExecutionResult> SshFailure(ToolOutcome outcome, ErrorCode code, std::string message) {
  return AgentResult<ToolExecutionResult>::Success({
      .outcome = outcome, .reason = AgentError{.code = code, .message = std::move(message)}});
}

Async<ToolExecutionResult> VerifyRemote(ToolContext context, const SshConfig& config, const std::shared_ptr<HostState>& host) {
  if (context.StopToken().stop_requested())
    co_return SshFailure(ToolOutcome::Cancelled, ErrorCode::AlreadyClosed, "Remote verification cancelled before launch");
  ++host->launches;
  term::TerminalShellOptions shell{.executable = config.executable,
      .arguments = {"-F", config.configuration, "-oBatchMode=yes", "-oStrictHostKeyChecking=yes",
          "-oConnectTimeout=6", "-oConnectionAttempts=1", "-oForwardAgent=no", "-oClearAllForwardings=yes",
          "-oPermitLocalCommand=no", "-T", "--", config.host, remote_command}};
  auto opened = co_await term::OpenShellAsync(std::move(shell), {.scrollback_lines = 0});
  if (!opened.Succeeded())
    co_return SshFailure(ToolOutcome::Failed, ErrorCode::ProviderFailure, "Could not start the SSH client: " + opened.Error().message);
  auto process = std::move(opened).Value();
  const auto end = std::chrono::steady_clock::now() + 12s;
  auto state = process.Snapshot();
  while (state.status == term::TerminalStatus::Open) {
    if (context.StopToken().stop_requested() || std::chrono::steady_clock::now() >= end) {
      static_cast<void>(co_await process.CloseAsync());
      co_return SshFailure(ToolOutcome::ResultUnknown, ErrorCode::ResultUnknown,
          "The SSH client stopped before confirming the remote exit status");
    }
    co_await Delay(20ms);
    state = process.Snapshot();
  }
  if (!state.exit || state.exit->reason != term::TerminalExitReason::Exited || !state.exit->code || *state.exit->code == 255)
    co_return SshFailure(ToolOutcome::ResultUnknown, ErrorCode::ResultUnknown, "SSH did not report a verified remote exit status");
  const auto code = *state.exit->code;
  const auto summary = "Remote verification exited with code " + std::to_string(code);
  ToolExecutionResult result{.outcome = code == 0 ? ToolOutcome::Succeeded : ToolOutcome::Failed,
      .model_value = JsonValue{static_cast<std::int64_t>(code)},
      .blocks = {{{NewCommandId().value}, TextBlock{summary, false}}}};
  if (code != 0) result.reason = AgentError{.code = ErrorCode::ProviderFailure, .message = summary};
  co_return AgentResult<ToolExecutionResult>::Success(std::move(result));
}

void RegisterSshTask(const std::shared_ptr<AgentRuntime>& runtime, SshConfig config, const std::shared_ptr<HostState>& host) {
  using Result = AgentResult<PreparedChange>;
  const JsonSchema input{JsonValue{JsonValue::Object{{"enum", JsonValue{JsonValue::Array{verify_arguments}}}}}};
  const auto summary = "Host " + config.host + "\nCommand " + remote_command;
  Take(runtime->RegisterTool({
      .id = ssh_task, .version = 1, .description = "Verify the build on the configured remote host",
      .input_schema = input, .effect = ToolEffect::ExternalSubmit,
      .prepare = [summary](ToolPrepareContext context, ToolRequest request) -> Async<PreparedChange> {
        if (context.readable.size() != 1 || context.readable.front().current != destination || request.arguments != verify_arguments)
          co_return Result::Failure({.code = ErrorCode::PermissionDenied, .message = "Remote verification is outside the host scope"});
        co_return Result::Success({.preview = {.title = "Run remote verification", .summary = summary},
            .targets = {{destination.id, destination}}, .execution_data = std::move(request.arguments)});
      },
      .execute = [config = std::move(config), host](ToolContext context, PreparedToolCall call) -> Async<ToolExecutionResult> {
        const auto access = context.Resources();
        if (access.size() != 1 || access.front().mode != ResourceAccessMode::Write ||
            access.front().current != destination || call.change.execution_data != verify_arguments)
          co_return SshFailure(ToolOutcome::Failed, ErrorCode::Conflict, "The approved remote scope changed");
        co_return co_await VerifyRemote(std::move(context), config, host);
      }}));
}

Task<void> VerifyHost(TaskScope tasks, SshConfig config) {
  const auto host = std::make_shared<HostState>();
  auto runtime = Take(AgentRuntime::Create({.tasks = tasks,
      .principal = [] { return AgentResult<PrincipalId>::Success({"remote.operator"}); },
      .access = [host](PolicyRequest request) -> Async<PolicyDecision> {
        using Result = AgentResult<PolicyDecision>;
        if (request.tool && request.tool->id == ssh_task) {
          if (!host->authorized || request.project != project)
            co_return Result::Success({AccessDecision::Deny, "SSH project access is unavailable"});
          if (request.capability == Capability::ExecuteTool)
            co_return Result::Success({AccessDecision::RequireConfirmation, "Confirm the remote verification"});
        }
        co_return Result::Success({AccessDecision::Allow, {}});
      },
      .tool_resources = [](ToolInvocation request) -> Async<std::vector<ResourceAccess>> {
        using Result = AgentResult<std::vector<ResourceAccess>>;
        if (request.tool.id != ssh_task || request.project != project) co_return Result::Success({});
        const auto mode = request.phase == ToolAccessPhase::Prepare ? ResourceAccessMode::Read : ResourceAccessMode::Write;
        co_return Result::Success({{destination.id, destination, mode}});
      }}));
  Take(runtime->RegisterModel({{model_id, 1}, {"host", "SSH test", "local"}, std::make_shared<LocalModel>()}));
  RegisterSshTask(runtime, config, host);
  Take(runtime->RegisterAgent({{.ref = {assistant, 1}, .display_name = "Remote verification", .model = model_id, .tools = {ssh_task}}}));
  Take(co_await runtime->StartAsync());
  const auto created = Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), {
      .options = {.agent = assistant, .project = project, .retention = SessionRetention::Ephemeral}}));
  const auto chat = Take(co_await runtime->Sessions().OpenAsync(std::get<SessionCreated>(created.outcome).session));
  auto observation = Take(co_await chat->ObserveAsync([](auto) {}));

  auto pending = [&]() -> Task<InteractionSnapshot> {
    const auto issued = Take(co_await chat->SendAsync(NewCommandId(), {.text = "Verify the remote build"}));
    const auto id = std::get<RunAccepted>(issued.outcome).run;
    const auto launches = host->launches;
    std::optional<InteractionSnapshot> approval;
    co_await WaitUntil([&] {
      const auto snapshot = chat->Snapshot();
      for (const auto& item : snapshot->interactions) {
        if (item.run != id || item.state != huxerui::agentkit::InteractionState::Pending) continue;
        const auto* plan = std::get_if<ToolApproval>(&item.body);
        if (!plan) continue;
        Check(plan->preview.title == "Run remote verification" && plan->preview.summary.find(config.host) != std::string::npos &&
            plan->preview.summary.find(remote_command) != std::string::npos, "Remote approval omitted the host or task");
        approval = item;
        return true;
      }
      const auto run = std::ranges::find(snapshot->runs, id, &RunSnapshot::id);
      if (run != snapshot->runs.end() && !snapshot->summary.active_run) {
        if (run->failure) throw std::runtime_error(run->failure->message);
        const auto call = std::ranges::find(snapshot->tools, id, &ToolCallSnapshot::run);
        if (call != snapshot->tools.end() && call->reason) throw std::runtime_error(call->reason->message);
        throw std::runtime_error("Remote verification ended before approval");
      }
      return false;
    }, "Remote verification did not reach approval");
    Check(host->launches == launches, "The SSH client started before approval");
    co_return *approval;
  };
  const auto settled = [&]() -> Task<ToolCallSnapshot> {
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "Remote verification did not settle");
    co_return chat->Snapshot()->tools.back();
  };

  auto approval = co_await pending();
  Take(co_await chat->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Deny}));
  Check((co_await settled()).state == ToolCallState::Denied && host->launches == 0, "Denial launched an SSH process");

  approval = co_await pending();
  host->authorized = false;
  Take(co_await runtime->RefreshAccessAsync());
  auto withdrawn = co_await chat->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve});
  if (!withdrawn.Succeeded()) {
    if (const auto run = chat->Snapshot()->summary.active_run) Take(co_await chat->CancelAsync(NewCommandId(), *run));
  }
  static_cast<void>(co_await settled());
  Check(host->launches == 0, "Approval after project revocation launched an SSH process");
  host->authorized = true;
  Take(co_await runtime->RefreshAccessAsync());

  for (const auto expected : {ToolCallState::Succeeded, ToolCallState::Failed, ToolCallState::ResultUnknown}) {
    approval = co_await pending();
    const auto launches = host->launches;
    const auto command = NewCommandId();
    const InteractionReply reply{approval.id, approval.version, ApprovalDecision::Approve};
    const auto applied = Take(co_await chat->SubmitAsync(command, reply));
    const auto result = co_await settled();
    const auto replay = Take(co_await chat->SubmitAsync(command, reply));
    Check(result.state == expected, "The remote exit did not produce its expected tool state");
    Check(applied.stamp.commit == replay.stamp.commit && host->launches == launches + 1, "Approval replay launched another SSH process");
  }
  Take(co_await runtime->CloseAsync());
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 4) throw std::runtime_error("Usage: ssh_task_host <absolute ssh client> <host config> <host alias>");
    SshConfig config{.executable = std::filesystem::u8path(argv[1]), .configuration = argv[2], .host = argv[3]};
    Check(config.executable.is_absolute() && std::filesystem::u8path(config.configuration).is_absolute() &&
        !config.host.empty() && config.host.front() != '-', "Expected an absolute client, an absolute config path and a host alias");
    std::exception_ptr failure;
    bool finished = false;
    const Application app{[]() -> View { return Spacer(); }, {.application_hooks = {[&](ApplicationContext&) {
      auto tasks = UseApplicationTaskScope();
      tasks.Launch([&, tasks]() -> Task<void> {
        try { co_await VerifyHost(tasks, config); }
        catch (...) { failure = std::current_exception(); }
        finished = true;
      });
    }}}};
    testing::UiTest ui(app, {.viewport = {640.0F, 480.0F}});
    const auto end = std::chrono::steady_clock::now() + 75s;
    while (!finished && std::chrono::steady_clock::now() < end) {
      ui.Pump(10ms);
      std::this_thread::sleep_for(2ms);
    }
    if (failure) std::rethrow_exception(failure);
    Check(finished, "SSH host verification timed out");
    std::cout << "SSH task approved once, remote exit states recorded, revoked access blocked\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
