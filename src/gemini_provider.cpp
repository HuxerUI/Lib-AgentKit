#include "gemini_provider_internal.h"

#include "runtime_internal.h"

#include <nlohmann/json.hpp>
#include <set>

namespace huxerui::agentkit::detail {

using Json = nlohmann::json;

namespace {

std::string Base64(std::span<const std::byte> bytes) {
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const auto remaining = bytes.size() - i;
    const auto a = std::to_integer<unsigned>(bytes[i]);
    const auto b = remaining > 1 ? std::to_integer<unsigned>(bytes[i + 1]) : 0U;
    const auto c = remaining > 2 ? std::to_integer<unsigned>(bytes[i + 2]) : 0U;
    encoded += alphabet[a >> 2];
    encoded += alphabet[((a & 3U) << 4) | (b >> 4)];
    encoded += remaining > 1 ? alphabet[((b & 15U) << 2) | (c >> 6)] : '=';
    encoded += remaining > 2 ? alphabet[c & 63U] : '=';
  }
  return encoded;
}

bool SupportsMedia(bool image, bool file, GeminiMediaFormat format, std::string_view mime) {
  return (image && (mime == "image/png" || mime == "image/jpeg" || mime == "image/webp" ||
      mime == "image/heic" || mime == "image/heif")) ||
      (file && (mime == "application/pdf" || (format == GeminiMediaFormat::GenerateContent &&
          (mime == "text/plain" || mime == "text/csv" || mime == "application/json"))));
}

} // namespace

GeminiArtifactKey GeminiMediaKey(const ArtifactRef& ref) { return {ref.id, ref.version}; }

Async<GeminiMediaParts> ReadGeminiMedia(const ModelTurnRequest& request, std::size_t byte_limit,
                                      bool image, bool file, GeminiMediaFormat format) {
  std::map<GeminiArtifactKey, std::size_t> selected;
  for (const auto& item : request.items) {
    const auto* content = std::get_if<ModelContent>(&item);
    const auto* artifact = content ? std::get_if<ArtifactRef>(&content->body) : nullptr;
    if (!artifact) continue;
    if (content->role != ModelContent::Role::User && content->role != ModelContent::Role::Context) {
      co_return Fail<GeminiMediaParts>(ErrorCode::Unsupported, "Gemini media must be attached to user or context input");
    }
    ++selected[GeminiMediaKey(*artifact)];
  }
  std::set<GeminiArtifactKey> declared;
  for (const auto& attachment : request.attachments) {
    const auto key = GeminiMediaKey(attachment.artifact);
    if (!selected.contains(key) || !declared.insert(key).second || !attachment.open) {
      co_return Fail<GeminiMediaParts>(ErrorCode::InvalidArgument, "Gemini attachment must match one selected artifact version");
    }
    if (!SupportsMedia(image, file, format, attachment.media_type)) {
      co_return Fail<GeminiMediaParts>(ErrorCode::Unsupported, "Gemini input media type is unavailable");
    }
  }
  if (selected.size() != declared.size()) co_return Fail<GeminiMediaParts>(ErrorCode::InvalidArgument, "Gemini attachment data is missing");
  GeminiMediaParts parts;
  auto remaining = byte_limit;
  for (const auto& attachment : request.attachments) {
    const auto key = GeminiMediaKey(attachment.artifact);
    const auto copies = selected.at(key);
    if (request.stop.stop_requested()) co_return Fail<GeminiMediaParts>(ErrorCode::AlreadyClosed, "Model turn cancelled");
    auto opened = co_await attachment.open();
    if (!opened.Succeeded()) co_return AgentResult<GeminiMediaParts>::Failure(opened.Error());
    auto stream = std::move(opened).Value();
    Bytes bytes;
    for (;;) {
      if (request.stop.stop_requested()) co_return Fail<GeminiMediaParts>(ErrorCode::AlreadyClosed, "Model turn cancelled");
      auto read = co_await stream.ReadAsync(std::min<std::size_t>(32 * 1024, remaining / copies + 1));
      if (!read.Succeeded()) co_return Fail<GeminiMediaParts>(ErrorCode::ProviderFailure, "Gemini attachment could not be read");
      if (read.Value().empty()) break;
      if (read.Value().size() > remaining / copies) {
        co_return Fail<GeminiMediaParts>(ErrorCode::BudgetExceeded, "Gemini inline attachments exceed their byte limit");
      }
      remaining -= read.Value().size() * copies;
      bytes.insert(bytes.end(), read.Value().begin(), read.Value().end());
    }
    if (bytes.empty()) co_return Fail<GeminiMediaParts>(ErrorCode::InvalidArgument, "Gemini attachment is empty");
    auto encoded = Base64(bytes);
    if (format == GeminiMediaFormat::Interactions) {
      const auto type = attachment.media_type.starts_with("image/") ? "image" : "document";
      parts.emplace(key, Json{{"type", type}, {"mime_type", attachment.media_type}, {"data", std::move(encoded)}});
    } else {
      parts.emplace(key, Json{{"inlineData", {{"mimeType", attachment.media_type}, {"data", std::move(encoded)}}}});
    }
  }
  co_return AgentResult<GeminiMediaParts>::Success(std::move(parts));
}

} // namespace huxerui::agentkit::detail
