#pragma once

#include <huxerui/agentkit/types.h>
#include <nlohmann/json.hpp>

namespace agentkit_preview {

inline huxerui::agentkit::JsonValue DecodeJson(const nlohmann::json& json) {
  using Value = huxerui::agentkit::JsonValue;
  if (json.is_object()) {
    Value::Object fields;
    for (auto it = json.begin(); it != json.end(); ++it) fields.emplace(it.key(), DecodeJson(it.value()));
    return {std::move(fields)};
  }
  if (json.is_array()) {
    Value::Array items;
    for (const auto& item : json) items.push_back(DecodeJson(item));
    return {std::move(items)};
  }
  if (json.is_string()) return {json.get<std::string>()};
  if (json.is_boolean()) return {json.get<bool>()};
  if (json.is_number_integer()) return {json.get<std::int64_t>()};
  if (json.is_number_float()) return {json.get<double>()};
  return {};
}

inline nlohmann::json EncodeJson(const huxerui::agentkit::JsonValue& value) {
  using Value = huxerui::agentkit::JsonValue;
  using Json = nlohmann::json;
  return std::visit([](const auto& item) -> Json {
    using Item = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<Item, Value::Object>) {
      auto object = Json::object();
      for (const auto& [key, entry] : item) object[key] = EncodeJson(entry);
      return object;
    } else if constexpr (std::is_same_v<Item, Value::Array>) {
      auto array = Json::array();
      for (const auto& entry : item) array.push_back(EncodeJson(entry));
      return array;
    } else {
      return item;
    }
  }, value.value);
}

} // namespace agentkit_preview
