#pragma once

#include "code_workspace.h"

#include <huxerui/resource.h>

#include <atomic>

namespace agentkit_preview {

struct LocalFilePermissions {
  bool full_access = false;
  std::vector<std::string> directories;
  std::string revision = "default";

  bool operator==(const LocalFilePermissions&) const = default;
};

class LocalFileAccess {
public:
  explicit LocalFileAccess(huxerui::File temporary_directory);

  void Apply(LocalFilePermissions permissions);

  std::shared_ptr<const LocalFilePermissions> Current() const;

  void SetDirectoryPicker(std::weak_ptr<huxerui::FilePicker> picker);

  std::shared_ptr<huxerui::FilePicker> DirectoryPicker() const;

  huxerui::agentkit::PolicyDecision Decide(const huxerui::agentkit::PolicyRequest& request) const;

  huxerui::File TaskDirectory(const huxerui::agentkit::RunId& run) const;

private:
  huxerui::File task_files_;
  std::weak_ptr<huxerui::FilePicker> picker_;
  std::atomic<std::shared_ptr<const LocalFilePermissions>> current_{std::make_shared<const LocalFilePermissions>()};
};

bool IsLocalFileTool(const huxerui::agentkit::ToolId& id);

std::optional<huxerui::StringResource> LocalFileToolLabel(const huxerui::agentkit::ToolId& id);

std::vector<huxerui::agentkit::ToolId> LocalFileTools();

huxerui::agentkit::AgentResult<std::string> LocalDirectory(const huxerui::File& directory);

huxerui::agentkit::ToolResourceResolver LocalFileResources(std::shared_ptr<CodeWorkspaces> workspaces);

huxerui::agentkit::AgentResult<void> RegisterLocalFileTools(
    const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, std::shared_ptr<LocalFileAccess> access);

} // namespace agentkit_preview
