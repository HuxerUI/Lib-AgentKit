#pragma once

#include <huxerui/agentkit/runtime.h>

namespace agentkit_preview {

struct BrowserWorkspace {
  huxerui::agentkit::WorkspaceId id;
  std::string name;
};

struct BrowserWorkspaceEntry {
  std::string name;
  bool directory;
  bool operator==(const BrowserWorkspaceEntry&) const = default;
};

struct BrowserWorkspaceReport {
  std::string name;
  std::string text;
};

huxerui::agentkit::Async<std::vector<BrowserWorkspace>> LoadBrowserWorkspaces(huxerui::File store);
huxerui::agentkit::Async<std::optional<BrowserWorkspace>> PickBrowserWorkspace(huxerui::File store);
huxerui::agentkit::Async<void> AuthorizeBrowserWorkspace(huxerui::File store, huxerui::agentkit::WorkspaceId workspace);
huxerui::agentkit::Async<std::vector<BrowserWorkspaceEntry>> ListBrowserWorkspaceReports(
    huxerui::File store, huxerui::agentkit::WorkspaceId workspace, std::string path);
huxerui::agentkit::Async<BrowserWorkspaceReport> ReadBrowserWorkspaceReport(
    huxerui::File store, huxerui::agentkit::WorkspaceId workspace, std::string path);

} // namespace agentkit_preview
