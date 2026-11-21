#include "code_workspace.h"

#ifdef __EMSCRIPTEN__
#include "browser_workspace.h"
#endif

#include <algorithm>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using huxerui::File;

template<class T>
kit::AgentResult<T> Unavailable(std::string message) {
  return kit::AgentResult<T>::Failure({.code = kit::ErrorCode::PermissionDenied, .message = std::move(message)});
}

std::filesystem::path CanonicalDirectory(File file) {
  std::error_code error;
  auto path = std::filesystem::canonical(std::filesystem::u8path(file.Path()), error);
  if (error || !std::filesystem::is_directory(path, error) || error) return {};
  return path;
}

bool PendingWorktree(const std::filesystem::path& path) {
  for (auto folder = path; !folder.empty();) {
    const auto dotgit = File((folder / ".git").u8string());
    const auto info = dotgit.Stat();
    if (info.Succeeded()) {
      if (info.Value().type != huxerui::FileType::File || info.Value().size > 8192) return false;
      auto pointer = dotgit.ReadString();
      if (!pointer.Succeeded() || !pointer.Value().starts_with("gitdir: ")) return false;
      auto target = pointer.Value().substr(8);
      while (!target.empty() && (target.back() == '\r' || target.back() == '\n')) target.pop_back();
      const auto git = File((folder / std::filesystem::u8path(target)).u8string());
      if (git.Child("agentkit-discard").Exists()) return true;
      const auto lock = git.Child("locked");
      auto state = lock.Stat();
      if (!state.Succeeded() || state.Value().size > 4096) return false;
      auto reason = lock.ReadString();
      return reason.Succeeded() && reason.Value().starts_with("AgentKit:");
    }
    const auto parent = folder.parent_path();
    if (parent == folder) break;
    folder = parent;
  }
  return false;
}

std::filesystem::path ImportableDirectory(File file) {
  auto path = CanonicalDirectory(std::move(file));
  return !path.empty() && PendingWorktree(path) ? std::filesystem::path{} : std::move(path);
}

bool DirectoryLink(const std::filesystem::path& path, std::filesystem::file_status status) {
  if (std::filesystem::is_symlink(status)) return true;
#ifdef _WIN32
  const auto flags = GetFileAttributesW(path.c_str());
  return flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

bool ValidWorkspaceId(std::string_view id) {
  return !id.empty() && id.size() <= 128 && std::ranges::all_of(id, [](char ch) {
    return (ch >= '0' && ch <= '9') || ch == '-';
  });
}

std::optional<CodeWorkspace> ReadWorkspace(File file, kit::WorkspaceId id) {
  auto value = file.ReadString();
  if (!value.Succeeded() || value.Value().empty()) return {};
  std::string path = std::move(value).Value();
  std::optional<kit::ProjectId> project;
  const bool prepared_fork = path.starts_with('{');
  if (prepared_fork) {
    auto stored = nlohmann::json::parse(path, nullptr, false);
    if (!stored.is_object() || !stored.contains("path") || !stored.at("path").is_string() ||
        !stored.contains("project") || !(stored.at("project").is_null() || stored.at("project").is_string())) return {};
    path = stored.at("path").get<std::string>();
    if (!stored.at("project").is_null()) project = kit::ProjectId{stored.at("project").get<std::string>()};
  }
  auto root = std::filesystem::u8path(path);
  if (!root.is_absolute()) return {};
  return CodeWorkspace{std::move(id), std::move(root), {}, std::move(project), prepared_fork};
}

std::vector<CodeWorkspace> ReadRegisteredDirectories(File directory) {
  std::vector<CodeWorkspace> entries;
  std::error_code error;
  auto iter = std::filesystem::directory_iterator(std::filesystem::u8path(directory.Path()), error);
  for (const auto& entry : iter) {
    if (entry.path().extension() != ".workspace" || !entry.is_regular_file(error) || error) continue;
    auto id = entry.path().stem().string();
    if (!ValidWorkspaceId(id)) continue;
    if (auto workspace = ReadWorkspace(File(entry.path().u8string()), {std::move(id)}))
      entries.push_back(std::move(*workspace));
  }
  return entries;
}

kit::AgentResult<void> CheckForkRegistration(File directory, const kit::WorkspaceId& id, const std::filesystem::path& checkout,
    const std::filesystem::path& scope, const std::optional<kit::ProjectId>& project, bool remove) {
  const auto conflict = [] {
    return kit::AgentResult<void>::Failure({
        .code = kit::ErrorCode::Conflict, .message = "The checkout has another workspace registration"});
  };
  const auto parent = std::filesystem::u8path(directory.Path());
  std::error_code error;
  std::filesystem::directory_iterator files(parent, error);
  if (error) return Unavailable<void>("Could not verify the workspace registry");
  const auto selected = directory.Child(id.value + ".workspace");
  for (const auto& file : files) {
    if (file.path().extension() != ".workspace") continue;
    kit::WorkspaceId registered{file.path().stem().string()};
    auto workspace = ReadWorkspace(File(file.path().u8string()), registered);
    if (!workspace) return Unavailable<void>("A workspace registration could not be verified");
    if (registered == id) {
      if (!workspace->prepared_fork || workspace->root != scope || workspace->fork_project != project) return conflict();
    } else {
      const auto relative = workspace->root.lexically_relative(checkout);
      if (!relative.empty() && *relative.begin() != "..") return conflict();
    }
  }
  if (remove && selected.Exists() && !selected.Delete())
    return kit::AgentResult<void>::Failure({
        .code = kit::ErrorCode::PersistenceFailure, .message = "Could not remove the abandoned workspace registration"});
  return kit::AgentResult<void>::Success();
}

} // namespace

#ifdef __EMSCRIPTEN__
CodeWorkspaces::CodeWorkspaces(huxerui::File directory, huxerui::File browser_store)
    : directory_(std::move(directory)), browser_store_(std::move(browser_store)),
      editor_(std::make_shared<CodeEditorDrafts>(directory_.Child("editor-drafts"))) {}

kit::Async<std::optional<kit::WorkspaceId>> CodeWorkspaces::PickBrowserAsync() {
  using Result = kit::AgentResult<std::optional<kit::WorkspaceId>>;
  auto chosen = co_await PickBrowserWorkspace(browser_store_);
  if (!chosen.Succeeded()) co_return Result::Failure(chosen.Error());
  if (!chosen.Value()) co_return Result::Success(std::nullopt);
  auto folder = std::move(*chosen.Value());
  const auto id = folder.id;
  workspaces_.insert_or_assign(id, CodeWorkspace{.id = id, .browser_name = std::move(folder.name)});
  co_return Result::Success(id);
}
#else
CodeWorkspaces::CodeWorkspaces(huxerui::File directory)
    : directory_(std::move(directory)), editor_(std::make_shared<CodeEditorDrafts>(directory_.Child("editor-drafts"))) {}
#endif

kit::Async<void> CodeWorkspaces::LoadAsync() {
#ifdef __EMSCRIPTEN__
  auto restored = co_await LoadBrowserWorkspaces(browser_store_);
  if (!restored.Succeeded()) co_return kit::AgentResult<void>::Failure(restored.Error());
  for (auto& folder : restored.Value()) {
    auto id = std::move(folder.id);
    workspaces_.try_emplace(id, CodeWorkspace{.id = id, .browser_name = std::move(folder.name)});
  }
#else
  if (!co_await directory_.CreateDirectoriesAsync()) {
    co_return kit::AgentResult<void>::Failure({
        .code = kit::ErrorCode::PersistenceFailure, .message = "Could not open the workspace registry"});
  }
  auto entries = co_await huxerui::RunWorker(ReadRegisteredDirectories, directory_);
  for (auto& entry : entries) workspaces_.try_emplace(entry.id, std::move(entry));
#endif
  co_return kit::AgentResult<void>::Success();
}

kit::Async<kit::WorkspaceId> CodeWorkspaces::AddAsync(File folder, std::optional<huxerui::FileReference> grant) {
  if (grant && (!grant->CanWrite() || grant->Type() != huxerui::FileType::Directory)) {
    co_return Unavailable<kit::WorkspaceId>("Choose a writable local directory");
  }
  auto path = co_await huxerui::RunWorker(ImportableDirectory, std::move(folder));
  if (path.empty()) co_return Unavailable<kit::WorkspaceId>("The workspace is unavailable or its copy still needs recovery");
  for (auto& [id, workspace] : workspaces_) {
    if (workspace.root != path) continue;
    if (grant) workspace.grant = std::move(grant);
    co_return kit::AgentResult<kit::WorkspaceId>::Success(id);
  }
  kit::WorkspaceId id{kit::NewCommandId().value};
  const auto location = path.u8string();
  auto record = directory_.Child(id.value + ".workspace");
  auto pending = directory_.Child(id.value + ".pending");
  auto written = co_await pending.WriteStringAsync(std::string(location.begin(), location.end()));
  if (!written || !co_await pending.MoveToAsync(record)) {
    static_cast<void>(co_await pending.DeleteAsync());
    co_return kit::AgentResult<kit::WorkspaceId>::Failure({
        .code = kit::ErrorCode::PersistenceFailure, .message = "Could not record the selected workspace"});
  }
  workspaces_.emplace(id, CodeWorkspace{id, std::move(path), std::move(grant)});
  co_return kit::AgentResult<kit::WorkspaceId>::Success(std::move(id));
}

kit::Async<void> CodeWorkspaces::RegisterForkAsync(kit::WorkspaceId id, File folder, std::optional<kit::ProjectId> project) {
  auto path = co_await huxerui::RunWorker(CanonicalDirectory, std::move(folder));
  if (!ValidWorkspaceId(id.value) || path.empty()) co_return Unavailable<void>("The allocated workspace directory is unavailable");
  const auto target = directory_.Child(id.value + ".workspace");
  const auto pending = directory_.Child(kit::NewCommandId().value + ".pending");
  const auto location = path.u8string();
  const auto contents = nlohmann::json{{"path", std::string(location.begin(), location.end())},
      {"project", project ? nlohmann::json(project->value) : nlohmann::json(nullptr)}}.dump();
  auto written = co_await pending.WriteStringAsync(contents);
  if (!written || !co_await pending.MoveToAsync(target)) {
    static_cast<void>(co_await pending.DeleteAsync());
    auto source = co_await huxerui::RunWorker(ReadWorkspace, target, id);
    if (!source || !source->prepared_fork || source->root != path || source->fork_project != project)
      co_return kit::AgentResult<void>::Failure({
          .code = kit::ErrorCode::PersistenceFailure, .message = "Could not register the allocated workspace"});
  }
  workspaces_.try_emplace(id, CodeWorkspace{id, std::move(path), {}, std::move(project), true});
  co_return kit::AgentResult<void>::Success();
}

kit::Async<void> CodeWorkspaces::CheckForkRegistrationAsync(kit::WorkspaceId id, std::filesystem::path checkout,
    std::filesystem::path scope, std::optional<kit::ProjectId> project) {
  if (!ValidWorkspaceId(id.value)) co_return Unavailable<void>("Invalid worktree workspace ID");
  if (auto found = workspaces_.find(id); found != workspaces_.end() && found->second.grant)
    co_return Unavailable<void>("The worktree has been opened separately");
  co_return co_await huxerui::RunWorker(CheckForkRegistration, directory_, std::move(id), std::move(checkout),
                                      std::move(scope), std::move(project), false);
}

kit::Async<void> CodeWorkspaces::RemoveForkRegistrationAsync(kit::WorkspaceId id, std::filesystem::path checkout,
    std::filesystem::path scope, std::optional<kit::ProjectId> project) {
  if (!ValidWorkspaceId(id.value)) co_return Unavailable<void>("Invalid worktree workspace ID");
  auto removed = co_await huxerui::RunWorker(CheckForkRegistration, directory_, id, std::move(checkout),
                                           std::move(scope), std::move(project), true);
  if (removed.Succeeded()) workspaces_.erase(id);
  co_return removed;
}

std::vector<kit::WorkspaceId> CodeWorkspaces::ProjectForks(const kit::ProjectId& project) const {
  std::vector<kit::WorkspaceId> result;
  for (const auto& [id, workspace] : workspaces_) {
    if (workspace.fork_project == project) result.push_back(id);
  }
  return result;
}

std::vector<CodeWorkspace> CodeWorkspaces::Folders() const {
  std::vector<CodeWorkspace> folders;
  for (const auto& [id, workspace] : workspaces_) {
    if (workspace.fork_project || workspace.prepared_fork) continue;
    if (auto visible = Find(id)) folders.push_back(std::move(*visible));
  }
  return folders;
}

std::optional<CodeWorkspace> CodeWorkspaces::Find(kit::WorkspaceId id) const {
  const auto found = workspaces_.find(id);
  if (found == workspaces_.end()) return std::nullopt;
#ifdef __APPLE__
  // A path alone cannot restore a native file-picker grant on macOS.
  if (!found->second.grant) return std::nullopt;
#endif
  auto workspace = found->second;
  workspace.editor = editor_;
  return workspace;
}

namespace {

kit::AgentResult<File> ResolveCodePath(const CodeWorkspace& workspace, std::string_view relative, bool directory) {
  if (workspace.root.empty()) return Unavailable<File>("A native workspace is needed for this action");
  if (directory && relative == ".") relative = {};
  if ((!directory && relative.empty()) || relative.find_first_of("\\:\"<>|?*\r\n") != relative.npos ||
      relative.find('\0') != relative.npos || relative.starts_with('/') || relative.ends_with('/')) {
    return Unavailable<File>("Use a workspace-relative file path");
  }
  auto path = std::filesystem::u8path(relative);
  if (path.is_absolute() || path.has_root_name()) return Unavailable<File>("Use a workspace-relative file path");
  auto full = workspace.root;
  std::error_code error;
  const auto root = std::filesystem::canonical(full, error);
  if (error || root != full || DirectoryLink(full, std::filesystem::symlink_status(full, error)) || error) {
    return Unavailable<File>("The workspace directory changed");
  }
  for (const auto& component : path) {
    auto name = component.u8string();
    for (auto& ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (name.empty() || name.back() == '.' || name.back() == ' ' || name == u8".git") {
      return Unavailable<File>("The file is outside the editable workspace");
    }
    full /= component;
    const auto status = std::filesystem::symlink_status(full, error);
    if (error || DirectoryLink(full, status)) {
      return Unavailable<File>("A workspace file is unavailable or uses a linked path");
    }
  }
  const bool exists = directory ? std::filesystem::is_directory(full, error) : std::filesystem::is_regular_file(full, error);
  if (!exists || error) {
    return Unavailable<File>(directory ? "Choose an existing workspace directory" : "Choose an existing regular file in the workspace");
  }
  const auto canonical = std::filesystem::canonical(full, error);
  if (error) return Unavailable<File>("The workspace file could not be resolved");
  auto beneath = canonical.lexically_relative(workspace.root);
  if (beneath.empty() || *beneath.begin() == "..") return Unavailable<File>("The file is outside the editable workspace");
  return kit::AgentResult<File>::Success(File(canonical.u8string()));
}

} // namespace

kit::AgentResult<File> ResolveCodeFile(const CodeWorkspace& workspace, std::string_view relative) {
  return ResolveCodePath(workspace, relative, false);
}

kit::AgentResult<File> ResolveCodeDirectory(const CodeWorkspace& workspace, std::string_view relative) {
  return ResolveCodePath(workspace, relative, true);
}

} // namespace agentkit_preview
