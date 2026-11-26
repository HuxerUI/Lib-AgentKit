#pragma once

#include "code_workspace.h"

namespace agentkit_preview {

class RunSettings;

inline const huxerui::agentkit::ToolId ReadCodeFiles{"preview.code.read"};
inline const huxerui::agentkit::ToolId ApplyCodePatch{"preview.code.apply"};
inline const huxerui::agentkit::ToolId ListWorkspaceFiles{"list_files"};
inline const huxerui::agentkit::ToolId SearchWorkspaceFiles{"search_files"};
inline const huxerui::agentkit::ToolId ReadWorkspaceFiles{"read_files"};
inline const huxerui::agentkit::ToolId ApplyWorkspacePatch{"apply_patch"};
inline const huxerui::agentkit::ContextProviderId CodeEditorContext{"preview.code.editor"};
inline constexpr std::string_view CodeEditorVersionPrefix = "editor:";

inline bool IsCodePatchTool(const huxerui::agentkit::ToolId& id) {
  return id == ApplyCodePatch || id == ApplyWorkspacePatch;
}

bool IsWorkspaceTool(const huxerui::agentkit::ToolId& id);

std::vector<huxerui::agentkit::ToolId> WorkspaceModelTools(bool tests_configured);

inline constexpr std::string_view WorkspaceAnswerStage = "workspace-answer";

huxerui::agentkit::Async<bool> AllowsWorkspaceTools(huxerui::agentkit::RunContext run,
    const huxerui::agentkit::AgentId& agent, std::weak_ptr<huxerui::agentkit::AgentRuntime> runtime,
    const RunSettings& settings);

huxerui::agentkit::Async<huxerui::agentkit::RunCompletion> AnswerWithWorkspaceTools(
    huxerui::agentkit::RunContext run, bool tests_configured, bool restore, std::string findings = {});

struct CodeEditorFile {
  huxerui::agentkit::ResourceId id;
  CodeEditorDraft draft;
};

struct CodeFileText {
  std::string path;
  std::string content;
  std::optional<std::vector<huxerui::agentkit::PartId>> design_nodes;
  std::optional<CodeEditorFile> editor;
};

huxerui::agentkit::Async<CodeEditorFile> OpenCodeEditorFile(CodeWorkspace workspace, std::string path);
huxerui::agentkit::AgentResult<void> EditCodeBuffer(
    CodeEditorDrafts& drafts, CodeEditorFile& file, std::string text);

huxerui::agentkit::JsonSchema CodeFileListSchema();
huxerui::agentkit::JsonSchema CodeProposalSchema(bool design_nodes = false);
huxerui::agentkit::AgentResult<std::vector<CodeFileText>> ReadCodeProposal(const huxerui::agentkit::JsonValue& value);
huxerui::agentkit::AgentResult<huxerui::agentkit::JsonValue> PrepareCodePatch(
    const std::vector<CodeFileText>& originals, const std::vector<CodeFileText>& proposal);

huxerui::agentkit::ToolResourceResolver CodeResources(std::shared_ptr<CodeWorkspaces> workspaces);
bool HasCodeTargets(const std::vector<huxerui::agentkit::TargetBaseline>& targets);
huxerui::agentkit::Async<huxerui::agentkit::ToolExecutionResult> ReadCodeChangeResultAsync(
    huxerui::File evidence, CodeWorkspace workspace, huxerui::agentkit::ToolCallSnapshot call);
huxerui::agentkit::Async<huxerui::agentkit::CommandReceipt> ReconcileCodeChangeAsync(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::SessionId session,
    huxerui::agentkit::ToolCallSnapshot call, CodeWorkspace workspace, huxerui::File evidence, huxerui::agentkit::CommandId command);
huxerui::agentkit::Async<std::vector<std::string>> PreviewCodeCandidate(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::File evidence, huxerui::agentkit::ArtifactRef artifact);
huxerui::agentkit::Async<std::string> PreviewAppliedFile(huxerui::File evidence,
    const huxerui::agentkit::ArtifactRecord& record, const huxerui::agentkit::SessionSnapshot& owner,
    const huxerui::agentkit::PartId& part);
struct CodeAdoptionScope {
  std::vector<huxerui::agentkit::PartId> parts;
  std::vector<huxerui::agentkit::TargetBaseline> targets;
};

huxerui::agentkit::Async<CodeAdoptionScope> InspectCodeAdoption(
    std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::File evidence, CodeWorkspace workspace,
    huxerui::agentkit::ArtifactRef artifact);
huxerui::agentkit::ChangeAdapters WithCodeChanges(huxerui::agentkit::ChangeAdapters changes, huxerui::File evidence,
    std::shared_ptr<CodeWorkspaces> workspaces, std::shared_ptr<std::weak_ptr<huxerui::agentkit::AgentRuntime>> runtime);
huxerui::agentkit::AgentResult<void> RegisterCodeTools(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, huxerui::File evidence,
    std::shared_ptr<CodeWorkspaces> workspaces);
huxerui::agentkit::AgentResult<void> VerifyWorkspaceVersions(
    const huxerui::File& root, const huxerui::agentkit::JsonValue& document);

} // namespace agentkit_preview
