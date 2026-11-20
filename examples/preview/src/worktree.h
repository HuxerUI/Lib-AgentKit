#pragma once

#include "code_workspace.h"

namespace agentkit_preview {

struct WorktreePreview {
  huxerui::agentkit::WorkspaceCopyPlanRef plan;
  std::string source;
  std::string commit;
  std::string directory;
  std::string branch;
  huxerui::agentkit::CommandId command;
  huxerui::agentkit::ForkSessionRequest request;
  bool cleanup_started = false;
  std::optional<huxerui::agentkit::CommandId> comparison;
  std::optional<huxerui::agentkit::AgentError> recovery_error;
};

class WorktreeForks {
public:
  WorktreeForks(huxerui::File directory, std::shared_ptr<CodeWorkspaces> workspaces);
  huxerui::agentkit::Async<void> LoadAsync();
  huxerui::agentkit::Async<WorktreePreview> PrepareAsync(huxerui::agentkit::PrincipalId principal,
      huxerui::agentkit::HistoryBoundary boundary, huxerui::agentkit::WorkspaceId workspace,
      std::optional<huxerui::agentkit::ProjectId> project, std::string title, std::string revision, std::string directory,
      std::string branch);
  huxerui::agentkit::Async<std::vector<WorktreePreview>> PrepareComparisonAsync(
      huxerui::agentkit::PrincipalId principal, huxerui::agentkit::WorkspaceId workspace,
      huxerui::agentkit::CommandId command, huxerui::agentkit::ForkReviewRequest request);
  huxerui::agentkit::Async<std::vector<WorktreePreview>> PendingAsync(
      std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime, huxerui::agentkit::PrincipalId principal);
  huxerui::agentkit::Async<std::optional<huxerui::agentkit::WorkspaceId>> AllocateAsync(huxerui::agentkit::WorkspaceRequest request);
  huxerui::agentkit::Async<void> ConfirmAsync(huxerui::agentkit::WorkspaceCopyPlanRef plan,
      huxerui::agentkit::CommandId command, huxerui::agentkit::SessionId session);
  huxerui::agentkit::Async<void> DiscardAsync(std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
      huxerui::agentkit::PrincipalId principal, huxerui::agentkit::WorkspaceCopyPlanRef plan,
      huxerui::agentkit::CommandId command);

private:
  huxerui::File directory_;
  std::shared_ptr<CodeWorkspaces> workspaces_;
};

} // namespace agentkit_preview
