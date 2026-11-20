#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

inline const huxerui::agentkit::SkillSourceId PreviewSkills{"preview.packaged"};
inline const huxerui::agentkit::SkillId ReleasePresentationSkill{"release-presentation"};
inline constexpr std::string_view ReleaseTemplatePath = "assets/outline.json";
inline constexpr std::string_view ReleaseSkillStage = "release-presentation-skill";
inline constexpr std::string_view ReleaseSkillLabel = "Release presentation";

std::shared_ptr<huxerui::agentkit::SkillSource> MakePreviewSkills();
huxerui::View SkillPicker(std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    huxerui::agentkit::SkillCatalogQuery query, huxerui::State<std::vector<huxerui::agentkit::SkillRef>> selection);

} // namespace agentkit_preview
