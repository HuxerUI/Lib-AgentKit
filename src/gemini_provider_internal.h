#pragma once

#include "model_provider_internal.h"

namespace huxerui::agentkit::detail {

using GeminiArtifactKey = std::pair<ArtifactId, Revision>;
using GeminiMediaParts = std::map<GeminiArtifactKey, nlohmann::json>;
enum class GeminiMediaFormat { GenerateContent, Interactions };

GeminiArtifactKey GeminiMediaKey(const ArtifactRef& ref);

Async<GeminiMediaParts> ReadGeminiMedia(const ModelTurnRequest& request, std::size_t byte_limit,
                                      bool image, bool file, GeminiMediaFormat format);

} // namespace huxerui::agentkit::detail
