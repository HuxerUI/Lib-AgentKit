#pragma once

#include <huxerui/agentkit/mcp.h>
#include <huxerui/file.h>

#include <map>

namespace agentkit_preview {

struct McpServer {
  std::string id;
  std::string name;
  std::string url;
  std::string token_reference;
  bool enabled = true;
  bool legacy = false;
  bool operator==(const McpServer&) const = default;
};

void ValidateMcpServer(const McpServer& server);

class McpConnections : public std::enable_shared_from_this<McpConnections> {
public:
  explicit McpConnections(huxerui::File directory);

  std::vector<huxerui::agentkit::ToolCatalogId> Register(
      const std::shared_ptr<huxerui::agentkit::AgentRuntime>& runtime, const std::vector<McpServer>& servers);
  huxerui::Task<void> Start();
  huxerui::agentkit::Async<void> Refresh(std::string id);
  huxerui::agentkit::Async<void> Apply(const std::vector<McpServer>& servers);

  struct Status {
    std::size_t tools = 0;
    std::string error;
  };
  std::optional<Status> ConnectionStatus(const McpServer& server) const;

private:
  struct Connection;
  struct Entry {
    McpServer server;
    std::shared_ptr<Connection> connection;
    std::shared_ptr<huxerui::agentkit::McpToolCatalog> catalog;
    Status status;
  };

  huxerui::File directory_;
  std::map<std::string, Entry, std::less<>> servers_;
};

} // namespace agentkit_preview
