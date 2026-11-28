#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

inline const huxerui::agentkit::ToolId UpdateRunPlan{"update_plan"};
inline const huxerui::agentkit::ContentTypeRef RunPlanContent{"ganwork.run_plan", 1};

huxerui::agentkit::AgentResult<void> RegisterRunPlan(huxerui::agentkit::AgentRuntime& runtime);

struct RunPlanStep {
  std::string title;
  std::string_view status;
  std::string detail;
};

huxerui::Task<void> RecordRunPlan(huxerui::agentkit::RunContext run, std::string_view revision,
    std::vector<RunPlanStep> steps);

huxerui::View RunPlanCard(huxerui::agentkit::RunId run, huxerui::agentkit::JsonValue plan,
    std::shared_ptr<huxerui::agentkit::Conversation> conversation,
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    std::function<void(huxerui::agentkit::SessionId, huxerui::agentkit::InteractionId)> open_request);

} // namespace agentkit_preview
