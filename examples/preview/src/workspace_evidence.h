#pragma once

#include <huxerui/agentkit/runtime.h>
#include <huxerui/resource.h>

namespace agentkit_preview {

inline constexpr std::string_view ImplementationEvidence = "workspace/implementation+json";
inline constexpr std::string_view CodePatchPreview = "workspace/patch-preview+json";
inline constexpr std::string_view VerificationEvidence = "workspace/verification+json";
inline constexpr std::string_view VerificationScreenshot = "image/png";

huxerui::File VerificationCaptureFile(huxerui::File directory, const huxerui::agentkit::ToolCallId& call);
bool IsVerificationScreenshot(const huxerui::agentkit::ArtifactRef& ref);
huxerui::agentkit::AgentResult<huxerui::ImageAsset> DecodeVerificationScreenshot(huxerui::Bytes bytes);

huxerui::agentkit::AgentResult<huxerui::agentkit::ArtifactDraft> StageVerificationScreenshot(
    const huxerui::File& directory, const huxerui::agentkit::ToolCallId& call);
huxerui::agentkit::Async<huxerui::ImageAsset> LoadVerificationScreenshot(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::ArtifactRef ref);

huxerui::agentkit::ArtifactStore WithWorkspaceEvidence(
    huxerui::agentkit::ArtifactStore fallback, huxerui::File directory);

huxerui::agentkit::Async<huxerui::agentkit::ArtifactDraft> StageWorkspaceEvidence(
    huxerui::File directory, huxerui::agentkit::ToolCallId call, std::string type, std::string name,
    huxerui::agentkit::JsonValue contents);

} // namespace agentkit_preview
