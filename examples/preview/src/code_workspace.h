#pragma once

#include <huxerui/agentkit/runtime.h>
#include <huxerui/file.h>

#include <filesystem>
#include <mutex>
#include <set>

namespace agentkit_preview {

inline const huxerui::agentkit::AgentId CodeAgent{"preview.code"};

struct CodeEditorDraft {
  std::string version;
  std::string disk_text;
  std::string text;
};

struct CodeEditorDrafts {
  explicit CodeEditorDrafts(huxerui::File directory) : directory(std::move(directory)) {}

  huxerui::File directory;
  std::mutex mutex;
  std::set<huxerui::agentkit::ResourceId> failed_saves;
};

struct CodeWorkspace {
  huxerui::agentkit::WorkspaceId id;
  std::filesystem::path root;
  std::optional<huxerui::FileReference> grant;
  std::optional<huxerui::agentkit::ProjectId> fork_project;
  bool prepared_fork = false;
  std::string browser_name;
  std::shared_ptr<CodeEditorDrafts> editor;

  std::optional<huxerui::File> NativeFolder() const {
    return root.empty() ? std::nullopt : std::optional{huxerui::File(root.u8string())};
  }
};

class CodeWorkspaces {
public:
#ifdef __EMSCRIPTEN__
  CodeWorkspaces(huxerui::File directory, huxerui::File browser_store);
  huxerui::agentkit::Async<std::optional<huxerui::agentkit::WorkspaceId>> PickBrowserAsync();

  const huxerui::File& BrowserStore() const { return browser_store_; }
#else
  explicit CodeWorkspaces(huxerui::File directory);
#endif
  huxerui::agentkit::Async<void> LoadAsync();
  huxerui::agentkit::Async<huxerui::agentkit::WorkspaceId> AddAsync(
      huxerui::File folder, std::optional<huxerui::FileReference> grant);
  huxerui::agentkit::Async<void> RegisterForkAsync(
      huxerui::agentkit::WorkspaceId id, huxerui::File folder, std::optional<huxerui::agentkit::ProjectId> project);
  huxerui::agentkit::Async<void> CheckForkRegistrationAsync(huxerui::agentkit::WorkspaceId id,
      std::filesystem::path checkout, std::filesystem::path scope, std::optional<huxerui::agentkit::ProjectId> project);
  huxerui::agentkit::Async<void> RemoveForkRegistrationAsync(huxerui::agentkit::WorkspaceId id,
      std::filesystem::path checkout, std::filesystem::path scope, std::optional<huxerui::agentkit::ProjectId> project);
  std::optional<CodeWorkspace> Find(huxerui::agentkit::WorkspaceId id) const;
  std::vector<CodeWorkspace> Folders() const;
  std::vector<huxerui::agentkit::WorkspaceId> ProjectForks(const huxerui::agentkit::ProjectId& project) const;
  const std::shared_ptr<CodeEditorDrafts>& Editor() const { return editor_; }

private:
  huxerui::File directory_;
#ifdef __EMSCRIPTEN__
  huxerui::File browser_store_;
#endif
  std::map<huxerui::agentkit::WorkspaceId, CodeWorkspace> workspaces_;
  std::shared_ptr<CodeEditorDrafts> editor_;
};

huxerui::agentkit::AgentResult<huxerui::File> ResolveCodeFile(const CodeWorkspace& workspace, std::string_view relative);

} // namespace agentkit_preview
