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
constexpr std::size_t max_private = 4 << 20;
constexpr std::size_t max_text = 4 << 20;
constexpr std::size_t max_arguments = 1 << 20;
constexpr std::size_t max_request = 20'000'000;

Json TextContent(std::string_view text) { return {{"type", "text"}, {"text", text}}; }

Json TextStep(std::string_view role, std::string_view text) {
  return {{"type", role}, {"content", Json::array({TextContent(text)})}};
}

struct Resume {
  Layouts layouts;
  std::string interaction;
  std::size_t prefix = 0;
};

Resume ReadResume(const GeminiInteractionsOptions& options, const ModelTurnRequest& request) {
  Resume resume;
  if (!request.continuation) return resume;
  const auto& continuation = *request.continuation;
  const auto& state = continuation.state;
  if (state.provider != options.ref || state.format_version != resume_format ||
      continuation.prefix_items > request.items.size() || state.opaque.empty() || state.opaque.size() > max_private) {
    throw std::invalid_argument("Continuation does not match this Interactions model");
  }
  const auto* begin = reinterpret_cast<const char*>(state.opaque.data());
  const auto document = Json::parse(begin, begin + state.opaque.size());
  if (String(document, "run") != request.run.value) throw std::invalid_argument("Interactions continuation belongs to a different run");
  const auto& layouts = document.at("layouts");
  if (!layouts.is_object()) throw std::invalid_argument("Invalid Interactions continuation");
  for (auto item = layouts.begin(); item != layouts.end(); ++item) {
    const auto& key = item.key();
    std::size_t position = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), position);
    if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || !position ||
        position > continuation.prefix_items || !item->is_array() || item->empty() ||
        !resume.layouts.emplace(position, *item).second) {
      throw std::invalid_argument("Interactions continuation is outside its input boundary");
    }
  }
  if (const auto* link = Member(document, "link")) {
    resume.prefix = Count(link->at("prefix"));
    if (!resume.prefix || resume.prefix != continuation.prefix_items) {
      throw std::invalid_argument("Interactions link does not cover the authorized prefix");
    }
    resume.interaction = String(*link, "id");
    if (resume.interaction.empty() || resume.interaction.size() > 512) throw std::invalid_argument("Invalid Interactions link");
  }
  return resume;
}

struct Call {
  std::string id;
  std::string alias;
  Json arguments;
};

Json FunctionStep(const Call& call) {
  return {{"type", "function_call"}, {"id", call.id}, {"name", call.alias}, {"arguments", call.arguments}};
}

void OutputSteps(Json& steps, std::string_view text, std::vector<Call>& calls, const Json* layout) {
  if (!layout) {
    if (!text.empty()) steps.push_back(TextStep("model_output", text));
    for (const auto& call : calls) steps.push_back(FunctionStep(call));
    return;
  }
  std::size_t index = 0;
  for (const auto& entry : *layout) {
    const auto type = String(entry, "type");
    if (type == "model_output") {
      auto content = Json::array();
      const auto take = [&](std::size_t bytes) {
        if (bytes > text.size() || !ValidUtf8(text.substr(0, bytes))) throw std::invalid_argument("Interactions output text changed");
        content.push_back(TextContent(text.substr(0, bytes)));
        text.remove_prefix(bytes);
      };
      if (const auto* sizes = Member(entry, "content_bytes")) {
        if (!sizes->is_array()) throw std::invalid_argument("Invalid Interactions content layout");
        for (const auto& size : *sizes) take(Count(size));
      } else {
        take(Count(entry.at("bytes")));
      }
      steps.push_back({{"type", "model_output"}, {"content", std::move(content)}});
    } else if (type == "function_call") {
      const auto alias = String(entry, "name");
      if (index >= calls.size() || calls[index].id != String(entry, "id") || alias.empty()) {
        throw std::invalid_argument("Interactions function call changed");
      }
      calls[index].alias = alias;
      steps.push_back(FunctionStep(calls[index++]));
    } else if (type == "thought") {
      steps.push_back(entry);
    } else throw std::invalid_argument("Unsupported Interactions continuation step");
  }
  if (!text.empty() || index != calls.size()) throw std::invalid_argument("Interactions continuation is incomplete");
}

struct Input {
  Json all = Json::array();
  std::optional<std::size_t> suffix;
};

Input MakeInput(const ModelTurnRequest& request, const ToolNames& names, const Resume& resume, const GeminiMediaParts& media) {
  Input input;
  std::map<std::string, std::string> pending;
  std::set<std::string> ids;
  std::size_t layouts_used = 0;
  for (std::size_t i = 0; i < request.items.size();) {
    if (i == resume.prefix) input.suffix = input.all.size();
    const auto& item = request.items[i];
    if (const auto* result = std::get_if<ModelToolResult>(&item)) {
      const auto call = pending.find(result->provider_call_id);
      if (call == pending.end()) throw std::invalid_argument("Interactions tool result has no matching call");
      Json step{{"type", "function_result"}, {"call_id", result->provider_call_id}, {"name", call->second},
          {"result", Json::array({TextContent(ToolContent(*result))})}};
      if (result->error) step["is_error"] = true;
      input.all.push_back(std::move(step));
      pending.erase(call);
      ++i;
      continue;
    }
    if (!pending.empty()) throw std::invalid_argument("Interactions tool calls need their results before other input");
    const auto* content = std::get_if<ModelContent>(&item);
    if (content && content->role != ModelContent::Role::Assistant) {
      if (content->role == ModelContent::Role::Tool) throw std::invalid_argument("Tool content needs a call identity");
      if (const auto* artifact = std::get_if<ArtifactRef>(&content->body)) {
        input.all.push_back({{"type", "user_input"}, {"content", Json::array({media.at(ModelMediaIdentity(*artifact))})}});
      } else {
        const auto* text = std::get_if<std::string>(&content->body);
        auto value = text ? *text : ToWireJson(std::get<JsonValue>(content->body)).dump();
        if (!value.empty()) input.all.push_back(TextStep("user_input", value));
      }
      ++i;
      continue;
    }
    std::string text;
    if (content) {
      if (const auto* value = std::get_if<std::string>(&content->body)) text = *value;
      else text = ToWireJson(std::get<JsonValue>(content->body)).dump();
      ++i;
    }
    std::vector<Call> calls;
    while (i < request.items.size()) {
      const auto* call = std::get_if<ModelToolCall>(&request.items[i]);
      if (!call) break;
      auto arguments = ToWireJson(call->arguments);
      if (call->provider_call_id.empty() || !ids.insert(call->provider_call_id).second || !arguments.is_object()) {
        throw std::invalid_argument("Invalid Interactions function call history");
      }
      calls.push_back({call->provider_call_id, names.Alias(call->name), std::move(arguments)});
      ++i;
    }
    const auto layout = resume.layouts.find(i);
    layouts_used += layout != resume.layouts.end();
    OutputSteps(input.all, text, calls, layout == resume.layouts.end() ? nullptr : &layout->second);
    for (auto& call : calls) pending.emplace(std::move(call.id), std::move(call.alias));
  }
  if (input.all.empty() || !pending.empty() || layouts_used != resume.layouts.size()) {
    throw std::invalid_argument("Interactions input is incomplete");
  }
  return input;
}

Json RequestBody(const GeminiInteractionsOptions& options, const ModelTurnRequest& request,
                 const ToolNames& names, const Input& input, bool store, std::string_view previous) {
  Json body{{"model", options.model}, {"stream", true}, {"store", store}};
  if (!previous.empty()) {
    body["previous_interaction_id"] = previous;
    if (!input.suffix || *input.suffix >= input.all.size()) throw std::invalid_argument("Interactions link requires new authorized input");
    body["input"] = Json::array();
    for (std::size_t i = *input.suffix; i < input.all.size(); ++i) body["input"].push_back(input.all[i]);
  } else body["input"] = input.all;
  auto instructions = ModelInstructions(request);
  if (!instructions.empty()) body["system_instruction"] = std::move(instructions);
  if (request.output_schema) {
    body["response_format"] = {{"type", "text"}, {"mime_type", "application/json"},
        {"schema", ToWireJson(request.output_schema->document)}};
  }
  if (!request.tools.empty()) {
    auto declarations = Json::array();
    for (const auto& tool : request.tools) {
      auto parameters = ToWireJson(tool.parameters.document);
      if (parameters == true) parameters = {{"type", "object"}};
      if (!parameters.is_object() || String(parameters, "type") != "object") {
        throw std::invalid_argument("Interactions function parameters must have an object schema");
      }
      declarations.push_back({{"type", "function"}, {"name", names.Alias(tool.name)}, {"description", tool.description},
          {"parameters", std::move(parameters)}});
    }
    body["tools"] = std::move(declarations);
  }
  return body;
}

enum class StepKind { Output, Thought, Call };

struct Step {
  StepKind kind;
  Json layout;
  std::string arguments;
  bool closed = false;
};

class InteractionsProtocol final : public SseModelProtocol {
public:
  InteractionsProtocol(const GeminiInteractionsOptions& options, const ModelTurnRequest& request,
                       ToolNames names, Layouts layouts, bool store)
      : model_(options.ref), run_(request.run), input_count_(request.items.size()), structured_(request.output_schema.has_value()),
        multiple_(options.multi_tool_calls), store_(store), names_(std::move(names)), layouts_(std::move(layouts)) {}

  AgentResult<bool> Accept(SseFrame frame, std::deque<ModelEvent>& events) override {
    try {
      const auto event = Json::parse(frame.data);
      auto type = String(event, "event_type");
      if (type.empty()) type = String(event, "type");
      if (type.empty() || (frame.type != "message" && frame.type != type)) {
        throw std::invalid_argument("Invalid Interactions event type");
      }
      if (type == "error" || type == "interaction.failed") {
        const auto* fault = Member(event, "error");
        if (!fault) {
          if (const auto* interaction = Member(event, "interaction")) fault = Member(*interaction, "error");
        }
        if (fault) {
          if (auto context = ModelContextError(*fault)) return AgentResult<bool>::Failure(std::move(*context));
        }
        const auto rate_limited = fault && (String(*fault, "code") == "RESOURCE_EXHAUSTED" ||
            String(*fault, "code") == "rate_limit_exceeded");
        return Fail<bool>(rate_limited ? ErrorCode::RateLimited : ErrorCode::ProviderFailure, "Interactions provider reported a stream failure");
      }
      if (type == "interaction.created") {
        if (!interaction_.empty()) throw std::invalid_argument("Interactions stream was created twice");
        interaction_ = String(event.at("interaction"), "id");
        if (interaction_.empty() || interaction_.size() > 512) throw std::invalid_argument("Invalid interaction ID");
      } else if (type == "interaction.status_update" || type == "interaction.in_progress") {
        const auto id = String(event, "interaction_id");
        if (!id.empty() && id != interaction_) throw std::invalid_argument("Interaction ID changed");
      } else if (type == "step.start") {
        Start(event, events);
      } else if (type == "step.delta") {
        Delta(event, events);
      } else if (type == "step.stop") {
        ReadStep(event).closed = true;
      } else if (type == "interaction.completed") {
        return Finish(event.at("interaction"), events);
      }
      return AgentResult<bool>::Success(false);
    } catch (const std::exception&) {
      return Fail<bool>(ErrorCode::ProtocolError, "Malformed Interactions event");
    }
  }

private:
  Step& ReadStep(const Json& event) {
    auto& step = steps_.at(Count(event.at("index")));
    if (step.closed) throw std::invalid_argument("Interaction step already closed");
    return step;
  }

  void PushText(Step& step, std::size_t index, std::string text, std::deque<ModelEvent>& events) {
    if (text.empty()) return;
    if ((last_text_index_ && index < *last_text_index_) || text.size() > max_text - text_.size()) {
      throw std::invalid_argument("Interactions text is out of order or too large");
    }
    last_text_index_ = index;
    step.layout["bytes"] = Count(step.layout.at("bytes")) + text.size();
    if (auto sizes = step.layout.find("content_bytes"); sizes != step.layout.end()) {
      if (sizes->empty()) sizes->push_back(0);
      sizes->back() = Count(sizes->back()) + text.size();
    }
    text_ += text;
    events.emplace_back(ModelTextDelta{std::move(text), !structured_});
  }

  void PushArguments(Step& step, std::string value, std::deque<ModelEvent>& events) {
    if (value.empty()) return;
    if (value.size() > max_arguments - argument_bytes_) throw std::length_error("Interactions arguments exceed limits");
    argument_bytes_ += value.size();
    step.arguments += value;
    events.emplace_back(ModelToolArguments{String(step.layout, "id"), std::move(value)});
  }

  void CheckPrivate(const Json& value) {
    const auto bytes = value.dump().size();
    if (bytes > max_private - private_bytes_) throw std::length_error("Interactions private step exceeds limits");
    private_bytes_ += bytes;
  }

  void Start(const Json& event, std::deque<ModelEvent>& events) {
    const auto index = Count(event.at("index"));
    if (interaction_.empty() || index >= 512 || (!steps_.empty() && index <= steps_.rbegin()->first)) {
      throw std::invalid_argument("Unexpected interaction step index");
    }
    const auto& wire = event.at("step");
    const auto type = String(wire, "type");
    if (type == "model_output") {
      auto& step = steps_.emplace(index, Step{StepKind::Output, {{"type", type}, {"bytes", 0}}}).first->second;
      if (const auto* content = Member(wire, "content")) {
        if (!content->is_array() || content->size() > 512) throw std::invalid_argument("Invalid Interactions output content");
        auto& sizes = step.layout["content_bytes"] = Json::array();
        for (const auto& item : *content) {
          if (String(item, "type") != "text") throw std::invalid_argument("Interactions output modality is unavailable");
          sizes.push_back(0);
          PushText(step, index, item.at("text").get<std::string>(), events);
        }
      }
    } else if (type == "thought") {
      CheckPrivate(wire);
      steps_.emplace(index, Step{StepKind::Thought, wire});
    } else if (type == "function_call") {
      const auto id = String(wire, "id");
      const auto alias = String(wire, "name");
      const auto* name = names_.Offered(alias);
      if (!name || id.empty() || id.size() > 512 || calls_.size() >= (multiple_ ? 128 : 1) || !calls_.insert(id).second) {
        throw std::invalid_argument("Unexpected Interactions function call");
      }
      auto& step = steps_.emplace(index, Step{StepKind::Call, {{"type", type}, {"id", id}, {"name", alias}}}).first->second;
      events.emplace_back(ModelToolStarted{id, *name});
      if (const auto* arguments = Member(wire, "arguments")) {
        if (!arguments->is_object()) throw std::invalid_argument("Invalid initial Interactions arguments");
        if (!arguments->empty()) PushArguments(step, arguments->dump(), events);
      }
    } else throw std::invalid_argument("Unsupported Interactions step");
  }

  void Delta(const Json& event, std::deque<ModelEvent>& events) {
    auto& step = ReadStep(event);
    const auto& delta = event.at("delta");
    const auto type = String(delta, "type");
    if (step.kind == StepKind::Output && type == "text") {
      PushText(step, Count(event.at("index")), String(delta, "text"), events);
    } else if (step.kind == StepKind::Call && type == "arguments_delta") {
      PushArguments(step, String(delta, "arguments"), events);
    } else if (step.kind == StepKind::Thought && type == "thought_signature") {
      const auto& signature = delta.at("signature");
      CheckPrivate(signature);
      if (signature.get_ref<const std::string&>().empty() || !String(step.layout, "signature").empty()) {
        throw std::invalid_argument("Invalid Interactions thought signature");
      }
      step.layout["signature"] = signature;
    } else if (step.kind == StepKind::Thought && type == "thought_summary") {
      const auto& content = delta.at("content");
      CheckPrivate(content);
      if (!content.is_object()) throw std::invalid_argument("Invalid Interactions thought summary");
      auto& summary = step.layout["summary"];
      if (summary.is_null()) summary = Json::array();
      summary.push_back(content);
    } else throw std::invalid_argument("Unexpected Interactions step delta");
  }

  AgentResult<bool> Finish(const Json& interaction, std::deque<ModelEvent>& events) {
    if (interaction_.empty() || String(interaction, "id") != interaction_) {
      return Fail<bool>(ErrorCode::ProtocolError, "Interaction identity did not match");
    }
    if (const auto* usage = Member(interaction, "usage")) {
      const auto count = [&](const char* key) {
        const auto* value = Member(*usage, key);
        return value ? Count(*value) : std::uint64_t{0};
      };
      const auto output = count("total_output_tokens");
      const auto thoughts = count("total_thought_tokens");
      events.emplace_back(ModelUsage{{.input_tokens = count("total_input_tokens"),
          .output_tokens = output + std::min(thoughts, UINT64_MAX - output)}});
    }
    const auto status = String(interaction, "status");
    if (status == "incomplete") {
      events.emplace_back(ModelFinished{ModelFinishReason::Length});
      return AgentResult<bool>::Success(true);
    }
    if (status == "failed" || status == "cancelled" || status == "requires_continuation") {
      return Fail<bool>(ErrorCode::ProviderFailure, "Interaction stopped before completing the foreground request");
    }
    const bool awaiting_tools = status == "requires_action";
    if ((!awaiting_tools && status != "completed") || awaiting_tools != !calls_.empty() ||
        std::ranges::any_of(steps_, [](const auto& entry) { return !entry.second.closed; })) {
      return Fail<bool>(ErrorCode::ProtocolError, "Interaction finished with incomplete steps");
    }
    std::size_t outputs = 0;
    bool replay_layout = store_ && awaiting_tools;
    bool preceding_call = false;
    for (auto& [index, step] : steps_) {
      if (step.kind == StepKind::Thought) replay_layout = true;
      if (step.kind == StepKind::Output) {
        const auto* sizes = Member(step.layout, "content_bytes");
        replay_layout |= ++outputs > 1 || preceding_call || (sizes && sizes->size() != 1) || !Count(step.layout.at("bytes"));
      }
      if (step.kind != StepKind::Call) continue;
      preceding_call = true;
      if (step.arguments.empty()) PushArguments(step, "{}", events);
      if (!Json::parse(step.arguments, nullptr, false).is_object()) {
        return Fail<bool>(ErrorCode::ProtocolError, "Interaction returned incomplete function arguments");
      }
      events.emplace_back(ModelToolFinished{String(step.layout, "id")});
    }
    if (structured_ && !awaiting_tools) {
      auto value = ParseJson(text_);
      if (!value.Succeeded()) return Fail<bool>(ErrorCode::ProtocolError, "Interactions structured output is invalid JSON");
      events.emplace_back(ModelStructuredOutput{std::move(value).Value()});
    }
    const auto end = input_count_ + (!text_.empty() ? 1 : 0) + calls_.size();
    if (replay_layout && end > input_count_) {
      auto layout = Json::array();
      for (auto& [index, step] : steps_) layout.push_back(std::move(step.layout));
      layouts_[end] = std::move(layout);
    }
    std::optional<ModelResumeState> resume;
    if (store_ || !layouts_.empty()) {
      auto layouts = Json::object();
      for (auto& [position, layout] : layouts_) layouts[std::to_string(position)] = std::move(layout);
      Json data{{"run", run_.value}, {"layouts", std::move(layouts)}};
      if (store_) data["link"] = {{"id", interaction_}, {"prefix", end}};
      const auto encoded = data.dump();
      if (encoded.size() > max_private) return Fail<bool>(ErrorCode::ProtocolError, "Interactions continuation exceeds its limit");
      resume = ModelResumeState{model_, resume_format, ByteString(encoded)};
    }
    events.emplace_back(ModelFinished{awaiting_tools ? ModelFinishReason::ToolCalls : ModelFinishReason::Completed, std::move(resume)});
    return AgentResult<bool>::Success(true);
  }

  ModelProviderRef model_;
  RunId run_;
  std::size_t input_count_;
  bool structured_;
  bool multiple_;
  bool store_;
  ToolNames names_;
  Layouts layouts_;
  std::string interaction_;
  std::map<std::size_t, Step> steps_;
  std::set<std::string> calls_;
  std::optional<std::size_t> last_text_index_;
  std::string text_;
  std::size_t argument_bytes_ = 0;
  std::size_t private_bytes_ = 0;
};

class InteractionsProvider final : public PositionedModelProvider {
public:
  InteractionsProvider(GeminiInteractionsOptions options, std::string url, TaskScope tasks, ModelHttpOpen open)
      : options_(std::move(options)), url_(std::move(url)), tasks_(tasks), open_(std::move(open)) {}

  Layout ResumeLayout() const noexcept override { return Layout::Interactions; }

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
      co_return Fail<Stream>(ErrorCode::Unsupported, "Requested Interactions capability is unavailable");
    }
    const auto deadline = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!deadline.Succeeded()) co_return AgentResult<Stream>::Failure(deadline.Error());
    auto media = co_await ReadGeminiMedia(request, options_.max_inline_bytes,
        options_.image_input, options_.file_input, GeminiMediaFormat::Interactions);
    if (!media.Succeeded()) co_return AgentResult<Stream>::Failure(media.Error());
    try {
      ToolNames names(request, true);
      auto resume = ReadResume(options_, request);
      auto input = MakeInput(request, names, resume, media.Value());
      const bool store = options_.allow_server_state && request.allow_server_state && !request.run.value.empty();
      std::string_view previous = store ? std::string_view(resume.interaction) : std::string_view{};
      auto opened = co_await Open(request, names, resume.layouts, input, store, previous);
      if (!opened.Succeeded() && !previous.empty() && (opened.Error().provider_code == "404" || opened.Error().provider_code == "410")) {
        co_return co_await Open(request, names, resume.layouts, input, false, {});
      }
      co_return opened;
    } catch (const std::exception&) {
      co_return Fail<Stream>(ErrorCode::InvalidArgument, "Invalid Interactions input or continuation");
    }
  }

private:
  Async<std::unique_ptr<ModelTurnStream>> Open(const ModelTurnRequest& request, const ToolNames& names,
      const Layouts& layouts, const Input& input, bool store, std::string_view previous) {
    using Stream = std::unique_ptr<ModelTurnStream>;
    auto post = MakeModelPost(url_, options_.timeout, request.deadline);
    if (!post.Succeeded()) co_return AgentResult<Stream>::Failure(post.Error());
    auto call = std::move(post).Value();
    auto body = RequestBody(options_, request, names, input, store, previous).dump();
    if (body.size() > max_request) co_return Fail<Stream>(ErrorCode::BudgetExceeded, "Interactions request exceeds its byte limit");
    call.body = ByteString(body);
    auto protocol = std::make_unique<InteractionsProtocol>(options_, request, names, layouts, store);
    co_return co_await OpenModelStream(std::move(call), tasks_, open_, options_.api_key,
        request.stop, std::move(protocol), options_.stream_timeouts, ModelCredential::GoogleApiKey);
  }

  GeminiInteractionsOptions options_;
  std::string url_;
  TaskScope tasks_;
  ModelHttpOpen open_;
};

} // namespace

AgentResult<ModelRegistration> MakeGeminiInteractionsModel(GeminiInteractionsOptions options, TaskScope tasks, ModelHttpOpen open) {
  if (options.model.starts_with("models/")) options.model.erase(0, std::string_view("models/").size());
  auto url = ModelUrl(options.base_url, bool(options.api_key), "/interactions");
  if (!url.Succeeded()) return AgentResult<ModelRegistration>::Failure(url.Error());
  if (!options.ref.version || options.ref.id.value.empty() || options.model.empty() || options.model.size() > 256 ||
      !ValidUtf8(options.model) || !ValidModelTimeouts(options.timeout, options.stream_timeouts) || !open ||
      !options.max_inline_bytes || options.max_inline_bytes > 14'000'000) {
    return Fail<ModelRegistration>(ErrorCode::InvalidArgument, "Invalid Gemini Interactions model configuration");
  }
  ModelRegistration registration{options.ref, {"google", options.model, "interactions", options.base_url},
      {}, options.allow_server_state, options.allow_persist_resume_state};
  registration.provider = std::make_shared<InteractionsProvider>(std::move(options), std::move(url).Value(), tasks, std::move(open));
  return AgentResult<ModelRegistration>::Success(std::move(registration));
}

} // namespace detail

AgentResult<ModelRegistration> MakeGeminiInteractionsModel(
    GeminiInteractionsOptions options, std::shared_ptr<HttpClient> client) {
  try {
    auto tasks = UseApplicationTaskScope();
    return detail::MakeGeminiInteractionsModel(std::move(options), tasks, detail::NativeModelTransport(std::move(client)));
  } catch (const std::logic_error&) {
    return detail::Fail<ModelRegistration>(ErrorCode::InvalidState, "Create the model adapter on the application thread");
  }
}

} // namespace huxerui::agentkit
