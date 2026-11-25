#pragma once

#include "model_provider_internal.h"

namespace huxerui::agentkit::detail {

using GeminiMediaParts = std::map<ModelMediaKey, nlohmann::json>;
enum class GeminiMediaFormat { GenerateContent, Interactions };

Async<GeminiMediaParts> ReadGeminiMedia(const ModelTurnRequest& request, std::size_t byte_limit,
                                      bool image, bool file, GeminiMediaFormat format);

} // namespace huxerui::agentkit::detail
