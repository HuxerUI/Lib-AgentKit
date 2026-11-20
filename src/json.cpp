#include "runtime_internal.h"
#include "json_wire.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <utility>
#include <nlohmann/json.hpp>

namespace huxerui::agentkit::detail {

JsonValue FromWireJson(const nlohmann::json& json) {
  if (json.is_null()) return {};
  if (json.is_boolean()) return {json.get<bool>()};
  if (json.is_number_unsigned()) {
    auto number = json.get<std::uint64_t>();
    if (number > static_cast<std::uint64_t>(INT64_MAX)) throw std::range_error("Integer exceeds supported range");
    return {static_cast<std::int64_t>(number)};
  }
  if (json.is_number_integer()) return {json.get<std::int64_t>()};
  if (json.is_number_float()) return {json.get<double>()};
  if (json.is_string()) return {json.get<std::string>()};
  if (json.is_array()) {
    JsonValue::Array values;
    values.reserve(json.size());
    for (auto& child : json) values.push_back(FromWireJson(child));
    return {std::move(values)};
  }
  JsonValue::Object object;
  for (auto it = json.begin(); it != json.end(); ++it) object.emplace(it.key(), FromWireJson(it.value()));
  return {std::move(object)};
}

nlohmann::json ToWireJson(const JsonValue& value) {
  return std::visit([](const auto& node) -> nlohmann::json {
    using T = std::decay_t<decltype(node)>;
    if constexpr (std::is_same_v<T, JsonValue::Array>) {
      auto array = nlohmann::json::array();
      for (const auto& child : node) array.push_back(ToWireJson(child));
      return array;
    } else if constexpr (std::is_same_v<T, JsonValue::Object>) {
      auto object = nlohmann::json::object();
      for (const auto& [name, child] : node) object[name] = ToWireJson(child);
      return object;
    } else if constexpr (std::is_same_v<T, double>) {
      if (!std::isfinite(node)) throw std::invalid_argument("JSON numbers must be finite");
      return node;
    } else {
      return node;
    }
  }, value.value);
}

namespace {

const JsonValue* Field(const JsonValue::Object& object, std::string_view name) {
  auto it = object.find(name);
  return it == object.end() ? nullptr : &it->second;
}

std::string ChildPath(const std::string& path, std::string_view child) {
  auto result = path + "/";
  for (char ch : child) {
    if (ch == '/') result += "~1";
    else if (ch == '~') result += "~0";
    else result += ch;
  }
  return result;
}

bool MatchesType(const JsonValue& value, std::string_view type) {
  if (type == "null") return std::holds_alternative<std::nullptr_t>(value.value);
  if (type == "boolean") return std::holds_alternative<bool>(value.value);
  if (type == "integer") return std::holds_alternative<std::int64_t>(value.value);
  if (type == "number") return MatchesType(value, "integer") || std::holds_alternative<double>(value.value);
  if (type == "string") return std::holds_alternative<std::string>(value.value);
  if (type == "array") return std::holds_alternative<JsonValue::Array>(value.value);
  if (type == "object") return std::holds_alternative<JsonValue::Object>(value.value);
  return false;
}

std::partial_ordering CompareIntegerToReal(std::int64_t integer, double real) {
  if (std::isnan(real)) return std::partial_ordering::unordered;
  constexpr double upper = 0x1p63;
  if (real >= upper) return std::partial_ordering::less;
  if (real < -upper) return std::partial_ordering::greater;
  const auto whole = static_cast<std::int64_t>(real);
  if (auto order = integer <=> whole; order != 0) return order;
  return static_cast<double>(whole) <=> real;
}

std::partial_ordering CompareNumbers(const JsonValue& left, const JsonValue& right) {
  const auto* left_integer = std::get_if<std::int64_t>(&left.value);
  const auto* right_integer = std::get_if<std::int64_t>(&right.value);
  if (left_integer && right_integer) return *left_integer <=> *right_integer;
  if (left_integer) return CompareIntegerToReal(*left_integer, std::get<double>(right.value));
  if (right_integer) return 0 <=> CompareIntegerToReal(*right_integer, std::get<double>(left.value));
  return std::get<double>(left.value) <=> std::get<double>(right.value);
}

void CheckValue(const JsonValue& value, const std::string& path, std::vector<FieldError>& errors,
                unsigned depth) {
  if (depth > 64 || errors.size() >= 32) return;
  if (const auto* text = std::get_if<std::string>(&value.value); text && !ValidUtf8(*text)) {
    errors.push_back({path, "utf8", "Expected valid UTF-8"});
  } else if (const auto* number = std::get_if<double>(&value.value); number && !std::isfinite(*number)) {
    errors.push_back({path, "number", "Expected a finite number"});
  } else if (const auto* array = std::get_if<JsonValue::Array>(&value.value)) {
    for (std::size_t i = 0; i < array->size(); ++i) {
      CheckValue((*array)[i], ChildPath(path, std::to_string(i)), errors, depth + 1);
    }
  } else if (const auto* object = std::get_if<JsonValue::Object>(&value.value)) {
    for (const auto& [name, child] : *object) {
      if (!ValidUtf8(name)) errors.push_back({path, "utf8", "Property name must be valid UTF-8"});
      CheckValue(child, ChildPath(path, name), errors, depth + 1);
      if (errors.size() >= 32) break;
    }
  }
  if (depth == 64 && (MatchesType(value, "object") || MatchesType(value, "array"))) {
    errors.push_back({path, "depth", "Payload is too deeply nested"});
  }
}

bool CheckSchema(const JsonValue& value, const JsonValue& schema, const std::string& path,
                 std::vector<FieldError>& errors, unsigned depth) {
  if (errors.size() >= 32) return true;
  if (depth > 64) return false;
  if (const auto* boolean = std::get_if<bool>(&schema.value)) {
    if (!*boolean) errors.push_back({path, "schema", "Value is forbidden"});
    return true;
  }
  const auto* object = std::get_if<JsonValue::Object>(&schema.value);
  if (!object) return false;
  for (const auto& [name, entry] : *object) {
    if (name == "minLength" || name == "maxLength" || name == "minItems" || name == "maxItems") {
      const auto* limit = std::get_if<std::int64_t>(&entry.value);
      if (!limit || *limit < 0) return false;
      std::optional<std::size_t> size;
      if (name.ends_with("Length")) {
        if (const auto* text = std::get_if<std::string>(&value.value)) {
          size = std::ranges::count_if(*text, [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
        }
      } else if (const auto* array = std::get_if<JsonValue::Array>(&value.value)) {
        size = array->size();
      }
      if (size && (name.starts_with("min") ? std::cmp_less(*size, *limit) : std::cmp_greater(*size, *limit))) {
        errors.push_back({path, name, "Length is outside the permitted range"});
      }
      continue;
    }
    if (name == "minimum" || name == "maximum") {
      const auto* real = std::get_if<double>(&entry.value);
      if (!MatchesType(entry, "number") || (real && !std::isfinite(*real))) return false;
      const auto invalid = name == "minimum" ? std::partial_ordering::less : std::partial_ordering::greater;
      if (MatchesType(value, "number") && CompareNumbers(value, entry) == invalid) {
        errors.push_back({path, name, "Number is outside the permitted range"});
      }
      continue;
    }
    if (name != "type" && name != "properties" && name != "required" && name != "additionalProperties" &&
        name != "items" && name != "enum" && name != "const" && name != "title" &&
        name != "description" && name != "default" && name != "$schema") return false;
  }
  if (const auto* type = Field(*object, "type")) {
    const auto* text = std::get_if<std::string>(&type->value);
    if (!text) return false;
    if (!MatchesType(value, *text)) { errors.push_back({path, "type", "Expected " + *text}); return true; }
  }
  if (const auto* enumeration = Field(*object, "enum")) {
    const auto* array = std::get_if<JsonValue::Array>(&enumeration->value);
    if (!array) return false;
    if (std::ranges::find(*array, value) == array->end()) errors.push_back({path, "enum", "Value is not a permitted option"});
  }
  if (const auto* constant = Field(*object, "const"); constant && *constant != value) {
    errors.push_back({path, "const", "Value does not match the required value"});
  }
  if (const auto* values = std::get_if<JsonValue::Object>(&value.value)) {
    if (const auto* required = Field(*object, "required")) {
      const auto* array = std::get_if<JsonValue::Array>(&required->value);
      if (!array) return false;
      for (const auto& item : *array) {
        const auto* name = std::get_if<std::string>(&item.value);
        if (!name) return false;
        if (!values->contains(*name)) errors.push_back({ChildPath(path, *name), "required", "Field is required"});
      }
    }
    const auto* declared = Field(*object, "properties");
    const auto* props = declared ? std::get_if<JsonValue::Object>(&declared->value) : nullptr;
    if (declared && !props) return false;
    const auto* additional = Field(*object, "additionalProperties");
    for (const auto& [name, child] : *values) {
      const auto* matcher = props ? Field(*props, name) : nullptr;
      if (!matcher) matcher = additional;
      if (matcher && !CheckSchema(child, *matcher, ChildPath(path, name), errors, depth + 1)) return false;
    }
  }
  if (const auto* item = Field(*object, "items")) {
    if (const auto* array = std::get_if<JsonValue::Array>(&value.value)) {
      for (std::size_t i = 0; i < array->size(); ++i) {
        if (!CheckSchema((*array)[i], *item, ChildPath(path, std::to_string(i)), errors, depth + 1)) return false;
      }
    }
  }
  return true;
}

} // namespace

AgentResult<JsonValue> ParseJson(std::string_view text) {
  if (text.size() > (1 << 20)) return Fail<JsonValue>(ErrorCode::InvalidArgument, "JSON exceeds 1 MiB");
  std::vector<std::set<std::string>> objects;
  try {
    auto json = nlohmann::json::parse(text, [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& item) {
      if (depth > 64) throw std::range_error("JSON exceeds the supported nesting depth");
      if (event == nlohmann::json::parse_event_t::object_start) objects.emplace_back();
      if (event == nlohmann::json::parse_event_t::object_end) objects.pop_back();
      if (event == nlohmann::json::parse_event_t::key &&
          !objects.back().insert(item.get<std::string>()).second) throw std::invalid_argument("Duplicate JSON key");
      return true;
    });
    return AgentResult<JsonValue>::Success(FromWireJson(json));
  } catch (const std::exception&) {
    return Fail<JsonValue>(ErrorCode::InvalidArgument, "Invalid JSON, duplicate keys, or unsupported JSON size");
  }
}

AgentResult<void> ValidateJson(const JsonValue& value, const JsonSchema& schema) {
  std::vector<FieldError> errors;
  CheckValue(value, "", errors, 0);
  if (!CheckSchema(value, schema.document, "", errors, 0)) {
    return Fail<void>(ErrorCode::Unsupported, "Schema uses unsupported or malformed constraints");
  }
  if (errors.empty()) return AgentResult<void>::Success();
  auto failure = Error(ErrorCode::InvalidArgument, "Input does not satisfy the schema");
  failure.fields = std::move(errors);
  return AgentResult<void>::Failure(std::move(failure));
}

} // namespace huxerui::agentkit::detail
