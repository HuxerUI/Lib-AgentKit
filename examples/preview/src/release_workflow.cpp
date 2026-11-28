#include "release_workflow.h"
#include "run_plan.h"

#include "code_tools.h"
#include "design.h"
#include "general_workflow.h"
#include "workspace_evidence.h"
#include "workflow_sessions.h"

#include <app_resources.h>
#include <huxerui/app.h>
#include <array>

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using kit::JsonValue;
using Completion = kit::AgentResult<kit::RunCompletion>;

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

const JsonValue::Object& Object(const JsonValue& value) { return std::get<JsonValue::Object>(value.value); }

const std::string& Read(const JsonValue& value, const char* name) { return std::get<std::string>(Object(value).at(name).value); }

bool ReleaseConditions(const JsonValue& conditions) {
  const auto* object = std::get_if<JsonValue::Object>(&conditions.value);
  return object && object->contains("kind") && object->at("kind") == JsonValue{std::string(WorkspaceRelease)};
}

JsonValue Encode(const kit::ArtifactRef& ref) {
  return JsonValue{JsonValue::Object{{"id", JsonValue{ref.id.value}},
      {"version", JsonValue{static_cast<std::int64_t>(ref.version)}}}};
}

kit::ArtifactRef Decode(const JsonValue& ref) {
  return {{Read(ref, "id")}, static_cast<kit::Revision>(std::get<std::int64_t>(Object(ref).at("version").value))};
}

kit::CommandId Key(const kit::RunContext& run, std::string_view stage, std::string_view operation) {
  return {"release:" + run.Id().value + "/" + std::string(stage) + "/" + std::string(operation)};
}

struct Stage {
  std::string name;
  kit::AgentId agent;
  kit::SendInput input;
  std::vector<std::string> output;
  bool replace_on_revision = false;
};

kit::Async<kit::TaskUpdated> PriorTask(kit::RunContext run, const kit::CommandId& id) {
  using Result = kit::AgentResult<kit::TaskUpdated>;
  auto receipt = co_await run.Commands().ReadAsync(id);
  if (!receipt.Succeeded()) co_return Result::Failure(receipt.Error());
  const auto& result = receipt.Value().result;
  if (!result || !std::holds_alternative<kit::TaskUpdated>(*result)) {
    co_return Fail<kit::TaskUpdated>(kit::ErrorCode::Conflict, "The command is not a task receipt");
  }
  co_return Result::Success(std::get<kit::TaskUpdated>(*result));
}

kit::Async<kit::ProjectTaskId> EnsureTask(kit::RunContext run, kit::ProjectId project, kit::GoalRef goal, const Stage& stage,
    std::vector<kit::TaskDependency> dependencies) {
  using Result = kit::AgentResult<kit::ProjectTaskId>;
  for (auto& edge : dependencies) std::ranges::sort(edge.required_artifact_types);
  std::ranges::sort(dependencies, {}, &kit::TaskDependency::upstream);
  const kit::ProjectTaskSpec spec{.title = stage.name, .input = stage.input, .goal = goal.id,
      .dependencies = std::move(dependencies)};
  while (!run.StopToken().stop_requested()) {
    auto board = co_await run.Projects().ReadAsync(project);
    if (!board.Succeeded()) co_return Result::Failure(board.Error());
    const auto is_stage = [&](const auto& task) { return task.spec.goal == goal.id && task.spec.title == stage.name; };
    const auto existing = std::ranges::find_if(board.Value().tasks, is_stage);
    if (existing == board.Value().tasks.end()) {
      auto created = co_await run.Projects().UpdateTaskAsync(project, Key(run, stage.name, "task"),
          kit::CreateTask{board.Value().revision, spec});
      if (!created.Succeeded()) co_return Result::Failure(created.Error());
      co_return Result::Success(std::get<kit::TaskUpdated>(created.Value().outcome).task);
    }
    if (std::find_if(std::next(existing), board.Value().tasks.end(), is_stage) != board.Value().tasks.end()) {
      co_return Fail<kit::ProjectTaskId>(kit::ErrorCode::Conflict, "Multiple tasks have the same release stage");
    }
    if (existing->spec == spec) co_return Result::Success(existing->id);
    if (existing->state == kit::ProjectTaskState::Running && existing->attempt && existing->attempt_goal != goal) {
      auto settled = co_await run.Projects().WaitForTaskAsync({project, existing->id, *existing->attempt});
      if (!settled.Succeeded()) co_return Result::Failure(settled.Error());
      continue;
    }
    auto edited = co_await run.Projects().UpdateTaskAsync(project, Key(run, stage.name, "revise"),
        kit::ChangeTask{existing->id, existing->revision, kit::EditTask{spec, board.Value().revision}});
    if (!edited.Succeeded()) co_return Result::Failure(edited.Error());
    co_return Result::Success(existing->id);
  }
  co_return Fail<kit::ProjectTaskId>(kit::ErrorCode::AlreadyClosed, "The release run has stopped");
}

kit::AgentResult<std::vector<kit::ArtifactRef>> StageOutputs(const kit::TaskOutcome& outcome, const Stage& stage) {
  if (outcome.state != kit::ProjectTaskState::Succeeded) {
    return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::InvalidState,
        stage.name + ": " + (outcome.reason ? outcome.reason->message : "The task did not complete"));
  }
  if (outcome.artifacts.size() != stage.output.size()) {
    return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::Conflict, "The saved release stage has an unexpected artifact set");
  }
  return kit::AgentResult<std::vector<kit::ArtifactRef>>::Success(outcome.artifacts);
}

kit::Async<std::vector<kit::ArtifactRef>> ExecuteStage(kit::RunContext run, kit::ProjectId project, kit::WorkspaceId workspace,
    kit::ProjectTaskId task, const Stage& stage, std::optional<kit::SessionId> existing = {}) {
  using Result = kit::AgentResult<std::vector<kit::ArtifactRef>>;
  const auto claim = Key(run, stage.name, "claim");
  auto claimed = co_await PriorTask(run, claim);
  if (!claimed.Succeeded()) {
    if (claimed.Error().code != kit::ErrorCode::NotFound) co_return Result::Failure(claimed.Error());
    auto current = co_await FindProjectTask(run, project, task);
    if (!current.Succeeded()) co_return Result::Failure(current.Error());
    if (current.Value().outcome) co_return StageOutputs(*current.Value().outcome, stage);
    if (current.Value().state == kit::ProjectTaskState::Running && current.Value().attempt) {
      auto settled = co_await run.Projects().WaitForTaskAsync({project, task, *current.Value().attempt});
      if (!settled.Succeeded()) co_return Result::Failure(settled.Error());
      co_return StageOutputs(settled.Value(), stage);
    }
  }
  if (!existing) {
    auto created = co_await run.Sessions().CreateAsync(Key(run, stage.name, "conversation"), {
        .options = {.agent = stage.agent, .project = project, .workspace = workspace, .title = stage.name}});
    if (!created.Succeeded()) co_return Result::Failure(created.Error());
    const auto* session = std::get_if<kit::SessionCreated>(&created.Value().outcome);
    if (!session) co_return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::PermissionDenied, "The release session needs approval");
    existing = session->session;
  }
  if (!claimed.Succeeded()) {
    if (claimed.Error().code != kit::ErrorCode::NotFound) co_return Result::Failure(claimed.Error());
    auto current = co_await FindProjectTask(run, project, task);
    if (!current.Succeeded()) co_return Result::Failure(current.Error());
    auto taken = co_await run.Projects().UpdateTaskAsync(project, claim,
        kit::ChangeTask{task, current.Value().revision, kit::ClaimTask{*existing}});
    if (!taken.Succeeded()) co_return Result::Failure(taken.Error());
    claimed = kit::AgentResult<kit::TaskUpdated>::Success(std::get<kit::TaskUpdated>(taken.Value().outcome));
  }
  if (!claimed.Value().attempt) co_return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::Conflict, "The release task has no claim");

  auto sent = co_await run.Collaboration().SendMessageAsync(Key(run, stage.name, "dispatch"),
      {.to = *existing, .body = kit::WorkRequest{.input = stage.input, .request_start = true,
          .reply = kit::ReplyExpectation{.artifact_types = stage.output},
          .task = kit::TaskExecutionRef{project, task, *claimed.Value().attempt},
          .cancel_on_goal_revision = stage.replace_on_revision}});
  if (!sent.Succeeded()) co_return Result::Failure(sent.Error());
  const auto* request = std::get_if<kit::MessageAccepted>(&sent.Value().outcome);
  if (!request) co_return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::PermissionDenied, "Release work is waiting for authorization");
  auto answer = co_await run.Collaboration().WaitForReplyAsync(request->message);
  if (!answer.Succeeded()) co_return Result::Failure(answer.Error());
  const auto& reply = answer.Value().reply;
  if (answer.Value().end != kit::ReplyEnd::Received || !reply || reply->status != kit::ReplyStatus::Succeeded) {
    co_return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::InvalidState,
        stage.name + ": " + (reply ? reply->summary : "The assigned assistant did not deliver a result"));
  }
  if (reply->artifacts.size() != stage.output.size()) {
    co_return Fail<std::vector<kit::ArtifactRef>>(kit::ErrorCode::Conflict, "The release stage returned an unexpected artifact set");
  }
  co_return Result::Success(reply->artifacts);
}

} // namespace

bool IsWorkspaceRelease(const kit::GoalDefinition& definition) { return ReleaseConditions(definition.completion_conditions); }

std::optional<std::size_t> ReleaseSlideCount(const JsonValue& conditions) {
  if (!ReleaseConditions(conditions)) return {};
  const auto& fields = Object(conditions);
  auto count = fields.find("slide_count");
  if (count == fields.end()) return {};
  const auto* number = std::get_if<std::int64_t>(&count->second.value);
  if (!number || *number < 1 || *number > 30) throw std::invalid_argument("The release slide count must be between 1 and 30");
  return static_cast<std::size_t>(*number);
}

kit::Async<kit::RunCompletion> RunWorkspaceRelease(kit::RunContext run, kit::GoalRef goal, kit::GoalDefinition definition,
    std::optional<kit::RunCheckpoint> checkpoint) {
  const auto& settings = definition.completion_conditions;
  const kit::ProjectId project{Read(settings, "project")};
  const kit::WorkspaceId workspace{Read(settings, "workspace")};
  const kit::SessionId deck{Read(settings, "deck")};
  const Stage design{"Release design", DesignAgent, {.text = definition.objective}, {"design/board+json"}};
  const Stage code{"Release implementation", CodeAgent,
      {.text = "Implement the approved design for this release. " + definition.objective},
      {std::string(ImplementationEvidence), std::string(VerificationEvidence), std::string(VerificationScreenshot)}};
  Stage presentation{"Release presentation", PptAgent,
      {.text = "Prepare the release presentation using the approved wireframe and verified workspace results. " + definition.objective},
      {"presentation/pptx"}, true};
  presentation.input.text += "\nRelease definition version: " + std::to_string(goal.version) + ".";
  if (const auto count = ReleaseSlideCount(settings)) {
    presentation.input.text += "\nRequired slide count: " + std::to_string(*count) + ".";
  }

  auto phase = checkpoint ? checkpoint->stage : std::string("release-design");
  if (checkpoint && ((checkpoint->schema_version != 1 && checkpoint->schema_version != 2) ||
      (phase != "release-design" && phase != "release-code" && phase != "release-deck"))) {
    co_return Fail<kit::RunCompletion>(kit::ErrorCode::Unsupported, "Unknown release checkpoint");
  }
  JsonValue::Array sources;
  if (checkpoint) sources = std::get<JsonValue::Array>(checkpoint->state.value);
  else {
    auto saved = co_await run.SaveCheckpointAsync({.stage = phase, .schema_version = 1, .state = JsonValue{sources}});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }

  auto design_task = co_await EnsureTask(run, project, goal, design, {});
  if (!design_task.Succeeded()) co_return Completion::Failure(design_task.Error());
  auto code_task = co_await EnsureTask(run, project, goal, code, {{design_task.Value(), design.output}});
  if (!code_task.Succeeded()) co_return Completion::Failure(code_task.Error());
  auto deck_task = co_await EnsureTask(run, project, goal, presentation,
      {{design_task.Value(), design.output}, {code_task.Value(), code.output}});
  if (!deck_task.Succeeded()) co_return Completion::Failure(deck_task.Error());

  const bool show_plan = !checkpoint || checkpoint->schema_version == 2;
  const auto version = show_plan ? 2U : 1U;
  const auto resources = huxerui::UseService<huxerui::Resources>();
  std::vector<RunPlanStep> progress{
      {resources->GetString(::app::strings::ui_run_plan_release_design), "in_progress"},
      {resources->GetString(::app::strings::ui_run_plan_release_implementation), "pending"},
      {resources->GetString(::app::strings::ui_run_plan_release_deck), "pending"},
  };
  if (phase == "release-design") {
    if (show_plan) co_await RecordRunPlan(run, "release-design", progress);
    auto drafted = co_await ExecuteStage(run, project, workspace, design_task.Value(), design);
    if (!drafted.Succeeded()) co_return Completion::Failure(drafted.Error());
    for (const auto& ref : drafted.Value()) sources.push_back(Encode(ref));
    phase = "release-code";
    auto saved = co_await run.SaveCheckpointAsync({.stage = phase, .schema_version = version, .state = JsonValue{sources}});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }
  if (phase == "release-code") {
    if (show_plan) {
      progress[0].status = "completed";
      progress[1].status = "in_progress";
      co_await RecordRunPlan(run, "release-code", progress);
    }
    auto implemented = co_await ExecuteStage(run, project, workspace, code_task.Value(), code);
    if (!implemented.Succeeded()) co_return Completion::Failure(implemented.Error());
    for (const auto& ref : implemented.Value()) sources.push_back(Encode(ref));
    auto saved = co_await run.SaveCheckpointAsync({.stage = "release-deck", .schema_version = version, .state = JsonValue{sources}});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }

  if (show_plan) {
    progress[0].status = "completed";
    progress[1].status = "completed";
    progress[2].status = "in_progress";
    co_await RecordRunPlan(run, "release-deck", progress);
  }
  auto exported = co_await ExecuteStage(run, project, workspace, deck_task.Value(), presentation, deck);
  if (!exported.Succeeded()) co_return Completion::Failure(exported.Error());
  if (show_plan) {
    progress[2].status = "completed";
    co_await RecordRunPlan(run, "release-delivered", progress);
  }
  std::vector<kit::ArtifactRef> artifacts;
  for (const auto& source : sources) artifacts.push_back(Decode(source));
  artifacts.insert(artifacts.end(), exported.Value().begin(), exported.Value().end());
  auto view = co_await run.Goals().ReadAsync();
  if (!view.Succeeded()) co_return Completion::Failure(view.Error());
  if (!view.Value() || view.Value()->ref != goal) {
    co_return Fail<kit::RunCompletion>(kit::ErrorCode::Conflict, "The release goal changed");
  }
  kit::Evidence evidence{.values = JsonValue{true}};
  for (const auto& artifact : artifacts) evidence.sources.push_back({artifact});
  auto submitted = co_await run.Goals().UpdateAsync(Key(run, "goal", "evidence"),
      kit::ChangeGoal{goal.id, view.Value()->revision, kit::SubmitGoalEvidence{goal, std::move(evidence)}});
  if (!submitted.Succeeded()) co_return Completion::Failure(submitted.Error());
  co_return Completion::Success({.summary = "Release evidence is ready for review", .artifacts = std::move(artifacts)});
}

kit::Async<kit::VerificationResult> VerifyWorkspaceRelease(std::shared_ptr<kit::AgentRuntime> runtime,
    std::shared_ptr<CodeWorkspaces> workspaces, PresentationFiles files, kit::VerificationRequest request) {
  using Result = kit::AgentResult<kit::VerificationResult>;
  if (std::holds_alternative<kit::TaskExecutionRef>(request.subject) &&
      std::holds_alternative<std::nullptr_t>(request.conditions.value) && request.evidence.values == JsonValue{true}) {
    co_return Result::Success({kit::VerificationStatus::Passed, {}, request.evidence.sources});
  }
  const auto reject = [](std::string reason) {
    return Result::Success({kit::VerificationStatus::MissingEvidence, std::move(reason)});
  };
  if (std::holds_alternative<kit::GoalRef>(request.subject) && !ReleaseConditions(request.conditions))
    co_return reject("No evidence verifier is configured for this goal.");
  if (!std::holds_alternative<kit::GoalRef>(request.subject) || !runtime || request.evidence.sources.size() != 5) {
    co_return reject("Expected a design, implementation, verification report, implementation screenshot and exported presentation");
  }
  std::map<std::string, kit::ArtifactRecord> records;
  for (const auto& source : request.evidence.sources) {
    const auto* artifact = std::get_if<kit::ArtifactRef>(&source.source);
    if (!artifact || request.stop.stop_requested()) co_return reject("Release evidence is incomplete");
    auto known = co_await runtime->Artifacts().ReadAsync(*artifact);
    if (!known.Succeeded()) co_return Result::Failure(known.Error());
    auto record = std::move(known).Value();
    records.emplace(record.metadata.type, std::move(record));
  }
  const auto design = records.find("design/board+json");
  const auto code = records.find(std::string(ImplementationEvidence));
  const auto report = records.find(std::string(VerificationEvidence));
  const auto screenshot = records.find(std::string(VerificationScreenshot));
  const auto deck = records.find("presentation/pptx");
  if (design == records.end() || code == records.end() || report == records.end() ||
      screenshot == records.end() || deck == records.end()) {
    co_return reject("The expected release artifacts are missing");
  }
  const auto linked = [](const kit::ArtifactRecord& output, const kit::ArtifactRecord& input) {
    return std::ranges::any_of(output.dependencies, [&](const auto& dependency) {
      return dependency.input.source == kit::SourceRef{input.metadata.ref};
    });
  };
  if (!linked(code->second, design->second) || !linked(report->second, code->second) ||
      !linked(screenshot->second, code->second) || screenshot->second.call != report->second.call ||
      screenshot->second.run != report->second.run ||
      !linked(deck->second, design->second) || !linked(deck->second, report->second) || !linked(deck->second, screenshot->second)) {
    co_return reject("The release artifacts do not belong to one verified version chain");
  }
  auto verified = co_await runtime->Artifacts().PreviewAsync(report->second.metadata.ref);
  if (!verified.Succeeded()) co_return Result::Failure(verified.Error());
  const auto* result = verified.Value().structured
      ? std::get_if<JsonValue::Object>(&verified.Value().structured->value) : nullptr;
  if (!result || !result->contains("exit_code") || result->at("exit_code") != JsonValue{std::int64_t{0}}) {
    co_return reject("The workspace tests did not pass");
  }
  if (!result->contains("screenshot_captured") || result->at("screenshot_captured") != JsonValue{true}) {
    co_return reject("The test report has no implementation screenshot");
  }
  auto implementation = co_await runtime->Artifacts().PreviewAsync(code->second.metadata.ref);
  if (!implementation.Succeeded()) co_return Result::Failure(implementation.Error());
  const auto workspace = workspaces->Find({Read(request.conditions, "workspace")});
  if (!workspace || !implementation.Value().structured || !result->contains("implementation") ||
      *implementation.Value().structured != result->at("implementation")) {
    co_return reject("The test report does not match the implemented workspace version");
  }
  auto current = co_await huxerui::RunWorker(VerifyWorkspaceVersions, huxerui::File(workspace->root.u8string()),
      *implementation.Value().structured);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  auto conversation = co_await runtime->Sessions().OpenAsync(deck->second.session);
  if (!conversation.Succeeded()) co_return Result::Failure(conversation.Error());
  const auto snapshot = conversation.Value()->Snapshot();
  if (!snapshot) co_return reject("The export record is unavailable");
  const auto producing = std::ranges::find(snapshot->runs, deck->second.run, &kit::RunSnapshot::id);
  if (producing == snapshot->runs.end() || producing->goal != std::get<kit::GoalRef>(request.subject) ||
      deck->second.session.value != Read(request.conditions, "deck")) {
    co_return reject("The presentation was not produced for the current goal version");
  }
  const auto& ref = deck->second.metadata.ref;
  const auto export_call = std::ranges::find_if(snapshot->tools, [&](const auto& call) {
    return call.run == deck->second.run && call.tool.id == ExportPresentation && call.state == kit::ToolCallState::Succeeded &&
        std::ranges::find(call.artifacts, ref) != call.artifacts.end();
  });
  if (export_call == snapshot->tools.end() || export_call->effects.size() != 1 ||
      export_call->effects.front().state != kit::ResourceEffectState::Applied) {
    co_return reject("No successful export was recorded for this presentation version");
  }
  auto image = co_await LoadVerificationScreenshot(runtime, screenshot->second.metadata.ref);
  if (!image.Succeeded()) co_return Result::Failure(image.Error());
  auto staged = co_await ReadPresentationBytes(runtime, ref);
  if (!staged.Succeeded()) co_return Result::Failure(staged.Error());
  auto exported = co_await huxerui::RunWorker(VerifyPresentationExport, files, std::move(staged).Value(), export_call->effects.front().resource,
      image.Value(), ReleaseSlideCount(request.conditions));
  if (!exported.Succeeded()) co_return Result::Failure(exported.Error());
  co_return Result::Success({kit::VerificationStatus::Passed,
      "The design, implementation, passing tests, screenshot and presentation share the approved versions",
      request.evidence.sources});
}

} // namespace agentkit_preview
