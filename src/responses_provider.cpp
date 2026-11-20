#include "model_provider_internal.h"

#include "json_wire.h"
#include "runtime_internal.h"

#include <charconv>
#include <nlohmann/json.hpp>
#include <set>

namespace huxerui::agentkit {
namespace detail {
namespace {

using Json = nlohmann::json;
constexpr Revision resume_format = 1;
constexpr Revision compatible_resume_format = 2;
constexpr std::size_t max_arguments = 1 << 20;
constexpr std::size_t max_text = 4 << 20;
constexpr std::size_t max_resume = 4 << 20;

enum class OutputKind { Message, Function, Reasoning };

OutputKind Kind(const Json& item) {
  const auto type = String(item, "type");
  if (type == "message") return OutputKind::Message;
  if (type == "function_call") return OutputKind::Function;
  if (type == "reasoning") return OutputKind::Reasoning;
  throw std::invalid_argument("Unsupported Responses output item");
}

bool EncryptedReasoning(const Json& item) {
  return Kind(item) == OutputKind::Reasoning &&
      !String(item, "id").empty() && !String(item, "encrypted_content").empty();
}

std::string_view ProviderName(ResponsesDialect dialect) {
  switch (dialect) {
    case ResponsesDialect::OpenAI: return "openai";
    case ResponsesDialect::DeepSeek: return "deepseek";
    case ResponsesDialect::GlmCompatible: return "glm";
  }
  throw std::invalid_argument("Unknown Responses dialect");
}

bool ReasoningTextPart(const Json& item) {
  const auto* text = Member(item, "text");
  return String(item, "type") == "reasoning_text" && text && text->is_string();
}

std::optional<Json> ReasoningForInput(const Json& wire, ResponsesDialect dialect) {
  if (dialect == ResponsesDialect::OpenAI) {
    return EncryptedReasoning(wire) ? std::optional<Json>(wire) : std::nullopt;
  }
  const auto* content = Member(wire, "content");
  if (!content) return std::nullopt;
  const bool valid = dialect == ResponsesDialect::DeepSeek
      ? content->is_array() && std::ranges::all_of(*content, ReasoningTextPart)
      : ReasoningTextPart(*content);
  if (!valid) throw std::invalid_argument("Invalid Responses reasoning content");
  if (content->empty()) return std::nullopt;
  return Json{{"type", "reasoning"}, {"content", *content}};
}

using ReasoningItems = std::map<std::size_t, Json>;

struct ResumeState {
  ReasoningItems reasoning;
  std::string previous;
};

ResumeState ReadResume(const ResponsesOptions& options, const ModelTurnRequest& request) {
  ResumeState result;
  const bool openai = options.dialect == ResponsesDialect::OpenAI;
  if (!request.continuation || (openai && !options.encrypted_reasoning)) return result;
  const auto& continuation = *request.continuation;
  const auto& state = continuation.state;
  const auto version = openai ? resume_format : compatible_resume_format;
  if (state.provider != options.ref || state.format_version != version ||
      continuation.prefix_items > request.items.size() || state.opaque.empty() || state.opaque.size() > max_resume) {
    throw std::invalid_argument("Continuation does not match this model");
  }
  const auto* begin = reinterpret_cast<const char*>(state.opaque.data());
  const auto document = Json::parse(begin, begin + state.opaque.size());
  if (!openai) {
    if (String(document, "dialect") != ProviderName(options.dialect) || String(document, "run") != request.run.value ||
        Count(document.at("prefix")) != continuation.prefix_items) {
      throw std::invalid_argument("Responses continuation does not match this run or authorized prefix");
    }
    result.previous = String(document, "previous_response_id");
    if (!result.previous.empty() && (options.dialect != ResponsesDialect::GlmCompatible || result.previous.size() > 512)) {
      throw std::invalid_argument("Invalid previous response ID");
    }
  }
  const auto& fields = openai ? document : document.at("reasoning");
  if (!fields.is_object()) throw std::invalid_argument("Invalid Responses continuation");
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    const auto& key = it.key();
    std::size_t position = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), position);
    if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() ||
        position > continuation.prefix_items || !it->is_array() ||
        !std::ranges::all_of(*it, [&](const auto& item) {
          if (Kind(item) != OutputKind::Reasoning) return false;
          const auto replay = ReasoningForInput(item, options.dialect);
          return replay && *replay == item;
        }) || !result.reasoning.emplace(position, *it).second) {
      throw std::invalid_argument("Invalid reasoning continuation boundary");
    }
  }
  return result;
}

Json InputItems(const ModelTurnRequest& request, const ToolNames& names, const ReasoningItems& reasoning,
                ResponsesDialect dialect, std::size_t first) {
  auto input = Json::array();
  std::set<std::string> awaiting;
  std::set<std::string> calls;
  const auto append_reasoning = [&](std::size_t index) {
    if (first) return;
    if (const auto found = reasoning.find(index); found != reasoning.end()) {
      for (const auto& item : found->second) input.push_back(item);
    }
  };
  for (std::size_t i = 0; i < request.items.size(); ++i) {
    append_reasoning(i);
    const auto& item = request.items[i];
    if (const auto* call = std::get_if<ModelToolCall>(&item)) {
      if (call->provider_call_id.empty() || !calls.insert(call->provider_call_id).second) {
        throw std::invalid_argument("Duplicate or missing tool call ID");
      }
      awaiting.insert(call->provider_call_id);
      if (i >= first) input.push_back({{"type", "function_call"}, {"call_id", call->provider_call_id},
          {"name", names.Alias(call->name)}, {"arguments", ToWireJson(call->arguments).dump()}});
    } else if (const auto* finished = std::get_if<ModelToolResult>(&item)) {
      if (!awaiting.erase(finished->provider_call_id)) throw std::invalid_argument("Tool result has no matching call");
      if (i >= first) input.push_back({{"type", "function_call_output"}, {"call_id", finished->provider_call_id},
          {"output", ToolContent(*finished)}});
    } else {
      if (!awaiting.empty()) throw std::invalid_argument("Tool calls must be followed by their results");
      const auto& message = std::get<ModelContent>(item);
      const auto* text = std::get_if<std::string>(&message.body);
      const auto* value = std::get_if<JsonValue>(&message.body);
      if (message.role == ModelContent::Role::Tool || (!text && !value)) {
        throw std::invalid_argument("Unsupported Responses message content");
      }
      if (i >= first) {
        Json wire{{"role", message.role == ModelContent::Role::Assistant ? "assistant" : "user"},
            {"content", text ? *text : ToWireJson(*value).dump()}};
        if (dialect != ResponsesDialect::OpenAI) wire["type"] = "message";
        input.push_back(std::move(wire));
      }
    }
  }
  append_reasoning(request.items.size());
  if (!awaiting.empty() || input.empty()) throw std::invalid_argument("Incomplete Responses input");
  return input;
}

Json RequestBody(const ResponsesOptions& options, const ModelTurnRequest& request,
                 const ToolNames& names, const ReasoningItems& reasoning, bool store, std::string_view previous) {
  const auto first = previous.empty() ? 0 : request.continuation->prefix_items;
  Json body{{"model", options.model}, {"stream", true},
      {"input", InputItems(request, names, reasoning, options.dialect, first)}};
  if (options.dialect != ResponsesDialect::DeepSeek) body["store"] = store;
  if (!previous.empty()) body["previous_response_id"] = previous;
  auto instructions = ModelInstructions(request);
  if (options.encrypted_reasoning) body["include"] = Json::array({"reasoning.encrypted_content"});
  if (!request.tools.empty()) {
    auto tools = Json::array();
    for (const auto& tool : request.tools) {
      auto parameters = ToWireJson(tool.parameters.document);
      Json declaration{{"type", "function"}, {"name", names.Alias(tool.name)}, {"description", tool.description}};
      if (options.dialect == ResponsesDialect::OpenAI) {
        declaration["strict"] = false;
      } else if (parameters == true) {
        parameters = {{"type", "object"}};
      } else if (!parameters.is_object()) {
        throw std::invalid_argument("Responses function parameters must have an object schema");
      }
      declaration["parameters"] = std::move(parameters);
      tools.push_back(std::move(declaration));
    }
    body["tools"] = std::move(tools);
    if (options.dialect == ResponsesDialect::OpenAI) body["parallel_tool_calls"] = options.multi_tool_calls;
  }
  if (request.output_schema) {
    auto schema = ToWireJson(request.output_schema->document);
    if (options.output_format == ResponsesOutputFormat::JsonObject) {
      if (!instructions.empty()) instructions += "\n\n";
      instructions += "Return only a JSON object that matches this JSON Schema:\n" + schema.dump();
      auto& input = body["input"];
      input.insert(input.begin(), Json{{"type", "message"}, {"role", "system"},
          {"content", "Respond with a JSON object that matches the requested schema."}});
      body["text"] = {{"format", {{"type", "json_object"}}}};
    } else {
      body["text"] = {{"format", {{"type", "json_schema"}, {"name", "agentkit_output"},
          {"schema", std::move(schema)}}}};
      if (options.dialect == ResponsesDialect::OpenAI) body["text"]["format"]["strict"] = true;
    }
  }
  if (!instructions.empty()) body["instructions"] = std::move(instructions);
  return body;
}

struct OutputItem {
  OutputKind kind;
  std::string id;
  std::string call_id;
  std::string alias;
  std::string content;
  bool arguments_done = false;
  bool refused = false;
};

class ResponsesProtocol final : public SseModelProtocol {
public:
  ResponsesProtocol(const ResponsesOptions& options, const ModelTurnRequest& request, ToolNames names,
                    ReasoningItems reasoning, bool store)
      : model_(options.ref), run_(request.run), input_count_(request.items.size()), dialect_(options.dialect),
        structured_(request.output_schema.has_value()), multiple_(options.multi_tool_calls),
        keep_reasoning_(options.encrypted_reasoning || options.dialect != ResponsesDialect::OpenAI), store_(store),
        names_(std::move(names)), reasoning_(std::move(reasoning)) {}

  AgentResult<bool> Accept(SseFrame frame, std::deque<ModelEvent>& events) override {
    try {
      auto event = Json::parse(frame.data);
      const auto type = String(event, "type");
      if (type.empty() || (frame.type != "message" && frame.type != type)) {
        throw std::invalid_argument("Invalid Responses event type");
      }
      if (const auto* seq = Member(event, "sequence_number")) {
        const auto number = Count(*seq);
        if (sequence_ && number <= *sequence_) throw std::invalid_argument("Repeated or unordered Responses event");
        sequence_ = number;
      }
      if (type == "error" || type == "response.failed" || type == "response.cancelled") {
        return Fail<bool>(ErrorCode::ProviderFailure, "Responses request did not complete");
      }
      if (type == "response.created" && store_) {
        if (!response_id_.empty()) throw std::invalid_argument("Response stream was created twice");
        response_id_ = String(event.at("response"), "id");
        if (response_id_.empty() || response_id_.size() > 512) throw std::invalid_argument("Invalid response ID");
      } else if (type == "response.output_item.added") {
        AddItem(event, events);
      } else if (type == "response.output_text.delta" || type == "response.refusal.delta") {
        auto& item = FindItem(event);
        if (item.kind != OutputKind::Message) throw std::invalid_argument("Invalid output text target");
        item.refused |= type == "response.refusal.delta";
        auto delta = String(event, "delta");
        if (delta.size() > max_text - text_bytes_) throw std::length_error("Response text exceeds limits");
        text_bytes_ += delta.size();
        item.content += delta;
        if (!delta.empty())
          events.emplace_back(ModelTextDelta{std::move(delta), !structured_ || type == "response.refusal.delta"});
      } else if (type == "response.function_call_arguments.delta" || type == "response.function_call_arguments.done") {
        auto& item = FindItem(event);
        if (item.kind != OutputKind::Function || item.arguments_done) throw std::invalid_argument("Unexpected tool arguments");
        if (type == "response.function_call_arguments.done") FinishArguments(item, String(event, "arguments"), events);
        else AppendArguments(item, String(event, "delta"), events);
      } else if (type == "response.output_item.done") {
        const auto& final = event.at("item");
        auto& item = items_.at(Count(event.at("output_index")));
        VerifyIdentity(item, final);
        if (item.kind == OutputKind::Function) FinishArguments(item, String(final, "arguments"), events);
      } else if (type == "response.completed" || type == "response.incomplete") {
        return Finish(event.at("response"), type == "response.incomplete", events);
      }
      return AgentResult<bool>::Success(false);
    } catch (const std::exception&) {
      return Fail<bool>(ErrorCode::ProtocolError, "Malformed Responses event");
    }
  }

private:
  void VerifyIdentity(const OutputItem& item, const Json& wire) const {
    if (item.kind != Kind(wire) || item.id != String(wire, "id") ||
        (item.kind == OutputKind::Function && (item.call_id != String(wire, "call_id") ||
                                             item.alias != String(wire, "name")))) {
      throw std::invalid_argument("Response item identity changed");
    }
  }

  OutputItem& FindItem(const Json& event) {
    auto& item = items_.at(Count(event.at("output_index")));
    if (item.id != String(event, "item_id")) throw std::invalid_argument("Unexpected output item ID");
    return item;
  }

  void AddItem(const Json& event, std::deque<ModelEvent>& events) {
    const auto index = Count(event.at("output_index"));
    if (index != items_.size() || index >= 256) throw std::invalid_argument("Invalid output index");
    const auto& wire = event.at("item");
    OutputItem item{Kind(wire), String(wire, "id")};
    if (item.id.empty() || !ids_.insert(item.id).second) throw std::invalid_argument("Invalid output item ID");
    if (item.kind == OutputKind::Function) {
      item.call_id = String(wire, "call_id");
      item.alias = String(wire, "name");
      const auto* name = names_.Offered(item.alias);
      if (!name || item.call_id.empty() || item.call_id.size() > 512 ||
          calls_.size() >= (multiple_ ? 128 : 1) || !calls_.insert(item.call_id).second) {
        throw std::invalid_argument("Invalid or unoffered function call");
      }
      events.emplace_back(ModelToolStarted{item.call_id, *name});
      AppendArguments(item, String(wire, "arguments"), events);
    }
    items_.push_back(std::move(item));
  }

  void AppendArguments(OutputItem& item, std::string bytes, std::deque<ModelEvent>& events) {
    if (bytes.empty()) return;
    if (bytes.size() > max_arguments - argument_bytes_) throw std::length_error("Function arguments exceed limits");
    argument_bytes_ += bytes.size();
    item.content += bytes;
    events.emplace_back(ModelToolArguments{item.call_id, std::move(bytes)});
  }

  void FinishArguments(OutputItem& item, std::string arguments, std::deque<ModelEvent>& events) {
    if (!item.arguments_done && item.content.empty()) AppendArguments(item, arguments, events);
    if (arguments.empty() || item.content != arguments) throw std::invalid_argument("Function arguments changed");
    item.arguments_done = true;
  }

  AgentResult<bool> Finish(const Json& response, bool incomplete, std::deque<ModelEvent>& events) {
    const auto status = String(response, "status");
    if (status != (incomplete ? "incomplete" : "completed")) throw std::invalid_argument("Unexpected response status");
    if (store_ && (response_id_.empty() || String(response, "id") != response_id_)) {
      throw std::invalid_argument("Response identity changed");
    }
    if (const auto* usage = Member(response, "usage")) {
      events.emplace_back(ModelUsage{{.input_tokens = Count(usage->at("input_tokens")),
                                       .output_tokens = Count(usage->at("output_tokens"))}});
    }
    if (incomplete) {
      const auto* details = Member(response, "incomplete_details");
      const auto reason = details ? String(*details, "reason") : "";
      if (reason == "content_filter") events.emplace_back(ModelFinished{ModelFinishReason::Filtered});
      else if (reason == "max_output_tokens" || reason == "max_messages") {
        events.emplace_back(ModelFinished{ModelFinishReason::Length});
      } else return Fail<bool>(ErrorCode::ProviderFailure, "Responses turn ended incomplete");
      return AgentResult<bool>::Success(true);
    }
    const auto& output = response.at("output");
    if (!output.is_array() || output.size() != items_.size()) throw std::invalid_argument("Response output is incomplete");
    bool refused = false;
    std::string text;
    for (std::size_t index = 0; index < items_.size(); ++index) {
      const auto& item = items_[index];
      const auto& wire = output[index];
      VerifyIdentity(item, wire);
      const auto item_status = String(wire, "status");
      if (item.kind == OutputKind::Function &&
          (!item.arguments_done || item.content != String(wire, "arguments") ||
           (!item_status.empty() && item_status != "completed"))) {
        throw std::invalid_argument("Function call did not complete");
      }
      if (item.kind != OutputKind::Message) continue;
      std::string final_text;
      const auto& content = wire.at("content");
      const auto append = [&](const Json& part) {
        const auto type = String(part, "type");
        if (type == "output_text") final_text += String(part, "text");
        else if (type == "refusal") {
          final_text += String(part, "refusal");
          refused = true;
        } else throw std::invalid_argument("Unsupported message output");
      };
      if (content.is_array()) {
        for (const auto& part : content) append(part);
      } else if (dialect_ == ResponsesDialect::GlmCompatible && content.is_object()) {
        append(content);
      } else {
        throw std::invalid_argument("Malformed message output");
      }
      if (item.content != final_text) throw std::invalid_argument("Final text differs from streamed output");
      text += final_text;
      refused |= item.refused;
    }
    if (refused) {
      events.emplace_back(ModelFinished{ModelFinishReason::Refused});
      return AgentResult<bool>::Success(true);
    }
    for (const auto& item : items_) {
      if (item.kind == OutputKind::Function) events.emplace_back(ModelToolFinished{item.call_id});
    }
    if (structured_ && calls_.empty()) {
      auto value = ParseJson(text);
      if (!value.Succeeded()) return Fail<bool>(ErrorCode::ProtocolError, "Structured response is invalid JSON");
      events.emplace_back(ModelStructuredOutput{std::move(value).Value()});
    }
    auto resume = SaveResume(output, !text.empty());
    events.emplace_back(ModelFinished{calls_.empty() ? ModelFinishReason::Completed : ModelFinishReason::ToolCalls, std::move(resume)});
    return AgentResult<bool>::Success(true);
  }

  std::optional<ModelResumeState> SaveResume(const Json& output, bool has_text) {
    if (keep_reasoning_ && (has_text || !calls_.empty())) {
      auto position = input_count_;
      bool message = false;
      for (const auto& wire : output) {
        switch (Kind(wire)) {
          case OutputKind::Message:
            if (!std::exchange(message, true) && has_text) ++position;
            break;
          case OutputKind::Function:
            ++position;
            break;
          case OutputKind::Reasoning:
            if (auto state = ReasoningForInput(wire, dialect_)) reasoning_[position].push_back(std::move(*state));
            break;
        }
      }
    }
    if (reasoning_.empty() && !store_) return std::nullopt;
    auto fields = Json::object();
    for (auto& [position, items] : reasoning_) fields[std::to_string(position)] = std::move(items);
    const bool openai = dialect_ == ResponsesDialect::OpenAI;
    if (!openai) {
      fields = Json{{"dialect", ProviderName(dialect_)}, {"run", run_.value},
          {"prefix", input_count_ + (has_text ? 1 : 0) + calls_.size()}, {"reasoning", std::move(fields)}};
      if (store_) fields["previous_response_id"] = response_id_;
    }
    auto data = fields.dump();
    if (data.size() > max_resume) throw std::length_error("Responses continuation exceeds limits");
    return ModelResumeState{model_, openai ? resume_format : compatible_resume_format, ByteString(data)};
  }

  ModelProviderRef model_;
  RunId run_;
  std::size_t input_count_;
  ResponsesDialect dialect_;
  bool structured_;
  bool multiple_;
  bool keep_reasoning_;
  bool store_;
  ToolNames names_;
  ReasoningItems reasoning_;
  std::string response_id_;
  std::vector<OutputItem> items_;
  std::set<std::string> ids_;
  std::set<std::string> calls_;
  std::optional<std::uint64_t> sequence_;
  std::size_t argument_bytes_ = 0;
  std::size_t text_bytes_ = 0;
};

class ResponsesProvider final : public ModelProvider {
public:
  ResponsesProvider(ResponsesOptions options, std::string url, TaskScope tasks, ModelHttpOpen open)
      : options_(std::move(options)), url_(std::move(url)), tasks_(tasks), open_(std::move(open)) {}

  ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = options_.tool_calls,
        .structured_output = options_.output_format != ResponsesOutputFormat::Text, .usage = true,
        .multi_tool_calls = options_.tool_calls && options_.multi_tool_calls};
  }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    using Stream = std::unique_ptr<ModelTurnStream>;
    if (request.stop.stop_requested()) co_return Fail<Stream>(ErrorCode::AlreadyClosed, "Model turn cancelled");
    if (request.provider != options_.ref) co_return Fail<Stream>(ErrorCode::Conflict, "Model registration changed");
    if (!request.attachments.empty() || (!options_.tool_calls && !request.tools.empty()) ||
        (request.output_schema && options_.output_format == ResponsesOutputFormat::Text)) {
      co_return Fail<Stream>(ErrorCode::Unsupported, "Requested model capability is unavailable");
    }
    try {
      const ToolNames names(request);
      const auto resume = ReadResume(options_, request);
      const bool store = options_.allow_server_state && request.allow_server_state && !request.run.value.empty();
      const auto previous = store ? std::string_view(resume.previous) : std::string_view{};
      auto opened = co_await Open(request, names, resume.reasoning, store, previous);
      if (!opened.Succeeded() && !previous.empty() &&
          (opened.Error().provider_code == "404" || opened.Error().provider_code == "410")) {
        co_return co_await Open(request, names, resume.reasoning, false, {});
      }
      co_return opened;
    } catch (const std::exception&) {
      co_return Fail<Stream>(ErrorCode::InvalidArgument, "Invalid Responses input or continuation");
    }
  }

private:
  Async<std::unique_ptr<ModelTurnStream>> Open(const ModelTurnRequest& request, const ToolNames& names,
      const ReasoningItems& reasoning, bool store, std::string_view previous) {
    auto post = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!post.Succeeded()) co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Failure(post.Error());
    auto call = std::move(post).Value();
    call.body = ByteString(RequestBody(options_, request, names, reasoning, store, previous).dump());
    auto protocol = std::make_unique<ResponsesProtocol>(options_, request, names, reasoning, store);
    co_return co_await OpenModelStream(std::move(call), tasks_, open_, options_.bearer_token,
        request.stop, std::move(protocol));
  }

  ResponsesOptions options_;
  std::string url_;
  TaskScope tasks_;
  ModelHttpOpen open_;
};

} // namespace

AgentResult<ModelRegistration> MakeResponsesModel(ResponsesOptions options, TaskScope tasks, ModelHttpOpen open) {
  auto url = ModelUrl(options.base_url, bool(options.bearer_token), "/responses");
  if (!url.Succeeded()) return AgentResult<ModelRegistration>::Failure(url.Error());
  if (!options.ref.version || options.ref.id.value.empty() || options.model.empty() ||
      !ValidUtf8(options.model) || options.timeout <= Duration::zero() || !open) {
    return Fail<ModelRegistration>(ErrorCode::InvalidArgument, "Invalid Responses model configuration");
  }
  const bool glm = options.dialect == ResponsesDialect::GlmCompatible;
  if (options.output_format == ResponsesOutputFormat::Automatic) {
    options.output_format = glm ? ResponsesOutputFormat::JsonObject : ResponsesOutputFormat::JsonSchema;
  }
  if ((glm && options.output_format == ResponsesOutputFormat::JsonSchema) || (!glm && options.allow_server_state)) {
    return Fail<ModelRegistration>(ErrorCode::Unsupported, "Unsupported Responses dialect capability");
  }
  options.encrypted_reasoning &= options.dialect == ResponsesDialect::OpenAI;
  ModelRegistration registration{options.ref, {std::string(ProviderName(options.dialect)), options.model, "responses", options.base_url},
      {}, options.allow_server_state, options.allow_persist_resume_state};
  registration.provider = std::make_shared<ResponsesProvider>(std::move(options), std::move(url).Value(), tasks, std::move(open));
  return AgentResult<ModelRegistration>::Success(std::move(registration));
}

} // namespace detail

AgentResult<ModelRegistration> MakeResponsesModel(ResponsesOptions options, std::shared_ptr<HttpClient> client) {
  try {
    auto tasks = UseApplicationTaskScope();
    return detail::MakeResponsesModel(std::move(options), tasks, detail::NativeModelTransport(std::move(client)));
  } catch (const std::logic_error&) {
    return detail::Fail<ModelRegistration>(ErrorCode::InvalidState, "Create the model adapter on the application thread");
  }
}

} // namespace huxerui::agentkit
