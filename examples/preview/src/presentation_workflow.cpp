#include "presentation_workflow.h"

#include "presentation_tools.h"
#include "workflow_sessions.h"
#include "workspace_evidence.h"
#include "release_workflow.h"
#include "release_skill.h"

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using kit::JsonValue;
using Completion = kit::AgentResult<kit::RunCompletion>;

kit::JsonSchema StringFields(std::initializer_list<std::string_view> names) {
  JsonValue::Object properties;
  JsonValue::Array required;
  for (auto name : names) {
    properties.emplace(name, JsonValue{JsonValue::Object{{"type", JsonValue{std::string("string")}}}});
    required.push_back(JsonValue{std::string(name)});
  }
  return {JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}},
      {"properties", JsonValue{std::move(properties)}}, {"required", JsonValue{std::move(required)}},
      {"additionalProperties", JsonValue{false}}}}};
}

const std::string& AnswerText(const JsonValue& answer, const char* name) {
  return std::get<std::string>(std::get<JsonValue::Object>(answer.value).at(name).value);
}

kit::CommandId StageCommand(const kit::RunContext& context, std::string_view stage) {
  return {"ppt:" + context.Id().value + "/" + std::string(stage)};
}

kit::Async<JsonValue> Confirm(kit::RunContext context, std::string_view stage, kit::FormRequest form) {
  auto receipt = co_await context.RequestInputAsync(StageCommand(context, stage), std::move(form));
  if (!receipt.Succeeded()) co_return kit::AgentResult<JsonValue>::Failure(receipt.Error());
  auto reply = co_await context.WaitForInputAsync(std::get<kit::InputRequested>(receipt.Value().outcome).id);
  if (!reply.Succeeded()) co_return kit::AgentResult<JsonValue>::Failure(reply.Error());
  if (reply.Value().state != kit::InteractionState::Submitted || !reply.Value().value) {
    co_return kit::AgentResult<JsonValue>::Failure({
        .code = reply.Value().state == kit::InteractionState::Expired ? kit::ErrorCode::DeadlineExceeded
                                                                     : kit::ErrorCode::AlreadyClosed,
        .message = "Presentation form was not confirmed"});
  }
  co_return kit::AgentResult<JsonValue>::Success(std::move(*reply.Value().value));
}

kit::Async<kit::ToolResult> Invoke(kit::RunContext context, std::string_view stage, kit::ToolRequest tool) {
  auto receipt = co_await context.StartToolAsync(StageCommand(context, stage), std::move(tool));
  if (!receipt.Succeeded()) co_return kit::AgentResult<kit::ToolResult>::Failure(receipt.Error());
  auto outcome = co_await context.WaitForToolAsync(std::get<kit::ToolCallAccepted>(receipt.Value().outcome).call);
  if (outcome.Succeeded() && outcome.Value().outcome != kit::ToolOutcome::Succeeded) {
    co_return kit::AgentResult<kit::ToolResult>::Failure(outcome.Value().reason.value_or(kit::AgentError{
        .code = kit::ErrorCode::ProviderFailure, .message = "Presentation tool did not succeed"}));
  }
  co_return outcome;
}

} // namespace

kit::Async<kit::RunCompletion> RunPresentation(
    kit::RunContext context, kit::RunInput input,
    std::optional<kit::RunCheckpoint> checkpoint, std::string export_directory,
    std::optional<PresentationSkillEvidence> skill) {
  if (input.artifact_action && input.artifact_action->action == ExportStagedPresentation) {
    const auto& action = *input.artifact_action;
    const auto* file_name = std::get_if<std::string>(&action.payload.value);
    auto valid = ValidatePresentationFilename(file_name ? std::string_view(*file_name) : std::string_view{});
    if (!valid.Succeeded()) co_return Completion::Failure(valid.Error());
    if (checkpoint && (checkpoint->stage != "staged-export" || checkpoint->schema_version != 1))
      co_return Completion::Failure({.code = kit::ErrorCode::Unsupported, .message = "Unknown export checkpoint"});
    if (!checkpoint) {
      auto saved = co_await context.SaveCheckpointAsync({.stage = "staged-export", .schema_version = 1});
      if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
    }
    auto exported = co_await Invoke(context, "export-staged", {
        ExportPresentation, PresentationExportArguments(action.artifact, *file_name)});
    if (!exported.Succeeded()) co_return Completion::Failure(exported.Error());
    co_return Completion::Success({.summary = "Presentation exported"});
  }
  if (checkpoint && (checkpoint->schema_version != 1 ||
      (checkpoint->stage != "brief" && checkpoint->stage != "outline"))) {
    co_return Completion::Failure({.code = kit::ErrorCode::Unsupported, .message = "Unknown presentation checkpoint"});
  }
  if (!checkpoint) {
    auto saved = co_await context.SaveCheckpointAsync({.stage = "brief", .schema_version = 1});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }
  std::optional<std::size_t> slide_count;
  if (input.goal) {
    auto definition = co_await context.Goals().ReadDefinitionAsync(*input.goal);
    if (!definition.Succeeded()) co_return Completion::Failure(definition.Error());
    slide_count = ReleaseSlideCount(definition.Value().completion_conditions);
  }
  std::optional<kit::ArtifactRef> screenshot;
  for (const auto& source : input.input.context) {
    const auto* ref = std::get_if<kit::ArtifactRef>(&source.source);
    if (!ref || !IsVerificationScreenshot(*ref)) continue;
    if (screenshot) co_return Completion::Failure({.code = kit::ErrorCode::InvalidArgument,
        .message = "Choose one implementation screenshot for this presentation"});
    screenshot = *ref;
  }

  JsonValue outline;
  if (checkpoint && checkpoint->stage == "outline") {
    outline = std::move(checkpoint->state);
  } else {
    JsonValue::Object brief_fields{{"kind", JsonValue{std::string(PresentationBrief)}}};
    if (slide_count) brief_fields.emplace("slide_count", JsonValue{static_cast<std::int64_t>(*slide_count)});
    auto brief = co_await Confirm(context, "brief", {
        .title = "Presentation brief",
        .schema = StringFields({"audience", "style", "length", "reference_notes"}),
        .presentation = JsonValue{std::move(brief_fields)}});
    if (!brief.Succeeded()) co_return Completion::Failure(brief.Error());
    auto instruction = "Draft a slide outline for this topic: " + input.input.text +
        "\nAudience: " + AnswerText(brief.Value(), "audience") +
        "\nStyle: " + AnswerText(brief.Value(), "style") +
        "\nTarget length: " + (slide_count ? std::to_string(*slide_count) : AnswerText(brief.Value(), "length")) +
        "\nUser-provided reference notes: " + AnswerText(brief.Value(), "reference_notes") +
        "\nReturn a JSON object with title and slides. Each slide has title and bullets. "
        "Use 1 to 30 slides and up to 6 concise bullets per slide.";
    if (screenshot) instruction += " Put the verification conclusions on the last slide, next to the implementation screenshot.";
    if (slide_count) instruction += " Required slide count: " + std::to_string(*slide_count) + ".";
    if (skill) {
      auto layout = co_await context.ReadSkillTextForModelAsync(skill->skill, std::string(ReleaseTemplatePath));
      if (!layout.Succeeded()) co_return Completion::Failure(layout.Error());
      instruction += "\nUse the following packaged release outline template as a guide:\n" + std::move(layout).Value() +
          "\nVerification report (source material):\n" + skill->report;
    }
    auto response = co_await context.CompleteAsync({
        .instruction = std::move(instruction), .output_schema = PresentationOutlineSchema()});
    if (!response.Succeeded()) co_return Completion::Failure(response.Error());
    auto verified = ReadPresentationOutline(*response.Value().structured);
    if (!verified.Succeeded()) co_return Completion::Failure(verified.Error());
    if (slide_count && verified.Value().slides.size() != *slide_count) co_return Completion::Failure({
        .code = kit::ErrorCode::InvalidArgument, .message = "The model did not return the slide count required by this goal"});
    outline = std::move(*response.Value().structured);
    auto saved = co_await context.SaveCheckpointAsync({.stage = "outline", .schema_version = 1, .state = outline});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }

  JsonValue::Object review{{"kind", JsonValue{std::string(PresentationOutlineReview)}}, {"outline", std::move(outline)}};
  if (screenshot) review.emplace("includes_screenshot", JsonValue{true});
  if (slide_count) review.emplace("slide_count", JsonValue{static_cast<std::int64_t>(*slide_count)});
  for (const auto& source : input.input.context) {
    const auto* ref = std::get_if<kit::SharedContextRef>(&source.source);
    if (!ref || !source.parts.empty() || source.text_range) continue;
    auto item = co_await context.Projects().ReadContextAsync(*ref);
    if (!item.Succeeded()) co_return Completion::Failure(item.Error());
    if (const auto report = ReadImportedReport(item.Value())) {
      review.emplace("report_name", JsonValue{report->name});
      review.emplace("report_summary", JsonValue{report->summary});
      break;
    }
  }
  auto approved = co_await Confirm(context, "outline", {
      .title = "Review and edit outline", .schema = PresentationOutlineSchema(),
      .presentation = JsonValue{std::move(review)}});
  if (!approved.Succeeded()) co_return Completion::Failure(approved.Error());
  if (slide_count) {
    auto outline = ReadPresentationOutline(approved.Value());
    if (!outline.Succeeded()) co_return Completion::Failure(outline.Error());
    if (outline.Value().slides.size() != *slide_count) co_return Completion::Failure({
        .code = kit::ErrorCode::Conflict, .message = "The approved outline does not have the goal's slide count"});
  }

  auto generated = co_await Invoke(context, "generate", {GeneratePresentation, approved.Value()});
  if (!generated.Succeeded()) co_return Completion::Failure(generated.Error());
  if (generated.Value().published_artifacts.size() != 1) {
    co_return Completion::Failure({.code = kit::ErrorCode::InvalidState, .message = "Generation did not return one staged PPTX"});
  }
  const auto artifact = generated.Value().published_artifacts.front();
  if (input.trigger == kit::RunTrigger::ForkComparison)
    co_return Completion::Success({.summary = "Presentation candidate staged", .artifacts = {artifact}});
  JsonValue::Object preview{{"kind", JsonValue{std::string(PresentationExport)}}, {"outline", approved.Value()},
      {"directory", JsonValue{std::move(export_directory)}}};
  if (screenshot) {
    preview.emplace("screenshot_id", JsonValue{screenshot->id.value});
    preview.emplace("screenshot_version", JsonValue{static_cast<std::int64_t>(screenshot->version)});
  }
  auto export_to = co_await Confirm(context, "destination", {
      .title = "Preview slides and choose a file name", .schema = StringFields({"file_name"}), .presentation = JsonValue{std::move(preview)}});
  if (!export_to.Succeeded()) co_return Completion::Failure(export_to.Error());

  auto exported = co_await Invoke(context, "export", {
      ExportPresentation, PresentationExportArguments(artifact, AnswerText(export_to.Value(), "file_name"))});
  if (!exported.Succeeded()) co_return Completion::Failure(exported.Error());
  if (input.task) {
    auto finished = co_await FinishProjectTask(context, *input.task, {artifact});
    if (!finished.Succeeded()) co_return Completion::Failure(finished.Error());
  }
  if (input.work_request) {
    auto reply = co_await context.Collaboration().ReplyAsync(StageCommand(context, "reply"), *input.work_request,
        {.status = kit::ReplyStatus::Succeeded, .summary = "Presentation exported", .artifacts = {artifact}});
    if (!reply.Succeeded()) co_return Completion::Failure(reply.Error());
    if (!std::holds_alternative<kit::MessageAccepted>(reply.Value().outcome)) {
      co_return Completion::Failure({.code = kit::ErrorCode::InvalidState, .message = "PPT reply is waiting for approval"});
    }
  }
  co_return Completion::Success({.summary = "Presentation exported", .artifacts = {artifact}});
}

} // namespace agentkit_preview
