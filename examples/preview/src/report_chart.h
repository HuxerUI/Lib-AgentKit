#pragma once

#include <huxerui/agentkit.h>
#include <huxerui/file.h>

namespace agentkit_preview {

inline const huxerui::agentkit::ContentTypeRef ReportChart{"preview.report.chart", 1};
inline const huxerui::agentkit::ToolId PublishReportChart{"preview.report.chart.publish"};
inline const huxerui::agentkit::ToolId ExportReportChart{"preview.report.chart.export"};
inline constexpr std::string_view ReportChartStage = "report-chart";

huxerui::View RenderReportChart(huxerui::agentkit::CustomBlockRef origin, const huxerui::agentkit::CustomBlock& block,
                              std::shared_ptr<huxerui::agentkit::Conversation> conversation);

huxerui::agentkit::AgentResult<void> RegisterReportChart(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, huxerui::File exports);
huxerui::agentkit::ToolResourceResolver ReportChartResources(huxerui::File exports);
huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunReportChart(huxerui::agentkit::RunContext run,
    const huxerui::agentkit::RunInput& input, std::optional<huxerui::agentkit::RunCheckpoint> checkpoint);

} // namespace agentkit_preview
