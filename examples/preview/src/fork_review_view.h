#pragma once

#include <huxerui/agentkit.h>

namespace agentkit_preview {

class WorktreeForks;
class CodeWorkspaces;

struct ComparisonWorkspaceServices {
  std::shared_ptr<WorktreeForks> worktrees;
  std::shared_ptr<CodeWorkspaces> workspaces;
  huxerui::File evidence;
  huxerui::agentkit::PrincipalId principal;
  bool code_available;
  std::function<void()> refresh_worktrees;
};

huxerui::View ComparisonWorkspace(std::shared_ptr<huxerui::agentkit::AgentRuntime> runtime,
    std::shared_ptr<huxerui::agentkit::Conversation> source, std::optional<huxerui::agentkit::ForkReviewId> review,
    ComparisonWorkspaceServices services,
    std::function<void(std::optional<huxerui::agentkit::ForkReviewId>)> select, std::function<void()> back,
    std::function<void(huxerui::agentkit::SessionId)> open_branch,
    std::function<void(huxerui::agentkit::SessionId)> open_approval);

} // namespace agentkit_preview
