#include "gemini_provider_internal.h"

#include "json_wire.h"
#include "runtime_internal.h"

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
constexpr std::size_t max_request = 20'000'000;
constexpr std::string_view legacy_call_prefix = "agentkit:gemini-legacy:";

Json ContentPart(const ModelContent& content, const GeminiMediaParts& media) {
  if (const auto* text = std::get_if<std::string>(&content.body)) return {{"text", *text}};
  if (const auto* value = std::get_if<JsonValue>(&content.body)) return {{"text", ToWireJson(*value).dump()}};
  return media.at(ModelMediaIdentity(std::get<ArtifactRef>(content.body)));
}

Layouts ReadLayouts(const GeminiGenerateContentOptions& options, const ModelTurnRequest& request) {
  Layouts layouts;
  if (!request.continuation) return layouts;
  const auto& continuation = *request.continuation;
  const auto& state = continuation.state;
  if (state.provider != options.ref || state.format_version != resume_format ||
      continuation.prefix_items > request.items.size() || state.opaque.empty() || state.opaque.size() > max_private) {
    throw std::invalid_argument("Continuation does not match this Gemini model");
  }
  const auto* start = reinterpret_cast<const char*>(state.opaque.data());
  const auto decoded = Json::parse(start, start + state.opaque.size());
  if (!decoded.is_object()) throw std::invalid_argument("Invalid Gemini continuation");
  for (auto entry = decoded.begin(); entry != decoded.end(); ++entry) {
    const auto& key = entry.key();
    std::size_t position = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), position);
    if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || !position ||
        position > continuation.prefix_items || !entry->is_array() || entry->empty() ||
        !layouts.emplace(position, *entry).second) {
      throw std::invalid_argument("Gemini continuation is outside its input boundary");
    }
  }
  return layouts;
}

struct Call {
  std::string id;
  std::string alias;
  Json args;
};

Json CallPart(const Call& call) {
  Json function{{"name", call.alias}, {"args", call.args}};
  if (!call.id.starts_with(legacy_call_prefix)) function["id"] = call.id;
  return {{"functionCall", std::move(function)}};
}

Json ModelParts(std::string_view text, std::vector<Call>& calls, const Json* layout) {
  auto parts = Json::array();
  if (!layout) {
    if (!text.empty()) parts.push_back({{"text", text}});
    for (const auto& call : calls) parts.push_back(CallPart(call));
    return parts;
  }
  std::size_t index = 0;
  for (const auto& entry : *layout) {
    Json part;
    if (const auto* bytes = Member(entry, "bytes")) {
      const auto size = Count(*bytes);
      if (size > text.size() || !ValidUtf8(text.substr(0, size))) throw std::invalid_argument("Gemini signed text changed");
      part = {{"text", text.substr(0, size)}};
      text.remove_prefix(size);
    } else if (const auto call_id = String(entry, "call"); !call_id.empty()) {
      const auto alias = String(entry, "name");
      if (index >= calls.size() || calls[index].id != call_id || alias.empty()) {
        throw std::invalid_argument("Gemini signed call changed");
      }
      calls[index].alias = alias;
      part = CallPart(calls[index++]);
    } else if (entry.value("thought", false) && entry.contains("text")) {
      part = {{"thought", true}, {"text", entry.at("text").get<std::string>()}};
    } else throw std::invalid_argument("Invalid Gemini continuation part");
    if (const auto signature = String(entry, "thoughtSignature"); !signature.empty()) part["thoughtSignature"] = signature;
    parts.push_back(std::move(part));
  }
  if (!text.empty() || index != calls.size()) throw std::invalid_argument("Gemini continuation does not cover its model turn");
  return parts;
}

Json ToolResponses(const ModelTurnRequest& request, std::size_t& start, const std::vector<Call>& calls) {
  std::map<std::string, const ModelToolResult*> results;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    const auto* result = start < request.items.size() ? std::get_if<ModelToolResult>(&request.items[start]) : nullptr;
    if (!result || !results.emplace(result->provider_call_id, result).second) {
      throw std::invalid_argument("Gemini tool calls require one result each");
    }
    ++start;
  }
  auto parts = Json::array();
  for (const auto& call : calls) {
    const auto& result = *results.at(call.id);
    auto value = result.error ? Json::parse(ToolContent(result)) :
        Json{{"output", result.value ? ToWireJson(*result.value) : Json(nullptr)}};
    Json response{{"name", call.alias}, {"response", std::move(value)}};
    if (!call.id.starts_with(legacy_call_prefix)) response["id"] = call.id;
    parts.push_back({{"functionResponse", std::move(response)}});
  }
  return parts;
}

Json Contents(const ModelTurnRequest& request, const ToolNames& names, const Layouts& layouts, const GeminiMediaParts& media) {
  auto contents = Json::array();
  std::set<std::string> ids;
  std::size_t layouts_used = 0;
  for (std::size_t i = 0; i < request.items.size();) {
    const auto& item = request.items[i];
    if (std::holds_alternative<ModelToolResult>(item)) throw std::invalid_argument("Unexpected Gemini tool result");
    const auto* content = std::get_if<ModelContent>(&item);
    if (content && content->role != ModelContent::Role::Assistant) {
      if (content->role == ModelContent::Role::Tool) throw std::invalid_argument("Tool output must have a call ID");
      auto part = ContentPart(*content, media);
      if (!part.contains("text") || !String(part, "text").empty()) {
        contents.push_back({{"role", "user"}, {"parts", Json::array({std::move(part)})}});
      }
      ++i;
      continue;
    }
    const auto text = content ? String(ContentPart(*content, media), "text") : std::string{};
    if (content) ++i;
    std::vector<Call> calls;
    while (i < request.items.size()) {
      const auto* call = std::get_if<ModelToolCall>(&request.items[i]);
      if (!call) break;
      auto args = ToWireJson(call->arguments);
      if (call->provider_call_id.empty() || !ids.insert(call->provider_call_id).second || !args.is_object()) {
        throw std::invalid_argument("Invalid Gemini function call history");
      }
      calls.push_back({call->provider_call_id, names.Alias(call->name), std::move(args)});
      ++i;
    }
    const auto layout = layouts.find(i);
    layouts_used += layout != layouts.end();
    auto parts = ModelParts(text, calls, layout == layouts.end() ? nullptr : &layout->second);
    if (!parts.empty()) contents.push_back({{"role", "model"}, {"parts", std::move(parts)}});
    if (!calls.empty()) contents.push_back({{"role", "user"}, {"parts", ToolResponses(request, i, calls)}});
  }
  if (contents.empty() || layouts_used != layouts.size()) throw std::invalid_argument("Gemini input or continuation is incomplete");
  return contents;
}

Json RequestBody(const ModelTurnRequest& request, const ToolNames& names, const Layouts& layouts, const GeminiMediaParts& media) {
  Json body{{"contents", Contents(request, names, layouts, media)}, {"generationConfig", {{"candidateCount", 1}}}};
  auto instructions = ModelInstructions(request);
  if (!instructions.empty()) body["systemInstruction"] = {{"parts", Json::array({{{"text", std::move(instructions)}}})}};
  if (request.output_schema) {
    body["generationConfig"]["responseMimeType"] = "application/json";
    body["generationConfig"]["responseJsonSchema"] = ToWireJson(request.output_schema->document);
  }
  if (!request.tools.empty()) {
    auto declarations = Json::array();
    for (const auto& tool : request.tools) {
      auto parameters = ToWireJson(tool.parameters.document);
      if (parameters == true) parameters = {{"type", "object"}};
      if (!parameters.is_object() || String(parameters, "type") != "object") {
        throw std::invalid_argument("Gemini function parameters must have an object schema");
      }
      declarations.push_back({{"name", names.Alias(tool.name)}, {"description", tool.description},
          {"parametersJsonSchema", std::move(parameters)}});
    }
    body["tools"] = Json::array({{{"functionDeclarations", std::move(declarations)}}});
    if (request.output_schema) body["toolConfig"] = {{"functionCallingConfig", {{"mode", "VALIDATED"}}}};
  }
  return body;
}

class GeminiProtocol final : public SseModelProtocol {
public:
  GeminiProtocol(const GeminiGenerateContentOptions& options, const ModelTurnRequest& request, ToolNames names, Layouts layouts)
      : model_(options.ref), turn_(request.turn), input_count_(request.items.size()),
        structured_(request.output_schema.has_value()), multiple_(options.multi_tool_calls),
        names_(std::move(names)), layouts_(std::move(layouts)) {}

  AgentResult<bool> Accept(SseFrame frame, std::deque<ModelEvent>& events) override {
    try {
      const auto chunk = Json::parse(frame.data);
      if (const auto* failure = Member(chunk, "error")) {
        if (auto context = ModelContextError(*failure)) return AgentResult<bool>::Failure(std::move(*context));
        const auto code = String(*failure, "status") == "RESOURCE_EXHAUSTED" ? ErrorCode::RateLimited : ErrorCode::ProviderFailure;
        return Fail<bool>(code, "Gemini reported a stream failure");
      }
      if (const auto* usage = Member(chunk, "usageMetadata")) ReadUsage(*usage);
      if (const auto* feedback = Member(chunk, "promptFeedback")) {
        const auto blocked = String(*feedback, "blockReason");
        if (!blocked.empty() && blocked != "BLOCK_REASON_UNSPECIFIED") {
          if (candidate_seen_) throw std::invalid_argument("Gemini prompt block arrived after a candidate");
          finish_ = "SAFETY";
        }
      }
      if (const auto* candidates = Member(chunk, "candidates")) {
        if (!candidates->is_array() || candidates->size() > 1) throw std::invalid_argument("Only one Gemini candidate is supported");
        if (!candidates->empty()) Candidate(candidates->front(), events);
      }
      return AgentResult<bool>::Success(false);
    } catch (const std::exception&) {
      return Fail<bool>(ErrorCode::ProtocolError, "Malformed Gemini GenerateContent chunk");
    }
  }

  AgentResult<void> End(std::deque<ModelEvent>& events) override {
    ModelFinishReason reason;
    if (finish_ == "STOP") reason = calls_.empty() ? ModelFinishReason::Completed : ModelFinishReason::ToolCalls;
    else if (finish_ == "MAX_TOKENS") reason = ModelFinishReason::Length;
    else if (finish_ == "SAFETY" || finish_ == "RECITATION" || finish_ == "BLOCKLIST" || finish_ == "PROHIBITED_CONTENT" ||
        finish_ == "SPII" || finish_ == "IMAGE_SAFETY" || finish_ == "IMAGE_PROHIBITED_CONTENT") reason = ModelFinishReason::Filtered;
    else if (finish_.empty() || finish_ == "MALFORMED_FUNCTION_CALL" || finish_ == "UNEXPECTED_TOOL_CALL" ||
        finish_ == "TOO_MANY_TOOL_CALLS" || finish_ == "MISSING_THOUGHT_SIGNATURE") {
      return Fail<void>(ErrorCode::ProtocolError, "Gemini stream did not complete a valid candidate");
    } else return Fail<void>(ErrorCode::ProviderFailure, "Gemini candidate ended without completing");

    if (usage_) {
      events.emplace_back(ModelUsage{{.input_tokens = prompt_tokens_,
          .output_tokens = completion_tokens_ + std::min(thought_tokens_, UINT64_MAX - completion_tokens_)}});
    }
    if (reason != ModelFinishReason::Completed && reason != ModelFinishReason::ToolCalls) {
      events.emplace_back(ModelFinished{reason});
      return AgentResult<void>::Success();
    }
    if (structured_ && calls_.empty()) {
      auto result = ParseJson(text_);
      if (!result.Succeeded()) return Fail<void>(ErrorCode::ProtocolError, "Gemini structured output is invalid JSON");
      events.emplace_back(ModelStructuredOutput{std::move(result).Value()});
    }
    for (const auto& id : calls_) events.emplace_back(ModelToolFinished{id});
    const auto end = input_count_ + (!text_.empty() ? 1 : 0) + calls_.size();
    if (has_signature_ && end > input_count_) layouts_[end] = std::move(layout_);
    std::optional<ModelResumeState> state;
    if (!layouts_.empty()) {
      auto saved = Json::object();
      for (auto& [position, layout] : layouts_) saved[std::to_string(position)] = std::move(layout);
      const auto opaque = saved.dump();
      if (opaque.size() > max_private) return Fail<void>(ErrorCode::ProtocolError, "Gemini continuation exceeds its limit");
      state = ModelResumeState{model_, resume_format, ByteString(opaque)};
    }
    events.emplace_back(ModelFinished{reason, std::move(state)});
    return AgentResult<void>::Success();
  }

private:
  void ReadUsage(const Json& usage) {
    if (const auto* value = Member(usage, "promptTokenCount")) prompt_tokens_ = Count(*value);
    if (const auto* value = Member(usage, "candidatesTokenCount")) completion_tokens_ = Count(*value);
    if (const auto* value = Member(usage, "thoughtsTokenCount")) thought_tokens_ = Count(*value);
    usage_ = true;
  }

  void Candidate(const Json& candidate, std::deque<ModelEvent>& events) {
    candidate_seen_ = true;
    if (const auto* index = Member(candidate, "index"); index && Count(*index) != 0) {
      throw std::invalid_argument("Unexpected Gemini candidate index");
    }
    if (const auto* content = Member(candidate, "content")) {
      if (const auto role = String(*content, "role"); !role.empty() && role != "model") {
        throw std::invalid_argument("Unexpected Gemini candidate role");
      }
      if (const auto* parts = Member(*content, "parts")) {
        if (!parts->is_array() || (!finish_.empty() && !parts->empty())) {
          throw std::invalid_argument("Gemini returned content after its finish reason");
        }
        for (const auto& part : *parts) Part(part, events);
      }
    }
    const auto reason = String(candidate, "finishReason");
    if (!reason.empty() && reason != "FINISH_REASON_UNSPECIFIED") {
      if (!finish_.empty() && finish_ != reason) throw std::invalid_argument("Gemini candidate finish reason changed");
      finish_ = reason;
    }
  }

  void Part(const Json& wire, std::deque<ModelEvent>& events) {
    if (Member(wire, "inlineData") || Member(wire, "fileData") || Member(wire, "toolCall") ||
        Member(wire, "codeExecutionResult") || Member(wire, "executableCode") ||
        Member(wire, "toolResponse") || Member(wire, "functionResponse")) {
      throw std::invalid_argument("Unsupported Gemini output part");
    }
    auto signature = String(wire, "thoughtSignature");
    const bool thought = wire.value("thought", false);
    const auto* text_value = Member(wire, "text");
    const auto* function = Member(wire, "functionCall");
    Json entry;
    if (function && !thought && !text_value) {
      const auto alias = String(*function, "name");
      const auto* tool = names_.Offered(alias);
      auto id = String(*function, "id");
      if (!tool || id.size() > 512 || id.starts_with(legacy_call_prefix) || calls_.size() >= (multiple_ ? 128 : 1)) {
        throw std::invalid_argument("Unexpected Gemini function call");
      }
      if (id.empty()) {
        if (turn_.value.empty()) throw std::invalid_argument("A legacy function call requires a turn ID");
        id = std::string(legacy_call_prefix) + turn_.value + "/" + std::to_string(calls_.size());
      }
      if (!ids_.insert(id).second) throw std::invalid_argument("Duplicate Gemini function ID");
      const auto* args = Member(*function, "args");
      if (args && !args->is_object()) throw std::invalid_argument("Gemini function arguments must be complete objects");
      auto arguments = args ? args->dump() : "{}";
      if (arguments.size() > max_arguments - argument_bytes_) throw std::length_error("Gemini arguments exceed limits");
      argument_bytes_ += arguments.size();
      entry = {{"call", id}, {"name", alias}};
      calls_.push_back(id);
      events.emplace_back(ModelToolStarted{id, *tool});
      events.emplace_back(ModelToolArguments{std::move(id), std::move(arguments)});
    } else if (!function && (text_value || !signature.empty())) {
      auto text = text_value ? text_value->get<std::string>() : std::string{};
      if (thought) entry = {{"thought", true}, {"text", std::move(text)}};
      else {
        if (text.size() > max_text - text_.size()) throw std::length_error("Gemini text exceeds limits");
        entry = {{"bytes", text.size()}};
        text_ += text;
        if (!text.empty()) events.emplace_back(ModelTextDelta{std::move(text), !structured_});
      }
    } else throw std::invalid_argument("Unsupported Gemini output part");
    has_signature_ |= !signature.empty();
    if (!signature.empty()) entry["thoughtSignature"] = std::move(signature);
    const auto size = entry.dump().size();
    if (size > max_private - layout_bytes_) throw std::length_error("Gemini output layout exceeds limits");
    layout_bytes_ += size;
    layout_.push_back(std::move(entry));
  }

  ModelProviderRef model_;
  TurnId turn_;
  std::size_t input_count_;
  bool structured_;
  bool multiple_;
  ToolNames names_;
  Layouts layouts_;
  Json layout_ = Json::array();
  std::vector<std::string> calls_;
  std::set<std::string> ids_;
  std::string text_;
  std::string finish_;
  std::size_t argument_bytes_ = 0;
  std::size_t layout_bytes_ = 0;
  std::uint64_t prompt_tokens_ = 0;
  std::uint64_t completion_tokens_ = 0;
  std::uint64_t thought_tokens_ = 0;
  bool has_signature_ = false;
  bool usage_ = false;
  bool candidate_seen_ = false;
};

class GeminiProvider final : public PositionedModelProvider {
public:
  GeminiProvider(GeminiGenerateContentOptions options, std::string url, TaskScope tasks, ModelHttpOpen open)
      : options_(std::move(options)), url_(std::move(url)), tasks_(tasks), open_(std::move(open)) {}

  ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = options_.tool_calls, .structured_output = options_.structured_output,
        .image_input = options_.image_input, .usage = true, .file_input = options_.file_input,
        .multi_tool_calls = options_.tool_calls && options_.multi_tool_calls};
  }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    using Stream = std::unique_ptr<ModelTurnStream>;
    if (request.stop.stop_requested()) co_return Fail<Stream>(ErrorCode::AlreadyClosed, "Model turn cancelled");
    if (request.provider != options_.ref) co_return Fail<Stream>(ErrorCode::Conflict, "Model registration changed");
    if ((!options_.tool_calls && !request.tools.empty()) || (request.output_schema &&
        (!options_.structured_output || (!request.tools.empty() && !options_.tools_with_structured_output)))) {
      co_return Fail<Stream>(ErrorCode::Unsupported, "Requested Gemini capability is unavailable");
    }
    auto post = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!post.Succeeded()) co_return AgentResult<Stream>::Failure(post.Error());
    auto media = co_await ReadGeminiMedia(request, options_.max_inline_bytes,
        options_.image_input, options_.file_input, GeminiMediaFormat::GenerateContent);
    if (!media.Succeeded()) co_return AgentResult<Stream>::Failure(media.Error());
    post = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!post.Succeeded()) co_return AgentResult<Stream>::Failure(post.Error());
    auto call = std::move(post).Value();
    std::unique_ptr<SseModelProtocol> protocol;
    try {
      ToolNames names(request, true);
      auto layouts = ReadLayouts(options_, request);
      auto body = RequestBody(request, names, layouts, media.Value()).dump();
      if (body.size() > max_request) co_return Fail<Stream>(ErrorCode::BudgetExceeded, "Gemini request exceeds its byte limit");
      call.body = ByteString(body);
      protocol = std::make_unique<GeminiProtocol>(options_, request, std::move(names), std::move(layouts));
    } catch (const std::exception&) {
      co_return Fail<Stream>(ErrorCode::InvalidArgument, "Invalid Gemini input or continuation");
    }
    co_return co_await OpenModelStream(std::move(call), tasks_, open_, options_.api_key, request.stop,
        std::move(protocol), options_.stream_timeouts, ModelCredential::GoogleApiKey);
  }

private:
  GeminiGenerateContentOptions options_;
  std::string url_;
  TaskScope tasks_;
  ModelHttpOpen open_;
};

} // namespace

AgentResult<ModelRegistration> MakeGeminiGenerateContentModel(
    GeminiGenerateContentOptions options, TaskScope tasks, ModelHttpOpen open) {
  constexpr std::string_view prefix = "models/";
  if (options.model.starts_with(prefix)) options.model.erase(0, prefix.size());
  if (!options.ref.version || options.ref.id.value.empty() || options.model.empty() || options.model.size() > 256 ||
      options.model == "." || options.model == ".." || !ValidModelTimeouts(options.timeout, options.stream_timeouts) || !open ||
      !options.max_inline_bytes || options.max_inline_bytes > 14'000'000 ||
      !std::ranges::all_of(options.model, [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '-' || value == '_';
      })) {
    return Fail<ModelRegistration>(ErrorCode::InvalidArgument, "Invalid Gemini GenerateContent model configuration");
  }
  auto url = ModelUrl(options.base_url, bool(options.api_key), "/models/" + options.model + ":streamGenerateContent?alt=sse");
  if (!url.Succeeded()) return AgentResult<ModelRegistration>::Failure(url.Error());
  ModelRegistration registration{options.ref, {"google", options.model, "generate-content", options.base_url},
      {}, false, options.allow_persist_resume_state};
  registration.provider = std::make_shared<GeminiProvider>(std::move(options), std::move(url).Value(), tasks, std::move(open));
  return AgentResult<ModelRegistration>::Success(std::move(registration));
}

} // namespace detail

AgentResult<ModelRegistration> MakeGeminiGenerateContentModel(
    GeminiGenerateContentOptions options, std::shared_ptr<HttpClient> client) {
  try {
    auto tasks = UseApplicationTaskScope();
    return detail::MakeGeminiGenerateContentModel(std::move(options), tasks, detail::NativeModelTransport(std::move(client)));
  } catch (const std::logic_error&) {
    return detail::Fail<ModelRegistration>(ErrorCode::InvalidState, "Create the model adapter on the application thread");
  }
}

} // namespace huxerui::agentkit
