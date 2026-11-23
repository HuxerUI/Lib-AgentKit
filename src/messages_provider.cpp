#include "model_provider_internal.h"

#include "json_wire.h"
#include "runtime_internal.h"

#include <array>
#include <charconv>
#include <nlohmann/json.hpp>
#include <set>

namespace huxerui::agentkit {
namespace detail {
namespace {

using Json = nlohmann::json;
using Layouts = std::map<std::size_t, Json>;
constexpr Revision resume_format = 1;
constexpr std::size_t max_text = 4 << 20;
constexpr std::size_t max_arguments = 1 << 20;
constexpr std::size_t max_private = 4 << 20;

Json TextBlock(std::string_view text) {
  return {{"type", "text"}, {"text", text}};
}

std::string ContentText(const ModelContent& content) {
  if (const auto* text = std::get_if<std::string>(&content.body)) return *text;
  if (const auto* value = std::get_if<JsonValue>(&content.body)) return ToWireJson(*value).dump();
  throw std::invalid_argument("Unsupported Messages content");
}

bool PrivateBlock(const Json& wire, MessagesDialect dialect) {
  const auto type = String(wire, "type");
  if (type == "redacted_thinking") return dialect != MessagesDialect::DeepSeek && !String(wire, "data").empty();
  if (type != "thinking" || !wire.at("thinking").is_string()) return false;
  const auto* signature = Member(wire, "signature");
  if (dialect == MessagesDialect::Anthropic) return signature && signature->is_string() && !signature->get_ref<const std::string&>().empty();
  return !signature || signature->is_string();
}

Layouts ReadLayouts(const MessagesOptions& options, const ModelTurnRequest& request) {
  Layouts layouts;
  if (!request.continuation) return layouts;
  const auto& continuation = *request.continuation;
  const auto& state = continuation.state;
  if (state.provider != options.ref || state.format_version != resume_format ||
      continuation.prefix_items > request.items.size() || state.opaque.empty() || state.opaque.size() > max_private) {
    throw std::invalid_argument("Continuation does not match this Messages model");
  }
  const auto* begin = reinterpret_cast<const char*>(state.opaque.data());
  const auto data = Json::parse(begin, begin + state.opaque.size());
  if (!data.is_object()) throw std::invalid_argument("Invalid Messages continuation");
  for (auto item = data.begin(); item != data.end(); ++item) {
    const auto& key = item.key();
    std::size_t position = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), position);
    if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || !position ||
        position > continuation.prefix_items || !item->is_array() || item->empty() ||
        !layouts.emplace(position, *item).second) {
      throw std::invalid_argument("Messages continuation is outside its input boundary");
    }
  }
  return layouts;
}

Json AssistantBlocks(std::string_view text, const Json& calls, const Json* layout, MessagesDialect dialect) {
  auto output = Json::array();
  if (!layout) {
    if (!text.empty()) output.push_back(TextBlock(text));
    for (const auto& call : calls) output.push_back(call);
    return output;
  }
  std::size_t call_index = 0;
  for (const auto& entry : *layout) {
    const auto type = String(entry, "type");
    if (type == "text") {
      const auto size = Count(entry.at("bytes"));
      if (size > text.size() || !ValidUtf8(text.substr(0, size))) throw std::invalid_argument("Message text changed");
      if (size) output.push_back(TextBlock(text.substr(0, size)));
      text.remove_prefix(size);
    } else if (type == "tool_use") {
      const auto alias = String(entry, "name");
      if (alias.empty() || call_index >= calls.size() || String(entry, "id") != String(calls[call_index], "id")) {
        throw std::invalid_argument("Message tool identity changed");
      }
      auto call = calls[call_index++];
      call["name"] = alias;
      output.push_back(std::move(call));
    } else if (PrivateBlock(entry, dialect)) {
      output.push_back(entry);
    } else throw std::invalid_argument("Invalid Messages continuation block");
  }
  if (!text.empty() || call_index != calls.size()) throw std::invalid_argument("Message continuation is incomplete");
  return output;
}

Json ConversationMessages(const MessagesOptions& options, const ModelTurnRequest& request,
                          const ToolNames& names, const Layouts& layouts) {
  auto messages = Json::array();
  const auto append = [&](std::string_view role, Json blocks) {
    if (blocks.empty()) return;
    if (!messages.empty() && String(messages.back(), "role") == role) {
      auto& previous = messages.back().at("content");
      for (auto& block : blocks) previous.push_back(std::move(block));
    } else messages.push_back({{"role", role}, {"content", std::move(blocks)}});
  };
  std::set<std::string> pending;
  std::set<std::string> call_ids;
  std::size_t used_layouts = 0;
  for (std::size_t i = 0; i < request.items.size();) {
    const auto& item = request.items[i];
    if (const auto* result = std::get_if<ModelToolResult>(&item)) {
      if (!pending.erase(result->provider_call_id)) throw std::invalid_argument("Tool result has no matching call");
      auto block = Json{{"type", "tool_result"}, {"tool_use_id", result->provider_call_id},
          {"content", ToolContent(*result)}};
      if (result->error) block["is_error"] = true;
      append("user", Json::array({std::move(block)}));
      ++i;
      continue;
    }
    if (!pending.empty()) throw std::invalid_argument("Tool results must immediately follow tool calls");
    const auto* content = std::get_if<ModelContent>(&item);
    if (content && content->role != ModelContent::Role::Assistant) {
      if (content->role == ModelContent::Role::Tool) throw std::invalid_argument("Tool content needs a call ID");
      const auto text = ContentText(*content);
      if (!text.empty()) append("user", Json::array({TextBlock(text)}));
      ++i;
      continue;
    }
    auto text = content ? ContentText(*content) : std::string{};
    if (content) ++i;
    auto calls = Json::array();
    while (i < request.items.size()) {
      const auto* call = std::get_if<ModelToolCall>(&request.items[i]);
      if (!call) break;
      if (call->provider_call_id.empty() || !call_ids.insert(call->provider_call_id).second) {
        throw std::invalid_argument("Duplicate or missing tool call ID");
      }
      auto arguments = ToWireJson(call->arguments);
      if (!arguments.is_object()) throw std::invalid_argument("Messages tool inputs must be objects");
      pending.insert(call->provider_call_id);
      calls.push_back({{"type", "tool_use"}, {"id", call->provider_call_id},
          {"name", names.Alias(call->name)}, {"input", std::move(arguments)}});
      ++i;
    }
    const auto layout = layouts.find(i);
    used_layouts += layout != layouts.end();
    append("assistant", AssistantBlocks(text, calls, layout == layouts.end() ? nullptr : &layout->second, options.dialect));
  }
  if (!pending.empty() || messages.empty() || used_layouts != layouts.size()) throw std::invalid_argument("Incomplete Messages input");
  return messages;
}

Json RequestBody(const MessagesOptions& options, const ModelTurnRequest& request,
                 const ToolNames& names, const Layouts& layouts) {
  Json body{{"model", options.model}, {"max_tokens", options.max_output_tokens}, {"stream", true},
      {"messages", ConversationMessages(options, request, names, layouts)}};
  auto instructions = ModelInstructions(request);
  if (request.output_schema) {
    auto schema = ToWireJson(request.output_schema->document);
    if (options.output_format == MessagesOutputFormat::PromptedJson) {
      if (!instructions.empty()) instructions += "\n\n";
      instructions += "Respond with one JSON value and no surrounding text or code fence. Use this JSON Schema:\n" + schema.dump();
    } else body["output_config"] = {{"format", {{"type", "json_schema"}, {"schema", std::move(schema)}}}};
  }
  if (!instructions.empty()) body["system"] = std::move(instructions);
  if (!request.tools.empty()) {
    auto tools = Json::array();
    for (const auto& tool : request.tools) {
      auto schema = ToWireJson(tool.parameters.document);
      if (schema == true) schema = Json{{"type", "object"}};
      if (!schema.is_object() || String(schema, "type") != "object") {
        throw std::invalid_argument("Messages tool schemas require an object");
      }
      tools.push_back({{"name", names.Alias(tool.name)}, {"description", tool.description}, {"input_schema", std::move(schema)}});
    }
    body["tools"] = std::move(tools);
    if (!options.multi_tool_calls && options.dialect != MessagesDialect::DeepSeek) {
      body["tool_choice"] = {{"type", "auto"}, {"disable_parallel_tool_use", true}};
    }
  }
  if (options.thinking) {
    using Mode = MessagesThinking::Mode;
    switch (options.thinking->mode) {
    case Mode::Disabled: body["thinking"] = {{"type", "disabled"}}; break;
    case Mode::Adaptive: body["thinking"] = {{"type", "adaptive"}}; break;
    case Mode::Budgeted:
      body["thinking"] = {{"type", "enabled"}, {"budget_tokens", options.thinking->budget_tokens}};
      break;
    }
  }
  return body;
}

enum class BlockKind { Text, Tool, Private };

struct ContentBlock {
  BlockKind kind;
  Json wire;
  std::string arguments;
  bool closed = false;
};

class MessagesProtocol final : public SseModelProtocol {
public:
  MessagesProtocol(const MessagesOptions& options, const ModelTurnRequest& request, ToolNames names, Layouts layouts)
      : model_(options.ref), dialect_(options.dialect), input_count_(request.items.size()),
        structured_(request.output_schema.has_value()), multiple_(options.multi_tool_calls),
        names_(std::move(names)), layouts_(std::move(layouts)) {}

  AgentResult<bool> Accept(SseFrame frame, std::deque<ModelEvent>& events) override {
    try {
      const auto event = Json::parse(frame.data);
      const auto type = String(event, "type");
      if (type.empty() || (frame.type != "message" && frame.type != type)) throw std::invalid_argument("Messages event type changed");
      if (type == "ping") return AgentResult<bool>::Success(false);
      if (type == "error") {
        const auto& failure = event.at("error");
        if (auto context = ModelContextError(failure)) return AgentResult<bool>::Failure(std::move(*context));
        const auto code = String(failure, "type") == "rate_limit_error" ? ErrorCode::RateLimited : ErrorCode::ProviderFailure;
        return Fail<bool>(code, "Messages provider reported a stream error");
      }
      if (type == "message_start") {
        const auto& message = event.at("message");
        if (std::exchange(started_, true) || String(message, "role") != "assistant" || !message.at("content").empty()) {
          throw std::invalid_argument("Invalid assistant message start");
        }
        if (const auto* usage = Member(message, "usage")) RecordUsage(*usage);
      } else if (!started_) {
        throw std::invalid_argument("Messages stream has no message start");
      } else if (type == "content_block_start") {
        StartBlock(event, events);
      } else if (type == "content_block_delta") {
        ApplyDelta(Block(event), event.at("delta"), events);
      } else if (type == "content_block_stop") {
        Block(event).closed = true;
      } else if (type == "message_delta") {
        if (const auto* usage = Member(event, "usage")) RecordUsage(*usage);
        const auto reason = String(event.at("delta"), "stop_reason");
        if (!reason.empty()) {
          if (!stop_reason_.empty() && stop_reason_ != reason) throw std::invalid_argument("Messages stop reason changed");
          stop_reason_ = reason;
        }
      } else if (type == "message_stop") {
        return Finish(events);
      }
      return AgentResult<bool>::Success(false);
    } catch (const std::exception&) {
      return Fail<bool>(ErrorCode::ProtocolError, "Malformed Messages stream event");
    }
  }

private:
  ContentBlock& Block(const Json& event) {
    auto& block = blocks_.at(Count(event.at("index")));
    if (block.closed || !stop_reason_.empty()) throw std::invalid_argument("Message block already closed");
    return block;
  }

  void AddText(ContentBlock& block, std::string text, std::deque<ModelEvent>& events) {
    if (text.size() > max_text - text_.size()) throw std::length_error("Message text exceeds limits");
    if (text.empty()) return;
    const auto index = static_cast<std::size_t>(&block - blocks_.data());
    if (last_text_block_ && index < *last_text_block_) throw std::invalid_argument("Out-of-order message text");
    last_text_block_ = index;
    text_ += text;
    block.wire["bytes"] = Count(block.wire.at("bytes")) + text.size();
    events.emplace_back(ModelTextDelta{std::move(text), !structured_});
  }

  void AddArguments(ContentBlock& block, std::string fragment, std::deque<ModelEvent>& events) {
    if (fragment.size() > max_arguments - argument_bytes_) throw std::length_error("Tool arguments exceed limits");
    if (fragment.empty()) return;
    argument_bytes_ += fragment.size();
    block.arguments += fragment;
    events.emplace_back(ModelToolArguments{String(block.wire, "id"), std::move(fragment)});
  }

  void StartBlock(const Json& event, std::deque<ModelEvent>& events) {
    const auto index = Count(event.at("index"));
    if (index != blocks_.size() || index >= 256 || !stop_reason_.empty()) throw std::invalid_argument("Invalid block order");
    const auto& wire = event.at("content_block");
    const auto type = String(wire, "type");
    if (type == "text") {
      blocks_.push_back({BlockKind::Text, {{"type", "text"}, {"bytes", 0}}});
      AddText(blocks_.back(), String(wire, "text"), events);
    } else if (type == "tool_use") {
      const auto id = String(wire, "id");
      const auto alias = String(wire, "name");
      const auto* offered = names_.Offered(alias);
      if (!offered || id.empty() || id.size() > 512 || ids_.size() >= (multiple_ ? 128 : 1) || !ids_.insert(id).second) {
        throw std::invalid_argument("Unregistered or duplicate tool call");
      }
      const auto& input = wire.at("input");
      if (!input.is_object()) throw std::invalid_argument("Invalid initial tool input");
      blocks_.push_back({BlockKind::Tool, {{"type", "tool_use"}, {"id", id}, {"name", alias}}});
      events.emplace_back(ModelToolStarted{id, *offered});
      if (!input.empty()) AddArguments(blocks_.back(), input.dump(), events);
    } else if (type == "thinking" || type == "redacted_thinking") {
      const auto bytes = wire.dump().size();
      if (bytes > max_private - private_bytes_) throw std::length_error("Thinking blocks exceed limits");
      private_bytes_ += bytes;
      blocks_.push_back({BlockKind::Private, wire});
    } else {
      throw std::invalid_argument("Unsupported Messages content block");
    }
  }

  void ApplyDelta(ContentBlock& block, const Json& delta, std::deque<ModelEvent>& events) {
    const auto type = String(delta, "type");
    if (type == "text_delta" && block.kind == BlockKind::Text) {
      AddText(block, String(delta, "text"), events);
    } else if (type == "input_json_delta" && block.kind == BlockKind::Tool) {
      AddArguments(block, String(delta, "partial_json"), events);
    } else if (block.kind == BlockKind::Private && String(block.wire, "type") == "thinking" &&
               (type == "thinking_delta" || type == "signature_delta")) {
      const auto field = type == "thinking_delta" ? "thinking" : "signature";
      auto value = String(delta, field);
      if (value.size() > max_private - private_bytes_) throw std::length_error("Thinking block exceeds limits");
      private_bytes_ += value.size();
      auto& target = block.wire[field];
      if (target.is_null()) target = "";
      target.get_ref<std::string&>() += value;
    } else if (type != "citations_delta" || block.kind != BlockKind::Text) {
      throw std::invalid_argument("Unexpected Messages content delta");
    }
  }

  void RecordUsage(const Json& wire) {
    constexpr std::array fields{"input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens", "output_tokens"};
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (const auto* value = Member(wire, fields[i])) counts_[i] = Count(*value);
    }
    has_usage_ = true;
  }

  AgentResult<bool> Finish(std::deque<ModelEvent>& events) {
    ModelFinishReason reason;
    if (stop_reason_ == "end_turn" || stop_reason_ == "stop_sequence") reason = ModelFinishReason::Completed;
    else if (stop_reason_ == "tool_use") reason = ModelFinishReason::ToolCalls;
    else if (stop_reason_ == "max_tokens") reason = ModelFinishReason::Length;
    else if (stop_reason_ == "model_context_window_exceeded") reason = ModelFinishReason::ContextLimit;
    else if (stop_reason_ == "refusal") reason = ModelFinishReason::Refused;
    else if (stop_reason_ == "content_filter") reason = ModelFinishReason::Filtered;
    else if (stop_reason_ == "pause_turn") return Fail<bool>(ErrorCode::Unsupported, "Hosted Messages tools are unavailable");
    else return Fail<bool>(ErrorCode::ProtocolError, "Messages stream ended without a recognized stop reason");

    if (has_usage_) {
      std::uint64_t input = 0;
      for (auto count : std::span(counts_).first(3)) input += std::min(count, UINT64_MAX - input);
      events.emplace_back(ModelUsage{{.input_tokens = input, .output_tokens = counts_[3]}});
    }
    if (reason != ModelFinishReason::Completed && reason != ModelFinishReason::ToolCalls) {
      events.emplace_back(ModelFinished{reason});
      return AgentResult<bool>::Success(true);
    }
    if (std::ranges::any_of(blocks_, [](const auto& block) { return !block.closed; }) ||
        (reason == ModelFinishReason::ToolCalls) != !ids_.empty()) {
      return Fail<bool>(ErrorCode::ProtocolError, "Messages output or tool calls were incomplete");
    }
    bool contains_private = false;
    for (auto& block : blocks_) {
      if (block.kind == BlockKind::Private) {
        if (!PrivateBlock(block.wire, dialect_)) return Fail<bool>(ErrorCode::ProtocolError, "Invalid Messages thinking block");
        contains_private = true;
      }
      if (block.kind != BlockKind::Tool) continue;
      if (block.arguments.empty()) AddArguments(block, "{}", events);
      auto args = Json::parse(block.arguments, nullptr, false);
      if (!args.is_object()) return Fail<bool>(ErrorCode::ProtocolError, "Messages tool did not return a complete input object");
      events.emplace_back(ModelToolFinished{String(block.wire, "id")});
    }
    if (structured_ && reason == ModelFinishReason::Completed) {
      auto value = ParseJson(text_);
      if (!value.Succeeded()) return Fail<bool>(ErrorCode::ProtocolError, "Messages structured result is invalid JSON");
      events.emplace_back(ModelStructuredOutput{std::move(value).Value()});
    }
    const auto end = input_count_ + (!text_.empty() ? 1 : 0) + ids_.size();
    if (contains_private && end > input_count_) {
      auto layout = Json::array();
      for (auto& block : blocks_) layout.push_back(std::move(block.wire));
      layouts_[end] = std::move(layout);
    }
    std::optional<ModelResumeState> resume;
    if (!layouts_.empty()) {
      auto saved = Json::object();
      for (auto& [position, layout] : layouts_) saved[std::to_string(position)] = std::move(layout);
      auto bytes = saved.dump();
      if (bytes.size() > max_private) return Fail<bool>(ErrorCode::ProtocolError, "Messages continuation exceeds limits");
      resume = ModelResumeState{model_, resume_format, ByteString(bytes)};
    }
    events.emplace_back(ModelFinished{reason, std::move(resume)});
    return AgentResult<bool>::Success(true);
  }

  ModelProviderRef model_;
  MessagesDialect dialect_;
  std::size_t input_count_;
  bool structured_;
  bool multiple_;
  ToolNames names_;
  Layouts layouts_;
  std::vector<ContentBlock> blocks_;
  std::set<std::string> ids_;
  std::string text_;
  std::string stop_reason_;
  std::array<std::uint64_t, 4> counts_{};
  std::optional<std::size_t> last_text_block_;
  std::size_t argument_bytes_ = 0;
  std::size_t private_bytes_ = 0;
  bool started_ = false;
  bool has_usage_ = false;
};

class MessagesProvider final : public PositionedModelProvider {
public:
  MessagesProvider(MessagesOptions options, std::string url, TaskScope tasks, ModelHttpOpen open)
      : options_(std::move(options)), url_(std::move(url)), tasks_(tasks), open_(std::move(open)) {}

  ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = options_.tool_calls,
        .structured_output = options_.output_format != MessagesOutputFormat::Text, .usage = true,
        .multi_tool_calls = options_.tool_calls && options_.multi_tool_calls};
  }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    using Stream = std::unique_ptr<ModelTurnStream>;
    if (request.stop.stop_requested()) co_return Fail<Stream>(ErrorCode::AlreadyClosed, "Model turn cancelled");
    if (request.provider != options_.ref) co_return Fail<Stream>(ErrorCode::Conflict, "Model registration changed");
    if (!request.attachments.empty() || (!options_.tool_calls && !request.tools.empty()) ||
        (request.output_schema && options_.output_format == MessagesOutputFormat::Text)) {
      co_return Fail<Stream>(ErrorCode::Unsupported, "Requested model capability is unavailable");
    }
    auto post = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!post.Succeeded()) co_return AgentResult<Stream>::Failure(post.Error());
    auto call = std::move(post).Value();
    call.headers.push_back({"anthropic-version", options_.api_version});
    std::unique_ptr<SseModelProtocol> protocol;
    try {
      ToolNames names(request);
      auto layouts = ReadLayouts(options_, request);
      call.body = ByteString(RequestBody(options_, request, names, layouts).dump());
      protocol = std::make_unique<MessagesProtocol>(options_, request, std::move(names), std::move(layouts));
    } catch (const std::exception&) {
      co_return Fail<Stream>(ErrorCode::InvalidArgument, "Invalid Messages input or continuation");
    }
    co_return co_await OpenModelStream(std::move(call), tasks_, open_, options_.api_key, request.stop,
        std::move(protocol), options_.stream_timeouts, ModelCredential::ApiKey);
  }

private:
  MessagesOptions options_;
  std::string url_;
  TaskScope tasks_;
  ModelHttpOpen open_;
};

} // namespace

AgentResult<ModelRegistration> MakeMessagesModel(MessagesOptions options, TaskScope tasks, ModelHttpOpen open) {
  auto url = ModelUrl(options.base_url, bool(options.api_key), "/messages");
  if (!url.Succeeded()) return AgentResult<ModelRegistration>::Failure(url.Error());
  using Mode = MessagesThinking::Mode;
  const bool budgeted = options.thinking && options.thinking->mode == Mode::Budgeted;
  const auto& version = options.api_version;
  if (!options.ref.version || options.ref.id.value.empty() || options.model.empty() ||
      !ValidUtf8(options.model) || !ValidModelTimeouts(options.timeout, options.stream_timeouts) || !open || !options.max_output_tokens ||
      version.size() != 10 || version[4] != '-' || version[7] != '-' ||
      !std::ranges::all_of(version, [](char value) { return value == '-' || (value >= '0' && value <= '9'); }) ||
      (budgeted && (options.thinking->budget_tokens < 1024 || options.thinking->budget_tokens >= options.max_output_tokens))) {
    return Fail<ModelRegistration>(ErrorCode::InvalidArgument, "Invalid Messages model configuration");
  }
  if (options.dialect != MessagesDialect::Anthropic && (budgeted || options.output_format == MessagesOutputFormat::JsonSchema)) {
    return Fail<ModelRegistration>(ErrorCode::Unsupported, "Messages compatibility endpoint requires its supported thinking and output formats");
  }
  const char* provider = options.dialect == MessagesDialect::Anthropic ? "anthropic" :
      options.dialect == MessagesDialect::DeepSeek ? "deepseek" : "zhipu";
  ModelRegistration registration{options.ref, {provider, options.model, "messages", options.base_url},
      {}, false, options.allow_persist_resume_state};
  registration.provider = std::make_shared<MessagesProvider>(std::move(options), std::move(url).Value(), tasks, std::move(open));
  return AgentResult<ModelRegistration>::Success(std::move(registration));
}

} // namespace detail

AgentResult<ModelRegistration> MakeMessagesModel(MessagesOptions options, std::shared_ptr<HttpClient> client) {
  try {
    auto tasks = UseApplicationTaskScope();
    return detail::MakeMessagesModel(std::move(options), tasks, detail::NativeModelTransport(std::move(client)));
  } catch (const std::logic_error&) {
    return detail::Fail<ModelRegistration>(ErrorCode::InvalidState, "Create the model adapter on the application thread");
  }
}

} // namespace huxerui::agentkit
