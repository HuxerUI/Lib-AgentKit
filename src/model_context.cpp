#include "model_context.h"

#include "json_wire.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace huxerui::agentkit::detail {
namespace {

bool WordByte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
}

std::string_view Utf8Prefix(std::string_view text, std::size_t bytes) {
  auto end = std::min(bytes, text.size());
  while (end < text.size() && end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
  return text.substr(0, end);
}

} // namespace

std::uint64_t EstimateTokens(std::string_view text) {
  std::uint64_t tokens = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (WordByte(byte) || byte == ' ') {
      const auto begin = i++;
      while (i != text.size() && (byte == ' ' ? text[i] == ' ' : WordByte(text[i]))) ++i;
      tokens += (i - begin + 2) / 3;
    } else {
      if (byte < 0x80) ++tokens;
      else if (byte >= 0xc0) tokens += byte >= 0xf0 ? 3 : 2;
      ++i;
    }
  }
  return tokens;
}

std::uint64_t EstimateTokens(const ModelItem& item) {
  constexpr std::uint64_t item_overhead = 8;
  if (const auto* content = std::get_if<ModelContent>(&item)) {
    if (const auto* text = std::get_if<std::string>(&content->body)) return item_overhead + EstimateTokens(std::string_view(*text));
    if (const auto* value = std::get_if<JsonValue>(&content->body))
      return item_overhead + EstimateTokens(std::string_view(ToWireJson(*value).dump()));
    return item_overhead + 2048;
  }
  if (const auto* call = std::get_if<ModelToolCall>(&item))
    return item_overhead + EstimateTokens(std::string_view(call->name)) + EstimateTokens(std::string_view(call->provider_call_id)) +
        EstimateTokens(std::string_view(ToWireJson(call->arguments).dump()));
  const auto& result = std::get<ModelToolResult>(item);
  return item_overhead + EstimateTokens(std::string_view(result.provider_call_id)) +
      (result.value ? EstimateTokens(std::string_view(ToWireJson(*result.value).dump())) : 0) +
      (result.error ? EstimateTokens(std::string_view(result.error->message)) : 0);
}

std::uint64_t EstimateTokens(const ModelTurnRequest& request) {
  std::uint64_t count = 24 + EstimateTokens(std::string_view(request.instructions));
  for (const auto& item : request.items) count += EstimateTokens(item);
  for (const auto& tool : request.tools)
    count += 24 + EstimateTokens(std::string_view(tool.name)) + EstimateTokens(std::string_view(tool.description)) +
        EstimateTokens(std::string_view(ToWireJson(tool.parameters.document).dump()));
  for (const auto& skill : request.available_skills)
    count += 24 + EstimateTokens(std::string_view(skill.name)) + EstimateTokens(std::string_view(skill.description));
  for (const auto& skill : request.skill_instructions) count += 24 + EstimateTokens(std::string_view(skill.markdown));
  if (request.output_schema) count += 24 + EstimateTokens(std::string_view(ToWireJson(request.output_schema->document).dump()));
  return count;
}

std::uint64_t ModelInputLimit(const ModelContextLimits& limits, std::uint64_t reserved_output) {
  const auto window = limits.context_window.value_or(UINT64_MAX);
  const auto room = window - std::min(window, reserved_output);
  return std::min(room, limits.max_input_tokens.value_or(UINT64_MAX));
}

std::uint64_t ConversationInputLimit(const ModelContextLimits& limits, const ModelTurnRequest& request,
    std::span<const ModelItem> history, std::string_view task) {
  const auto schema = request.output_schema ?
      EstimateTokens(std::string_view(ToWireJson(request.output_schema->document).dump())) : 0;
  const auto reply = std::min(limits.max_output_tokens.value_or(8192),
      512 + std::min<std::uint64_t>(EstimateTokens(task), 768) + std::min<std::uint64_t>(schema, 4096));
  std::uint64_t next_tools = 0;
  if (!request.tools.empty()) {
    std::uint64_t last_batch = 0;
    bool found_result = false;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (!std::holds_alternative<ModelToolResult>(*it)) {
        if (found_result) break;
        continue;
      }
      found_result = true;
      last_batch += EstimateTokens(*it);
    }
    next_tools = std::clamp<std::uint64_t>(last_batch, 512, 1536) + 128;
  }
  const auto window = ModelInputLimit(limits, reply + next_tools + 128);
  if (!limits.max_input_tokens) return window;
  const auto input = *limits.max_input_tokens;
  return std::min(window, input - std::min(input, next_tools));
}

std::optional<JsonValue> ShortenToolResult(const ModelToolResult& result) {
  if (!result.value) return {};
  const auto wire = ToWireJson(*result.value).dump();
  if (wire.size() <= 320) return {};
  auto reduced = JsonValue{JsonValue::Object{{"older_tool_output_reduced", JsonValue{true}},
      {"excerpt", JsonValue{std::string(Utf8Prefix(wire, 144))}}}};
  if (ToWireJson(reduced).dump().size() >= wire.size()) return {};
  return reduced;
}

} // namespace huxerui::agentkit::detail
