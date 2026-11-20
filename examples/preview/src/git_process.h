#pragma once

#include <huxerui/agentkit/types.h>
#include <huxerui/file.h>

#include <filesystem>

namespace agentkit_preview {

struct GitResult {
  int exit_code;
  std::string output;
};

huxerui::agentkit::AgentResult<GitResult> RunGit(huxerui::File scratch, std::filesystem::path directory,
    std::vector<std::string> arguments);

} // namespace agentkit_preview
