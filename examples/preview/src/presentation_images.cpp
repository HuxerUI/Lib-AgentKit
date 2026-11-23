#include "presentation_images.h"

#include <huxerui/http.h>
#include <nlohmann/json.hpp>

namespace agentkit_preview {
namespace {
namespace kit = huxerui::agentkit;

huxerui::Bytes DecodeImage(std::string_view encoded) {
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (encoded.empty() || encoded.size() % 4 != 0) throw std::invalid_argument("Invalid image encoding");
  huxerui::Bytes bytes;
  bytes.reserve(encoded.size() / 4 * 3);
  unsigned buffer = 0;
  int bits = 0;
  const auto padding = encoded.ends_with("==") ? 2U : encoded.ends_with("=") ? 1U : 0U;
  encoded.remove_suffix(padding);
  for (const auto character : encoded) {
    const auto value = alphabet.find(character);
    if (value == alphabet.npos) throw std::invalid_argument("Invalid image encoding");
    buffer = (buffer << 6) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<std::byte>((buffer >> bits) & 255));
    }
  }
  if (bits && (buffer & ((1U << bits) - 1U))) throw std::invalid_argument("Invalid image encoding");
  return bytes;
}

} // namespace

kit::Async<huxerui::ImageAsset> GeneratePresentationImage(const PresentationImageModel& model,
    std::string prompt, bool portrait, std::stop_token stop) {
  using Result = kit::AgentResult<huxerui::ImageAsset>;
  const auto failed = [](kit::ErrorCode code, std::string message) {
    return Result::Failure({.code = code, .message = std::move(message)});
  };
  if (stop.stop_requested())
    co_return failed(kit::ErrorCode::AlreadyClosed, "Presentation illustration cancelled");
  huxerui::HttpClient http;
  auto url = model.base_url;
  while (url.ends_with('/')) url.pop_back();
  nlohmann::json parameters{{"model", model.model}, {"prompt", std::move(prompt)}, {"n", 1}};
  if (model.model.starts_with("gpt-image-")) {
    parameters["size"] = portrait ? "1024x1536" : "1536x1024";
    parameters["quality"] = "medium";
    parameters["output_format"] = "jpeg";
  } else {
    parameters["response_format"] = "b64_json";
  }
  const auto json = parameters.dump();
  const auto body = std::as_bytes(std::span(json));
  huxerui::HttpRequest request{.url = url + "/images/generations", .method = huxerui::HttpMethod::Post,
      .headers = {{"Content-Type", "application/json"}}, .body = {body.begin(), body.end()},
      .timeout = std::chrono::minutes(3)};
  if (!model.api_key.empty()) request.headers.push_back({"Authorization", "Bearer " + model.api_key});
  auto response = co_await http.SendStreamAsync(std::move(request));
  if (stop.stop_requested()) co_return failed(kit::ErrorCode::AlreadyClosed, "Presentation illustration cancelled");
  if (!response.Succeeded()) co_return failed(kit::ErrorCode::ProviderFailure, "The image endpoint did not respond");
  if (response.Value().StatusCode() != 200)
    co_return failed(kit::ErrorCode::ProviderFailure, "The image endpoint returned HTTP " +
        std::to_string(response.Value().StatusCode()) + " for " + model.model);
  std::string payload;
  constexpr std::size_t max_response = 12 * 1024 * 1024;
  while (!stop.stop_requested()) {
    auto chunk = co_await response.Value().Body().ReadAsync(64 * 1024);
    if (!chunk.Succeeded()) co_return failed(kit::ErrorCode::ProviderFailure, "The generated image could not be downloaded");
    if (chunk.Value().empty()) break;
    if (chunk.Value().size() > max_response - payload.size())
      co_return failed(kit::ErrorCode::ProviderFailure, "The generated image is too large");
    payload.append(reinterpret_cast<const char*>(chunk.Value().data()), chunk.Value().size());
  }
  if (stop.stop_requested()) co_return failed(kit::ErrorCode::AlreadyClosed, "Presentation illustration cancelled");
  try {
    const auto reply = nlohmann::json::parse(payload);
    const auto& encoded = reply.at("data").at(0).at("b64_json").get_ref<const std::string&>();
    auto image = huxerui::ImageAsset::FromEncoded(DecodeImage(encoded));
    if (!image.PixelWidth() || !image.PixelHeight() || image.PixelWidth() > 4096 || image.PixelHeight() > 4096 ||
        (image.MimeType() != "image/png" && image.MimeType() != "image/jpeg"))
      throw std::invalid_argument("Invalid image dimensions or format");
    co_return Result::Success(std::move(image));
  } catch (const std::exception&) {
    co_return failed(kit::ErrorCode::ProviderFailure, "The image model did not return a supported PNG or JPEG");
  }
}

} // namespace agentkit_preview
