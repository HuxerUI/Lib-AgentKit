#pragma once

#include <huxerui/agentkit/runtime.h>
#include <huxerui/view.h>

namespace agentkit_preview {
inline const huxerui::agentkit::ToolId RunCodeTests{"preview.code.ctest"};
inline const huxerui::agentkit::ToolId RunWorkspaceTests{"run_tests"};
inline const huxerui::agentkit::AgentId VerificationAgent{"preview.verify"};
inline const huxerui::agentkit::JsonValue VerifyCandidateTests{std::string("verify-candidate")};

std::optional<huxerui::View> RenderCTestDiagnostics(const huxerui::agentkit::Message& message,
    const huxerui::agentkit::ContentBlock& block);

huxerui::agentkit::AgentResult<void> RegisterCodeTests(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, huxerui::File executable,
    huxerui::File logs, huxerui::File evidence);
huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> RunVerification(
    huxerui::agentkit::RunContext run, huxerui::agentkit::RunInput input,
    std::optional<huxerui::agentkit::RunCheckpoint> checkpoint);

} // namespace agentkit_preview
