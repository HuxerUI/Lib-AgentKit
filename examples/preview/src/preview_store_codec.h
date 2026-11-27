#pragma once

#include "preview_store.h"
#include "session_store_validation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ranges>

namespace agentkit_preview::storage {
namespace kit = huxerui::agentkit;

using agentkit_host::storage::Fail;
using agentkit_host::storage::max_revision;

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
    navigation.last_assistant.value = document.value("last_assistant", std::string{});
    navigation.auto_review_new_conversations = document.value("auto_review_new_conversations", false);
    if (const auto links = document.find("collaboration_links"); links != document.end()) {
      if (!links->is_array()) return invalid();
      for (const auto& link : *links) {
        if (!link.is_array() || link.size() != 2) return invalid();
        kit::WorkspaceId a{link.at(0).get<std::string>()};
        kit::WorkspaceId b{link.at(1).get<std::string>()};
        if (a.value.empty() || b.value.empty() || a == b) return invalid();
        navigation.collaboration_links.insert(WorkspaceLink(std::move(a), std::move(b)));
      }
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
  auto links = nlohmann::json::array();
  for (const auto& [a, b] : navigation.collaboration_links) links.push_back({a.value, b.value});
  return {{"groups", std::move(groups)}, {"sessions", std::move(sessions)},
      {"last_assistant", navigation.last_assistant.value}, {"auto_review_new_conversations", navigation.auto_review_new_conversations},
      {"collaboration_links", std::move(links)}};
}
} // namespace agentkit_preview::storage
