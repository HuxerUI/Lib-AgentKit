#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/presentation.h>

#include "presentation_tools.h"

#include <map>

namespace agentkit_preview {

class CodeWorkspaces;

struct PresentationLocation {
  huxerui::agentkit::SessionId session;
  huxerui::agentkit::ArtifactRef deck;
  std::optional<huxerui::agentkit::PartId> part;
  bool operator==(const PresentationLocation&) const = default;
};

using PresentationReviewPages = std::map<std::pair<huxerui::agentkit::ArtifactId, huxerui::agentkit::Revision>,
    huxerui::agentkit::PartId>;
using OpenPresentationLocation = std::function<void(PresentationLocation)>;

huxerui::View ImportReportControl(std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    huxerui::agentkit::ProjectId project, std::function<void(ImportedPresentationReport)> imported);
huxerui::View SharedReportsDialog(huxerui::DialogContext dialog,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::ProjectId project);

#ifdef __EMSCRIPTEN__
huxerui::View BrowserWorkspaceReportControl(std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    huxerui::agentkit::ProjectId project, huxerui::File store, huxerui::agentkit::WorkspaceId workspace,
    std::string folder, std::function<void(ImportedPresentationReport)> imported);
huxerui::agentkit::AgentResult<void> RegisterBrowserPresentationExport(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime);
huxerui::View RenderBrowserPresentationExport(const huxerui::agentkit::CustomBlock& block, const huxerui::File& directory,
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime);
#endif

huxerui::agentkit::Async<std::shared_ptr<huxerui::agentkit::Conversation>> OpenPresentationReviewAsync(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, PresentationLocation target);
void ConfigurePreviewArtifactReview(huxerui::agentkit::AgentChatOptions& options,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::State<PresentationReviewPages> pages,
    OpenPresentationLocation open = {}, std::shared_ptr<CodeWorkspaces> workspaces = {});

huxerui::View PresentationReviewChoice(huxerui::State<huxerui::TextEditingValue> draft);

huxerui::View PresentationChat(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::AgentChatOptions options = {});

} // namespace agentkit_preview
