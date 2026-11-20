#include <huxerui/agentkit/mcp.h>

#include "runtime_internal.h"

namespace huxerui::agentkit {
namespace {

using detail::Error;
using detail::Fail;
using Object = JsonValue::Object;
using Array = JsonValue::Array;

const JsonValue* Get(const JsonValue& value, std::string_view key) {
  const auto* object = std::get_if<Object>(&value.value);
  if (!object) return nullptr;
  const auto member = object->find(key);
  return member == object->end() ? nullptr : &member->second;
}

template<class T>
const T* Get(const JsonValue& value, std::string_view key) {
  const auto* member = Get(value, key);
  return member ? std::get_if<T>(&member->value) : nullptr;
}

bool Is(const JsonValue& value, std::string_view key, std::string_view expected) {
  const auto* member = Get<std::string>(value, key);
  return member && *member == expected;
}

struct ToolRpc {
  Object metadata;
  McpToolExchange exchange;
  Revision authority = 1;
  bool catalog_verified = false;

  Async<JsonValue> Request(std::string method, Object params, std::stop_token stop,
                           std::optional<McpToolDescription> tool = {}) const {
    if (stop.stop_requested()) co_return Fail<JsonValue>(ErrorCode::AlreadyClosed, "MCP request cancelled");
    const auto id = NewCommandId().value;
    params.emplace("_meta", JsonValue{metadata});
    McpToolRequest request{
        .message = JsonValue{Object{{"jsonrpc", JsonValue{std::string{"2.0"}}}, {"id", JsonValue{id}},
            {"method", JsonValue{std::move(method)}}, {"params", JsonValue{std::move(params)}}}},
        .tool = std::move(tool), .stop = stop};
    auto response = co_await exchange(std::move(request));
    if (!response.Succeeded()) co_return response;
    auto value = std::move(response).Value();
    const auto* result = Get(value, "result");
    const auto* error = Get(value, "error");
    if (!detail::ValidateJson(value, {JsonValue{true}}).Succeeded() ||
        !Is(value, "jsonrpc", "2.0") || !Is(value, "id", id) || (!!result == !!error)) {
      co_return Fail<JsonValue>(ErrorCode::ProtocolError, "Invalid MCP JSON-RPC response");
    }
    if (error) {
      const auto* code = Get<std::int64_t>(*error, "code");
      if (!code || !Get<std::string>(*error, "message"))
        co_return Fail<JsonValue>(ErrorCode::ProtocolError, "Invalid MCP JSON-RPC error");
      auto failure = Error(ErrorCode::ProviderFailure, "MCP server returned an RPC error");
      failure.provider_code = std::to_string(*code);
      co_return AgentResult<JsonValue>::Failure(std::move(failure));
    }
    co_return AgentResult<JsonValue>::Success(*result);
  }
};

AgentResult<McpToolDescription> ReadTool(const JsonValue& value) {
  const auto* name = Get<std::string>(value, "name");
  const auto* description = Get<std::string>(value, "description");
  const auto* input = Get(value, "inputSchema");
  const auto* output = Get(value, "outputSchema");
  if (!name || name->empty() || name->size() > 128 ||
      !std::ranges::all_of(*name, [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
      }) || (Get(value, "description") && !description) || !Get<Object>(value, "inputSchema") ||
      (output && !Get<Object>(value, "outputSchema"))) {
    return Fail<McpToolDescription>(ErrorCode::ProtocolError, "Invalid MCP tool description");
  }
  return AgentResult<McpToolDescription>::Success({*name, description ? *description : *name,
      JsonSchema{*input}, output ? std::optional{JsonSchema{*output}} : std::nullopt, value});
}

Async<ToolExecutionResult> CallMcpTool(std::shared_ptr<ToolRpc> rpc, const McpToolDescription& description,
                                      Revision authority, ToolContext context, PreparedToolCall call) {
  using Result = AgentResult<ToolExecutionResult>;
  const auto failure = [](ErrorCode code, std::string reason) {
    return Result::Success({.outcome = ToolOutcome::Failed, .reason = Error(code, std::move(reason))});
  };
  if (authority != rpc->authority) co_return failure(ErrorCode::PermissionDenied, "MCP authorization was withdrawn");
  if (!rpc->catalog_verified) co_return failure(ErrorCode::Conflict, "Refresh the MCP tool catalog before executing");
  if (context.StopToken().stop_requested()) {
    co_return Result::Success({.outcome = ToolOutcome::Cancelled,
        .reason = Error(ErrorCode::AlreadyClosed, "MCP tool call cancelled before dispatch")});
  }
  Object params{{"name", JsonValue{description.name}},
                {"arguments", std::move(call.request.arguments)}};
  auto response = co_await rpc->Request("tools/call", std::move(params), context.StopToken(), description);
  if (!response.Succeeded()) co_return Result::Failure(response.Error());
  if (authority != rpc->authority) {
    co_return Result::Failure(Error(ErrorCode::PermissionDenied, "MCP authorization changed during the call"));
  }
  const auto& value = response.Value();
  if (Is(value, "resultType", "input_required"))
    co_return Fail<ToolExecutionResult>(ErrorCode::Unsupported, "MCP tool requested an unsupported client capability");
  const auto* error = Get<bool>(value, "isError");
  const auto* content = Get<Array>(value, "content");
  const auto* structured = Get(value, "structuredContent");
  if (!Is(value, "resultType", "complete") || !content || (Get(value, "isError") && !error))
    co_return Fail<ToolExecutionResult>(ErrorCode::ProtocolError, "Invalid MCP tool result");
  if (error && *error) co_return Fail<ToolExecutionResult>(ErrorCode::ProviderFailure, "MCP tool reported a failure");
  if (description.output_schema && (!structured || !detail::ValidateJson(*structured, *description.output_schema).Succeeded()))
    co_return Fail<ToolExecutionResult>(ErrorCode::ProtocolError, "MCP tool result did not match its output schema");
  co_return Result::Success({.outcome = ToolOutcome::Succeeded,
      .model_value = structured ? *structured : JsonValue{*content}});
}

ToolDefinition MakeTool(const ToolCatalogId& catalog, std::shared_ptr<ToolRpc> rpc, McpToolDescription tool,
                        McpToolBinding binding) {
  if (!binding.prepare && binding.effect == ToolEffect::Read) {
    binding.prepare = [title = tool.description](ToolPrepareContext, ToolRequest) -> Async<PreparedChange> {
      co_return AgentResult<PreparedChange>::Success({.preview = {.title = title}});
    };
  }
  const auto authority = rpc->authority;
  return {.id = {catalog.value + "/" + tool.name}, .version = binding.version, .description = tool.description,
      .input_schema = tool.input_schema, .output_schema = tool.output_schema, .effect = binding.effect,
      .concurrency = binding.concurrency, .prepare = std::move(binding.prepare),
      .execute = [rpc = std::move(rpc), tool = std::move(tool), authority](ToolContext context, PreparedToolCall call) {
        return CallMcpTool(rpc, tool, authority, std::move(context), std::move(call));
      }};
}

} // namespace

struct McpToolCatalog::State {
  std::shared_ptr<AgentRuntime> runtime;
  ToolCatalogId catalog;
  std::shared_ptr<ToolRpc> rpc;
  McpToolBinder bind;
  Revision revision = 0;
  bool refreshing = false;
  struct Entry {
    Revision version;
    JsonValue metadata;
  };
  std::map<std::string, Entry, std::less<>> active;

  Async<void> Refresh(std::stop_token stop) {
    if (refreshing) co_return Fail<void>(ErrorCode::Busy, "An MCP catalog refresh is already in progress");
    refreshing = true;
    rpc->catalog_verified = false;
    const auto reset = std::unique_ptr<bool, void(*)(bool*)>(&refreshing, [](bool* flag) { *flag = false; });
    const auto authority = rpc->authority;
    std::set<std::string> names, cursors;
    decltype(active) next;
    ToolCatalogSnapshot snapshot{};
    Object params;
    for (unsigned pages = 0; ; ++pages) {
      if (pages == 64) co_return Fail<void>(ErrorCode::ProtocolError, "MCP tool list exceeded the pagination limit");
      auto response = co_await rpc->Request("tools/list", std::move(params), stop);
      if (authority != rpc->authority) co_return Fail<void>(ErrorCode::PermissionDenied, "MCP authorization changed");
      if (stop.stop_requested()) co_return Fail<void>(ErrorCode::AlreadyClosed, "MCP catalog refresh cancelled");
      if (!response.Succeeded()) {
        if (response.Error().code == ErrorCode::PermissionDenied) {
          auto withdrawn = co_await Withdraw();
          if (!withdrawn.Succeeded()) co_return withdrawn;
        }
        co_return AgentResult<void>::Failure(response.Error());
      }
      const auto& result = response.Value();
      const auto* listed = Get<Array>(result, "tools");
      if (!Is(result, "resultType", "complete") || !listed)
        co_return Fail<void>(ErrorCode::ProtocolError, "Invalid MCP tools/list response");
      for (const auto& raw : *listed) {
        auto entry = ReadTool(raw);
        if (!entry.Succeeded()) co_return AgentResult<void>::Failure(entry.Error());
        auto tool = std::move(entry).Value();
        if (!names.insert(tool.name).second) co_return Fail<void>(ErrorCode::ProtocolError, "Duplicate MCP tool name");
        if (names.size() > 4096) co_return Fail<void>(ErrorCode::ProtocolError, "MCP catalog exceeded the tool limit");
        auto bound = bind(tool);
        if (!bound.Succeeded()) co_return AgentResult<void>::Failure(bound.Error());
        if (!bound.Value()) continue;
        auto binding = std::move(*bound.Value());
        const auto previous = active.find(tool.name);
        if (previous != active.end() && previous->second.version == binding.version &&
            previous->second.metadata != tool.metadata)
          co_return Fail<void>(ErrorCode::Conflict, "Changed MCP tool metadata requires a new version");
        next.emplace(tool.name, Entry{binding.version, tool.metadata});
        snapshot.tools.push_back(MakeTool(catalog, rpc, std::move(tool), std::move(binding)));
      }
      if (!Get(result, "nextCursor")) break;
      const auto* cursor = Get<std::string>(result, "nextCursor");
      if (!cursor || cursor->empty() || !cursors.insert(*cursor).second)
        co_return Fail<void>(ErrorCode::ProtocolError, "Invalid MCP tool list cursor");
      params = {{"cursor", JsonValue{*cursor}}};
    }
    snapshot.revision = ++revision;
    auto published = co_await runtime->ReplaceToolCatalogAsync(catalog, std::move(snapshot));
    if (published.Succeeded() && authority == rpc->authority) {
      active = std::move(next);
      rpc->catalog_verified = true;
    }
    co_return published;
  }

  Async<void> Withdraw() {
    ++rpc->authority;
    rpc->catalog_verified = false;
    active.clear();
    co_return co_await runtime->ReplaceToolCatalogAsync(catalog, ToolCatalogSnapshot{++revision, {}});
  }
};

McpToolCatalog::McpToolCatalog(std::shared_ptr<State> state) : state_(std::move(state)) {}

AgentResult<std::shared_ptr<McpToolCatalog>> McpToolCatalog::Register(
    std::shared_ptr<AgentRuntime> runtime, McpToolCatalogOptions options) {
  using Catalog = std::shared_ptr<McpToolCatalog>;
  if (!runtime || !options.exchange || !options.bind || options.client_name.empty() || options.client_version.empty() ||
      !detail::ValidUtf8(options.client_name) || !detail::ValidUtf8(options.client_version)) {
    return Fail<Catalog>(ErrorCode::InvalidArgument, "Invalid MCP catalog settings");
  }
  auto registered = runtime->RegisterToolCatalog(options.catalog);
  if (!registered.Succeeded()) return AgentResult<Catalog>::Failure(registered.Error());
  auto rpc = std::make_shared<ToolRpc>(ToolRpc{
      .metadata = {{"io.modelcontextprotocol/protocolVersion", JsonValue{std::string{McpToolProtocolVersion}}},
                   {"io.modelcontextprotocol/clientInfo", JsonValue{Object{
                       {"name", JsonValue{std::move(options.client_name)}},
                       {"version", JsonValue{std::move(options.client_version)}}}}},
                   {"io.modelcontextprotocol/clientCapabilities", JsonValue{Object{}}}},
      .exchange = std::move(options.exchange)});
  return AgentResult<Catalog>::Success(Catalog(new McpToolCatalog(std::make_shared<State>(
      State{std::move(runtime), std::move(options.catalog), std::move(rpc), std::move(options.bind)}))));
}

Async<void> McpToolCatalog::RefreshAsync(std::stop_token stop) {
  return [](std::shared_ptr<State> state, std::stop_token token) -> Async<void> {
    co_return co_await state->Refresh(token);
  }(state_, stop);
}

Async<bool> McpToolCatalog::HandleNotificationAsync(JsonValue notification, std::stop_token stop) {
  return [](std::shared_ptr<State> state, JsonValue message, std::stop_token token) -> Async<bool> {
    if (!Is(message, "jsonrpc", "2.0") || !Get<std::string>(message, "method") || Get(message, "id"))
      co_return Fail<bool>(ErrorCode::ProtocolError, "Invalid MCP notification");
    const bool changed = Is(message, "method", "notifications/tools/list_changed");
    if (changed) {
      auto refreshed = co_await state->Refresh(token);
      if (!refreshed.Succeeded()) co_return AgentResult<bool>::Failure(refreshed.Error());
    }
    co_return AgentResult<bool>::Success(changed);
  }(state_, std::move(notification), stop);
}

Async<void> McpToolCatalog::WithdrawAsync() {
  return [](std::shared_ptr<State> state) -> Async<void> {
    co_return co_await state->Withdraw();
  }(state_);
}

} // namespace huxerui::agentkit
