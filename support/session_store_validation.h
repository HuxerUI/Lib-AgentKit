#pragma once

#include <huxerui/agentkit/store.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <ranges>
#include <tuple>

namespace agentkit_host::storage {
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

} // namespace agentkit_host::storage
