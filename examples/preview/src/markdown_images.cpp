#include "markdown_images.h"

#include <huxerui/http.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>

namespace agentkit_preview {
namespace {

using namespace huxerui;
constexpr std::size_t max_bytes = 8 * 1024 * 1024;

bool IsImageUrl(std::string_view destination) {
  const auto uri = Uri::Parse(destination);
  if (!uri) return false;
  const auto authority = uri->Authority();
  std::string scheme(uri->Scheme());
  for (auto& character : scheme) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
  return authority && !authority->empty() && !authority->starts_with(':') && authority->find('@') == std::string_view::npos &&
      (scheme == "https" || scheme == "http");
}

ImageAsset ValidateImage(Bytes bytes) {
  try {
    auto asset = ImageAsset::FromEncoded(std::move(bytes));
    if (asset.PixelWidth() > STBI_MAX_DIMENSIONS || asset.PixelHeight() > STBI_MAX_DIMENSIONS) return {};
    const auto encoded = asset.EncodedBytes();
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),
        static_cast<int>(encoded.size()), &width, &height, &channels, 0);
    if (!pixels) return {};
    stbi_image_free(pixels);
    return asset;
  } catch (const std::invalid_argument&) {
    return {};
  }
}

Task<ImageAsset> LoadImage(std::shared_ptr<HttpClient> client, std::string destination) {
  if (!IsImageUrl(destination)) co_return ImageAsset{};
  auto opened = co_await client->SendStreamAsync({.url = std::move(destination), .timeout = std::chrono::seconds(15)});
  if (!opened.Succeeded()) co_return ImageAsset{};
  auto response = std::move(opened).Value();
  if (response.StatusCode() != 200 || !IsImageUrl(response.Url())) co_return ImageAsset{};
  Bytes bytes;
  for (;;) {
    auto read = co_await response.Body().ReadAsync(std::min<std::size_t>(32 * 1024, max_bytes - bytes.size() + 1));
    if (!read.Succeeded()) co_return ImageAsset{};
    auto chunk = std::move(read).Value();
    if (chunk.empty()) break;
    if (chunk.size() > max_bytes - bytes.size()) co_return ImageAsset{};
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  }
#if defined(__EMSCRIPTEN__)
  co_return ValidateImage(std::move(bytes));
#else
  co_return co_await RunWorker(ValidateImage, std::move(bytes));
#endif
}

} // namespace

std::shared_ptr<huxerui::agentkit::AgentChatImageLoader> MakePreviewImageLoader() {
  return std::make_shared<huxerui::agentkit::AgentChatImageLoader>(
      [client = std::make_shared<huxerui::HttpClient>()](huxerui::agentkit::AgentChatImage image) {
        return LoadImage(client, std::move(image.destination));
      });
}

} // namespace agentkit_preview
