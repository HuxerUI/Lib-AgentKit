#include "run_settings.h"

#include <huxerui/data.h>

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <set>

namespace agentkit_preview {

std::string TrimModelField(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string ModelKeyEnvironment(std::string_view key) {
  if (!key.starts_with("${")) return {};
  const auto name = key.ends_with('}') ? key.substr(2, key.size() - 3) : std::string_view{};
  if (name.empty() || name.size() > 128 || (name.front() >= '0' && name.front() <= '9') ||
      !std::ranges::all_of(name, [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
      })) {
    throw std::invalid_argument("Use ${ENV_NAME} to read the API key from an environment variable.");
  }
  return std::string(name);
}

namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using CatalogResult = kit::AgentResult<std::vector<DiscoveredModel>>;

std::string EncodeCursor(std::string_view cursor) {
  std::string result;
  constexpr std::string_view hex = "0123456789ABCDEF";
  for (const unsigned char ch : cursor) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      result += ch;
    } else {
      result += '%';
      result += hex[ch >> 4];
      result += hex[ch & 15];
    }
  }
  return result;
}

std::optional<std::uint64_t> TokenLimit(const nlohmann::json& model, std::initializer_list<const char*> keys) {
  for (const auto key : keys) {
    const auto field = model.find(key);
    if (field == model.end() || !field->is_number_integer()) continue;
    if (field->is_number_unsigned()) {
      const auto tokens = field->get<std::uint64_t>();
      if (tokens) return tokens;
    } else {
      const auto tokens = field->get<std::int64_t>();
      if (tokens > 0) return static_cast<std::uint64_t>(tokens);
    }
  }
  return {};
}

std::string StringField(const nlohmann::json& value, const char* key) {
  const auto field = value.find(key);
  return field != value.end() && field->is_string() ? field->get<std::string>() : std::string{};
}

DiscoveredModel ReadModel(const nlohmann::json& model, bool gemini) {
  auto id = StringField(model, gemini ? "name" : "id");
  if (gemini && id.starts_with("models/")) id.erase(0, 7);
  auto name = StringField(model, gemini ? "displayName" : "display_name");
  if (name.empty() && !gemini) name = StringField(model, "name");
  if (name.empty()) name = id;
  ModelLimits limits{
      TokenLimit(model, {"context_length", "context_window"}),
      TokenLimit(model, {"inputTokenLimit", "max_input_tokens"}),
      TokenLimit(model, {"outputTokenLimit", "max_output_tokens", "max_tokens", "max_completion_tokens"})};
  if (const auto route = model.find("top_provider"); route != model.end() && route->is_object()) {
    if (const auto context = TokenLimit(*route, {"context_length"})) {
      limits.context_window = limits.context_window ? std::min(*limits.context_window, *context) : context;
    }
    if (!limits.max_output_tokens) limits.max_output_tokens = TokenLimit(*route, {"max_completion_tokens"});
  }
  return {std::move(id), std::move(name), limits};
}

} // namespace

kit::Async<std::vector<DiscoveredModel>> DiscoverModels(ModelProfileInput endpoint, std::shared_ptr<HttpClient> client) {
  auto url = TrimModelField(std::move(endpoint.base_url));
  auto key = TrimModelField(std::move(endpoint.api_key));
  const auto base = Uri::Parse(url);
  if (!base || !base->Authority() || base->Authority()->empty() || base->Authority()->find('@') != std::string_view::npos ||
      base->Query() || base->Fragment() || (base->Scheme() != "http" && base->Scheme() != "https") || url.size() > 2048) {
    co_return CatalogResult::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Enter a valid HTTP(S) API base URL."});
  }
  try {
    if (const auto environment = ModelKeyEnvironment(key); !environment.empty()) {
      const auto* value = std::getenv(environment.c_str());
      if (!value || !*value) {
        co_return CatalogResult::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "The API key environment variable is unset."});
      }
      key = value;
    }
  } catch (const std::invalid_argument& error) {
    co_return CatalogResult::Failure({.code = kit::ErrorCode::InvalidArgument, .message = error.what()});
  }
  if (key.size() > 4096 || key.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
    co_return CatalogResult::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Enter a single-line API key."});
  }
  if (base->Scheme() == "http" && !key.empty()) {
    const auto authority = *base->Authority();
    const auto host = authority.starts_with('[') ? authority.substr(0, authority.find(']') + 1)
                                                 : authority.substr(0, authority.find(':'));
    if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") {
      co_return CatalogResult::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "A hosted API key requires HTTPS."});
    }
  }
  while (url.ends_with('/')) url.pop_back();
  url += "/models";

  const bool gemini = endpoint.provider == "gemini";
  const bool anthropic = endpoint.provider == "anthropic";
  HttpRequest request{.url = url, .headers = {{"Accept", "application/json"}}, .timeout = std::chrono::seconds(20)};
  if (anthropic) request.headers.push_back({"anthropic-version", "2023-06-01"});
  if (!key.empty()) {
    if (gemini) request.headers.push_back({"x-goog-api-key", std::move(key)});
    else if (anthropic) request.headers.push_back({"x-api-key", std::move(key)});
    else request.headers.push_back({"Authorization", "Bearer " + key});
  }

  std::vector<DiscoveredModel> models;
  std::set<std::string> ids;
  std::set<std::string> cursors;
  try {
    for (int page = 0; page < 128; ++page) {
      auto response = co_await client->SendAsync(request, [](const HttpProgress& progress) {
        if (progress.kind == HttpProgressKind::Download && progress.transferred_bytes > (8U << 20))
          throw std::length_error("The model catalog response is too large.");
      });
      if (!response.Succeeded()) {
        co_return CatalogResult::Failure({.code = kit::ErrorCode::ProviderFailure, .message = "Could not load the model catalog."});
      }
      if (const int status = response.Value().status_code; status < 200 || status >= 300) {
        co_return CatalogResult::Failure({.code = kit::ErrorCode::ProviderFailure,
            .message = "The model endpoint returned HTTP " + std::to_string(status) + "."});
      }
      const auto& body = response.Value().body;
      const auto* serialized = reinterpret_cast<const char*>(body.data());
      if (body.empty()) break;
      const auto payload = nlohmann::json::parse(serialized, serialized + body.size());
      const auto& entries = payload.at(gemini ? "models" : "data");
      if (!entries.is_array()) break;
      for (const auto& entry : entries) {
        if (!entry.is_object()) continue;
        auto model = ReadModel(entry, gemini);
        if (!model.id.empty() && model.id.size() <= 256 && ids.insert(model.id).second) models.push_back(std::move(model));
      }
      const auto after = payload.find("has_more");
      const bool more = gemini || (after != payload.end() && after->is_boolean() && after->get<bool>());
      const auto cursor = more ? StringField(payload, gemini ? "nextPageToken" : "last_id") : std::string{};
      if (cursor.empty() && (gemini || !more)) co_return CatalogResult::Success(std::move(models));
      if (cursor.empty() || cursor.size() > 4096 || !cursors.insert(cursor).second) break;
      request.url = url + (gemini ? "?pageToken=" : "?after_id=") + EncodeCursor(cursor);
    }
  } catch (const nlohmann::json::exception&) {
    co_return CatalogResult::Failure({.code = kit::ErrorCode::ProtocolError, .message = "The model endpoint returned an invalid catalog."});
  } catch (const std::exception&) {
    co_return CatalogResult::Failure({.code = kit::ErrorCode::ProviderFailure, .message = "Could not read the model catalog."});
  }
  co_return CatalogResult::Failure({.code = kit::ErrorCode::ProtocolError, .message = "The model endpoint did not return a complete catalog."});
}

} // namespace agentkit_preview
