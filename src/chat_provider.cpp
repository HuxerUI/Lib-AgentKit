#include "model_provider_internal.h"

#include "json_wire.h"
#include "runtime_internal.h"

#include <nlohmann/json.hpp>
#include <set>

namespace huxerui::agentkit {
namespace detail {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_argument_bytes = 1 << 20;
constexpr std::size_t max_private_bytes = 4 << 20;
constexpr std::size_t max_output_bytes = 4 << 20;

Json ResumeFields(const ChatCompletionsOptions& options, const ModelTurnRequest& request) {
  if (!request.continuation) return Json::object();
  const auto& continuation = *request.continuation;
  const auto& state = continuation.state;
  if (state.provider != options.ref || state.format_version != 1 || continuation.prefix_items > request.items.size() ||
      state.opaque.empty() || state.opaque.size() > max_private_bytes) {
    throw std::invalid_argument("Continuation does not match this model");
  }
  const auto* start = reinterpret_cast<const char*>(state.opaque.data());
  auto fields = Json::parse(start, start + state.opaque.size());
  if (!fields.is_object()) throw std::invalid_argument("Invalid continuation state");
  return fields;
}

Json ChatMessages(const ChatCompletionsOptions& options, const ModelTurnRequest& request,
                   const ToolNames& names, const Json& private_fields) {
  Json messages = Json::array();
  auto instructions = ModelInstructions(request);
  if (request.output_schema && options.output_format == ChatOutputFormat::JsonObject) {
    if (!instructions.empty()) instructions += "\n\n";
    instructions += "Return only a JSON object that matches this JSON Schema:\n";
    instructions += ToWireJson(request.output_schema->document).dump();
  }
  if (!instructions.empty()) {
    messages.push_back({{"role", options.developer_role ? "developer" : "system"}, {"content", std::move(instructions)}});
  }
  std::set<std::string> awaiting;
  for (std::size_t i = 0; i < request.items.size();) {
    const auto& item = request.items[i];
    if (const auto* result = std::get_if<ModelToolResult>(&item)) {
      if (!awaiting.erase(result->provider_call_id)) throw std::invalid_argument("Tool result has no matching call");
      messages.push_back({{"role", "tool"}, {"tool_call_id", result->provider_call_id}, {"content", ToolContent(*result)}});
      ++i;
      continue;
    }
    if (!awaiting.empty()) throw std::invalid_argument("Tool calls must be followed by their results");
    const auto* content = std::get_if<ModelContent>(&item);
    if (content && content->role != ModelContent::Role::Assistant) {
      if (content->role == ModelContent::Role::Tool) throw std::invalid_argument("Tool content needs a call ID");
      auto text = std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) return value;
        else if constexpr (std::is_same_v<T, JsonValue>) return ToWireJson(value).dump();
        else throw std::invalid_argument("Artifact contents require a media-capable adapter");
      }, content->body);
      messages.push_back({{"role", "user"}, {"content", std::move(text)}});
      ++i;
      continue;
    }
    Json assistant{{"role", "assistant"}, {"content", nullptr}};
    if (content) {
      const auto* text = std::get_if<std::string>(&content->body);
      const auto* value = std::get_if<JsonValue>(&content->body);
      if (!text && !value) throw std::invalid_argument("Artifact contents require a media-capable adapter");
      assistant["content"] = text ? *text : ToWireJson(*value).dump();
      ++i;
    }
    Json calls = Json::array();
    while (i < request.items.size()) {
      const auto* call = std::get_if<ModelToolCall>(&request.items[i]);
      if (!call) break;
      if (call->provider_call_id.empty() || !awaiting.insert(call->provider_call_id).second) {
        throw std::invalid_argument("Duplicate tool call identity");
      }
      calls.push_back({{"id", call->provider_call_id}, {"type", "function"},
          {"function", {{"name", names.Alias(call->name)}, {"arguments", ToWireJson(call->arguments).dump()}}}});
      ++i;
    }
    if (const auto* stored = Member(private_fields, std::to_string(i))) {
      if (const auto* reasoning = Member(*stored, "reasoning_content")) {
        assistant["reasoning_content"] = reasoning->get<std::string>();
      }
      if (const auto* extras = Member(*stored, "tool_extras")) {
        for (auto& call : calls) {
          if (const auto* extra = Member(*extras, call.at("id").get<std::string>())) call["extra_content"] = *extra;
        }
      }
    }
    if (!calls.empty()) assistant["tool_calls"] = std::move(calls);
    messages.push_back(std::move(assistant));
  }
  if (!awaiting.empty()) throw std::invalid_argument("The next model turn requires all tool results");
  if (messages.empty()) throw std::invalid_argument("Model input is empty");
  return messages;
}

Json ChatBody(const ChatCompletionsOptions& options, const ModelTurnRequest& request,
               const ToolNames& names, const Json& private_fields) {
  Json body{{"model", options.model}, {"stream", true}, {"messages", ChatMessages(options, request, names, private_fields)}};
  if (options.dialect == ChatCompletionsDialect::OpenAI) body["store"] = request.allow_server_state;
  if (options.thinking && (options.dialect == ChatCompletionsDialect::DeepSeek ||
                           options.dialect == ChatCompletionsDialect::GlmCompatible)) {
    body["thinking"] = {{"type", *options.thinking ? "enabled" : "disabled"}};
  }
  if (options.request_usage) body["stream_options"] = {{"include_usage", true}};
  if (!request.tools.empty()) {
    auto tools = Json::array();
    for (const auto& tool : request.tools) {
      tools.push_back({{"type", "function"}, {"function", {{"name", names.Alias(tool.name)},
          {"description", tool.description}, {"parameters", ToWireJson(tool.parameters.document)}}}});
    }
    body["tools"] = std::move(tools);
    if (!options.multi_tool_calls && options.dialect == ChatCompletionsDialect::OpenAI) body["parallel_tool_calls"] = false;
  }
  if (request.output_schema) {
    if (options.output_format == ChatOutputFormat::JsonObject) body["response_format"] = {{"type", "json_object"}};
    else body["response_format"] = {{"type", "json_schema"}, {"json_schema", {{"name", "agentkit_output"},
        {"schema", ToWireJson(request.output_schema->document)}, {"strict", true}}}};
  }
  return body;
}

struct CallDelta {
  std::string id;
  std::string wire_name;
  std::vector<std::string> chunks;
  std::optional<Json> extras;
  bool started = false;
};

class ChatProtocol final : public SseModelProtocol {
public:
  ChatProtocol(const ChatCompletionsOptions& options, const ModelTurnRequest& request,
               ToolNames names, Json private_fields)
      : model_(options.ref), dialect_(options.dialect), input_items_(request.items.size()),
        structured_(request.output_schema.has_value()), multiple_(options.multi_tool_calls),
        names_(std::move(names)), fields_(std::move(private_fields)) {}

  AgentResult<bool> Accept(SseFrame frame, std::deque<ModelEvent>& events) override {
    try {
      if (frame.type == "error") return Fail<bool>(ErrorCode::ProviderFailure, "Model reported a stream error");
      if (frame.data == "[DONE]") return Finish(events);
      auto document = Json::parse(frame.data);
      if (Member(document, "error")) return Fail<bool>(ErrorCode::ProviderFailure, "Model reported a stream error");
      const auto* usage = Member(document, "usage");
      if (usage) {
        const auto* input = Member(*usage, "prompt_tokens");
        const auto* output = Member(*usage, "completion_tokens");
        if (!input || !output) throw std::invalid_argument("Usage record is incomplete");
        events.emplace_back(ModelUsage{{.input_tokens = Count(*input), .output_tokens = Count(*output)}});
      }
      auto choices = document.at("choices");
      if (!choices.is_array() || choices.size() > 1) throw std::invalid_argument("Only one model choice is supported");
      if (choices.empty()) return AgentResult<bool>::Success(false);
      if (finish_) throw std::invalid_argument("Unexpected delta after the final choice");
      const auto& choice = choices.front();
      if (Count(choice.at("index")) != 0) throw std::invalid_argument("Unexpected model choice index");
      if (const auto* delta = Member(choice, "delta")) DecodeDelta(*delta, events);
      auto finish = String(choice, "finish_reason");
      if (!finish.empty()) {
        if (finish == "stop") finish_ = refused_ ? ModelFinishReason::Refused : ModelFinishReason::Completed;
        else if (finish == "tool_calls") finish_ = ModelFinishReason::ToolCalls;
        else if (finish == "length") finish_ = ModelFinishReason::Length;
        else if (finish == "content_filter" || finish == "sensitive") finish_ = ModelFinishReason::Filtered;
        else return Fail<bool>(ErrorCode::ProtocolError, "Model returned an unsupported finish reason");
      }
      return AgentResult<bool>::Success(false);
    } catch (const std::exception&) {
      return Fail<bool>(ErrorCode::ProtocolError, "Malformed Chat Completions event");
    }
  }

private:
  void AddText(std::string text, std::deque<ModelEvent>& events) {
    if (text.empty()) return;
    if (text.size() > max_output_bytes - output_bytes_) throw std::length_error("Model text exceeds limit");
    output_bytes_ += text.size();
    if (structured_) structured_text_ += text;
    events.emplace_back(ModelTextDelta{std::move(text), !structured_});
  }

  void DecodeDelta(const Json& delta, std::deque<ModelEvent>& events) {
    auto content = String(delta, "content");
    auto refusal = String(delta, "refusal");
    refused_ |= !refusal.empty();
    AddText(std::move(content), events);
    AddText(std::move(refusal), events);
    if (dialect_ == ChatCompletionsDialect::DeepSeek || dialect_ == ChatCompletionsDialect::GlmCompatible) {
      const auto reasoning = String(delta, "reasoning_content");
      if (reasoning.size() > max_private_bytes - reasoning_.size()) throw std::length_error("Private state exceeds limit");
      reasoning_ += reasoning;
    }
    const auto* tools = Member(delta, "tool_calls");
    if (!tools) return;
    if (!tools->is_array()) throw std::invalid_argument("Invalid tool call delta");
    for (const auto& chunk : *tools) {
      auto index = Count(chunk.at("index"));
      if (index >= (multiple_ ? 128 : 1)) throw std::invalid_argument("Tool call index exceeds limits");
      auto& call = calls_[static_cast<std::size_t>(index)];
      auto type = String(chunk, "type");
      if (!type.empty() && type != "function") throw std::invalid_argument("Unsupported tool call kind");
      auto id = String(chunk, "id");
      if (!id.empty()) {
        if ((!call.id.empty() && call.id != id) || id.size() > 512) throw std::invalid_argument("Tool ID changed");
        call.id = std::move(id);
      }
      if (const auto* extra = Member(chunk, "extra_content")) {
        if (!extra->is_object() || (call.extras && *call.extras != *extra)) {
          throw std::invalid_argument("Invalid or changed tool continuation");
        }
        call.extras = *extra;
      }
      if (const auto* function = Member(chunk, "function")) {
        auto alias = String(*function, "name");
        if (!alias.empty() && alias != call.wire_name) {
          if (call.started || call.wire_name.size() + alias.size() > 64) throw std::invalid_argument("Tool name changed");
          call.wire_name += alias;
        }
        if (const auto* arguments = Member(*function, "arguments")) {
          auto bytes = arguments->get<std::string>();
          if (bytes.size() > max_argument_bytes - argument_bytes_) throw std::length_error("Arguments exceed limit");
          argument_bytes_ += bytes.size();
          if (!bytes.empty()) call.chunks.push_back(std::move(bytes));
        }
      }
      FlushStarts(events);
    }
  }

  void FlushStarts(std::deque<ModelEvent>& events) {
    for (auto next = calls_.find(next_index_); next != calls_.end(); next = calls_.find(next_index_)) {
      auto& call = next->second;
      auto name = names_.Offered(call.wire_name);
      if (call.id.empty() || !name) break;
      if (!ids_.insert(call.id).second) throw std::invalid_argument("Duplicate tool call ID");
      call.started = true;
      ++next_index_;
      events.emplace_back(ModelToolStarted{call.id, *name});
    }
    for (auto& [index, call] : calls_) {
      if (!call.started) continue;
      for (auto& bytes : call.chunks) events.emplace_back(ModelToolArguments{call.id, std::move(bytes)});
      call.chunks.clear();
    }
  }

  AgentResult<bool> Finish(std::deque<ModelEvent>& events) {
    if (!finish_) return Fail<bool>(ErrorCode::ProtocolError, "Model stream ended without a finish reason");
    auto reason = *finish_;
    Json extras = Json::object();
    if (reason == ModelFinishReason::ToolCalls) {
      FlushStarts(events);
      if (calls_.empty() || next_index_ != calls_.size()) {
        return Fail<bool>(ErrorCode::ProtocolError, "Model returned incomplete or unoffered tool calls");
      }
      for (const auto& [index, call] : calls_) {
        events.emplace_back(ModelToolFinished{call.id});
        if (call.extras) extras[call.id] = *call.extras;
      }
    } else if (reason == ModelFinishReason::Completed && !calls_.empty()) {
      return Fail<bool>(ErrorCode::ProtocolError, "Model emitted calls without a tool finish reason");
    }
    if (reason == ModelFinishReason::Completed && structured_) {
      auto result = ParseJson(structured_text_);
      if (!result.Succeeded()) return Fail<bool>(ErrorCode::ProtocolError, "Structured model output is invalid JSON");
      events.emplace_back(ModelStructuredOutput{std::move(result).Value()});
    }
    if ((reason == ModelFinishReason::ToolCalls || reason == ModelFinishReason::Completed) &&
        (output_bytes_ || !calls_.empty()) && (!reasoning_.empty() || !extras.empty())) {
      auto position = input_items_ + (output_bytes_ ? 1 : 0) + calls_.size();
      auto& entry = fields_[std::to_string(position)] = Json::object();
      if (!reasoning_.empty()) entry["reasoning_content"] = std::move(reasoning_);
      if (!extras.empty()) entry["tool_extras"] = std::move(extras);
    }
    std::optional<ModelResumeState> resume;
    if (!fields_.empty()) {
      auto opaque = fields_.dump();
      if (opaque.size() > max_private_bytes) return Fail<bool>(ErrorCode::ProtocolError, "Private continuation exceeds limit");
      resume = ModelResumeState{model_, 1, ByteString(opaque)};
    }
    events.emplace_back(ModelFinished{reason, std::move(resume)});
    return AgentResult<bool>::Success(true);
  }

  ModelProviderRef model_;
  ChatCompletionsDialect dialect_;
  std::size_t input_items_;
  bool structured_;
  bool multiple_;
  ToolNames names_;
  Json fields_;
  std::string reasoning_;
  std::string structured_text_;
  std::map<std::size_t, CallDelta> calls_;
  std::set<std::string> ids_;
  std::optional<ModelFinishReason> finish_;
  std::size_t next_index_ = 0;
  std::size_t output_bytes_ = 0;
  std::size_t argument_bytes_ = 0;
  bool refused_ = false;
};

class ChatProvider final : public ModelProvider {
public:
  ChatProvider(ChatCompletionsOptions options, std::string url, TaskScope tasks, ModelHttpOpen open)
      : options_(std::move(options)), url_(std::move(url)), tasks_(tasks), open_(std::move(open)) {}

  ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = options_.tool_calls,
            .structured_output = options_.output_format != ChatOutputFormat::Text, .usage = options_.request_usage,
            .multi_tool_calls = options_.tool_calls && options_.multi_tool_calls};
  }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    using Opened = AgentResult<std::unique_ptr<ModelTurnStream>>;
    if (request.stop.stop_requested()) co_return Fail<std::unique_ptr<ModelTurnStream>>(ErrorCode::AlreadyClosed, "Model turn cancelled");
    if (request.provider != options_.ref) {
      co_return Fail<std::unique_ptr<ModelTurnStream>>(ErrorCode::Conflict, "Model registration changed");
    }
    if (!request.attachments.empty() || (!options_.tool_calls && !request.tools.empty()) ||
        (request.output_schema && options_.output_format == ChatOutputFormat::Text)) {
      co_return Fail<std::unique_ptr<ModelTurnStream>>(ErrorCode::Unsupported, "Requested model capability is unavailable");
    }

    auto post = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!post.Succeeded()) co_return Opened::Failure(post.Error());
    auto call = std::move(post).Value();
    std::unique_ptr<SseModelProtocol> protocol;
    try {
      ToolNames aliases(request);
      auto fields = ResumeFields(options_, request);
      call.body = ByteString(ChatBody(options_, request, aliases, fields).dump());
      protocol = std::make_unique<ChatProtocol>(options_, request, std::move(aliases), std::move(fields));
    } catch (const std::exception&) {
      co_return Fail<std::unique_ptr<ModelTurnStream>>(ErrorCode::InvalidArgument, "Invalid Chat Completions input or continuation");
    }
    co_return co_await OpenModelStream(std::move(call), tasks_, open_, options_.bearer_token,
        request.stop, std::move(protocol));
  }

private:
  ChatCompletionsOptions options_;
  std::string url_;
  TaskScope tasks_;
  ModelHttpOpen open_;
};

} // namespace

AgentResult<ModelRegistration> MakeChatCompletionsModel(ChatCompletionsOptions options, TaskScope tasks, ModelHttpOpen open) {
  auto url = ModelUrl(options.base_url, bool(options.bearer_token), "/chat/completions");
  if (!url.Succeeded()) return AgentResult<ModelRegistration>::Failure(url.Error());
  if (!options.ref.version || options.ref.id.value.empty() || options.model.empty() ||
      !ValidUtf8(options.model) || options.timeout <= Duration::zero() || !open) {
    return Fail<ModelRegistration>(ErrorCode::InvalidArgument, "Invalid Chat Completions model configuration");
  }
  std::string provider;
  switch (options.dialect) {
  case ChatCompletionsDialect::OpenAI: provider = "openai"; break;
  case ChatCompletionsDialect::DeepSeek: provider = "deepseek"; break;
  case ChatCompletionsDialect::GeminiCompatible: provider = "google"; break;
  case ChatCompletionsDialect::GlmCompatible: provider = "zhipu"; break;
  case ChatCompletionsDialect::Compatible: provider = "compatible"; break;
  }
  auto registration = ModelRegistration{options.ref, {std::move(provider), options.model, "chat-completions", options.base_url},
                                        {}, false, options.allow_persist_resume_state};
  registration.provider = std::make_shared<ChatProvider>(std::move(options), std::move(url).Value(), tasks, std::move(open));
  return AgentResult<ModelRegistration>::Success(std::move(registration));
}

} // namespace detail

AgentResult<ModelRegistration> MakeChatCompletionsModel(ChatCompletionsOptions options, std::shared_ptr<HttpClient> client) {
  try {
    auto tasks = UseApplicationTaskScope();
    return detail::MakeChatCompletionsModel(std::move(options), tasks, detail::NativeModelTransport(std::move(client)));
  } catch (const std::logic_error&) {
    return detail::Fail<ModelRegistration>(ErrorCode::InvalidState, "Create the model adapter on the application thread");
  }
}

} // namespace huxerui::agentkit
