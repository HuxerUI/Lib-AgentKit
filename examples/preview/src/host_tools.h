#pragma once

#include "local_files.h"

namespace agentkit_preview {

inline const huxerui::agentkit::ToolId RunPowerShell{"run_powershell"};
inline const huxerui::agentkit::ToolId WebSearch{"web_search"};
inline const huxerui::agentkit::ToolId WebFetch{"web_fetch"};

std::optional<huxerui::StringResource> HostToolLabel(const huxerui::agentkit::ToolId& id);

#ifdef _WIN32
struct WebSearchConfiguration {
  std::string endpoint;
  std::string key;

  bool Available() const;
};

huxerui::agentkit::ToolResourceResolver HostToolResources(std::shared_ptr<CodeWorkspaces> workspaces,
    std::shared_ptr<LocalFileAccess> access);

huxerui::agentkit::AgentResult<void> RegisterHostTools(const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime,
    std::shared_ptr<LocalFileAccess> access, WebSearchConfiguration search);
#endif

} // namespace agentkit_preview
