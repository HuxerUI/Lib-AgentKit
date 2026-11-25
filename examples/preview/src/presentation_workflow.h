#pragma once

#include <huxerui/agentkit/runtime.h>

namespace agentkit_preview {

inline constexpr std::string_view PresentationBrief = "presentation.brief";
inline constexpr std::string_view PresentationOutlineReview = "presentation.outline";
inline constexpr std::string_view PresentationExport = "presentation.export";

std::string PresentationTopic(std::string_view input);

struct PresentationSkillEvidence {
  huxerui::agentkit::SkillRef skill;
  std::string report;
};

huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunPresentation(
    huxerui::agentkit::RunContext context, huxerui::agentkit::RunInput input,
    std::optional<huxerui::agentkit::RunCheckpoint> checkpoint, std::string export_directory,
    std::optional<PresentationSkillEvidence> skill = {}, bool image_generation = false);

} // namespace agentkit_preview
