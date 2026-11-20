#include "presentation_tools.h"
#include "general_workflow.h"
#include "workspace_evidence.h"

#include <huxerui/archive.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <set>
#include <type_traits>

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using namespace huxerui;
using kit::JsonValue;
constexpr std::string_view id_prefix = "presentation:";
constexpr std::string_view version_resource = "preview.presentation/";
constexpr std::string_view adoption_handle = "adopted-";
constexpr std::string_view imported_report = "preview.imported-presentation-report";
constexpr std::string_view screenshot_part_suffix = "/picture-6";
constexpr std::string_view screenshot_entry = "ppt/media/implementation.png";

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

template<class Operation, class... Args>
Task<std::invoke_result_t<Operation, Args...>> ReadPackageAsync(Operation operation, Args... args) {
#ifdef __EMSCRIPTEN__
  co_return std::invoke(std::move(operation), std::move(args)...);
#else
  co_return co_await RunWorker(std::move(operation), std::move(args)...);
#endif
}

#ifdef __EMSCRIPTEN__
kit::Async<void> PersistPackageAsync(File source, File destination, std::stop_token stop) {
  auto prior = co_await destination.StatAsync();
  if (prior.Succeeded()) co_return Fail<void>(kit::ErrorCode::Conflict, "PowerPoint destination already exists");
  if (prior.Error().code != IoErrorCode::NotFound)
    co_return Fail<void>(kit::ErrorCode::ProviderFailure, "Could not check the PowerPoint destination");
  if (stop.stop_requested()) co_return Fail<void>(kit::ErrorCode::AlreadyClosed, "PowerPoint write cancelled");
  if (co_await source.CopyToAsync(destination)) co_return kit::AgentResult<void>::Success();
  const auto after = co_await destination.StatAsync();
  if (after.Succeeded() || after.Error().code != IoErrorCode::NotFound)
    co_return Fail<void>(kit::ErrorCode::ResultUnknown, "PowerPoint file persistence could not be confirmed");
  co_return Fail<void>(kit::ErrorCode::ProviderFailure, "Could not persist the PowerPoint file");
}
#endif

template<class Writer>
kit::Async<void> WritePackageAsync(const PresentationFiles& files, File destination, std::stop_token stop, Writer write) {
#ifdef __EMSCRIPTEN__
  if (!files.temporary) co_return Fail<void>(kit::ErrorCode::Unsupported, "PowerPoint temporary storage is unavailable");
  if (stop.stop_requested()) co_return Fail<void>(kit::ErrorCode::AlreadyClosed, "PowerPoint write cancelled");
  const auto scratch = files.temporary->Child("ppt-" + kit::NewCommandId().value + ".scratch");
  const auto remove = [](const File* file) { static_cast<void>(file->Delete()); };
  const std::unique_ptr<const File, decltype(remove)> cleanup(&scratch, remove);
  auto built = write(scratch);
  if (!built.Succeeded()) co_return built;
  co_return co_await PersistPackageAsync(scratch, std::move(destination), stop);
#else
  co_return co_await RunWorker(std::move(write), std::move(destination));
#endif
}

bool SafeHandle(std::string_view handle) {
  return !handle.empty() && handle.size() <= 128 && std::ranges::all_of(handle, [](char ch) {
    return (ch >= '0' && ch <= '9') || ch == '-';
  });
}

std::optional<std::string> Handle(const kit::ArtifactRef& ref) {
  if (!ref.version || !ref.id.value.starts_with(id_prefix)) return std::nullopt;
  auto handle = ref.id.value.substr(id_prefix.size());
  if (!SafeHandle(handle)) return std::nullopt;
  if (ref.version > 1) handle += "-v" + std::to_string(ref.version);
  return handle;
}

File PptxFile(const PresentationFiles& files, std::string_view handle) {
  return files.staging.Child(std::string(handle) + ".pptx");
}

std::optional<File> ResolvePptx(const PresentationFiles& files, const kit::ArtifactResolveRequest& request) {
  const auto canonical = Handle(request.artifact);
  if (!canonical) return {};
  const std::string_view handle = request.storage_handle;
  if (handle == *canonical || (request.artifact.version > 1 && handle.starts_with(adoption_handle) &&
                               SafeHandle(handle.substr(adoption_handle.size()))))
    return PptxFile(files, handle);
  return {};
}

std::optional<kit::ArtifactRef> TargetVersion(const kit::TargetBaseline& target) {
  if (!target.expected || target.expected->id != target.target || !target.target.value.starts_with(version_resource)) return {};
  kit::ArtifactRef ref{{target.target.value.substr(version_resource.size())}, 0};
  const auto& version = target.expected->version;
  const auto parsed = std::from_chars(version.data(), version.data() + version.size(), ref.version);
  if (parsed.ec != std::errc{} || parsed.ptr != version.data() + version.size() || !Handle(ref)) return {};
  return ref;
}

std::string_view Xml(const Bytes& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::size_t SlideCount(std::string_view xml) {
  constexpr std::string_view entry = "<p:sldId ";
  std::size_t count = 0;
  for (auto offset = xml.find(entry); offset != xml.npos; offset = xml.find(entry, offset + entry.size())) ++count;
  return count;
}

std::optional<std::string> DecodeSlideText(std::string_view xml) {
  std::string text;
  while (!xml.empty()) {
    const auto escaped = xml.find_first_of("&<");
    text += xml.substr(0, escaped);
    if (escaped == xml.npos) break;
    xml.remove_prefix(escaped);
    const auto end = xml.find(';');
    if (end == xml.npos) return {};
    const auto entity = xml.substr(0, end + 1);
    if (entity == "&amp;") text += '&';
    else if (entity == "&lt;") text += '<';
    else if (entity == "&gt;") text += '>';
    else if (entity == "&quot;") text += '"';
    else if (entity == "&apos;") text += '\'';
    else return {};
    xml.remove_prefix(entity.size());
  }
  return text;
}

bool HasSlideScreenshot(archive::ArchiveReader& package, std::string_view stem, std::string_view slide) {
  if (slide.find("<p:pic><p:nvPicPr><p:cNvPr id=\"6\"") == slide.npos ||
      slide.find("<a:blip r:embed=\"rId2\"/>") == slide.npos) return false;
  const auto relationships = package.ReadEntry("ppt/slides/_rels/" + std::string(stem) + ".rels");
  if (!relationships.Succeeded() || Xml(relationships.Value()).find(
      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\""
      " Target=\"../media/implementation.png\"/>") == std::string_view::npos) return false;
  const auto entries = package.ListEntries();
  return entries.Succeeded() && std::ranges::count(entries.Value(), screenshot_entry, &archive::ArchiveEntry::path) == 1;
}

struct StagedSlides {
  PresentationOutline outline;
  std::vector<bool> screenshots;
};

kit::AgentResult<StagedSlides> ReadStagedSlides(File file, const std::vector<kit::PartId>& parts) {
  auto archive = archive::ArchiveReader::Open(file, {.max_entries = 256, .max_output_bytes = 256 * 1024});
  if (!archive.Succeeded())
    return Fail<StagedSlides>(kit::ErrorCode::InvalidState, "The staged PowerPoint package could not be read");
  auto manifest = archive.Value().ReadEntry("ppt/presentation.xml");
  const auto count = manifest.Succeeded() ? SlideCount(Xml(manifest.Value())) : 0;
  if (count == 0 || count > 30)
    return Fail<StagedSlides>(kit::ErrorCode::InvalidState, "The staged PowerPoint package has an invalid slide list");
  std::vector<std::size_t> pages;
  if (parts.empty()) {
    for (std::size_t i = 1; i <= count; ++i) pages.push_back(i);
  } else {
    for (const auto& part : parts) {
      const auto page = PresentationPartPage(part);
      if (!page || *page > count) return Fail<StagedSlides>(kit::ErrorCode::NotFound, "The selected slide is unavailable");
      pages.push_back(*page);
    }
  }
  StagedSlides result;
  for (const auto page : pages) {
    const auto stem = "slide" + std::to_string(page) + ".xml";
    auto entry = archive.Value().ReadEntry("ppt/slides/" + stem);
    if (!entry.Succeeded())
      return Fail<StagedSlides>(kit::ErrorCode::NotFound, "The selected slide is missing from the PowerPoint package");
    auto xml = Xml(entry.Value());
    result.screenshots.push_back(HasSlideScreenshot(archive.Value(), stem, xml));
    std::vector<std::string> paragraphs;
    for (auto begin = xml.find("<a:t>"); begin != xml.npos; begin = xml.find("<a:t>")) {
      xml.remove_prefix(begin + 5);
      const auto end = xml.find("</a:t>");
      auto decoded = end == xml.npos ? std::nullopt : DecodeSlideText(xml.substr(0, end));
      if (!decoded || paragraphs.size() >= 8)
        return Fail<StagedSlides>(kit::ErrorCode::Unsupported, "This slide does not use the Preview text layout");
      paragraphs.push_back(std::move(*decoded));
      xml.remove_prefix(end + 6);
    }
    if (paragraphs.size() < 2)
      return Fail<StagedSlides>(kit::ErrorCode::Unsupported, "This slide does not use the Preview text layout");
    if (result.outline.slides.empty()) result.outline.title = paragraphs.front();
    SlideOutline slide{.title = std::move(paragraphs[1])};
    for (std::size_t i = 2; i < paragraphs.size(); ++i) slide.bullets.push_back(std::move(paragraphs[i]));
    result.outline.slides.push_back(std::move(slide));
  }
  const auto checked = ReadPresentationOutline(WritePresentationOutline(result.outline));
  if (!checked.Succeeded()) return kit::AgentResult<StagedSlides>::Failure(checked.Error());
  return kit::AgentResult<StagedSlides>::Success(std::move(result));
}

kit::AgentResult<ImageAsset> ReadPackageScreenshot(Bytes bytes, File scratch, std::size_t page) {
  auto output = scratch.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew);
  if (!output.Succeeded()) return Fail<ImageAsset>(kit::ErrorCode::ProviderFailure, "Could not open the presentation for review");
  const auto remove = [](const File* file) { static_cast<void>(file->Delete()); };
  const std::unique_ptr<const File, decltype(remove)> cleanup(&scratch, remove);
  const auto written = output.Value().WriteAt(0, bytes);
  const auto closed = output.Value().Close();
  if (!written.Succeeded() || !closed.Succeeded())
    return Fail<ImageAsset>(kit::ErrorCode::ProviderFailure, "Could not open the presentation for review");
  auto package = archive::ArchiveReader::Open(scratch, {.max_entries = 256, .max_output_bytes = 8 * 1024 * 1024});
  if (!package.Succeeded()) return Fail<ImageAsset>(kit::ErrorCode::InvalidState, "The staged PowerPoint package could not be read");
  const auto stem = "slide" + std::to_string(page) + ".xml";
  const auto slide = package.Value().ReadEntry("ppt/slides/" + stem);
  if (!slide.Succeeded() || !HasSlideScreenshot(package.Value(), stem, Xml(slide.Value())))
    return Fail<ImageAsset>(kit::ErrorCode::NotFound, "The implementation screenshot is unavailable on this slide");
  auto image = package.Value().ReadEntry(screenshot_entry);
  if (!image.Succeeded()) return Fail<ImageAsset>(kit::ErrorCode::NotFound, "The embedded screenshot could not be read");
  return DecodeVerificationScreenshot(std::move(image).Value());
}

const JsonValue* Field(const JsonValue::Object& object, const char* key) {
  const auto found = object.find(key);
  return found == object.end() ? nullptr : &found->second;
}

std::optional<kit::ArtifactRef> ReadPptxReference(const JsonValue& data) {
  const auto* object = std::get_if<JsonValue::Object>(&data.value);
  if (!object) return {};
  const auto* id = Field(*object, "artifact_id");
  const auto* version = Field(*object, "version");
  const auto* name = id ? std::get_if<std::string>(&id->value) : nullptr;
  const auto* revision = version ? std::get_if<std::int64_t>(&version->value) : nullptr;
  if (!name || !revision || *revision <= 0) return {};
  kit::ArtifactRef ref{{*name}, static_cast<kit::Revision>(*revision)};
  return Handle(ref) ? std::optional{std::move(ref)} : std::nullopt;
}

JsonValue PptxReference(const kit::ArtifactRef& ref) {
  return {JsonValue::Object{{"artifact_id", JsonValue{ref.id.value}},
      {"version", JsonValue{static_cast<std::int64_t>(ref.version)}}}};
}

kit::AgentResult<void> ReworkStagedPresentation(Bytes source, File scratch, std::optional<File> destination,
    std::vector<PptxTextChange> edits, std::stop_token stop) {
  if (stop.stop_requested()) return Fail<void>(kit::ErrorCode::AlreadyClosed, "Presentation rework cancelled");
  const auto remove = [&scratch](const File*) { static_cast<void>(scratch.Delete()); };
  const std::unique_ptr<const File, decltype(remove)> cleanup(&scratch, remove);
  if (!scratch.WriteBytes(source))
    return Fail<void>(kit::ErrorCode::ProviderFailure, "The staged PowerPoint package could not be prepared for rework");
  return ReworkPptxText(scratch, destination, edits, stop);
}

kit::Async<void> ReworkPackageAsync(const PresentationFiles& files, Bytes source, std::optional<File> destination,
    std::vector<PptxTextChange> edits, std::stop_token stop) {
#ifdef __EMSCRIPTEN__
  if (!files.temporary) co_return Fail<void>(kit::ErrorCode::Unsupported, "PowerPoint temporary storage is unavailable");
  const auto& working = *files.temporary;
#else
  const auto& working = files.staging;
#endif
  auto scratch = working.Child("rework-" + kit::NewCommandId().value + ".scratch");
  if (!destination) co_return co_await ReadPackageAsync(ReworkStagedPresentation, std::move(source), std::move(scratch),
      std::optional<File>{}, std::move(edits), stop);
  auto write = [source = std::move(source), scratch = std::move(scratch), edits = std::move(edits), stop](File output) mutable {
    return ReworkStagedPresentation(std::move(source), std::move(scratch), std::move(output), std::move(edits), stop);
  };
  co_return co_await WritePackageAsync(files, std::move(*destination), stop, std::move(write));
}

kit::Async<std::string> ReportConclusion(std::shared_ptr<kit::AgentRuntime> runtime, const kit::SourceSlice& source) {
  using Result = kit::AgentResult<std::string>;
  if (!source.parts.empty() || source.text_range)
    co_return Fail<std::string>(kit::ErrorCode::Unsupported, "Choose a complete workspace verification report");
  if (const auto* shared = std::get_if<kit::SharedContextRef>(&source.source)) {
    auto item = co_await runtime->Projects().ReadContextAsync(*shared);
    if (!item.Succeeded()) co_return Result::Failure(item.Error());
    const auto report = ReadImportedReport(item.Value());
    if (!report) co_return Fail<std::string>(kit::ErrorCode::Unsupported, "Choose a published project report");
    co_return Result::Success(report->summary);
  }
  const auto* artifact = std::get_if<kit::ArtifactRef>(&source.source);
  if (!artifact) co_return Fail<std::string>(kit::ErrorCode::Unsupported, "Choose a verification report");
  auto record = co_await runtime->Artifacts().ReadAsync(*artifact);
  if (!record.Succeeded()) co_return kit::AgentResult<std::string>::Failure(record.Error());
  if (record.Value().metadata.type != VerificationEvidence)
    co_return Fail<std::string>(kit::ErrorCode::Unsupported, "Choose a workspace verification report");
  auto rendered = co_await runtime->Artifacts().PreviewAsync(*artifact);
  if (!rendered.Succeeded()) co_return kit::AgentResult<std::string>::Failure(rendered.Error());
  co_return kit::AgentResult<std::string>::Success(std::move(rendered).Value().summary);
}

JsonValue PptxPlan(const kit::ArtifactRef& baseline, const std::vector<PptxTextChange>& edits) {
  JsonValue::Array rows;
  for (const auto& edit : edits) {
    rows.push_back(JsonValue{JsonValue::Object{{"slide", JsonValue{static_cast<std::int64_t>(edit.slide)}},
        {"previous", JsonValue{edit.previous}}, {"replacement", JsonValue{edit.replacement}}}});
  }
  return {JsonValue::Object{{"artifact_id", JsonValue{baseline.id.value}},
      {"version", JsonValue{static_cast<std::int64_t>(baseline.version)}}, {"edits", JsonValue{std::move(rows)}}}};
}

struct PptxReworkPlan {
  kit::ArtifactRef baseline;
  std::vector<PptxTextChange> edits;
};

kit::AgentResult<PptxReworkPlan> ReadPptxPlan(const JsonValue& data) {
  const auto* object = std::get_if<JsonValue::Object>(&data.value);
  if (!object) return Fail<PptxReworkPlan>(kit::ErrorCode::InvalidArgument, "Invalid presentation rework plan");
  const auto* id = Field(*object, "artifact_id");
  const auto* version = Field(*object, "version");
  const auto* edits = Field(*object, "edits");
  const auto* name = id ? std::get_if<std::string>(&id->value) : nullptr;
  const auto* revision = version ? std::get_if<std::int64_t>(&version->value) : nullptr;
  const auto* rows = edits ? std::get_if<JsonValue::Array>(&edits->value) : nullptr;
  if (!name || !revision || *revision <= 0 || !rows || rows->empty())
    return Fail<PptxReworkPlan>(kit::ErrorCode::InvalidArgument, "Invalid presentation rework plan");
  PptxReworkPlan plan{{{*name}, static_cast<kit::Revision>(*revision)}, {}};
  for (const auto& row : *rows) {
    const auto* fields = std::get_if<JsonValue::Object>(&row.value);
    if (!fields) return Fail<PptxReworkPlan>(kit::ErrorCode::InvalidArgument, "Invalid slide rework plan");
    const auto* slide = Field(*fields, "slide");
    const auto* previous = Field(*fields, "previous");
    const auto* replacement = Field(*fields, "replacement");
    const auto* number = slide ? std::get_if<std::int64_t>(&slide->value) : nullptr;
    const auto* before = previous ? std::get_if<std::string>(&previous->value) : nullptr;
    const auto* after = replacement ? std::get_if<std::string>(&replacement->value) : nullptr;
    if (!number || *number < 1 || *number > 30 || !before || !after)
      return Fail<PptxReworkPlan>(kit::ErrorCode::InvalidArgument, "Invalid slide rework plan");
    plan.edits.push_back({static_cast<std::size_t>(*number), *before, *after});
  }
  return kit::AgentResult<PptxReworkPlan>::Success(std::move(plan));
}

struct ExportRequest {
  kit::ArtifactRef source;
  File target;
};

kit::AgentResult<ExportRequest> ReadExport(const PresentationFiles& files, const JsonValue& value) {
  const auto* object = std::get_if<JsonValue::Object>(&value.value);
  if (!object) return Fail<ExportRequest>(kit::ErrorCode::InvalidArgument, "Expected presentation export fields");
  const auto* name = Field(*object, "file_name");
  const auto* id = Field(*object, "artifact_id");
  const auto* version = Field(*object, "version");
  const auto* file_name = name ? std::get_if<std::string>(&name->value) : nullptr;
  const auto* artifact_id = id ? std::get_if<std::string>(&id->value) : nullptr;
  const auto* artifact_version = version ? std::get_if<std::int64_t>(&version->value) : nullptr;
  if (!artifact_id || !artifact_version || *artifact_version < 1) {
    return Fail<ExportRequest>(kit::ErrorCode::InvalidArgument, "Choose a staged PPTX version");
  }
  kit::ArtifactRef ref{{*artifact_id}, static_cast<kit::Revision>(*artifact_version)};
  if (!Handle(ref)) return Fail<ExportRequest>(kit::ErrorCode::InvalidArgument, "Invalid staged presentation");
  auto valid = ValidatePresentationFilename(file_name ? std::string_view(*file_name) : std::string_view{});
  if (!valid.Succeeded()) return kit::AgentResult<ExportRequest>::Failure(valid.Error());
  return kit::AgentResult<ExportRequest>::Success({std::move(ref), files.exports.Child(*file_name)});
}

kit::JsonSchema ExportSchema() {
  const auto primitive = [](std::string type) { return JsonValue{JsonValue::Object{{"type", JsonValue{std::move(type)}}}}; };
  return {JsonValue{JsonValue::Object{
      {"type", JsonValue{std::string("object")}}, {"additionalProperties", JsonValue{false}},
      {"properties", JsonValue{JsonValue::Object{
          {"file_name", primitive("string")}, {"artifact_id", primitive("string")}, {"version", primitive("integer")}}}},
      {"required", JsonValue{JsonValue::Array{
          JsonValue{std::string("file_name")}, JsonValue{std::string("artifact_id")}, JsonValue{std::string("version")}}}}}}};
}

#ifndef __EMSCRIPTEN__
kit::AgentResult<kit::ToolExecutionResult> CopyPresentation(
    Bytes bytes, File target, kit::ArtifactRef artifact, std::stop_token stop) {
  using Result = kit::AgentResult<kit::ToolExecutionResult>;
  if (stop.stop_requested()) return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
  auto opened = target.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew);
  const kit::ResourceId resource{target.Path()};
  if (!opened.Succeeded()) {
    const bool exists = opened.Error().code == IoErrorCode::AlreadyExists;
    return Result::Success({.outcome = kit::ToolOutcome::Failed,
        .effects = {{.resource = resource, .state = kit::ResourceEffectState::Unchanged}},
        .reason = kit::AgentError{.code = exists ? kit::ErrorCode::Conflict : kit::ErrorCode::ProviderFailure,
                                  .message = exists ? "Export target already exists" : "Export target could not be opened"}});
  }
  auto written = opened.Value().WriteAt(0, bytes);
  auto closed = opened.Value().Close();
  if (!written.Succeeded() || !closed.Succeeded()) {
    return Result::Success({.outcome = kit::ToolOutcome::PartiallyApplied,
        .effects = {{.resource = resource, .state = kit::ResourceEffectState::Unknown}},
        .reason = kit::AgentError{.code = kit::ErrorCode::ResultUnknown,
                                  .message = "Export may contain a partial file; the staged PPTX is still available"}});
  }
  return Result::Success({.outcome = kit::ToolOutcome::Succeeded,
      .model_value = JsonValue{true},
      .blocks = {{kit::BlockId{kit::NewCommandId().value}, kit::TextBlock{"Saved " + target.Path(), false}}},
      .existing_artifacts = {artifact},
      .effects = {{.resource = resource, .state = kit::ResourceEffectState::Applied}}});
}
#else
kit::Async<kit::ToolExecutionResult> ExportBrowserPresentation(const PresentationFiles& files, Bytes bytes, File target,
    kit::ArtifactRef artifact, std::stop_token stop) {
  using Result = kit::AgentResult<kit::ToolExecutionResult>;
  if (stop.stop_requested()) co_return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
  auto write = [bytes = std::move(bytes)](File scratch) {
    return scratch.WriteBytes(bytes) ? kit::AgentResult<void>::Success()
        : Fail<void>(kit::ErrorCode::ProviderFailure, "Could not prepare the PowerPoint export");
  };
  auto saved = co_await WritePackageAsync(files, target, stop, std::move(write));
  if (!saved.Succeeded()) {
    const auto error = saved.Error();
    const auto uncertain = error.code == kit::ErrorCode::ResultUnknown;
    co_return Result::Success({.outcome = uncertain ? kit::ToolOutcome::ResultUnknown :
          error.code == kit::ErrorCode::AlreadyClosed ? kit::ToolOutcome::Cancelled : kit::ToolOutcome::Failed,
        .effects = {{.resource = {target.Path()},
            .state = uncertain ? kit::ResourceEffectState::Unknown : kit::ResourceEffectState::Unchanged}},
        .reason = error});
  }
  co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .model_value = JsonValue{true},
      .blocks = {{{}, kit::CustomBlock{BrowserPresentationExport, JsonValue{target.Name()},
          "Ready to download " + target.Name(), {}, {artifact}}}},
      .existing_artifacts = {artifact},
      .effects = {{.resource = {target.Path()}, .state = kit::ResourceEffectState::Applied}}});
}
#endif

} // namespace

const kit::ToolId GeneratePresentation{"preview.ppt.generate"};
const kit::ToolId ExportPresentation{"preview.ppt.export"};

std::optional<ImportedPresentationReport> ReadImportedReport(const kit::SharedContextItem& item) {
  if (item.state != kit::SharedContextState::Published) return {};
  return ReadImportedReportVersion(item);
}

std::optional<ImportedPresentationReport> ReadImportedReportVersion(const kit::SharedContextItem& item) {
  const auto* data = std::get_if<JsonValue::Object>(&item.data.content.value);
  if (item.state == kit::SharedContextState::Withdrawn || item.data.access_profile != imported_report || !data) return {};
  const auto* summary = Field(*data, "summary");
  const auto* text = summary ? std::get_if<std::string>(&summary->value) : nullptr;
  if (!text || text->empty() || text->size() > 720) return {};
  return ImportedPresentationReport{item.ref, item.data.title, *text};
}

kit::AgentResult<void> ValidatePresentationReportSummary(std::string_view summary) {
  PresentationOutline sample{"Report", {{"Summary", {std::string(summary)}}}};
  auto valid = ReadPresentationOutline(WritePresentationOutline(sample));
  if (!valid.Succeeded()) return kit::AgentResult<void>::Failure(valid.Error());
  return kit::AgentResult<void>::Success();
}

static kit::Async<ImportedPresentationReport> PublishPresentationReport(
    kit::ProjectService projects, kit::ProjectId project, std::string name, std::string text) {
  using Result = kit::AgentResult<ImportedPresentationReport>;
  const auto document = nlohmann::json::parse(text, nullptr, false);
  const auto summary = document.is_object() ? document.find("summary") : document.end();
  if (summary == document.end() || !summary->is_string())
    co_return Fail<ImportedPresentationReport>(kit::ErrorCode::InvalidArgument, "Choose a JSON report with a summary string");
  auto conclusion = summary->get<std::string>();
  auto valid = ValidatePresentationReportSummary(conclusion);
  if (!valid.Succeeded()) co_return Result::Failure(valid.Error());
  auto snapshot = co_await projects.ReadAsync(project);
  if (!snapshot.Succeeded()) co_return Result::Failure(snapshot.Error());
  kit::ProposeSharedContext proposal{snapshot.Value().revision,
      {name, JsonValue{JsonValue::Object{{"summary", JsonValue{conclusion}}}}, {}, std::string(imported_report)}};
  auto draft = co_await projects.UpdateContextAsync(project, kit::NewCommandId(), std::move(proposal));
  if (!draft.Succeeded()) co_return Result::Failure(draft.Error());
  const auto* ref = std::get_if<kit::SharedContextRef>(&draft.Value().outcome);
  if (!ref) co_return Fail<ImportedPresentationReport>(kit::ErrorCode::InvalidState, "The report is waiting for approval");
  auto review = co_await projects.UpdateContextAsync(project, kit::NewCommandId(), kit::ChangeSharedContext{*ref, kit::RequestContextReview{}});
  if (!review.Succeeded()) co_return Result::Failure(review.Error());
  if (!std::holds_alternative<kit::SharedContextRef>(review.Value().outcome))
    co_return Fail<ImportedPresentationReport>(kit::ErrorCode::InvalidState, "The report is waiting for review approval");
  auto published = co_await projects.UpdateContextAsync(project, kit::NewCommandId(), kit::ChangeSharedContext{*ref, kit::PublishContext{}});
  if (!published.Succeeded()) co_return Result::Failure(published.Error());
  const auto* ready = std::get_if<kit::SharedContextRef>(&published.Value().outcome);
  if (!ready) co_return Fail<ImportedPresentationReport>(kit::ErrorCode::InvalidState, "The report is waiting for publication approval");
  co_return Result::Success({*ready, std::move(name), std::move(conclusion)});
}

kit::Async<ImportedPresentationReport> ImportPresentationReport(kit::ProjectService projects, kit::ProjectId project,
    FileReference file) {
  constexpr auto limit = 64 * 1024;
  auto open = co_await file.OpenReadAsync();
  if (!open.Succeeded()) co_return Fail<ImportedPresentationReport>(kit::ErrorCode::PermissionDenied, "The selected report could not be opened");
  auto stream = std::move(open).Value();
  std::string text;
  for (;;) {
    auto next = co_await stream.ReadAsync(4096);
    if (!next.Succeeded()) co_return Fail<ImportedPresentationReport>(kit::ErrorCode::ProviderFailure, "Could not read the selected report");
    if (next.Value().empty()) break;
    if (next.Value().size() > limit - text.size())
      co_return Fail<ImportedPresentationReport>(kit::ErrorCode::BudgetExceeded, "Choose a JSON report up to 64 KiB");
    text.append(reinterpret_cast<const char*>(next.Value().data()), next.Value().size());
  }
  co_return co_await PublishPresentationReport(std::move(projects), std::move(project), file.Name(), std::move(text));
}

#ifdef __EMSCRIPTEN__
kit::Async<ImportedPresentationReport> ImportPresentationReport(
    kit::ProjectService projects, kit::ProjectId project, std::string name, std::string text) {
  co_return co_await PublishPresentationReport(std::move(projects), std::move(project), std::move(name), std::move(text));
}
#endif

kit::AgentResult<void> ValidatePresentationFilename(std::string_view name) {
  if (name.size() <= 5 || name.size() > 200 || name.find_first_of("/\\:\"<>|?*") != std::string::npos ||
      std::ranges::any_of(name, [](unsigned char ch) { return ch < 32; })) {
    return Fail<void>(kit::ErrorCode::InvalidArgument, "Enter a PPTX file name within the export folder");
  }
  std::string lower(name);
  for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  const auto stem = lower.substr(0, lower.find('.'));
  if (!lower.ends_with(".pptx") || stem.empty() || stem.ends_with(' ') ||
      stem == "con" || stem == "nul" || stem == "aux" || stem == "prn" ||
      (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9')) {
    return Fail<void>(kit::ErrorCode::InvalidArgument, "Enter a valid PPTX file name");
  }
  return kit::AgentResult<void>::Success();
}

kit::JsonValue PresentationExportArguments(kit::ArtifactRef source, std::string name) {
  return {JsonValue::Object{{"artifact_id", JsonValue{std::move(source.id.value)}},
      {"version", JsonValue{static_cast<std::int64_t>(source.version)}}, {"file_name", JsonValue{std::move(name)}}}};
}

std::optional<std::size_t> PresentationPartPage(const kit::PartId& part) {
  constexpr std::string_view prefix = "slide-";
  std::string_view id(part.value);
  if (!id.starts_with(prefix)) return {};
  id.remove_prefix(prefix.size());
  if (id.ends_with(screenshot_part_suffix)) id.remove_suffix(screenshot_part_suffix.size());
  std::size_t page = 0;
  const auto parsed = std::from_chars(id.data(), id.data() + id.size(), page);
  if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || !page || page > 30) return {};
  return page;
}

kit::PartId PresentationScreenshotPart(std::size_t page) {
  return {"slide-" + std::to_string(page) + std::string(screenshot_part_suffix)};
}

kit::ArtifactStore PresentationArtifactStore(PresentationFiles files) {
  return {
      .preview = [files](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        const auto file = ResolvePptx(files, request);
        if (!file) co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "PPTX version unavailable");
        auto info = co_await file->StatAsync();
        if (!info.Succeeded()) co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "Staged PPTX is missing");
        const std::vector<kit::PartId> whole;
        auto deck = co_await ReadPackageAsync(ReadStagedSlides, *file, whole);
        if (!deck.Succeeded()) co_return kit::AgentResult<kit::ArtifactPreview>::Failure(deck.Error());
        auto outline = WritePresentationOutline(deck.Value().outline);
        auto& fields = std::get<JsonValue::Object>(outline.value);
        const auto screenshot = std::ranges::find(deck.Value().screenshots, true);
        if (screenshot != deck.Value().screenshots.end())
          fields.emplace("screenshot_page", JsonValue{static_cast<std::int64_t>(screenshot - deck.Value().screenshots.begin() + 1)});
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact,
            .structured = std::move(outline), .summary = "Staged PowerPoint presentation"});
      },
      .open = [files](kit::ArtifactResolveRequest request) -> kit::Async<AsyncInputStream> {
        const auto file = ResolvePptx(files, request);
        if (!file) co_return Fail<AsyncInputStream>(kit::ErrorCode::NotFound, "PPTX version unavailable");
        auto opened = co_await file->OpenReadAsync();
        if (!opened.Succeeded()) co_return Fail<AsyncInputStream>(kit::ErrorCode::NotFound, "Staged PPTX could not be opened");
        co_return kit::AgentResult<AsyncInputStream>::Success(std::move(opened).Value());
      },
      .selection = [files](kit::ArtifactSelectionRequest request) -> kit::Async<std::string> {
        const auto file = ResolvePptx(files, request.resource);
        if (!file)
          co_return Fail<std::string>(kit::ErrorCode::NotFound, "PPTX version unavailable");
        auto deck = co_await ReadPackageAsync(ReadStagedSlides, *file, request.parts);
        if (!deck.Succeeded()) co_return kit::AgentResult<std::string>::Failure(deck.Error());
        std::string text;
        for (std::size_t index = 0; index < deck.Value().outline.slides.size(); ++index) {
          if (!text.empty()) text += "\n\n";
          if (!request.parts.empty() && request.parts[index].value.ends_with(screenshot_part_suffix)) {
            if (!deck.Value().screenshots[index])
              co_return Fail<std::string>(kit::ErrorCode::NotFound, "The selected screenshot is unavailable");
            text += PresentationScreenshotSelection;
          } else {
            text += SlideSelectionText(deck.Value().outline.slides[index]);
          }
        }
        if (text.size() > request.byte_limit)
          co_return Fail<std::string>(kit::ErrorCode::BudgetExceeded, "The selected slide text exceeds its byte limit");
        co_return kit::AgentResult<std::string>::Success(std::move(text));
      }};
}

kit::Async<Bytes> ReadPresentationBytes(const std::shared_ptr<kit::AgentRuntime>& runtime, kit::ArtifactRef version,
                                       std::optional<std::size_t> byte_limit) {
  using Result = kit::AgentResult<Bytes>;
  auto opened = co_await runtime->Artifacts().OpenAsync(std::move(version));
  if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
  auto stream = std::move(opened).Value();
  Bytes bytes;
  for (;;) {
    auto chunk = co_await stream.ReadAsync(64 * 1024);
    if (!chunk.Succeeded()) co_return Fail<Bytes>(kit::ErrorCode::ProviderFailure, "Staged PPTX could not be read");
    if (chunk.Value().empty()) break;
    if (byte_limit && chunk.Value().size() > *byte_limit - bytes.size())
      co_return Fail<Bytes>(kit::ErrorCode::BudgetExceeded, "The staged PowerPoint package is too large for image review");
    bytes.insert(bytes.end(), chunk.Value().begin(), chunk.Value().end());
  }
  co_return Result::Success(std::move(bytes));
}

kit::Async<PresentationScreenshot> LoadPresentationScreenshot(std::shared_ptr<kit::AgentRuntime> runtime,
    kit::ArtifactRef version, std::size_t page, File temporary) {
  using Result = kit::AgentResult<PresentationScreenshot>;
  auto archive = co_await ReadPresentationBytes(runtime, version, 9 * 1024 * 1024);
  if (!archive.Succeeded()) co_return Result::Failure(archive.Error());
  const auto scratch = temporary.Child("ppt-review-" + kit::NewCommandId().value + ".pptx");
  auto image = co_await ReadPackageAsync(ReadPackageScreenshot, std::move(archive).Value(), scratch, page);
  if (!image.Succeeded()) co_return Result::Failure(image.Error());
  auto current = co_await runtime->Artifacts().ReadAsync(version);
  if (!current.Succeeded()) co_return Result::Failure(current.Error());
  const auto& parts = current.Value().parts;
  const auto selectable = std::ranges::find(parts, PresentationScreenshotPart(page), &kit::ArtifactPart::id) != parts.end();
  co_return Result::Success({std::move(image).Value(), selectable});
}

kit::TargetBaseline PresentationTargetBaseline(const kit::ArtifactRef& version) {
  const kit::ResourceRef resource{{std::string(version_resource) + version.id.value}, std::to_string(version.version)};
  return {resource.id, resource};
}

kit::ChangeAdapters PresentationChanges(PresentationFiles files,
    std::shared_ptr<std::weak_ptr<kit::AgentRuntime>> runtime) {
  return {
      .prepare_rework = [files, runtime](kit::ChangePrepareContext context, kit::ReworkRequest request)
          -> kit::Async<kit::PreparedChange> {
        using Result = kit::AgentResult<kit::PreparedChange>;
        if (!Handle(request.baseline) || !context.artifact || context.artifact->metadata.type != PresentationArtifactType ||
            !request.targets.empty() || request.baseline.version >= INT64_MAX)
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::Unsupported, "Choose a staged PowerPoint deck");
        auto owner = runtime->lock();
        if (!owner) co_return Fail<kit::PreparedChange>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        std::vector<PptxTextChange> changes;
        std::string summary;
        std::vector<kit::SourceSlice> compared;
        for (const auto& replacement : request.replacements) {
          if (std::ranges::find(context.access.sources, replacement.previous) == context.access.sources.end())
            co_return Fail<kit::PreparedChange>(kit::ErrorCode::PermissionDenied, "The prior report is unavailable for comparison");
          auto before = co_await ReportConclusion(owner, replacement.previous);
          if (!before.Succeeded()) co_return Result::Failure(before.Error());
          auto after = co_await ReportConclusion(owner, replacement.replacement);
          if (!after.Succeeded()) co_return Result::Failure(after.Error());
          compared.push_back(replacement.previous);
          std::set<kit::PartId> pages;
          for (const auto& dependency : context.artifact->dependencies) {
            if (dependency.output_part && dependency.input == replacement.previous &&
                (request.parts.empty() || std::ranges::find(request.parts, *dependency.output_part) != request.parts.end()))
              pages.insert(*dependency.output_part);
          }
          for (std::size_t index = 0; index < context.artifact->parts.size(); ++index) {
            const auto& part = context.artifact->parts[index];
            if (!pages.contains(part.id)) continue;
            if (!summary.empty()) summary += "\n\n";
            summary += "Slide " + std::to_string(index + 1) + ": " + part.label +
                "\nBefore: " + before.Value() + "\nAfter: " + after.Value();
            changes.push_back({index + 1, before.Value(), after.Value()});
          }
        }
        auto source = co_await ReadPresentationBytes(owner, request.baseline);
        if (!source.Succeeded()) co_return Result::Failure(source.Error());
        auto checked = co_await ReworkPackageAsync(files, std::move(source).Value(), {}, changes, context.access.stop);
        if (!checked.Succeeded()) co_return Result::Failure(checked.Error());
        co_return Result::Success({.preview = {.title = "Review PowerPoint rework", .summary = std::move(summary),
            .artifacts = {request.baseline}}, .inputs = std::move(compared), .execution_data = PptxPlan(request.baseline, changes)});
      },
      .execute_rework = [files, runtime](kit::ToolContext context, kit::PreparedToolCall call)
          -> kit::Async<kit::ToolExecutionResult> {
        using Result = kit::AgentResult<kit::ToolExecutionResult>;
        auto parsed = ReadPptxPlan(call.change.execution_data);
        if (!parsed.Succeeded()) co_return Result::Failure(parsed.Error());
        auto plan = std::move(parsed).Value();
        auto owner = runtime->lock();
        if (!owner) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        auto baseline = co_await owner->Artifacts().ReadAsync(plan.baseline);
        if (!baseline.Succeeded()) co_return Result::Failure(baseline.Error());
        kit::ArtifactRef updated{plan.baseline.id, plan.baseline.version + 1};
        const auto to = Handle(updated);
        if (!to) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::InvalidArgument, "Invalid staged deck version");
        auto source = co_await ReadPresentationBytes(owner, plan.baseline);
        if (!source.Succeeded()) co_return Result::Failure(source.Error());
        const auto destination = PptxFile(files, *to);
        auto written = co_await ReworkPackageAsync(files, std::move(source).Value(), destination,
            std::move(plan.edits), context.StopToken());
        if (!written.Succeeded()) co_return Result::Failure(written.Error());
        const auto info = co_await destination.StatAsync();
        if (!info.Succeeded()) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::ProviderFailure, "Could not read the updated PPTX");
        const auto& original = baseline.Value();
        kit::ArtifactDraft draft{.id = updated.id, .expected_latest = plan.baseline.version, .type = original.metadata.type,
            .name = original.metadata.name, .storage_handle = *to, .size_bytes = info.Value().size, .parts = original.parts};
        std::vector<kit::ArtifactDraft> drafts;
        drafts.push_back(std::move(draft));
        co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(drafts)});
      },
      .prepare_adoption = [runtime](kit::ChangePrepareContext context, kit::AdoptCandidateRequest request)
          -> kit::Async<kit::PreparedChange> {
        using Result = kit::AgentResult<kit::PreparedChange>;
        if (!context.artifact || context.artifact->metadata.type != PresentationArtifactType ||
            request.targets.size() != 1)
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::Unsupported, "Choose a PowerPoint candidate and one target deck");
        if (!request.parts.empty())
          co_return Result::Failure({.code = kit::ErrorCode::ScopeExpansionRequired,
              .message = "This adoption replaces the whole presentation",
              .proposed_scope = kit::ScopeExpansion{.whole_artifact = true, .summary = "Review and adopt the whole presentation"}});
        const auto current = TargetVersion(request.targets.front());
        if (!current || current->version >= INT64_MAX || request.candidate_artifact.version > INT64_MAX)
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::Conflict, "The presentation baseline cannot be updated");
        const auto owner = runtime->lock();
        if (!owner) co_return Fail<kit::PreparedChange>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        auto original = co_await owner->Artifacts().PreviewAsync(*current);
        if (!original.Succeeded()) co_return Result::Failure(original.Error());
        auto candidate = co_await owner->Artifacts().PreviewAsync(request.candidate_artifact);
        if (!candidate.Succeeded()) co_return Result::Failure(candidate.Error());
        auto before = original.Value().structured ? ReadPresentationOutline(*original.Value().structured) :
            Fail<PresentationOutline>(kit::ErrorCode::InvalidState, "The current deck has no reviewable outline");
        if (!before.Succeeded()) co_return Result::Failure(before.Error());
        auto after = candidate.Value().structured ? ReadPresentationOutline(*candidate.Value().structured) :
            Fail<PresentationOutline>(kit::ErrorCode::InvalidState, "The candidate has no reviewable outline");
        if (!after.Succeeded()) co_return Result::Failure(after.Error());
        const auto summary = "Current v" + std::to_string(current->version) + " · " + before.Value().title +
            " (" + std::to_string(before.Value().slides.size()) + " slides)\nSelected candidate v" +
            std::to_string(request.candidate_artifact.version) + " · " + after.Value().title +
            " (" + std::to_string(after.Value().slides.size()) + " slides)";
        co_return Result::Success({.preview = {.title = "Review PowerPoint adoption", .summary = summary,
            .artifacts = {*current, request.candidate_artifact}}, .targets = std::move(request.targets),
            .inputs = {{*current}, {request.candidate_artifact}}, .execution_data = PptxReference(request.candidate_artifact)});
      },
      .execute_adoption = [files, runtime](kit::ToolContext context, kit::PreparedToolCall call)
          -> kit::Async<kit::ToolExecutionResult> {
        using Result = kit::AgentResult<kit::ToolExecutionResult>;
        const auto reject = [](kit::AgentError reason) {
          return Result::Success({.outcome = kit::ToolOutcome::Failed, .reason = std::move(reason)});
        };
        const auto current = call.change.targets.size() == 1 ? TargetVersion(call.change.targets.front()) : std::nullopt;
        const auto selected = ReadPptxReference(call.change.execution_data);
        if (!current || !selected || current->version >= INT64_MAX)
          co_return reject({.code = kit::ErrorCode::Conflict, .message = "The reviewed presentation is unavailable"});
        const auto before = PresentationTargetBaseline(*current);
        const auto grants = context.Resources();
        if (grants.size() != 1 || grants.front().mode != kit::ResourceAccessMode::Write ||
            grants.front().resource != before.target || grants.front().current != before.expected)
          co_return reject({.code = kit::ErrorCode::Conflict, .message = "The target deck no longer matches the reviewed version"});
        const auto owner = runtime->lock();
        if (!owner) co_return reject({.code = kit::ErrorCode::RuntimeClosed, .message = "Agent runtime closed"});
        auto candidate = co_await owner->Artifacts().ReadAsync(*selected);
        if (!candidate.Succeeded()) co_return reject(candidate.Error());
        const auto& record = candidate.Value();
        if (record.metadata.type != PresentationArtifactType || !SafeHandle(context.CallId().value))
          co_return reject({.code = kit::ErrorCode::InvalidState, .message = "The selected deck cannot be staged"});
        auto source = co_await ReadPresentationBytes(owner, *selected);
        if (!source.Succeeded()) co_return reject(source.Error());
        if (context.StopToken().stop_requested()) co_return Result::Success({.outcome = kit::ToolOutcome::Cancelled});
        const auto size = source.Value().size();
        const auto handle = std::string(adoption_handle) + context.CallId().value;
        if (!co_await PptxFile(files, handle).WriteBytesAsync(std::move(source).Value()))
          co_return reject({.code = kit::ErrorCode::ProviderFailure, .message = "The selected deck could not be staged"});
        const auto after = PresentationTargetBaseline({current->id, current->version + 1});
        kit::ArtifactDraft draft{.id = current->id, .expected_latest = current->version, .type = record.metadata.type,
            .name = record.metadata.name, .storage_handle = handle, .size_bytes = size, .parts = record.parts,
            .dependencies = {{{}, {*current}}},
            .published_effect = kit::ResourceEffect{before.target, kit::ResourceEffectState::Applied, before.expected, after.expected}};
        for (const auto& part : record.parts) draft.dependencies.push_back({part.id, {*selected, {part.id}}});
        std::vector<kit::ArtifactDraft> drafts;
        drafts.push_back(std::move(draft));
        co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(drafts)});
      }};
}

std::string SlideSelectionText(const SlideOutline& slide) {
  std::string text = slide.title;
  for (const auto& bullet : slide.bullets) text += "\n" + bullet;
  return text;
}

kit::AgentResult<void> VerifyPresentationExport(
    const PresentationFiles& files, const Bytes& source, const kit::ResourceId& target, const ImageAsset& screenshot,
    std::optional<std::size_t> required_slides) {
  const auto path = std::filesystem::u8path(target.value);
  const auto file_name = path.filename().u8string();
  const std::string name(file_name.begin(), file_name.end());
  if (!ValidatePresentationFilename(name).Succeeded() || files.exports.Child(name).Path() != target.value) {
    return Fail<void>(kit::ErrorCode::PermissionDenied, "The presentation export is outside the approved directory");
  }
  std::error_code error;
  if (std::filesystem::symlink_status(path, error).type() != std::filesystem::file_type::regular || error) {
    return Fail<void>(kit::ErrorCode::NotFound, "The presentation export is missing");
  }
  const auto exported = files.exports.Child(name);
  const auto actual = exported.Stat();
  if (!actual.Succeeded() || source.size() != actual.Value().size) {
    return Fail<void>(kit::ErrorCode::Conflict, "The presentation export does not match the approved version");
  }
  const auto copy = exported.ReadBytes();
  if (!copy.Succeeded() || source != copy.Value()) {
    return Fail<void>(kit::ErrorCode::Conflict, "The presentation export does not match the approved version");
  }
  auto archive = archive::ArchiveReader::Open(exported);
  if (!archive.Succeeded()) return Fail<void>(kit::ErrorCode::InvalidState, "The exported PowerPoint package cannot be opened");
  auto presentation = archive.Value().ReadEntry("ppt/presentation.xml");
  if (!presentation.Succeeded() || !archive.Value().ReadEntry("ppt/slides/slide1.xml").Succeeded()) {
    return Fail<void>(kit::ErrorCode::InvalidState, "The exported PowerPoint package cannot be opened");
  }
  if (required_slides) {
    if (SlideCount(Xml(presentation.Value())) != *required_slides)
      return Fail<void>(kit::ErrorCode::Conflict, "The export does not match the current goal's slide count");
  }
  auto image = archive.Value().ReadEntry("ppt/media/implementation.png");
  if (!image.Succeeded() || !std::ranges::equal(image.Value(), screenshot.EncodedBytes())) {
    return Fail<void>(kit::ErrorCode::Conflict, "The export does not contain the verified implementation screenshot");
  }
  return kit::AgentResult<void>::Success();
}

kit::ToolResourceResolver PresentationResources(PresentationFiles files,
    std::shared_ptr<std::weak_ptr<kit::AgentRuntime>> runtime) {
  return [files, runtime](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
    using Result = kit::AgentResult<std::vector<kit::ResourceAccess>>;
    if (std::ranges::any_of(invocation.prepared_targets, [](const auto& target) {
          return target.target.value.starts_with(version_resource);
        })) {
      const auto baseline = invocation.prepared_targets.size() == 1
          ? TargetVersion(invocation.prepared_targets.front()) : std::nullopt;
      if (!baseline)
        co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::InvalidArgument, "Select one target presentation");
      const auto owner = runtime->lock();
      if (!owner) co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
      auto opened = co_await owner->Sessions().OpenAsync(invocation.session);
      if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
      auto snapshot = opened.Value()->Snapshot();
      if (!snapshot) co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::NotFound, "Presentation session unavailable");
      const auto current = std::ranges::find(snapshot->artifacts.rbegin(), snapshot->artifacts.rend(),
          baseline->id, &kit::ArtifactRef::id);
      if (current == snapshot->artifacts.rend())
        co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::NotFound, "Presentation unavailable in this session");
      auto record = co_await owner->Artifacts().ReadAsync(*current);
      if (!record.Succeeded()) co_return Result::Failure(record.Error());
      if (record.Value().session != invocation.session || record.Value().metadata.type != PresentationArtifactType)
        co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::PermissionDenied, "Presentation target is unavailable");
      const auto resource = PresentationTargetBaseline(*current);
      co_return Result::Success({{resource.target, resource.expected, invocation.phase == kit::ToolAccessPhase::Prepare
          ? kit::ResourceAccessMode::Read : kit::ResourceAccessMode::Write, {}}});
    }
    if (invocation.tool.id != ExportPresentation || invocation.phase == kit::ToolAccessPhase::Prepare) {
      co_return Result::Success({});
    }
    auto export_to = ReadExport(files, invocation.arguments);
    if (!export_to.Succeeded()) co_return Result::Failure(export_to.Error());
    const auto& target = export_to.Value().target;
    if (invocation.prepared_targets.size() != 1 || invocation.prepared_targets.front().target.value != target.Path()) {
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::PermissionDenied, "Export scope does not match its plan");
    }
    auto info = co_await target.StatAsync();
    if (info.Succeeded()) co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::Conflict, "Export target already exists");
    if (info.Error().code != IoErrorCode::NotFound) {
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::ProviderFailure, "Export target could not be checked");
    }
    co_return Result::Success({{{target.Path()}, {}, kit::ResourceAccessMode::Write, {}}});
  };
}

kit::AgentResult<void> RegisterPresentationTools(
    const std::shared_ptr<kit::AgentRuntime>& runtime, PresentationFiles files) {
  auto generation = runtime->RegisterTool({
      .id = GeneratePresentation, .version = 1, .description = "Stage a presentation from its confirmed outline",
      .input_schema = PresentationOutlineSchema(), .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext context, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        auto outline = ReadPresentationOutline(request.arguments);
        if (!outline.Succeeded()) co_return kit::AgentResult<kit::PreparedChange>::Failure(outline.Error());
        co_return kit::AgentResult<kit::PreparedChange>::Success({
            .preview = {.title = "Generate presentation", .summary = outline.Value().title},
            .inputs = std::move(context.sources),
            .execution_data = std::move(request.arguments), .undo_support = kit::UndoSupport::None});
      },
      .execute = [files, weak = std::weak_ptr<kit::AgentRuntime>(runtime)](
                     kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        using Result = kit::AgentResult<kit::ToolExecutionResult>;
        auto outline = ReadPresentationOutline(call.change.execution_data);
        if (!outline.Succeeded()) co_return Result::Failure(outline.Error());
        const auto id = context.CallId().value;
        const auto owner = weak.lock();
        if (!owner) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        std::optional<ImageAsset> screenshot;
        std::vector<std::pair<kit::SourceSlice, std::string>> imported_summaries;
        for (const auto& input : call.change.inputs) {
          if (const auto* ref = std::get_if<kit::SharedContextRef>(&input.source)) {
            auto item = co_await owner->Projects().ReadContextAsync(*ref);
            if (!item.Succeeded()) co_return Result::Failure(item.Error());
            if (const auto report = ReadImportedReport(item.Value()))
              imported_summaries.emplace_back(input, report->summary);
          }
          const auto* ref = std::get_if<kit::ArtifactRef>(&input.source);
          if (!ref || !IsVerificationScreenshot(*ref)) continue;
          if (screenshot) co_return Fail<kit::ToolExecutionResult>(
              kit::ErrorCode::InvalidArgument, "Choose one implementation screenshot for this presentation");
          auto image = co_await LoadVerificationScreenshot(owner, *ref);
          if (!image.Succeeded()) co_return Result::Failure(image.Error());
          screenshot = std::move(image).Value();
        }
        auto destination = PptxFile(files, id);
        if (!co_await files.staging.CreateDirectoriesAsync()) {
          co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::ProviderFailure, "Could not create presentation staging directory");
        }
        static_cast<void>(context.ReportProgress("Packaging PowerPoint slides"));
        const bool includes_screenshot = screenshot.has_value();
        auto build = [deck = outline.Value(), stop = context.StopToken(), screenshot = std::move(screenshot)](File output) {
          if (stop.stop_requested()) return Fail<void>(kit::ErrorCode::AlreadyClosed, "Presentation generation cancelled");
          return CreatePptx(deck, output, screenshot);
        };
        auto created = co_await WritePackageAsync(files, destination, context.StopToken(), std::move(build));
        if (!created.Succeeded()) co_return kit::AgentResult<kit::ToolExecutionResult>::Failure(created.Error());
        auto info = co_await destination.StatAsync();
        if (!info.Succeeded()) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::ProviderFailure, "Staged PPTX metadata is unavailable");
        kit::ArtifactDraft draft{.id = kit::ArtifactId{std::string(id_prefix) + id}, .type = "presentation/pptx",
                                 .name = outline.Value().title + ".pptx", .storage_handle = id, .size_bytes = info.Value().size};
        for (std::size_t i = 0; i < outline.Value().slides.size(); ++i) {
          const kit::PartId slide{"slide-" + std::to_string(i + 1)};
          draft.parts.push_back({slide, outline.Value().slides[i].title});
          for (const auto& input : call.change.inputs) {
            const auto report = std::ranges::find_if(imported_summaries, [&](const auto& summary) { return summary.first == input; });
            const auto& bullets = outline.Value().slides[i].bullets;
            if (report != imported_summaries.end() && std::ranges::find(bullets, report->second) == bullets.end()) continue;
            const auto* ref = std::get_if<kit::ArtifactRef>(&input.source);
            if (!ref || !IsVerificationScreenshot(*ref) || i + 1 == outline.Value().slides.size()) {
              draft.dependencies.push_back({slide, input});
            }
          }
        }
        if (includes_screenshot)
          draft.parts.push_back({PresentationScreenshotPart(outline.Value().slides.size()), std::string(PresentationScreenshotLabel)});
        kit::ToolExecutionResult result{.outcome = kit::ToolOutcome::Succeeded,
                                       .model_value = std::move(call.change.execution_data)};
        result.blocks.push_back({kit::BlockId{kit::NewCommandId().value},
            kit::TextBlock{"Staged " + draft.name, false}});
        result.new_artifacts.push_back(std::move(draft));
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success(std::move(result));
      }});
  if (!generation.Succeeded()) return generation;

  auto export_tool = runtime->RegisterTool({
      .id = ExportPresentation, .version = 1, .description = "Export the selected version of a staged PowerPoint presentation",
      .input_schema = ExportSchema(), .effect = kit::ToolEffect::WriteResource,
      .prepare = [files, weak = std::weak_ptr<kit::AgentRuntime>(runtime)](
          kit::ToolPrepareContext context, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        using Result = kit::AgentResult<kit::PreparedChange>;
        auto export_to = ReadExport(files, request.arguments);
        if (!export_to.Succeeded()) co_return Result::Failure(export_to.Error());
        auto owner = weak.lock();
        if (!owner) co_return Fail<kit::PreparedChange>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        auto source = co_await owner->Artifacts().ReadAsync(export_to.Value().source);
        if (!source.Succeeded()) co_return Result::Failure(source.Error());
        const bool explicitly_selected = std::ranges::any_of(context.sources, [&](const auto& input) {
          return input.source == kit::SourceRef{export_to.Value().source} && input.parts.empty() && !input.text_range;
        });
        const auto& action = context.artifact_action;
        const bool from_review = action && action->action == ExportStagedPresentation && action->artifact == export_to.Value().source &&
            action->payload == *Field(std::get<JsonValue::Object>(request.arguments.value), "file_name");
        if (source.Value().metadata.type != "presentation/pptx" || source.Value().session != context.actor.session ||
            (source.Value().run != context.actor.run && !explicitly_selected && !from_review)) {
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::PermissionDenied, "Choose a staged version in this conversation");
        }
        auto existed = co_await export_to.Value().target.StatAsync();
        if (existed.Succeeded()) co_return Fail<kit::PreparedChange>(kit::ErrorCode::Conflict, "Export target already exists");
        if (existed.Error().code != IoErrorCode::NotFound) {
          co_return Fail<kit::PreparedChange>(kit::ErrorCode::ProviderFailure, "Export target could not be checked");
        }
        std::vector<kit::SourceSlice> inputs{{export_to.Value().source}};
        for (const auto& dependency : source.Value().dependencies) {
          if (std::ranges::find(inputs, dependency.input) == inputs.end()) inputs.push_back(dependency.input);
        }
        co_return Result::Success({.preview = {.title = "Export PowerPoint",
            .summary = source.Value().metadata.name + " (version " +
                std::to_string(export_to.Value().source.version) + ")",
            .artifacts = {export_to.Value().source}},
            .targets = {{{export_to.Value().target.Path()}, {}}}, .inputs = std::move(inputs),
            .execution_data = std::move(request.arguments), .undo_support = kit::UndoSupport::None});
      },
      .execute = [files, weak = std::weak_ptr<kit::AgentRuntime>(runtime)](
          kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        auto export_to = ReadExport(files, call.change.execution_data);
        if (!export_to.Succeeded()) co_return kit::AgentResult<kit::ToolExecutionResult>::Failure(export_to.Error());
        const auto grants = context.Resources();
        const auto& destination = export_to.Value().target;
        if (grants.size() != 1 || grants.front().mode != kit::ResourceAccessMode::Write ||
            grants.front().resource.value != destination.Path()) {
          co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::PermissionDenied, "Export target was not granted");
        }
        auto owner = weak.lock();
        if (!owner) co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        auto allowed = co_await owner->Artifacts().ReadAsync(export_to.Value().source);
        if (!allowed.Succeeded()) {
          co_return kit::AgentResult<kit::ToolExecutionResult>::Success(
              {.outcome = kit::ToolOutcome::Failed, .reason = allowed.Error()});
        }
        auto source = co_await ReadPresentationBytes(owner, export_to.Value().source);
        if (!source.Succeeded())
          co_return kit::AgentResult<kit::ToolExecutionResult>::Success(
              {.outcome = kit::ToolOutcome::Failed, .reason = source.Error()});
#ifdef __EMSCRIPTEN__
        co_return co_await ExportBrowserPresentation(files, std::move(source).Value(), destination,
                                                     export_to.Value().source, context.StopToken());
#else
        co_return co_await RunWorker(CopyPresentation, std::move(source).Value(), destination,
                                     export_to.Value().source, context.StopToken());
#endif
      }});
  if (!export_tool.Succeeded()) return export_tool;
  return runtime->RegisterArtifactAction({.artifact_type = "presentation/pptx", .action = std::string(ExportStagedPresentation),
      .payload_schema = {JsonValue{JsonValue::Object{{"type", JsonValue{std::string("string")}}}}},
      .prepare = [weak = std::weak_ptr<kit::AgentRuntime>(runtime)](kit::ArtifactActionContext context, JsonValue payload)
          -> kit::Async<kit::SendInput> {
        using Result = kit::AgentResult<kit::SendInput>;
        const auto* file_name = std::get_if<std::string>(&payload.value);
        auto name = ValidatePresentationFilename(file_name ? std::string_view(*file_name) : std::string_view{});
        if (!name.Succeeded()) co_return Result::Failure(name.Error());
        auto owner = weak.lock();
        if (!owner) co_return Fail<kit::SendInput>(kit::ErrorCode::RuntimeClosed, "Agent runtime closed");
        auto source = co_await owner->Artifacts().ReadAsync(context.artifact);
        if (!source.Succeeded()) co_return Result::Failure(source.Error());
        if (source.Value().session != context.session)
          co_return Fail<kit::SendInput>(kit::ErrorCode::PermissionDenied, "Open the conversation that owns this presentation to export it");
        auto opened = co_await owner->Sessions().OpenAsync(context.session);
        if (!opened.Succeeded()) co_return Result::Failure(opened.Error());
        auto session = opened.Value()->Snapshot();
        if (!session || (session->summary.agent.id != PptAgent && session->summary.agent.id != GeneralAgent))
          co_return Fail<kit::SendInput>(kit::ErrorCode::Unsupported, "The current assistant cannot export this presentation");
        co_return Result::Success({.text = "Export PowerPoint version " + std::to_string(context.artifact.version) +
            " as " + *file_name});
      }});
}

} // namespace agentkit_preview
