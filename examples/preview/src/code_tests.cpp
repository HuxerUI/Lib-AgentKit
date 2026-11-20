#include "code_tests.h"
#include "code_tools.h"
#include "workspace_evidence.h"

#include <huxerui/huxerui.h>
#include <huxerui/terminal.h>

#include <algorithm>

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
namespace term = huxerui::terminal;
using namespace huxerui;
using namespace std::chrono_literals;
using Result = kit::AgentResult<kit::ToolExecutionResult>;
using kit::JsonValue;

Result Failed(kit::ErrorCode code, std::string message, kit::ToolOutcome outcome = kit::ToolOutcome::Failed) {
  return Result::Success({.outcome = outcome, .reason = kit::AgentError{.code = code, .message = std::move(message)}});
}

struct CaptureOutput {
  std::optional<File> file;
  ~CaptureOutput() { if (file) static_cast<void>(file->Delete()); }
};

void SkipTerminalEscape(std::string_view text, std::size_t& offset) {
  if (offset == text.size()) return;
  const auto lead = text[offset];
  if (lead == '[') {
    ++offset;
    while (offset < text.size() && text[offset] >= ' ' && text[offset] <= '?') ++offset;
    if (offset < text.size() && text[offset] >= '@' && text[offset] <= '~') ++offset;
  } else if (lead == ']' || lead == 'P' || lead == 'X' || lead == '^' || lead == '_') {
    ++offset;
    while (offset < text.size() && text[offset] != '\r' && text[offset] != '\n') {
      if (text[offset++] == '\a') break;
      if (text[offset - 1] == '\x1b' && offset < text.size() && text[offset] == '\\') {
        ++offset;
        break;
      }
    }
  } else {
    while (offset < text.size() && text[offset] >= ' ' && text[offset] <= '/') ++offset;
    if (offset < text.size() && text[offset] >= '0' && text[offset] <= '~') ++offset;
  }
}

std::string CTestDisplayLog(std::string_view raw) {
  std::string plain;
  plain.reserve(raw.size());
  for (std::size_t offset = 0; offset < raw.size();) {
    const auto byte = static_cast<unsigned char>(raw[offset++]);
    if (byte == 0x1b) {
      SkipTerminalEscape(raw, offset);
    } else if (byte == '\r') {
      if (offset == raw.size() || raw[offset] != '\n') plain.push_back('\n');
    } else if (byte == '\n' || byte == '\t' || (byte >= ' ' && byte != 0x7f)) {
      plain.push_back(static_cast<char>(byte));
    }
  }

  std::string log;
  const std::string_view lines = plain;
  for (std::size_t start = 0; start < lines.size();) {
    const auto end = lines.find('\n', start);
    const auto line = lines.substr(start, end == lines.npos ? lines.size() - start : end - start);
    const auto label = line.substr(0, line.find_last_not_of(" \t") + 1);
    if (label != "[HANDLER_OUTPUT]" && label != "[ERROR_MESSAGE]") {
      log.append(line);
      if (end != lines.npos) log.push_back('\n');
    }
    if (end == lines.npos) break;
    start = end + 1;
  }
  while (!log.empty() && log.back() == '\n') log.pop_back();
  return log;
}

[[huxerui::composable]]
View CTestDiagnostics(std::string text) {
  const auto expanded = UseState(false);
  const auto colors = UseTheme().colors;
  auto style = UseEnvironment<ButtonStyle>();
  style.background = Color::Transparent();
  style.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), colors.on_surface_variant};
  style.padding = EdgeInsets::Symmetric(8.0F, 4.0F);
  style.minimum_height = 32.0F;
  std::vector<View> body{Row{ProvideEnvironment(style,
      Button(expanded.Get() ? "Hide test output" : "Show test output")
          .OnClick([expanded] { expanded = !expanded.Get(); })
          .With(Semantics{.expanded = expanded.Get()}).Key("ctest-output-toggle")), Spacer()}};
  if (expanded.Get()) {
    auto log = Text(std::move(text)).Style({Font::Monospace(12.0F), colors.on_surface});
    body.push_back(ScrollView(SelectionArea(std::move(log))).With(Frame{.max_height = 220.0F},
        ScrollBar(), Padding(8.0F), Background(colors.surface_container_high), CornerRadius(6.0F)).Key("ctest-output"));
  }
  return Column(std::move(body)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

kit::Async<kit::ToolExecutionResult> ExecuteTests(kit::ToolContext context, File executable, File log,
    File evidence, const JsonValue* implementation, bool capture_png, const std::vector<kit::SourceSlice>& sources) {
  const auto grants = context.Resources();
  if (grants.size() != 1) co_return Failed(kit::ErrorCode::PermissionDenied, "Test directory is unavailable");
  auto root = File(grants.front().resource.value);
  auto info = co_await root.StatAsync();
  if (!info.Succeeded() || info.Value().type != FileType::Directory) {
    co_return Failed(kit::ErrorCode::NotFound, "Test directory is unavailable");
  }
  if (context.StopToken().stop_requested()) {
    co_return Failed(kit::ErrorCode::AlreadyClosed, "Tests cancelled", kit::ToolOutcome::Cancelled);
  }
  CaptureOutput capture;
  if (implementation) {
    auto current = co_await RunWorker(VerifyWorkspaceVersions, root, *implementation);
    if (!current.Succeeded()) co_return Failed(current.Error().code, current.Error().message);
  }
  if (capture_png) {
    if (!co_await evidence.CreateDirectoriesAsync()) {
      co_return Failed(kit::ErrorCode::ProviderFailure, "Could not prepare the verification screenshot directory");
    }
    const auto destination = VerificationCaptureFile(evidence, context.CallId());
    auto existing = co_await destination.StatAsync();
    if (existing.Succeeded() || existing.Error().code != IoErrorCode::NotFound) {
      co_return Failed(kit::ErrorCode::Conflict, "The test screenshot output already exists or is unavailable");
    }
    capture.file = destination;
  }
  term::TerminalShellOptions shell{
      .executable = std::filesystem::u8path(executable.Path()),
      .arguments = {"--test-dir", root.Path(), "--output-on-failure", "--no-tests=error", "--output-log", log.Path()},
      .working_directory = std::filesystem::u8path(root.Path()),
      .environment = {{"HUXERUI_AGENTKIT_API_KEY", ""},
          {"HUXERUI_AGENTKIT_VERIFICATION_PNG", capture.file ? capture.file->Path() : std::string{}}}};
  auto opened = co_await term::OpenShellAsync(std::move(shell));
  if (!opened.Succeeded()) co_return Failed(kit::ErrorCode::ProviderFailure, "Could not start CTest: " + opened.Error().message);
  auto process = std::move(opened).Value();
  const auto until = std::chrono::steady_clock::now() + 5min;
  auto state = process.Snapshot();
  while (state.status == term::TerminalStatus::Open) {
    if (context.StopToken().stop_requested() || std::chrono::steady_clock::now() >= until) {
      auto stopped = co_await process.CloseAsync();
      if (!stopped.Succeeded()) {
        co_return Failed(kit::ErrorCode::ResultUnknown, "Could not confirm CTest shutdown", kit::ToolOutcome::ResultUnknown);
      }
      co_return Failed(context.StopToken().stop_requested() ? kit::ErrorCode::AlreadyClosed : kit::ErrorCode::DeadlineExceeded,
                       "CTest stopped before completion", kit::ToolOutcome::Cancelled);
    }
    co_await Delay(30ms);
    state = process.Snapshot();
  }
  if (!state.exit || state.exit->reason != term::TerminalExitReason::Exited || !state.exit->code) {
    co_return Failed(kit::ErrorCode::ResultUnknown, "CTest ended without an exit code", kit::ToolOutcome::ResultUnknown);
  }
  const auto code = *state.exit->code;
  if (implementation) {
    auto current = co_await RunWorker(VerifyWorkspaceVersions, root, *implementation);
    if (!current.Succeeded()) co_return Failed(current.Error().code, current.Error().message);
  }
  const auto summary = std::string(code == 0 ? "Tests passed" : "Tests failed") + " (exit " + std::to_string(code) + ").";
  kit::ToolExecutionResult result{
      .outcome = code == 0 ? kit::ToolOutcome::Succeeded : kit::ToolOutcome::Failed,
      .model_value = kit::JsonValue{static_cast<std::int64_t>(code)},
      .blocks = {{{kit::NewCommandId().value}, kit::TextBlock{summary, false}}}};
  auto properties = co_await log.StatAsync();
  if (properties.Succeeded() && properties.Value().size <= 64 * 1024) {
    auto diagnostics = co_await log.ReadStringAsync();
    if (diagnostics.Succeeded()) {
      auto display = CTestDisplayLog(diagnostics.Value());
      if (!display.empty()) result.blocks.push_back({{kit::NewCommandId().value}, kit::TextBlock{std::move(display), false}});
    }
  }
  if (code != 0) result.reason = kit::AgentError{.code = kit::ErrorCode::ProviderFailure, .message = summary};
  if (capture.file && code == 0) {
    auto staged = co_await RunWorker(StageVerificationScreenshot, evidence, context.CallId());
    if (!staged.Succeeded()) {
      result.outcome = kit::ToolOutcome::Failed;
      result.reason = staged.Error();
    } else {
      auto screenshot = std::move(staged).Value();
      for (const auto& source : sources) screenshot.dependencies.push_back({.input = source});
      result.new_artifacts.push_back(std::move(screenshot));
    }
  }
  if (implementation) {
    auto record = co_await StageWorkspaceEvidence(evidence, context.CallId(), std::string(VerificationEvidence),
        "Workspace verification", JsonValue{JsonValue::Object{
            {"kind", JsonValue{std::string(VerificationEvidence)}}, {"summary", JsonValue{summary}},
            {"exit_code", JsonValue{static_cast<std::int64_t>(code)}}, {"implementation", *implementation},
            {"screenshot_captured", JsonValue{!result.new_artifacts.empty()}}}});
    if (!record.Succeeded()) co_return Failed(record.Error().code, record.Error().message);
    auto artifact = std::move(record).Value();
    for (const auto& source : sources) artifact.dependencies.push_back({.input = source});
    result.new_artifacts.push_back(std::move(artifact));
  }
  co_return Result::Success(std::move(result));
}

kit::Async<kit::ArtifactRef> CandidateImplementation(const std::shared_ptr<kit::AgentRuntime>& runtime,
    const kit::ActionParty& actor) {
  using Reference = kit::AgentResult<kit::ArtifactRef>;
  const kit::AgentError unavailable{.code = kit::ErrorCode::InvalidState, .message = "The candidate's applied version is unavailable"};
  if (!actor.session || !actor.run) co_return Reference::Failure(unavailable);
  auto session = co_await runtime->Sessions().OpenAsync(*actor.session);
  if (!session.Succeeded()) co_return Reference::Failure(session.Error());
  const auto snapshot = session.Value()->Snapshot();
  if (!snapshot) co_return Reference::Failure(unavailable);
  const auto run = std::ranges::find(snapshot->runs, *actor.run, &kit::RunSnapshot::id);
  if (run == snapshot->runs.end() || run->trigger != kit::RunTrigger::ForkComparison)
    co_return Reference::Failure(unavailable);
  const auto applied = std::ranges::find_if(snapshot->tools, [&](const auto& call) {
    return call.run == run->id && call.tool.id == ApplyCodePatch && call.state == kit::ToolCallState::Succeeded &&
        call.artifacts.size() == 1;
  });
  if (applied == snapshot->tools.end()) co_return Reference::Failure(unavailable);
  co_return Reference::Success(applied->artifacts.front());
}

} // namespace

std::optional<View> RenderCTestDiagnostics(const kit::Message& message, const kit::ContentBlock& block) {
  if (message.author != kit::MessageAuthor::Tool || message.blocks.size() < 2 || block.id != message.blocks[1].id) return {};
  const auto* summary = std::get_if<kit::TextBlock>(&message.blocks.front().body);
  const auto* log = std::get_if<kit::TextBlock>(&block.body);
  if (!summary || !log || (!summary->text.starts_with("Tests passed (exit ") &&
      !summary->text.starts_with("Tests failed (exit "))) return {};
  return CTestDiagnostics(CTestDisplayLog(log->text));
}

kit::AgentResult<void> RegisterCodeTests(
    const std::shared_ptr<kit::AgentRuntime>& runtime, File executable, File logs, File evidence) {
  return runtime->RegisterTool({
      .id = RunCodeTests, .version = 1, .description = "Run the workspace's configured CTest suite",
      .input_schema = {JsonValue{JsonValue::Object{{"enum", JsonValue{JsonValue::Array{JsonValue{}, JsonValue{true}, VerifyCandidateTests}}}}}},
      .effect = kit::ToolEffect::ExternalSubmit,
      .prepare = [executable, weak = std::weak_ptr<kit::AgentRuntime>(runtime)](
                     kit::ToolPrepareContext context, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        using Prepared = kit::AgentResult<kit::PreparedChange>;
        if (context.readable.size() != 1) co_return kit::AgentResult<kit::PreparedChange>::Failure({
            .code = kit::ErrorCode::PermissionDenied, .message = "No test directory is authorized"});
        const auto& cwd = context.readable.front().resource.value;
        auto command = "Run " + executable.Path() + " with the CTest configuration in " + cwd;
        kit::PreparedChange plan{.preview = {.title = "Run workspace tests", .summary = std::move(command)},
            .execution_data = JsonValue{executable.Path()}};
        const bool capture = request.arguments == JsonValue{true};
        if (capture || request.arguments == VerifyCandidateTests) {
          auto owner = weak.lock();
          if (!owner) co_return Prepared::Failure({.code = kit::ErrorCode::RuntimeClosed, .message = "Agent runtime closed"});
          kit::ArtifactRef artifact;
          if (capture) {
            const auto* supplied = context.sources.size() == 1 ? std::get_if<kit::ArtifactRef>(&context.sources.front().source) : nullptr;
            if (!supplied) co_return Prepared::Failure(
                {.code = kit::ErrorCode::InvalidArgument, .message = "Verification requires one implementation version"});
            artifact = *supplied;
            plan.inputs = std::move(context.sources);
          } else {
            auto applied = co_await CandidateImplementation(owner, context.actor);
            if (!applied.Succeeded()) co_return Prepared::Failure(applied.Error());
            artifact = std::move(applied).Value();
            plan.inputs.push_back({artifact});
          }
          auto read = co_await owner->Artifacts().ReadAsync(artifact);
          if (!read.Succeeded()) co_return Prepared::Failure(read.Error());
          if (read.Value().metadata.type != ImplementationEvidence) co_return Prepared::Failure(
              {.code = kit::ErrorCode::InvalidArgument, .message = "Verification requires applied workspace evidence"});
          auto draft = co_await owner->Artifacts().PreviewAsync(artifact);
          if (!draft.Succeeded()) co_return Prepared::Failure(draft.Error());
          if (!draft.Value().structured) co_return Prepared::Failure(
              {.code = kit::ErrorCode::InvalidState, .message = "Implementation evidence is missing"});
          auto current = co_await RunWorker(VerifyWorkspaceVersions, File(cwd), *draft.Value().structured);
          if (!current.Succeeded()) co_return Prepared::Failure(current.Error());
          plan.preview.artifacts.push_back(artifact);
          if (capture) plan.preview.summary += ". The configured test will capture the implementation in a separate screenshot file.";
          plan.execution_data = JsonValue{JsonValue::Object{{"executable", JsonValue{executable.Path()}},
              {"implementation", std::move(*draft.Value().structured)}, {"capture_png", JsonValue{capture}}}};
        }
        co_return Prepared::Success(std::move(plan));
      },
      .execute = [executable, logs, evidence](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto* fields = std::get_if<JsonValue::Object>(&call.change.execution_data.value);
        const bool capture = call.request.arguments == JsonValue{true};
        const bool verify = capture || call.request.arguments == VerifyCandidateTests;
        if (static_cast<bool>(fields) != verify ||
            (fields && (!fields->contains("executable") || !fields->contains("implementation") ||
                !fields->contains("capture_png") || fields->at("capture_png") != JsonValue{capture}))) {
          co_return Failed(kit::ErrorCode::Conflict, "The workspace verification plan must be prepared again");
        }
        const auto& bound = fields ? fields->at("executable") : call.change.execution_data;
        const auto* implementation = fields ? &fields->at("implementation") : nullptr;
        if (bound != kit::JsonValue{executable.Path()}) {
          co_return Failed(kit::ErrorCode::Conflict, "The configured test executable changed after review");
        }
        if (!co_await logs.CreateDirectoriesAsync()) {
          co_return Failed(kit::ErrorCode::ProviderFailure, "Could not prepare the test log directory");
        }
        const auto log = logs.Child(context.CallId().value + ".log");
        co_return co_await ExecuteTests(std::move(context), executable, log, evidence, implementation, capture, call.change.inputs);
      }});
}

kit::Async<kit::RunCompletion> RunVerification(kit::RunContext run, kit::RunInput input,
    std::optional<kit::RunCheckpoint> checkpoint) {
  using Completion = kit::AgentResult<kit::RunCompletion>;
  if (!input.work_request || input.input.context.size() != 1 ||
      (checkpoint && (checkpoint->stage != "workspace-verify" || checkpoint->schema_version != 1))) {
    co_return Completion::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Expected a scoped verification request"});
  }
  if (!checkpoint) {
    auto saved = co_await run.SaveCheckpointAsync({.stage = "workspace-verify", .schema_version = 1});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }
  const auto command = "verify:" + run.Id().value;
  auto receipt = co_await run.StartToolAsync({command + "/ctest"}, {RunCodeTests, JsonValue{true}});
  if (!receipt.Succeeded()) co_return Completion::Failure(receipt.Error());
  auto tested = co_await run.WaitForToolAsync(std::get<kit::ToolCallAccepted>(receipt.Value().outcome).call);
  if (!tested.Succeeded()) co_return Completion::Failure(tested.Error());
  const auto passed = tested.Value().outcome == kit::ToolOutcome::Succeeded && tested.Value().published_artifacts.size() == 2;
  const auto error = tested.Value().reason.value_or(kit::AgentError{.code = kit::ErrorCode::InvalidState,
      .message = "Workspace verification did not produce a report"});
  auto reply = co_await run.Collaboration().ReplyAsync({command + "/reply"}, *input.work_request,
      {.status = passed ? kit::ReplyStatus::Succeeded : kit::ReplyStatus::Failed,
       .summary = passed ? "Verified workspace file versions, CTest results and implementation screenshot" : error.message,
       .artifacts = tested.Value().published_artifacts});
  if (!reply.Succeeded()) co_return Completion::Failure(reply.Error());
  if (!std::holds_alternative<kit::MessageAccepted>(reply.Value().outcome)) {
    co_return Completion::Failure({.code = kit::ErrorCode::InvalidState, .message = "Verification reply is waiting for authorization"});
  }
  if (!passed) co_return Completion::Failure(error);
  co_return Completion::Success({.summary = "Workspace verification passed", .artifacts = tested.Value().published_artifacts});
}

} // namespace agentkit_preview
