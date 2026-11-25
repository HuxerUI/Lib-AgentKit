#include "gemini_provider_internal.h"

#include <nlohmann/json.hpp>

namespace huxerui::agentkit::detail {

using Json = nlohmann::json;

namespace {

bool SupportsMedia(bool image, bool file, GeminiMediaFormat format, std::string_view mime) {
  return (image && (mime == "image/png" || mime == "image/jpeg" || mime == "image/webp" ||
      mime == "image/heic" || mime == "image/heif")) ||
      (file && (mime == "application/pdf" || (format == GeminiMediaFormat::GenerateContent &&
          (mime == "text/plain" || mime == "text/csv" || mime == "application/json"))));
}

} // namespace

Async<GeminiMediaParts> ReadGeminiMedia(const ModelTurnRequest& request, std::size_t byte_limit,
                                      bool image, bool file, GeminiMediaFormat format) {
  auto media = co_await ReadInlineModelMedia(request, byte_limit, [image, file, format](std::string_view mime) {
    return SupportsMedia(image, file, format, mime);
  });
  if (!media.Succeeded()) co_return AgentResult<GeminiMediaParts>::Failure(media.Error());
  GeminiMediaParts parts;
  for (auto& [key, attachment] : media.Value()) {
    if (format == GeminiMediaFormat::Interactions) {
      const auto type = attachment.mime.starts_with("image/") ? "image" : "document";
      parts.emplace(key, Json{{"type", type}, {"mime_type", attachment.mime}, {"data", std::move(attachment.base64)}});
    } else {
      parts.emplace(key, Json{{"inlineData", {{"mimeType", attachment.mime}, {"data", std::move(attachment.base64)}}}});
    }
  }
  co_return AgentResult<GeminiMediaParts>::Success(std::move(parts));
}

} // namespace huxerui::agentkit::detail
