#include "mcp_connections.h"

#include "run_settings.h"
#include "preview_json.h"

#include <huxerui/huxerui.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr std::size_t response_limit = 2 << 20;
constexpr std::size_t ledger_limit = 1 << 20;
constexpr std::string_view legacy_protocol = "2025-11-25";

template<class T>
kit::AgentResult<T> Failure(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

bool EqualHeader(std::string_view a, std::string_view b) {
  const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
  return std::ranges::equal(a, b, [&](char left, char right) { return lower(left) == lower(right); });
}

std::string Header(std::span<const HttpHeader> headers, std::string_view name) {
  const auto found = std::ranges::find_if(headers, [&](const auto& entry) { return EqualHeader(entry.name, name); });
  return found == headers.end() ? std::string{} : found->value;
}

kit::Revision ToolVersion(std::string_view input) {
  kit::Revision hash = 14695981039346656037ULL;
  for (unsigned char ch : input) hash = (hash ^ ch) * 1099511628211ULL;
  return (hash & 0x7fffffffffffffffULL) + 1;
}

} // namespace

void ValidateMcpServer(const McpServer& server) {
  const auto uri = Uri::Parse(server.url);
  if (server.id.empty() || server.id.size() > 64 || !std::ranges::all_of(server.id, [](char ch) {
        return (ch >= '0' && ch <= '9') || ch == '-';
      }) || server.name.empty() || server.name.size() > 80 || server.url.size() > 2048 ||
      server.url.find_first_of("#\\ \t\r\n") != std::string::npos || !uri || !uri->Authority() || uri->Authority()->empty() ||
      uri->Authority()->find('@') != std::string_view::npos)
    throw std::invalid_argument("Enter a server name and a valid MCP endpoint URL");
  if (!server.url.starts_with("https://")) {
    const auto authority = *uri->Authority();
    if (!server.url.starts_with("http://") || !(authority == "127.0.0.1" || authority.starts_with("127.0.0.1:") ||
          authority == "localhost" || authority.starts_with("localhost:") || authority == "[::1]" || authority.starts_with("[::1]:")))
      throw std::invalid_argument("Use HTTPS for a remote MCP server or HTTP on localhost");
  }
  if (!server.token_reference.empty() && ModelKeyEnvironment(server.token_reference).empty())
    throw std::invalid_argument("Use a bearer token reference such as ${MCP_TOKEN}");
}

struct McpConnections::Connection {
  McpServer server;
  File ledger;
  std::shared_ptr<HttpClient> http;
  Json versions = Json::object();
  Json next = Json::object();
  std::string protocol;
  std::string session;
  bool enabled = true;
  bool refreshing = false;
  std::size_t listed = 0;

  kit::Async<Json> Post(Json body, std::stop_token stop) {
    using Result = kit::AgentResult<Json>;
    const auto stopped = [&] { return !enabled || stop.stop_requested(); };
    const auto cancelled = [&] {
      return enabled ? Failure<Json>(kit::ErrorCode::AlreadyClosed, "MCP request stopped") :
          Failure<Json>(kit::ErrorCode::PermissionDenied, "The MCP server was disabled");
    };
    if (stopped()) co_return cancelled();
    const auto id = body.value("id", std::string{});
    const auto method = body.at("method").get<std::string>();
    auto payload = body.dump();
    auto bytes = std::as_bytes(std::span(payload));
    auto headers = std::vector<HttpHeader>{{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    if (method != "initialize") headers.push_back({"Mcp-Protocol-Version",
        server.legacy ? protocol : std::string(kit::McpToolProtocolVersion)});
    if (server.legacy) {
      if (!session.empty()) headers.push_back({"Mcp-Session-Id", session});
    } else {
      headers.push_back({"Mcp-Method", method});
      if (method == "tools/call") headers.push_back({"Mcp-Name", body.at("params").at("name").get<std::string>()});
    }
    if (!server.token_reference.empty()) {
      const auto env = ModelKeyEnvironment(server.token_reference);
      const auto* token = std::getenv(env.c_str());
      if (!token || !*token) co_return Failure<Json>(kit::ErrorCode::PermissionDenied, "The MCP bearer token is unavailable");
      headers.push_back({"Authorization", "Bearer " + std::string(token)});
    }
    HttpRequest request{.url = server.url, .method = HttpMethod::Post, .headers = std::move(headers),
        .body = Bytes(bytes.begin(), bytes.end()), .timeout = method == "tools/call" ? 60s : 8s};
    auto opened = co_await http->SendStreamAsync(std::move(request));
    if (stopped()) co_return cancelled();
    if (!opened.Succeeded()) co_return Failure<Json>(kit::ErrorCode::ProviderFailure, "Could not contact the MCP server");
    auto stream = std::move(opened).Value();
    if (stream.Url() != server.url)
      co_return Failure<Json>(kit::ErrorCode::PermissionDenied, "The MCP endpoint redirected to a different URL");
    if (stream.StatusCode() == 401 || stream.StatusCode() == 403)
      co_return Failure<Json>(kit::ErrorCode::PermissionDenied, "The MCP server denied access");
    if (id.empty() && stream.StatusCode() == 202) co_return Result::Success(nullptr);
    if (server.legacy && stream.StatusCode() == 404 && !session.empty()) {
      protocol.clear();
      session.clear();
      co_return Failure<Json>(kit::ErrorCode::Conflict, "The MCP session expired. Refresh its tools to reconnect.");
    }
    if (stream.StatusCode() != 200)
      co_return Failure<Json>(kit::ErrorCode::ProviderFailure, "MCP endpoint returned HTTP " + std::to_string(stream.StatusCode()));
    if (method == "initialize") session = Header(stream.Headers(), "Mcp-Session-Id");
    const auto type = Header(stream.Headers(), "Content-Type");
    const bool sse = type.starts_with("text/event-stream");
    if (!sse && !type.starts_with("application/json"))
      co_return Failure<Json>(kit::ErrorCode::ProtocolError, "Unexpected MCP response content type");
    auto buffer = std::string{};
    auto data = std::string{};
    std::size_t total = 0;
    for (;;) {
      auto read = co_await stream.Body().ReadAsync(16384);
      if (stopped()) co_return cancelled();
      if (!read.Succeeded()) co_return Failure<Json>(kit::ErrorCode::ProviderFailure, "The MCP response stream ended unexpectedly");
      if (read.Value().empty()) {
        if (!sse) co_return Result::Success(Json::parse(buffer));
        break;
      }
      total += read.Value().size();
      if (total > response_limit) co_return Failure<Json>(kit::ErrorCode::BudgetExceeded, "MCP response is too large");
      buffer.append(reinterpret_cast<const char*>(read.Value().data()), read.Value().size());
      if (!sse) continue;
      std::size_t begin = 0;
      for (auto end = buffer.find('\n', begin); end != buffer.npos; end = buffer.find('\n', begin)) {
        auto line = std::string_view(buffer).substr(begin, end - begin);
        begin = end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() && !data.empty()) {
          auto message = Json::parse(data);
          if (message.value("id", Json{}) == id && (message.contains("result") || message.contains("error")))
            co_return Result::Success(std::move(message));
          data.clear();
        } else if (line.starts_with("data:")) {
          line.remove_prefix(5);
          if (line.starts_with(' ')) line.remove_prefix(1);
          if (!data.empty()) data += '\n';
          data += line;
        }
      }
      buffer.erase(0, begin);
    }
    co_return Failure<Json>(kit::ErrorCode::ProtocolError, "No matching MCP response was received");
  }

  kit::Async<void> Initialize(std::stop_token stop) {
    using Result = kit::AgentResult<void>;
    auto id = kit::NewCommandId().value;
    Json initialize{{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"}, {"params", {
        {"protocolVersion", legacy_protocol}, {"capabilities", Json::object()},
        {"clientInfo", {{"name", "GanWork"}, {"version", "0.1.0"}}}}}};
    auto received = co_await Post(std::move(initialize), stop);
    if (!received.Succeeded()) co_return Result::Failure(received.Error());
    const auto& response = received.Value();
    if (response.at("jsonrpc") != "2.0" || response.at("id") != id)
      co_return Failure<void>(kit::ErrorCode::ProtocolError, "Invalid MCP initialize reply");
    const auto& result = response.at("result");
    auto version = result.at("protocolVersion").get<std::string>();
    if ((version != legacy_protocol && version != "2025-06-18" && version != "2025-03-26") ||
        !result.at("capabilities").contains("tools"))
      co_return Failure<void>(kit::ErrorCode::Unsupported, "This MCP server does not offer a supported tools protocol");
    protocol = std::move(version);
    Json initialized{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    auto notify = co_await Post(std::move(initialized), stop);
    if (!notify.Succeeded()) { protocol.clear(); co_return Result::Failure(notify.Error()); }
    co_return Result::Success();
  }

  kit::Async<kit::JsonValue> Exchange(kit::McpToolRequest request) {
    using Result = kit::AgentResult<kit::JsonValue>;
    try {
      auto message = EncodeJson(request.message);
      if (server.legacy) {
        if (protocol.empty()) {
          if (request.tool) co_return Failure<kit::JsonValue>(kit::ErrorCode::Conflict, "Refresh the MCP tools before calling this server");
          auto initialized = co_await Initialize(request.stop);
          if (!initialized.Succeeded()) co_return Result::Failure(initialized.Error());
        }
        message["params"].erase("_meta");
      }
      auto received = co_await Post(std::move(message), request.stop);
      if (!received.Succeeded()) co_return Result::Failure(received.Error());
      if (server.legacy && received.Value().contains("result")) received.Value()["result"]["resultType"] = "complete";
      co_return Result::Success(DecodeJson(received.Value()));
    } catch (const std::exception&) {
      co_return Failure<kit::JsonValue>(kit::ErrorCode::ProtocolError, "Invalid MCP response or endpoint configuration");
    }
  }

  kit::AgentResult<std::optional<kit::McpToolBinding>> Bind(const kit::McpToolDescription& tool) {
    using Result = kit::AgentResult<std::optional<kit::McpToolBinding>>;
    ++listed;
    const auto signature = ToolVersion(server.url + "\n" + server.token_reference + "\n" +
        (server.legacy ? "legacy\n" : "current\n") + EncodeJson(tool.metadata).dump());
    kit::Revision version = signature;
    if (const auto previous = versions.find(tool.name); previous != versions.end()) {
      const auto old = previous->at("version").get<kit::Revision>();
      version = previous->at("active") == true && previous->at("signature") == signature ? old : std::max(signature, old + 1);
    }
    next[tool.name] = {{"version", version}, {"signature", signature}, {"active", true}};
    return Result::Success(kit::McpToolBinding{.version = version, .effect = kit::ToolEffect::ExternalSubmit,
        .prepare = [server_name = server.name, name = tool.name](kit::ToolPrepareContext, kit::ToolRequest request)
            -> kit::Async<kit::PreparedChange> {
          auto arguments = EncodeJson(request.arguments).dump(2);
          co_return kit::AgentResult<kit::PreparedChange>::Success({
              .preview = {.title = server_name + " / " + name, .summary = std::move(arguments)},
              .undo_support = kit::UndoSupport::None});
        }});
  }
};

McpConnections::McpConnections(File directory) : directory_(std::move(directory)) {}

std::vector<kit::ToolCatalogId> McpConnections::Register(const std::shared_ptr<kit::AgentRuntime>& runtime,
    const std::vector<McpServer>& servers) {
  std::vector<kit::ToolCatalogId> ids;
  for (const auto& server : servers) {
    if (!server.enabled) continue;
    kit::ToolCatalogId id{"ganwork.mcp/" + server.id};
    auto connection = std::make_shared<Connection>(Connection{server, directory_.Child(server.id + ".json"),
        std::make_shared<HttpClient>()});
    auto added = kit::McpToolCatalog::Register(runtime, {.catalog = id, .client_name = "GanWork", .client_version = "0.1.0",
        .exchange = [connection](kit::McpToolRequest request) { return connection->Exchange(std::move(request)); },
        .bind = [connection](const kit::McpToolDescription& tool) { return connection->Bind(tool); }});
    if (!added.Succeeded()) throw std::runtime_error(added.Error().message);
    servers_.emplace(server.id, Entry{server, std::move(connection), std::move(added).Value(), {}});
    ids.push_back(std::move(id));
  }
  return ids;
}

Task<void> McpConnections::Start() {
  if (servers_.empty()) co_return;
  if (!co_await directory_.CreateDirectoriesAsync()) {
    for (auto& [id, entry] : servers_) entry.status.error = "Could not open the MCP catalog history";
    co_return;
  }
  auto self = shared_from_this();
  auto tasks = UseApplicationTaskScope();
  co_await AwaitCallback<void>([self, tasks](CallbackCompletion<void> done) {
    const auto remaining = std::make_shared<std::size_t>(self->servers_.size());
    std::vector<TaskHandle> running;
    for (const auto& [id, entry] : self->servers_) {
      running.push_back(tasks.Launch([self, id, done, remaining]() -> Task<void> {
        auto& entry = self->servers_.at(id);
        auto& connection = *entry.connection;
        try {
          if (connection.ledger.Exists()) {
            auto stat = co_await connection.ledger.StatAsync();
            if (!stat.Succeeded() || stat.Value().size > ledger_limit) throw std::invalid_argument("Unexpected catalog history size");
            auto raw = co_await connection.ledger.ReadStringAsync();
            if (!raw.Succeeded()) throw std::runtime_error("Missing catalog history");
            connection.versions = Json::parse(raw.Value());
            if (!connection.versions.is_object()) throw std::invalid_argument("Invalid catalog history");
          }
          static_cast<void>(co_await self->Refresh(id));
        } catch (const std::exception&) {
          entry.status.error = "Could not read the MCP catalog history";
          connection.enabled = false;
        }
        if (!--*remaining) done.Complete();
      }));
    }
    return [running = std::move(running)] { for (const auto& task : running) task.Cancel(); };
  });
}

kit::Async<void> McpConnections::Refresh(std::string id) {
  using Result = kit::AgentResult<void>;
  const auto found = servers_.find(id);
  if (found == servers_.end() || !found->second.connection->enabled)
    co_return Failure<void>(kit::ErrorCode::NotFound, "Save this server and restart GanWork to connect");
  auto& entry = found->second;
  auto& connection = *entry.connection;
  if (connection.refreshing) co_return Failure<void>(kit::ErrorCode::Busy, "MCP refresh is in progress");
  connection.refreshing = true;
  const auto reset = std::unique_ptr<bool, void(*)(bool*)>{&connection.refreshing, [](bool* busy) { *busy = false; }};
  connection.listed = 0;
  connection.next = connection.versions;
  for (auto& value : connection.next) value["active"] = false;
  Result result = Result::Success();
  try {
    result = co_await entry.catalog->RefreshAsync();
  } catch (const std::exception&) {
    result = Failure<void>(kit::ErrorCode::PersistenceFailure, "Could not restore the MCP catalog history");
  }
  if (!result.Succeeded()) {
    if (result.Error().code == kit::ErrorCode::PermissionDenied)
      for (auto& value : connection.versions) value["active"] = false;
    entry.status = {0, result.Error().message};
    co_return result;
  }
  connection.versions = std::move(connection.next);
  auto temporary = directory_.Child(kit::NewCommandId().value + ".mcp-pending");
  auto record = connection.versions.dump();
  if (record.size() > ledger_limit || !co_await temporary.WriteStringAsync(std::move(record)) ||
      !co_await temporary.MoveToAsync(connection.ledger, true)) {
    static_cast<void>(co_await temporary.DeleteAsync());
    static_cast<void>(co_await entry.catalog->WithdrawAsync());
    entry.status = {0, "Could not save the MCP catalog history"};
    for (auto& value : connection.versions) value["active"] = false;
    co_return Failure<void>(kit::ErrorCode::PersistenceFailure, entry.status.error);
  }
  entry.status = {connection.listed, {}};
  co_return Result::Success();
}

kit::Async<void> McpConnections::Apply(const std::vector<McpServer>& servers) {
  std::vector<std::shared_ptr<kit::McpToolCatalog>> withdrawn_catalogs;
  for (auto& [id, entry] : servers_) {
    if (!entry.connection->enabled || std::ranges::find(servers, entry.server) != servers.end()) continue;
    entry.connection->enabled = false;
    withdrawn_catalogs.push_back(entry.catalog);
  }
  std::optional<kit::AgentError> error;
  for (const auto& catalog : withdrawn_catalogs) {
    auto withdrawn = co_await catalog->WithdrawAsync();
    if (!withdrawn.Succeeded() && !error) error = withdrawn.Error();
  }
  if (error) co_return kit::AgentResult<void>::Failure(std::move(*error));
  co_return kit::AgentResult<void>::Success();
}

std::optional<McpConnections::Status> McpConnections::ConnectionStatus(const McpServer& server) const {
  const auto found = servers_.find(server.id);
  if (found == servers_.end() || found->second.server != server) return {};
  if (!found->second.connection->enabled && found->second.status.error.empty()) return {};
  return found->second.status;
}

} // namespace agentkit_preview
