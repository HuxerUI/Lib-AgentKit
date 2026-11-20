#include "runtime_internal.h"

namespace huxerui::agentkit::detail {
namespace {

bool Under(std::string_view value, std::string_view prefix) {
  return value.size() > prefix.size() && value.starts_with(prefix) && value[prefix.size()] == '/';
}

bool ValidId(std::string_view value) {
  return !value.empty() && value.size() <= 384 && !value.starts_with('/') && !value.ends_with('/') &&
      value.find("//") == std::string_view::npos && ValidUtf8(value) &&
      std::ranges::none_of(value, [](unsigned char byte) { return byte < 33 || byte == 127; });
}

bool IsSchema(const JsonSchema& schema) {
  return std::holds_alternative<JsonValue::Object>(schema.document.value) ||
         std::holds_alternative<bool>(schema.document.value);
}

bool SameDeclaration(const ToolDefinition& first, const ToolDefinition& second) {
  return first.description == second.description && first.input_schema == second.input_schema &&
      first.output_schema == second.output_schema && first.effect == second.effect &&
      first.concurrency == second.concurrency && first.idempotent_by_call_id == second.idempotent_by_call_id &&
      bool(first.lookup_result) == bool(second.lookup_result);
}

RegisteredTool Binding(const std::shared_ptr<const ToolDefinition>& tool, std::optional<ToolCatalogId> catalog = {}) {
  return {{tool->id, tool->version, std::move(catalog)}, tool};
}

} // namespace

bool RuntimeState::ReservedToolId(const ToolId& id) const {
  return std::ranges::any_of(tool_catalogs, [&](const auto& entry) { return Under(id.value, entry.first.value); });
}

std::shared_ptr<const ToolDefinition> RuntimeState::ResolveTool(const ToolRef& ref) const {
  const auto* registry = &tools;
  if (ref.catalog) {
    const auto catalog = tool_catalogs.find(*ref.catalog);
    if (catalog == tool_catalogs.end()) return {};
    registry = &catalog->second.tools;
  }
  const auto found = registry->find(ref.id);
  return found != registry->end() && found->second->version == ref.version ? found->second : nullptr;
}

std::optional<RegisteredTool> RuntimeState::FindAgentTool(const AgentDefinition& agent, const ToolId& id) const {
  if (std::ranges::find(agent.tools, id) != agent.tools.end()) {
    if (const auto found = tools.find(id); found != tools.end()) return Binding(found->second);
  }
  for (const auto& catalog : agent.tool_catalogs) {
    if (!Under(id.value, catalog.value)) continue;
    const auto& entries = tool_catalogs.at(catalog).tools;
    if (const auto found = entries.find(id); found != entries.end()) return Binding(found->second, catalog);
  }
  return {};
}

std::vector<RegisteredTool> RuntimeState::CaptureAgentTools(const AgentDefinition& agent) const {
  std::vector<RegisteredTool> result;
  std::set<ToolId> included;
  for (const auto& id : agent.tools) {
    if (included.insert(id).second) result.push_back(Binding(tools.at(id)));
  }
  for (const auto& catalog : agent.tool_catalogs) {
    for (const auto& [id, tool] : tool_catalogs.at(catalog).tools) {
      if (included.insert(id).second) result.push_back(Binding(tool, catalog));
    }
  }
  return result;
}

Async<void> RuntimeState::ReplaceToolCatalog(ToolCatalogId id, ToolCatalogSnapshot snapshot) {
  if (stage == Stage::Closed) co_return Fail<void>(ErrorCode::RuntimeClosed, "Agent runtime closed");
  if (stage == Stage::Starting) co_return Fail<void>(ErrorCode::Busy, "Agent runtime is restoring");
  const auto registered = tool_catalogs.find(id);
  if (registered == tool_catalogs.end()) co_return Fail<void>(ErrorCode::NotFound, "Tool catalog is not registered");
  auto& catalog = registered->second;
  if (!snapshot.revision || snapshot.revision <= catalog.revision)
    co_return Fail<void>(ErrorCode::Conflict, "Tool catalog revision must increase");

  decltype(catalog.tools) replacement;
  for (auto& item : snapshot.tools) {
    if (!ValidId(item.id.value) || !Under(item.id.value, id.value) || ArtifactChangeTool(item.id) || !item.version ||
        !ValidUtf8(item.description) || !item.prepare || !item.execute || !IsSchema(item.input_schema) ||
        (item.output_schema && !IsSchema(*item.output_schema)) ||
        (item.concurrency == ToolConcurrency::ReadOnly && item.effect != ToolEffect::Read)) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid tool definition in catalog");
    }
    const auto previous = catalog.versions.find(item.id);
    std::shared_ptr<const ToolDefinition> entry;
    if (previous != catalog.versions.end() && item.version <= previous->second) {
      const auto active = catalog.tools.find(item.id);
      if (item.version != previous->second || active == catalog.tools.end() || !SameDeclaration(item, *active->second))
        co_return Fail<void>(ErrorCode::Conflict, "Changed or reintroduced catalog tools require a new version");
      entry = active->second;
    } else {
      entry = std::make_shared<const ToolDefinition>(std::move(item));
    }
    const auto key = entry->id;
    if (!replacement.emplace(key, std::move(entry)).second)
      co_return Fail<void>(ErrorCode::Conflict, "Duplicate tool in catalog snapshot");
  }
  for (const auto& [key, tool] : replacement) catalog.versions.insert_or_assign(key, tool->version);
  catalog.tools = std::move(replacement);
  catalog.revision = snapshot.revision;
  if (stage != Stage::Started) co_return AgentResult<void>::Success();

  std::set<SessionId> changed;
  for (auto& [run_id, run] : runs) {
    if (Terminal(run.view.state)) continue;
    auto& session = sessions.at(run.view.session);
    for (auto& [call, attempt] : run.tool_attempts) {
      if (attempt.issued || !attempt.plan.prepared || attempt.plan.prepared->tool.catalog != id ||
          ResolveTool(attempt.plan.prepared->tool)) continue;
      attempt.InterruptReview();
      if (!attempt.plan.approval) continue;
      auto approval = std::ranges::find(session.interactions, *attempt.plan.approval, &InteractionSnapshot::id);
      if (approval != session.interactions.end() && approval->state == InteractionState::Pending) {
        CloseInteraction(session, *approval, InteractionState::Invalidated);
        changed.insert(session.summary.id);
      }
    }
  }
  std::shared_ptr<SaveTicket> saved;
  for (const auto& session : changed) saved = Persist(sessions.at(session));
  co_return co_await WhenSaved(saved);
}

} // namespace huxerui::agentkit::detail

namespace huxerui::agentkit {

AgentResult<void> AgentRuntime::RegisterToolCatalog(ToolCatalogId catalog) {
  using detail::Fail;
  if (state_->stage != detail::RuntimeState::Stage::Registering)
    return Fail<void>(ErrorCode::InvalidState, "Registration closed");
  if (!detail::ValidId(catalog.value)) return Fail<void>(ErrorCode::InvalidArgument, "Invalid tool catalog ID");
  if (std::ranges::any_of(state_->tool_catalogs, [&](const auto& entry) {
        return entry.first == catalog || detail::Under(entry.first.value, catalog.value) ||
            detail::Under(catalog.value, entry.first.value);
      }) || std::ranges::any_of(state_->tools, [&](const auto& entry) {
        return detail::Under(entry.first.value, catalog.value);
      })) {
    return Fail<void>(ErrorCode::Conflict, "Tool catalog prefix is already in use");
  }
  state_->tool_catalogs.try_emplace(std::move(catalog));
  return AgentResult<void>::Success();
}

Async<void> AgentRuntime::ReplaceToolCatalogAsync(ToolCatalogId catalog, ToolCatalogSnapshot snapshot) {
  return [](std::shared_ptr<detail::RuntimeState> state, ToolCatalogId id, ToolCatalogSnapshot snapshot) -> Async<void> {
    co_return co_await state->ReplaceToolCatalog(std::move(id), std::move(snapshot));
  }(state_, std::move(catalog), std::move(snapshot));
}

} // namespace huxerui::agentkit
