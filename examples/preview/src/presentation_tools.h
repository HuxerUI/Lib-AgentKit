#pragma once

#include <huxerui/agentkit/runtime.h>

#include "presentation.h"
#include "presentation_images.h"

namespace agentkit_preview {

struct PresentationFiles {
  huxerui::File staging;
  huxerui::File exports;
  std::optional<huxerui::File> temporary;
};

struct ImportedPresentationReport {
  huxerui::agentkit::SharedContextRef ref;
  std::string name;
  std::string summary;
  bool operator==(const ImportedPresentationReport&) const = default;
};

std::optional<ImportedPresentationReport> ReadImportedReport(const huxerui::agentkit::SharedContextItem& item);
std::optional<ImportedPresentationReport> ReadImportedReportVersion(const huxerui::agentkit::SharedContextItem& item);
huxerui::agentkit::AgentResult<void> ValidatePresentationReportSummary(std::string_view summary);
huxerui::agentkit::Async<ImportedPresentationReport> ImportPresentationReport(
    huxerui::agentkit::ProjectService projects, huxerui::agentkit::ProjectId project, huxerui::FileReference file);
#ifdef __EMSCRIPTEN__
huxerui::agentkit::Async<ImportedPresentationReport> ImportPresentationReport(
    huxerui::agentkit::ProjectService projects, huxerui::agentkit::ProjectId project, std::string name, std::string text);
#endif

extern const huxerui::agentkit::ToolId GeneratePresentation;
extern const huxerui::agentkit::ToolId ExportPresentation;
inline constexpr std::string_view PresentationArtifactType = "presentation/pptx";
inline constexpr std::string_view ExportStagedPresentation = "export-powerpoint";
inline constexpr std::string_view PresentationScreenshotLabel = "Implementation screenshot";
inline constexpr std::string_view PresentationScreenshotSelection = "Implementation screenshot\nVerified workspace implementation";
#ifdef __EMSCRIPTEN__
inline const huxerui::agentkit::ContentTypeRef BrowserPresentationExport{"preview.ppt.browser-export", 1};
#endif

huxerui::agentkit::ArtifactStore PresentationArtifactStore(PresentationFiles files);
huxerui::agentkit::TargetBaseline PresentationTargetBaseline(const huxerui::agentkit::ArtifactRef& version);
huxerui::agentkit::ChangeAdapters PresentationChanges(PresentationFiles files,
    std::shared_ptr<std::weak_ptr<huxerui::agentkit::AgentRuntime>> runtime);
std::string SlideSelectionText(const SlideOutline& slide);
std::optional<std::size_t> PresentationPartPage(const huxerui::agentkit::PartId& part);
huxerui::agentkit::PartId PresentationScreenshotPart(std::size_t page);
struct PresentationScreenshot {
  huxerui::ImageAsset image;
  bool selectable;
};
huxerui::agentkit::Async<PresentationScreenshot> LoadPresentationScreenshot(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::ArtifactRef version,
    std::size_t page, huxerui::File temporary);
huxerui::agentkit::Async<huxerui::ImageAsset> LoadPresentationIllustration(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::ArtifactRef version,
    std::size_t page, huxerui::File temporary);
huxerui::agentkit::ToolResourceResolver PresentationResources(PresentationFiles files,
    std::shared_ptr<std::weak_ptr<huxerui::agentkit::AgentRuntime>> runtime);
huxerui::agentkit::Async<huxerui::Bytes> ReadPresentationBytes(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, huxerui::agentkit::ArtifactRef version,
    std::optional<std::size_t> byte_limit = {});
huxerui::agentkit::AgentResult<void> RegisterPresentationTools(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, PresentationFiles files, PresentationImageProvider image_provider = {});
huxerui::agentkit::AgentResult<void> ValidatePresentationFilename(std::string_view name);
huxerui::agentkit::AgentResult<void> VerifyPresentationExport(
    const PresentationFiles& files, const huxerui::Bytes& source, const huxerui::agentkit::ResourceId& target,
    const huxerui::ImageAsset& screenshot, std::optional<std::size_t> required_slides);
huxerui::agentkit::JsonValue PresentationExportArguments(huxerui::agentkit::ArtifactRef source, std::string file_name);

} // namespace agentkit_preview
