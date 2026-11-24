#include "presentation_workflow.h"

#include "presentation_tools.h"
#include "workflow_sessions.h"
#include "workspace_evidence.h"
#include "release_workflow.h"
#include "release_skill.h"

#include <ranges>

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

bool OutlineReviewLine(std::string_view line) {
  const auto begin = line.find_first_not_of(" \t\r");
  if (begin == line.npos) return false;
  line = line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1);
  return line == "Review the outline first" || line == "\xe5\x85\x88\xe5\xae\xa1\xe9\x98\x85\xe5\xa4\xa7\xe7\xba\xb2";
}

} // namespace

std::string PresentationTopic(std::string_view input) {
  std::string topic;
  for (auto line : input | std::views::split('\n')) {
    const std::string_view text(line.begin(), line.end());
    if (OutlineReviewLine(text)) continue;
    if (!topic.empty()) topic.push_back('\n');
    topic.append(text);
  }
  return topic;
}

bool HasOutlineReviewRequest(std::string_view input) {
  for (auto line : input | std::views::split('\n')) {
    if (OutlineReviewLine(std::string_view(line.begin(), line.end()))) return true;
  }
  return false;
}

std::string ToggleOutlineReviewRequest(std::string input, std::string_view label) {
  if (HasOutlineReviewRequest(input)) return PresentationTopic(input);
  if (!input.empty() && !input.ends_with('\n')) input.push_back('\n');
  input.append(label);
  return input;
}

kit::Async<kit::RunCompletion> RunPresentation(
    kit::RunContext context, kit::RunInput input,
    std::optional<kit::RunCheckpoint> checkpoint, std::string export_directory,
    std::optional<PresentationSkillEvidence> skill, bool image_generation) {
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
  const auto stage = checkpoint ? std::string_view(checkpoint->stage) : std::string_view{};
  if (checkpoint && (checkpoint->schema_version != 1 ||
      (stage != "brief" && stage != "draft" && stage != "outline" && stage != "review-outline" && stage != "generate"))) {
    co_return Completion::Failure({.code = kit::ErrorCode::Unsupported, .message = "Unknown presentation checkpoint"});
  }
  std::optional<std::size_t> slide_count;
  if (input.goal) {
    auto definition = co_await context.Goals().ReadDefinitionAsync(*input.goal);
    if (!definition.Succeeded()) co_return Completion::Failure(definition.Error());
    slide_count = ReleaseSlideCount(definition.Value().completion_conditions);
  }
  const bool managed = slide_count.has_value() || skill.has_value() || input.work_request.has_value() ||
      input.trigger == kit::RunTrigger::ForkComparison;
  const bool needs_brief = stage == "brief" || (!checkpoint && managed);
  const bool review_outline = managed || stage == "outline" || stage == "review-outline" ||
      (stage != "generate" && HasOutlineReviewRequest(input.input.text));
  if (!checkpoint) {
    auto saved = co_await context.SaveCheckpointAsync({.stage = needs_brief ? "brief" : "draft", .schema_version = 1});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }
  std::optional<kit::ArtifactRef> screenshot;
  for (const auto& source : input.input.context) {
    const auto* ref = std::get_if<kit::ArtifactRef>(&source.source);
    if (!ref || !IsVerificationScreenshot(*ref)) continue;
    if (screenshot) co_return Completion::Failure({.code = kit::ErrorCode::InvalidArgument,
        .message = "Choose one implementation screenshot for this presentation"});
    screenshot = *ref;
  }

  const auto topic = review_outline ? PresentationTopic(input.input.text) : input.input.text;
  const auto data = ReadSuppliedChart(topic);
  JsonValue outline;
  if (stage == "outline" || stage == "review-outline" || stage == "generate") {
    outline = std::move(checkpoint->state);
  } else {
    auto instruction = "Draft a slide outline for this topic: " + topic + "\n";
    if (needs_brief) {
      JsonValue::Object brief_fields{{"kind", JsonValue{std::string(PresentationBrief)}}};
      if (slide_count) brief_fields.emplace("slide_count", JsonValue{static_cast<std::int64_t>(*slide_count)});
      auto brief = co_await Confirm(context, "brief", {
          .title = "Presentation brief",
          .schema = StringFields({"audience", "style", "length", "reference_notes"}),
          .presentation = JsonValue{std::move(brief_fields)}});
      if (!brief.Succeeded()) co_return Completion::Failure(brief.Error());
      instruction += "Audience: " + AnswerText(brief.Value(), "audience") +
          "\nStyle: " + AnswerText(brief.Value(), "style") +
          "\nTarget length: " + (slide_count ? std::to_string(*slide_count) : AnswerText(brief.Value(), "length")) +
          "\nUser-provided reference notes: " + AnswerText(brief.Value(), "reference_notes") + "\n";
    } else {
      instruction += "Honor any audience, style, language, length and source requirements in the request. "
          "Infer unspecified choices from the topic and supplied sources. For a general presentation, aim for 6 to 8 slides. "
          "Write in the user's language. Give each slide one takeaway and build toward a concrete next step. "
          "Choose a layout for each slide: editorial for a text-only argument or action, chart for a relevant numerical comparison, "
          "flow for 2 to 4 ordered steps, relationship for a 4-node feedback loop. Cover, cards, comparison and list are also available. "
          "Vary the composition according to the material. Make every title a standalone message. "
          "Keep flow and relationship nodes to 12 Chinese characters or 5 words, without numbered prefixes. "
          "Use at most 3 short takeaways beside a chart. For chart, media is a dataset ID from the supplied evidence. "
          "Use no unsupported numbers or citations. Set media to an empty string for text and diagrams.\n";
      if (image_generation) {
        instruction += "Use layout image when a visual metaphor or scene communicates the point better than a diagram. "
            "Give each image slide a distinct media prompt describing composition, subject and palette in English. "
            "The image service will generate the illustration. Do not use URLs or tell the user to supply images. "
            "Unify the artwork and keep opening and closing titles under 18 Chinese characters or 7 English words. "
            "Use short supporting lines on these pages, up to 22 Chinese characters or 10 English words per line. "
            "If showcasing open-source culture, use an illustrated opening, a middle illustration and a distinct closing image, "
            "a sourced GitHub comparison, a feedback loop, an actionable native flow and a text-only slide when the topic allows.\n";
      } else {
        instruction += "Image generation is not configured. Use the other available layouts.\n";
      }
      instruction += DescribePresentationData(data);
    }
    instruction += "Return a JSON object with title and slides. Use 1 to 30 slides and up to 6 concise bullets per slide.";
    if (screenshot) instruction += " Put the verification conclusions on the last slide, next to the implementation screenshot.";
    if (slide_count) instruction += " Required slide count: " + std::to_string(*slide_count) + ".";
    if (skill) {
      auto layout = co_await context.ReadSkillTextForModelAsync(skill->skill, std::string(ReleaseTemplatePath));
      if (!layout.Succeeded()) co_return Completion::Failure(layout.Error());
      instruction += "\nUse the following packaged release outline template as a guide:\n" + std::move(layout).Value() +
          "\nVerification report (source material):\n" + skill->report;
    }
    auto response = co_await context.CompleteAsync({
        .instruction = std::move(instruction),
        .output_schema = PresentationOutlineSchema(needs_brief ? SlideLayoutField::Omit : SlideLayoutField::Visual)});
    if (!response.Succeeded()) co_return Completion::Failure(response.Error());
    if (!response.Value().structured) co_return Completion::Failure({
        .code = kit::ErrorCode::ProviderFailure, .message = "The model returned no structured presentation outline"});
    auto verified = ReadPresentationOutline(*response.Value().structured);
    if (!verified.Succeeded()) co_return Completion::Failure(verified.Error());
    if (slide_count && verified.Value().slides.size() != *slide_count) co_return Completion::Failure({
        .code = kit::ErrorCode::InvalidArgument, .message = "The model did not return the slide count required by this goal"});
    verified.Value().supplied_data = data;
    for (const auto& slide : verified.Value().slides) {
      if (slide.layout == SlideLayout::Chart && !ChartFor(verified.Value(), slide))
        co_return Completion::Failure({.code = kit::ErrorCode::ProviderFailure, .message = "The model chose a chart without an available dataset"});
      if (slide.layout == SlideLayout::Image && (!image_generation || !slide.media || slide.media->empty()))
        co_return Completion::Failure({.code = kit::ErrorCode::ProviderFailure, .message = "The model chose an image without a configured image service and prompt"});
    }
    outline = std::move(*response.Value().structured);
    auto saved = co_await context.SaveCheckpointAsync({
        .stage = managed ? "outline" : review_outline ? "review-outline" : "generate", .schema_version = 1, .state = outline});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }

  if (review_outline) {
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
        .title = "Review and edit outline", .schema = PresentationOutlineSchema(SlideLayoutField::Optional),
        .presentation = JsonValue{std::move(review)}});
    if (!approved.Succeeded()) co_return Completion::Failure(approved.Error());
    outline = std::move(approved).Value();
  }
  auto approved_deck = ReadPresentationOutline(outline);
  if (!approved_deck.Succeeded()) co_return Completion::Failure(approved_deck.Error());
  if (slide_count && approved_deck.Value().slides.size() != *slide_count)
    co_return Completion::Failure({.code = kit::ErrorCode::Conflict, .message = "The approved outline does not have the goal's slide count"});
  approved_deck.Value().supplied_data = data;
  auto generated = co_await Invoke(context, "generate", {GeneratePresentation, WritePresentationOutline(approved_deck.Value())});
  if (!generated.Succeeded()) co_return Completion::Failure(generated.Error());
  if (generated.Value().published_artifacts.size() != 1) {
    co_return Completion::Failure({.code = kit::ErrorCode::InvalidState, .message = "Generation did not return one staged PPTX"});
  }
  const auto artifact = generated.Value().published_artifacts.front();
  if (input.trigger == kit::RunTrigger::ForkComparison)
    co_return Completion::Success({.summary = "Presentation candidate staged", .artifacts = {artifact}});
  if (!managed && stage != "outline")
    co_return Completion::Success({.summary = "Presentation ready to preview", .artifacts = {artifact}});
  if (!generated.Value().model_value) co_return Completion::Failure({
      .code = kit::ErrorCode::InvalidState, .message = "The staged presentation has no slide preview"});
  auto preview_outline = std::move(*generated.Value().model_value);
  JsonValue::Object preview{{"kind", JsonValue{std::string(PresentationExport)}}, {"outline", std::move(preview_outline)},
      {"directory", JsonValue{std::move(export_directory)}}, {"pptx_id", JsonValue{artifact.id.value}},
      {"pptx_version", JsonValue{static_cast<std::int64_t>(artifact.version)}}};
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
