#pragma once

/// @file
/// @brief Host transport and tool binding for a Model Context Protocol (MCP) catalog.
/// Use McpToolCatalog with a registered AgentRuntime and expose its ToolCatalogId through AgentDefinition::tool_catalogs.
/// The host supplies the JSON-RPC exchange and declares the effects of each accepted remote tool.

#include <huxerui/agentkit/runtime.h>

namespace huxerui::agentkit {

/// @brief Protocol version offered in the MCP initialization request.
inline constexpr std::string_view McpToolProtocolVersion = "2026-07-28";

/// @brief Metadata decoded from the server's tools/list response and passed to McpToolBinder.
struct McpToolDescription {
  /// Tool name used by the server.
  std::string name;
  /// Description offered by the server, possibly empty.
  std::string description;
  /// Schema for arguments accepted by the remote tool.
  JsonSchema input_schema;
  /// Schema for structured results, when declared by the server.
  std::optional<JsonSchema> output_schema;
  /// Original tool descriptor, available for host binding decisions.
  JsonValue metadata;
};

/// @brief Host-approved execution contract for one remote tool.
struct McpToolBinding {
  /// Positive host revision identifying this binding across refresh and recovery.
  Revision version;
  /// Host declaration of whether calling the tool can have side effects.
  ToolEffect effect;
  /// Scheduling constraint in addition to any PreparedChange::conflict_keys.
  ToolConcurrency concurrency = ToolConcurrency::Serial;
  /// Optional host preparation that produces the reviewed plan.
  ToolPrepare prepare;
};

/// @brief A JSON-RPC exchange requested by an McpToolCatalog.
struct McpToolRequest {
  /// Owned JSON-RPC request or initialization notification.
  JsonValue message;
  /// Remote tool metadata for a tools/call exchange, when applicable.
  std::optional<McpToolDescription> tool;
  /// Cancellation signal to pass through to the host transport.
  std::stop_token stop;
};

/// @brief Host callback bound to one server and authorization scope.
/// Return the final JSON-RPC response for a request. A transport must avoid replaying a tools/call exchange.
using McpToolExchange = std::function<Async<JsonValue>(McpToolRequest)>;
/// @brief Maps a discovered remote descriptor to an executable host binding.
/// An empty successful value excludes the tool from the catalog.
using McpToolBinder = std::function<AgentResult<std::optional<McpToolBinding>>(const McpToolDescription&)>;

/// @brief Configuration consumed by McpToolCatalog::Register().
struct McpToolCatalogOptions {
  /// Catalog ID to reference from an AgentDefinition.
  ToolCatalogId catalog;
  /// Client name sent during MCP initialization.
  std::string client_name;
  /// Client version sent during MCP initialization.
  std::string client_version;
  /// Host-owned JSON-RPC transport.
  McpToolExchange exchange;
  /// Host policy that validates and binds each discovered tool.
  McpToolBinder bind;
};

/// @brief Adapter that publishes MCP tools through AgentRuntime's dynamic catalog.
class McpToolCatalog {
public:
  /// @brief Registers the catalog before AgentRuntime::StartAsync().
  /// Call RefreshAsync() to publish the first snapshot before restoring pending calls.
  /// @param runtime Owning runtime whose agents reference the catalog.
  /// @param options Catalog identity, client identity, transport and tool binder.
  /// @return A registered adapter or a registration error.
  static AgentResult<std::shared_ptr<McpToolCatalog>> Register(
      std::shared_ptr<AgentRuntime> runtime, McpToolCatalogOptions options);

  /// @brief Initializes the connection if needed and publishes the paginated, bound tool list.
  /// @param stop Cancellation token for connection setup and discovery.
  /// @return Success after a complete tool snapshot is published.
  Async<void> RefreshAsync(std::stop_token stop = {});

  /// @brief Processes supported notifications such as tools/list_changed.
  /// @return True when the notification is recognized and handled.
  /// @param notification A JSON-RPC 2.0 notification from this MCP connection, without a request ID.
  /// @param stop Cancellation token for any discovery triggered by the notification.
  Async<bool> HandleNotificationAsync(JsonValue notification, std::stop_token stop = {});

  /// @brief Clears the catalog and invalidates unissued calls bound to its retired tools.
  /// @return Success after the empty catalog snapshot is published.
  Async<void> WithdrawAsync();

private:
  struct State;
  explicit McpToolCatalog(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

} // namespace huxerui::agentkit
