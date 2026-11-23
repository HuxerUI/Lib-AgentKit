#pragma once

#include <huxerui/agentkit/model.h>
#include <span>

namespace huxerui::agentkit::detail {

std::uint64_t EstimateTokens(std::string_view text);

std::uint64_t EstimateTokens(const ModelItem& item);

std::uint64_t EstimateTokens(const ModelTurnRequest& request);

std::uint64_t ModelInputLimit(const ModelContextLimits& limits, std::uint64_t reserved_output);

std::uint64_t ConversationInputLimit(const ModelContextLimits& limits, const ModelTurnRequest& request,
    std::span<const ModelItem> history, std::string_view task);

std::optional<JsonValue> ShortenToolResult(const ModelToolResult& result);

} // namespace huxerui::agentkit::detail
