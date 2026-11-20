#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/app.h>

namespace agentkit_preview {

inline const huxerui::agentkit::AgentId DesignAgent{"preview.design"};
inline const huxerui::agentkit::ToolId ReadDesign{"preview.design.read"};
inline const huxerui::agentkit::ToolId PublishDesign{"preview.design.publish"};
inline constexpr std::string_view DesignReview = "design.review";
inline constexpr std::string_view DesignArtifactType = "design/board+json";

struct DesignNode {
  std::string id;
  std::string kind;
  std::string label;
  std::string note;
  int x;
  int y;
  int width;
  int height;
  bool operator==(const DesignNode&) const = default;
};

struct DesignBoard {
  std::string title;
  std::string brief;
  int width;
  int height;
  std::vector<DesignNode> nodes;
};

huxerui::agentkit::JsonSchema DesignSchema();
huxerui::agentkit::JsonValue SampleDesign(std::string title);
huxerui::agentkit::AgentResult<DesignBoard> ParseDesign(const huxerui::agentkit::JsonValue& document);
huxerui::View DesignArtboard(std::shared_ptr<const DesignBoard> board, std::string highlighted, float height = 340.0F);
std::optional<huxerui::View> RenderDesignArtifact(const huxerui::agentkit::ArtifactPreview& preview);
huxerui::agentkit::ArtifactStore WithDesignArtifacts(huxerui::agentkit::ArtifactStore fallback, huxerui::File directory);
bool HasDesignTargets(const std::vector<huxerui::agentkit::TargetBaseline>& targets);
huxerui::agentkit::TargetBaseline DesignTargetBaseline(const huxerui::agentkit::ArtifactRef& current);
huxerui::agentkit::ToolResourceResolver DesignChangeResources(
    std::shared_ptr<std::weak_ptr<huxerui::agentkit::AgentRuntime>> runtime);
huxerui::agentkit::ChangeAdapters WithDesignChanges(huxerui::agentkit::ChangeAdapters changes, huxerui::File directory,
    std::shared_ptr<std::weak_ptr<huxerui::agentkit::AgentRuntime>> runtime);
std::optional<huxerui::agentkit::RevertRequest> DesignRevertRequest(
    const huxerui::agentkit::SessionSnapshot& session, const huxerui::agentkit::ArtifactRef& current);
huxerui::agentkit::AgentResult<void> RegisterDesignTools(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, huxerui::File directory);
huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunDesign(
    huxerui::agentkit::RunContext context, huxerui::agentkit::RunInput input,
    std::optional<huxerui::agentkit::RunCheckpoint> checkpoint);
struct DesignReviewLocation {
  std::variant<huxerui::agentkit::ArtifactRef, huxerui::agentkit::AgentChatApprovalRef> source;
  std::string node;
  bool operator==(const DesignReviewLocation&) const = default;
};
huxerui::View DesignChat(std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::AgentChatOptions options = {},
    std::optional<DesignReviewLocation> window_review = {}, std::optional<huxerui::File> evidence = {});
huxerui::WindowHandle OpenPreviewDesignReview(const huxerui::ApplicationHandle& app,
    std::shared_ptr<huxerui::agentkit::Conversation> conversation, std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    DesignReviewLocation location, std::string title,
    std::optional<huxerui::State<huxerui::agentkit::AgentChatDraftSources>> draft_sources = {},
    std::optional<huxerui::File> evidence = {});

} // namespace agentkit_preview
