#pragma once

#include "preview_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>

namespace agentkit_preview::storage {
namespace kit = huxerui::agentkit;

inline constexpr auto max_revision = static_cast<kit::Revision>(std::numeric_limits<std::int64_t>::max());

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message, std::optional<kit::Revision> revision = {}) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message), .current_revision = revision});
}

inline kit::AgentResult<void> NormalizeCommit(kit::StoreTransaction& request) {
  if (request.commit.value.empty() || request.expected_store_revision > max_revision)
    return Fail<void>(kit::ErrorCode::InvalidArgument, "Invalid store commit identity or revision");
  auto key = [](const kit::StoreWrite& write) { return std::tie(write.collection, write.key); };
  std::ranges::sort(request.writes, std::less{}, key);
  const auto invalid = std::ranges::any_of(request.writes, [](const kit::StoreWrite& write) {
    return write.collection.empty() || write.key.empty() ||
           (write.expected && (!*write.expected || *write.expected > max_revision)) ||
           (!write.expected && !write.data);
  });
  if (invalid || std::ranges::adjacent_find(request.writes, std::equal_to{}, key) != request.writes.end())
    return Fail<void>(kit::ErrorCode::InvalidArgument, "Invalid or repeated store record");
  return kit::AgentResult<void>::Success();
}

inline bool ValidPreference(const kit::PrincipalId& principal, const kit::SessionId& session,
                            const kit::UserRunOptions& options) {
  const auto valid = [](const auto& ref) { return !ref || (!ref->id.value.empty() && ref->version && ref->version <= max_revision); };
  return !principal.value.empty() && !session.value.empty() && valid(options.model) && valid(options.execution_profile);
}

inline kit::AgentResult<NavigationPreferences> DecodeNavigation(const nlohmann::json& document) {
  NavigationPreferences navigation;
  const auto invalid = [] { return Fail<NavigationPreferences>(kit::ErrorCode::PersistenceFailure, "Invalid conversation navigation"); };
  try {
    const auto& groups = document.at("groups");
    const auto& sessions = document.at("sessions");
    if (!groups.is_array() || !sessions.is_object()) return invalid();
    for (const auto& value : groups) {
      ConversationGroup group{value.at("id").get<std::string>(), value.at("name").get<std::string>(), value.at("collapsed").get<bool>()};
      if (const auto workspace = value.find("workspace"); workspace != value.end()) {
        group.workspace = kit::WorkspaceId{workspace->get<std::string>()};
        if (group.workspace->value.empty()) return invalid();
      }
      if (group.id.empty() || group.name.empty() || std::ranges::find(navigation.groups, group.id, &ConversationGroup::id) != navigation.groups.end())
        return invalid();
      navigation.groups.push_back(std::move(group));
    }
    for (const auto& [id, value] : sessions.items()) {
      const auto group = value.get<std::string>();
      if (id.empty() || std::ranges::find(navigation.groups, group, &ConversationGroup::id) == navigation.groups.end()) return invalid();
      navigation.memberships.try_emplace(kit::SessionId{id}, group);
    }
  } catch (const nlohmann::json::exception&) {
    return invalid();
  }
  return kit::AgentResult<NavigationPreferences>::Success(std::move(navigation));
}

inline nlohmann::json EncodeNavigation(const NavigationPreferences& navigation) {
  auto groups = nlohmann::json::array();
  for (const auto& group : navigation.groups) {
    nlohmann::json value{{"id", group.id}, {"name", group.name}, {"collapsed", group.collapsed}};
    if (group.workspace) value["workspace"] = group.workspace->value;
    groups.push_back(std::move(value));
  }
  auto sessions = nlohmann::json::object();
  for (const auto& [id, group] : navigation.memberships) sessions[id.value] = group;
  return {{"groups", std::move(groups)}, {"sessions", std::move(sessions)}};
}
} // namespace agentkit_preview::storage
