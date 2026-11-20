#include "runtime_internal.h"

#include "json_wire.h"

#include <nlohmann/json.hpp>

namespace huxerui::agentkit {
namespace {

bool Namespaced(const std::string& id) {
  return !id.empty() && id.size() <= 128 && id.front() != '.' && id.back() != '.' &&
      id.find('.') != std::string::npos && id.find("..") == std::string::npos &&
      std::ranges::all_of(id, [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
      });
}

bool ValidSchema(const JsonSchema& schema) {
  return std::holds_alternative<JsonValue::Object>(schema.document.value) ||
      std::holds_alternative<bool>(schema.document.value);
}

bool ActionName(std::string_view id) {
  return !id.empty() && id.size() <= 128 && detail::ValidUtf8(id);
}

} // namespace

AgentResult<void> AgentRuntime::RegisterContentComponent(ContentComponentRegistration component) {
  if (state_->stage != detail::RuntimeState::Stage::Registering) {
    return detail::Fail<void>(ErrorCode::InvalidState, "Registration closed");
  }
  const auto& type = component.type;
  if (!Namespaced(type.id) || !type.schema_version || !ValidSchema(component.data_schema)) {
    return detail::Fail<void>(ErrorCode::InvalidArgument, "Invalid content component type or schema");
  }
  std::set<std::string_view> ids;
  for (const auto& action : component.actions) {
    if (!ActionName(action.id) || !action.prepare || !ValidSchema(action.payload_schema)) {
      return detail::Fail<void>(ErrorCode::InvalidArgument, "Invalid component action registration");
    }
    if (!ids.insert(action.id).second) return detail::Fail<void>(ErrorCode::Conflict, "Duplicate component action");
  }
  auto key = std::pair(type.id, type.schema_version);
  if (!state_->content_components.emplace(std::move(key), std::move(component)).second) {
    return detail::Fail<void>(ErrorCode::Conflict, "Content component version is already registered");
  }
  return AgentResult<void>::Success();
}

AgentResult<void> AgentRuntime::RegisterArtifactRenderer(std::string type, ArtifactRenderer renderer) {
  if (state_->stage != detail::RuntimeState::Stage::Registering) {
    return detail::Fail<void>(ErrorCode::InvalidState, "Registration closed");
  }
  if (!ActionName(type) || !renderer) return detail::Fail<void>(ErrorCode::InvalidArgument, "Invalid artifact renderer");
  if (!state_->artifact_renderers.emplace(std::move(type), std::move(renderer)).second) {
    return detail::Fail<void>(ErrorCode::Conflict, "Artifact renderer already registered");
  }
  return AgentResult<void>::Success();
}

AgentResult<void> AgentRuntime::RegisterArtifactAction(ArtifactActionRegistration action) {
  if (state_->stage != detail::RuntimeState::Stage::Registering) {
    return detail::Fail<void>(ErrorCode::InvalidState, "Registration closed");
  }
  if (!ActionName(action.artifact_type) || !ActionName(action.action) || !ValidSchema(action.payload_schema) || !action.prepare) {
    return detail::Fail<void>(ErrorCode::InvalidArgument, "Invalid artifact action registration");
  }
  auto key = std::pair(action.artifact_type, action.action);
  if (!state_->artifact_actions.emplace(std::move(key), std::move(action)).second) {
    return detail::Fail<void>(ErrorCode::Conflict, "Artifact action already registered");
  }
  return AgentResult<void>::Success();
}

std::optional<huxerui::View> Conversation::RenderContent(CustomBlockRef ref, const CustomBlock& block) const {
  const auto user = state_->User();
  if (!user.Succeeded() || user.Value() != principal_ || !state_->visible_sessions.contains({principal_, id_})) return {};
  const auto component = state_->content_components.find({block.type.id, block.type.schema_version});
  if (component == state_->content_components.end() || !component->second.render) return {};
  try {
    std::vector<InteractionSnapshot> interactions;
    const auto session = state_->sessions.find(id_);
    if (session != state_->sessions.end() && ref.message.session == id_) {
      if (const auto* image = state_->Published(session->second)) {
        const auto message = std::ranges::find(image->messages, ref.message, &Message::ref);
        if (message != image->messages.end()) {
          const auto entry = std::ranges::find(message->blocks, ref.block, &ContentBlock::id);
          const auto* published = entry == message->blocks.end() ? nullptr : std::get_if<CustomBlock>(&entry->body);
          if (published && published->type == block.type) {
            for (const auto& id : block.interactions) {
              if (std::ranges::find(published->interactions, id) == published->interactions.end()) continue;
              const auto interaction = std::ranges::find(image->interactions, id, &InteractionSnapshot::id);
              if (interaction != image->interactions.end() && interaction->recipient == principal_) {
                interactions.push_back(*interaction);
              }
            }
          }
        }
      }
    }
    auto dispatch = [state = state_, session = id_, user = principal_, binding = ref](CommandId command, RenderAction request) {
      return [](std::shared_ptr<detail::RuntimeState> owner, SessionId session, PrincipalId user, CustomBlockRef binding,
                CommandId command, RenderAction request) -> Async<CommandReceipt> {
        auto* action = std::get_if<CustomBlockAction>(&request);
        if (!action || action->target != binding)
          co_return detail::Fail<CommandReceipt>(ErrorCode::PermissionDenied, "This renderer is bound to a different component action");
        co_return co_await owner->DispatchRenderedAction(session, user, std::move(command), std::move(*action));
      }(state, session, user, binding, std::move(command), std::move(request));
    };
    return component->second.render(
        {std::make_shared<const CustomBlock>(block), std::move(ref), std::move(dispatch), std::move(interactions)});
  } catch (...) {
    state_->ReportException(std::current_exception());
    return {};
  }
}

std::optional<huxerui::View> Conversation::RenderArtifact(const ArtifactPreview& preview) const {
  const auto renderer = state_->artifact_renderers.find(preview.type);
  if (renderer == state_->artifact_renderers.end() || preview.access_revision != state_->access_revision) return {};
  const auto snapshot = Snapshot();
  if (!snapshot || !detail::ShowsArtifact(*snapshot, preview.artifact)) return {};
  try {
    auto dispatch = [state = state_, session = id_, user = principal_, binding = preview.artifact](CommandId command, RenderAction request) {
      return [](std::shared_ptr<detail::RuntimeState> owner, SessionId session, PrincipalId user, ArtifactRef binding,
                CommandId command, RenderAction request) -> Async<CommandReceipt> {
        auto* action = std::get_if<ArtifactAction>(&request);
        if (!action || action->artifact != binding)
          co_return detail::Fail<CommandReceipt>(ErrorCode::PermissionDenied, "This renderer is bound to a different artifact version");
        co_return co_await owner->DispatchRenderedAction(session, user, std::move(command), std::move(*action));
      }(state, session, user, binding, std::move(command), std::move(request));
    };
    return renderer->second({std::make_shared<const ArtifactPreview>(preview), {}, std::move(dispatch), {}});
  } catch (...) {
    state_->ReportException(std::current_exception());
    return {};
  }
}

namespace detail {

Async<void> RuntimeState::ValidateToolBlocks(const SessionData& session, const RunExecution& run,
                                            std::vector<ContentBlock>& blocks) {
  std::set<BlockId> ids;
  for (auto& block : blocks) {
    if (block.id.value.empty()) block.id = {MakeId()};
    if (!ids.insert(block.id).second || block.id.value.size() > 128 || !ValidUtf8(block.id.value)) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid or duplicate content block ID");
    }
    if (const auto* text = std::get_if<TextBlock>(&block.body)) {
      if (!ValidUtf8(text->text)) co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid content block text");
      continue;
    }
    const auto* custom = std::get_if<CustomBlock>(&block.body);
    if (!custom) co_return Fail<void>(ErrorCode::Unsupported, "Unsupported tool content block");
    const auto agent = agents.find(run.view.agent.id);
    if (agent == agents.end() || agent->second.ref != run.view.agent ||
        std::ranges::find(agent->second.content_types, custom->type) == agent->second.content_types.end()) {
      co_return Fail<void>(ErrorCode::PermissionDenied, "The agent cannot publish this content type");
    }
    const auto definition = content_components.find({custom->type.id, custom->type.schema_version});
    if (definition == content_components.end()) {
      co_return Fail<void>(ErrorCode::Conflict, "Content component schema version is unavailable");
    }
    if (custom->fallback.empty() || custom->fallback.size() > 4096 || !ValidUtf8(custom->fallback) ||
        custom->interactions.size() > 64 || custom->artifacts.size() > 64) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Invalid content fallback or bindings");
    }
    auto valid = ValidateJson(custom->data, definition->second.data_schema);
    if (!valid.Succeeded()) co_return valid;
    try {
      if (ToWireJson(custom->data).dump().size() > (256 << 10)) {
        co_return Fail<void>(ErrorCode::BudgetExceeded, "Content block exceeds 256 KiB");
      }
    } catch (const nlohmann::json::exception&) {
      co_return Fail<void>(ErrorCode::InvalidArgument, "Content data must be valid UTF-8 JSON");
    }

    const auto epoch = access_revision;
    for (const auto& ref : custom->artifacts) {
      const auto* image = Published(session);
      if (!image || std::ranges::none_of(image->artifacts, [&](const auto& entry) {
            return entry.record.metadata.ref == ref && entry.record.availability == ArtifactAvailability::Available;
          })) {
        co_return Fail<void>(ErrorCode::NotFound, "Bound artifact is unavailable in this conversation");
      }
      auto permit = Request(run.principal, Capability::ReadArtifact, &session, &run);
      permit.sources.push_back({ref});
      auto allowed = co_await Require(std::move(permit));
      if (!allowed.Succeeded()) co_return allowed;
      if (epoch != access_revision) co_return Fail<void>(ErrorCode::Conflict, "Artifact access changed while publishing content");
    }
    for (const auto& ref : custom->interactions) {
      const auto found = std::ranges::find(session.interactions, ref, &InteractionSnapshot::id);
      if (found == session.interactions.end() || found->recipient != run.principal || found->run != run.view.id) {
        co_return Fail<void>(ErrorCode::NotFound, "Bound interaction is unavailable to this run");
      }
    }
  }
  co_return AgentResult<void>::Success();
}

} // namespace detail
} // namespace huxerui::agentkit
