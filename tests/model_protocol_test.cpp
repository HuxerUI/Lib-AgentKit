#include "model_provider_internal.h"
#include "json_wire.h"
#include "preview_store.h"

#include <huxerui/agentkit/runtime.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ranges>

namespace {
using namespace std::chrono_literals;
using namespace huxerui;
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void CheckStructuredDeltas(const std::vector<kit::ModelEvent>& events) {
  std::string payload;
  for (const auto& event : events) {
    if (const auto* delta = std::get_if<kit::ModelTextDelta>(&event)) {
      Check(!delta->show_in_transcript, "structured wire text was marked for display");
      payload += delta->utf8;
    }
  }
  Check(payload == R"({"title":"Release"})", "structured wire text was not preserved for the host");
}

void AddFixtureSkill(kit::ModelTurnRequest& request) {
  const kit::SkillRef ref{{"test.packaged"}, {"release-brief"}, 9};
  request.available_skills.push_back({ref, "Release brief", "Use approved release evidence"});
  request.skill_instructions.push_back({ref, "Inspect the task"});
}

bool HasFixtureSkill(const Json& field) {
  const auto text = field.get<std::string>();
  return text.starts_with("Use the configured tools\n\nAvailable Skill metadata.") &&
      text.find("\"version\":\"9\"") != std::string::npos &&
      text.find("\"description\":\"Use approved release evidence\"") != std::string::npos &&
      text.find("Active Skill guidance.") != std::string::npos &&
      text.find("\"markdown\":\"Inspect the task\"") != std::string::npos;
}

template<class T>
T Take(kit::AgentResult<T> value) {
  if (!value.Succeeded()) throw std::runtime_error(value.Error().message);
  return std::move(value).Value();
}

template<class T>
void ErrorIs(const kit::AgentResult<T>& value, kit::ErrorCode expected) {
  if (value.Succeeded() || value.Error().code != expected) {
    throw std::runtime_error(value.Succeeded() ? "Operation unexpectedly succeeded" : value.Error().message);
  }
}

struct Script {
  std::function<std::string(const HttpRequest&, const Json&)> reply;
  int status = 200;
  int requests = 0;
  std::size_t fragment_bytes = 7;
  std::size_t read_calls = 0;
  bool hold = false;
  bool read_cancelled = false;

  kit::detail::ModelHttpOpen Open() {
    return [this](HttpRequest request) -> kit::Async<kit::detail::ModelHttpResponse> {
      ++requests;
      const auto* begin = reinterpret_cast<const char*>(request.body.data());
      auto syntax = Json::parse(begin, begin + request.body.size());
      auto fixture = reply ? reply(request, syntax) : std::string{};
      co_return kit::AgentResult<kit::detail::ModelHttpResponse>::Success({
          .status = status, .headers = {{"Content-Type", "text/event-stream; charset=utf-8"}},
          .read = [this, fixture = std::move(fixture), pos = std::size_t{0}](std::size_t maximum) mutable
              -> Task<IoResult<Bytes>> {
            ++read_calls;
            if (hold) {
              auto cancel = [this](CallbackCompletion<IoResult<Bytes>>) {
                return [this] { read_cancelled = true; };
              };
              co_return co_await AwaitCallback<IoResult<Bytes>>(std::move(cancel));
            }
            auto length = std::min({maximum, fragment_bytes, fixture.size() - pos});
            Bytes data(length);
            if (length) std::memcpy(data.data(), fixture.data() + pos, length);
            pos += length;
            co_return IoResult<Bytes>::Success(std::move(data));
          }});
    };
  }
};

kit::ModelProviderRef Ref() { return {{"protocol.fixture"}, 2}; }

kit::ChatCompletionsOptions Options(kit::ChatCompletionsDialect dialect = kit::ChatCompletionsDialect::OpenAI) {
  return {.ref = Ref(), .model = "fixture-model", .base_url = "https://provider.example/v1/", .dialect = dialect};
}

kit::ModelTurnRequest Request() {
  return {.turn = {"fixture-turn"}, .run = {"fixture-run"}, .provider = Ref(),
          .instructions = "Use the configured tools", .items = {
              kit::ModelContent{kit::ModelContent::Role::User, std::string("Hello")}}};
}

std::string Frame(const Json& json, std::string_view ending = "\r\n") {
  return "data: " + json.dump() + std::string(ending) + std::string(ending);
}

Json Choice(Json delta, Json reason = nullptr) {
  return {{"choices", Json::array({Json{{"index", 0}, {"delta", std::move(delta)}, {"finish_reason", std::move(reason)}}})}};
}

Json ToolFragment(unsigned index, std::string arguments, std::string id = {}, std::string name = {}) {
  Json delta{{"index", index}, {"function", {{"arguments", std::move(arguments)}}}};
  if (!id.empty()) {
    delta["id"] = std::move(id);
    delta["type"] = "function";
  }
  if (!name.empty()) delta["function"]["name"] = std::move(name);
  return delta;
}

Task<std::vector<kit::ModelEvent>> Collect(kit::ModelTurnStream& stream) {
  std::vector<kit::ModelEvent> events;
  while (auto event = Take(co_await stream.ReadAsync())) events.push_back(std::move(*event));
  co_return events;
}

Task<void> ToolProtocol(TaskScope tasks) {
  Script server;
  const std::string utf8 = "Ready \xe6\xb5\x8b\xe8\xaf\x95";
  server.reply = [&](const HttpRequest& request, const Json& body) {
    Check(request.url == "https://provider.example/v1/chat/completions" && request.method == HttpMethod::Post,
          "HTTP request used configured path and POST");
    Check(std::ranges::find(request.headers, HttpHeader{"Authorization", "Bearer fixture-credential"}) != request.headers.end(),
          "bearer credential applied at transport");
    Check(body["model"] == "fixture-model" && body["stream"] == true && body["store"] == false &&
          body["stream_options"]["include_usage"] == true && body["messages"][0]["role"] == "developer",
          "request preserves pinned model options");
    if (server.requests == 1) {
      auto name = body["tools"][0]["function"]["name"].get<std::string>();
      Check(name != "agentkit_tool_1" && name.find('.') == std::string::npos &&
            body["tools"][1]["function"]["name"] == "agentkit_tool_1", "aliases are provider-safe and collision-free");
      return "\xef\xbb\xbf: live\r\n\r\n" + Frame(Choice({{"content", utf8}})) +
          Frame(Choice({{"tool_calls", Json::array({
              ToolFragment(1, "{}", "wire-b", "agentkit_tool_1"),
              ToolFragment(0, "{\"value\":", "wire-a", name)})}}), "\r") +
          Frame(Choice({{"tool_calls", Json::array({ToolFragment(0, "42}")})}})) +
          Frame(Choice(Json::object(), "tool_calls")) +
          "data: {\"choices\":[],\r\ndata: \"usage\":{\"prompt_tokens\":13,\"completion_tokens\":7}}\r\n\r\n"
          "data: [DONE]\r\n\r\n";
    }
    const auto& messages = body["messages"];
    Check(messages.size() == 5 && messages[2]["content"] == utf8 &&
          messages[2]["tool_calls"][0]["id"] == "wire-a" && messages[2]["tool_calls"][1]["id"] == "wire-b",
          "next turn groups assistant text with both original calls");
    Check(messages[3]["role"] == "tool" && messages[3]["tool_call_id"] == "wire-a" &&
          Json::parse(messages[3]["content"].get<std::string>())["value"] == 42 &&
          messages[4]["tool_call_id"] == "wire-b" && Json::parse(messages[4]["content"].get<std::string>())["ok"] == false,
          "tool replies preserve IDs, values, and errors");
    return Frame(Choice({{"content", "Done"}})) + Frame(Choice(Json::object(), "stop")) + "data: [DONE]\n\n";
  };
  auto options = Options();
  options.request_usage = true;
  options.developer_role = true;
  options.bearer_token = []() -> kit::Async<std::string> {
    co_return kit::AgentResult<std::string>::Success("fixture-credential");
  };
  auto model = Take(kit::detail::MakeChatCompletionsModel(std::move(options), tasks, server.Open()));
  auto request = Request();
  request.tools = {{{{"preview.confirm"}, 1, {}}, "preview.confirm", "A write",
                    {kit::JsonValue{kit::JsonValue::Object{{"type", kit::JsonValue{std::string("object")}}}}}},
                   {{{"test.alt"}, 1, {}}, "agentkit_tool_1", "An alternative", {kit::JsonValue{true}}}};
  auto stream = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*stream);
  Check(events.size() == 10 && std::get<kit::ModelTextDelta>(events.front()).utf8 == utf8 &&
        std::get<kit::ModelToolStarted>(events[1]).name == "preview.confirm" &&
        std::get<kit::ModelToolStarted>(events[2]).provider_call_id == "wire-b" &&
        std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::ToolCalls,
        "fragmented SSE yielded assistant text, indexed calls, and tools finish");
  std::string arguments;
  std::uint64_t tokens = 0;
  for (const auto& event : events) {
    if (const auto* part = std::get_if<kit::ModelToolArguments>(&event); part && part->provider_call_id == "wire-a") {
      arguments += part->bytes;
    }
    if (const auto* usage = std::get_if<kit::ModelUsage>(&event)) tokens = usage->usage.output_tokens;
  }
  Check(arguments == "{\"value\":42}" && tokens == 7, "argument fragments and final usage preserved");

  auto value = kit::detail::FromWireJson(Json{{"value", 42}});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Assistant, utf8});
  request.items.push_back(kit::ModelToolCall{"wire-a", "preview.confirm", value});
  request.items.push_back(kit::ModelToolCall{"wire-b", "agentkit_tool_1", kit::JsonValue{kit::JsonValue::Object{}}});
  request.items.push_back(kit::ModelToolResult{"wire-a", kit::ToolCallId{"local-a"}, value, {}});
  request.items.push_back(kit::ModelToolResult{"wire-b", {}, {}, kit::AgentError{
      .code = kit::ErrorCode::PermissionDenied, .message = "Tool denied"}});
  auto next = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  auto continued = co_await Collect(*next);
  Check(std::get<kit::ModelTextDelta>(continued.front()).utf8 == "Done", "tool loop produced final response");
}

Task<void> PrivateState(TaskScope tasks) {
  Script server;
  server.reply = [&](const HttpRequest&, const Json& body) {
    if (server.requests == 1) {
      return Frame(Choice({{"reasoning_content", "test opaque value"}})) +
          Frame(Choice({{"tool_calls", Json::array({ToolFragment(0, "{}", "deep-call", "inspect")})}})) +
          Frame(Choice(Json::object(), "tool_calls")) + "data: [DONE]\n\n";
    }
    Check(body["messages"][2]["reasoning_content"] == "test opaque value" &&
          body["messages"][3]["tool_call_id"] == "deep-call", "DeepSeek private continuation returned to original assistant turn");
    return Frame(Choice({{"content", "Visible result"}})) + Frame(Choice(Json::object(), "stop")) + "data: [DONE]\n\n";
  };
  auto model = Take(kit::detail::MakeChatCompletionsModel(
      Options(kit::ChatCompletionsDialect::DeepSeek), tasks, server.Open()));
  auto request = Request();
  request.tools = {{{{"test.inspect"}, 1, {}}, "inspect", "Inspect", {kit::JsonValue{true}}}};
  auto stream = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*stream);
  Check(std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelTextDelta>(event); }),
        "private continuation must never appear as assistant text");
  auto finish = std::get<kit::ModelFinished>(std::move(events.back()));
  Check(finish.resume && finish.resume->provider == Ref() && model.allow_persist_resume_state == false,
        "private continuation is bound to model and persistence is opt-in");
  request.items.push_back(kit::ModelToolCall{"deep-call", "inspect", kit::JsonValue{kit::JsonValue::Object{}}});
  request.items.push_back(kit::ModelToolResult{"deep-call", {}, kit::JsonValue{true}, {}});
  request.continuation = kit::ModelContinuation{std::move(*finish.resume), 2};
  auto next = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  auto continued = co_await Collect(*next);
  Check(std::get<kit::ModelTextDelta>(continued.front()).utf8 == "Visible result", "continued model reply was visible");
}

Task<void> StructuredOutput(TaskScope tasks, kit::ChatOutputFormat output_format) {
  const Json schema{{"type", "object"}, {"properties", {{"title", {{"type", "string"}}}}},
      {"required", Json::array({"title"})}, {"additionalProperties", false}};
  Script server;
  server.reply = [&](const HttpRequest&, const Json& body) {
    const auto& format = body["response_format"];
    if (output_format == kit::ChatOutputFormat::JsonObject) {
      Check(format["type"] == "json_object" &&
          body["messages"][0]["content"].get<std::string>().ends_with(schema.dump()),
          "compatible JSON mode included the host schema in its instructions");
    } else {
      Check(format["type"] == "json_schema" && format["json_schema"]["strict"] == true &&
          format["json_schema"]["schema"] == schema, "host schema reached provider");
    }
    return Frame(Choice({{"content", "{\"title\":"}})) + Frame(Choice({{"content", "\"Release\"}"}})) +
        Frame(Choice(Json::object(), "stop")) + "data: [DONE]\n\n";
  };
  auto options = Options();
  options.output_format = output_format;
  auto model = Take(kit::detail::MakeChatCompletionsModel(std::move(options), tasks, server.Open()));
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)};
  auto stream = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  auto events = co_await Collect(*stream);
  CheckStructuredDeltas(events);
  auto result = std::ranges::find_if(events, [](const auto& event) {
    return std::holds_alternative<kit::ModelStructuredOutput>(event);
  });
  Check(result != events.end() && kit::detail::ToWireJson(std::get<kit::ModelStructuredOutput>(*result).value)["title"] == "Release",
        "streamed JSON produced a typed result at completion");
}

Task<void> VerifyCancellation(TaskScope tasks, Script& server, kit::ModelProvider& provider) {
  server.hold = true;
  const auto prior = server.read_calls;
  auto stream = std::shared_ptr<kit::ModelTurnStream>(Take(co_await provider.StartTurnAsync(Request())));
  auto result = std::make_shared<std::optional<kit::AgentResult<std::optional<kit::ModelEvent>>>>();
  tasks.Launch([stream, result]() -> Task<void> { result->emplace(co_await stream->ReadAsync()); });
  for (int attempt = 0; server.read_calls == prior && attempt < 100; ++attempt) co_await Delay(1ms);
  Check(server.read_calls > prior, "a model read was in flight before cancellation");
  Check(Take(co_await stream->StopAsync()) == kit::ModelStopOutcome::LocalStopped, "cancel reports local stop");
  for (int attempt = 0; (!*result || !server.read_cancelled) && attempt < 100; ++attempt) co_await Delay(1ms);
  Check(*result && !Take(std::move(**result)) && server.read_cancelled && !Take(co_await stream->ReadAsync()),
        "cancel released the reader and suppressed pending model data");
}

Task<void> ErrorsAndCancellation(TaskScope tasks) {
  Script server;
  auto model = Take(kit::detail::MakeChatCompletionsModel(Options(), tasks, server.Open()));
  server.status = 429;
  ErrorIs(co_await model.provider->StartTurnAsync(Request()), kit::ErrorCode::RateLimited);
  server.status = 200;
  server.reply = [](const auto&, const auto&) {
    return Frame(Choice({{"content", "Partial output"}})) + Frame(Choice(Json::object(), "stop"));
  };
  auto truncated = Take(co_await model.provider->StartTurnAsync(Request()));
  Check(std::holds_alternative<kit::ModelTextDelta>(*Take(co_await truncated->ReadAsync())), "partial reply was delivered");
  ErrorIs(co_await truncated->ReadAsync(), kit::ErrorCode::ProtocolError);
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::JsonValue{true}};
  ErrorIs(co_await model.provider->StartTurnAsync(std::move(request)), kit::ErrorCode::Unsupported);
  Check(server.requests == 2, "unsupported input failed before network access");

  co_await VerifyCancellation(tasks, server, *model.provider);
}

kit::ResponsesOptions ResponsesOptions() {
  return {.ref = Ref(), .model = "fixture-model", .base_url = "https://provider.example/v1/"};
}

std::string ResponseFrame(std::string type, Json body) {
  body["type"] = type;
  return "event: " + type + "\n" + Frame(body, "\n");
}

std::string ResponseItem(std::string phase, unsigned index, const Json& item) {
  return ResponseFrame("response.output_item." + phase, {{"output_index", index}, {"item", item}});
}

Json ResponseMessage(std::string text, bool refused = false) {
  return {{"type", "message"}, {"id", "msg_1"}, {"role", "assistant"}, {"status", "completed"},
      {"content", Json::array({{{"type", refused ? "refusal" : "output_text"},
          {refused ? "refusal" : "text", std::move(text)}}})}};
}

std::string ResponseText(std::string_view delta, unsigned index = 0, bool refused = false) {
  return ResponseFrame(refused ? "response.refusal.delta" : "response.output_text.delta",
      {{"item_id", "msg_1"}, {"output_index", index}, {"content_index", 0}, {"delta", delta}});
}

std::string ResponseCompleted(Json output, std::string_view id = {}) {
  Json response{{"status", "completed"}, {"output", std::move(output)},
      {"usage", {{"input_tokens", 13}, {"output_tokens", 7}}}};
  if (!id.empty()) response["id"] = id;
  return ResponseFrame("response.completed", {{"response", std::move(response)}});
}

Task<void> ResponsesToolProtocol(TaskScope tasks) {
  const std::string display = "\xe5\xae\xa1\xe9\x98\x85";
  const Json reasoning{{"type", "reasoning"}, {"id", "rs_1"}, {"summary", Json::array()},
      {"encrypted_content", "encrypted-test-value"}};
  const Json first{{"type", "function_call"}, {"id", "fc_9"}, {"call_id", "call_a"},
      {"name", "agentkit_tool_2"}, {"arguments", "{\"value\":42}"}, {"status", "completed"}};
  const Json second{{"type", "function_call"}, {"id", "fc_2"}, {"call_id", "call_b"},
      {"name", "agentkit_tool_1"}, {"arguments", "{}"}, {"status", "completed"}};
  Script server;
  server.reply = [&](const HttpRequest& request, const Json& body) {
    Check(request.url == "https://provider.example/v1/responses" && request.method == HttpMethod::Post &&
        body["stream"] == true && body["store"] == false && !body.contains("previous_response_id"),
        "Responses uses the configured stateless endpoint");
    Check(body["include"] == Json::array({"reasoning.encrypted_content"}) &&
        body["tools"][0]["name"] == "agentkit_tool_2" && body["tools"][0]["strict"] == false &&
        body["tools"][1]["name"] == "agentkit_tool_1" && body["parallel_tool_calls"] == true,
        "Responses preserved host schemas and collision-free tool aliases");
    const auto& input = body["input"];
    Check(input[0]["content"] == "Hello" && HasFixtureSkill(body["instructions"]),
        "Responses sent authorized input and activated Skill instructions");
    if (server.requests == 1) {
      auto first_pending = first;
      first_pending["arguments"] = "";
      first_pending["status"] = "in_progress";
      return ResponseItem("added", 0, reasoning) + ResponseItem("added", 1, ResponseMessage("")) +
          ResponseText(display, 1) + ResponseItem("done", 1, ResponseMessage(display)) +
          ResponseItem("added", 2, first_pending) + ResponseItem("added", 3, second) +
          ResponseFrame("response.function_call_arguments.delta", {{"item_id", "fc_9"}, {"output_index", 2}, {"delta", "{\"value\":"}}) +
          ResponseFrame("response.function_call_arguments.delta", {{"item_id", "fc_9"}, {"output_index", 2}, {"delta", "42}"}}) +
          ResponseFrame("response.function_call_arguments.done", {{"item_id", "fc_9"}, {"output_index", 2}, {"arguments", "{\"value\":42}"}}) +
          ResponseItem("done", 2, first) + ResponseItem("done", 3, second) +
          ResponseCompleted(Json::array({reasoning, ResponseMessage(display), first, second}));
    }
    const std::size_t text_index = server.requests == 2 ? 2 : 1;
    if (server.requests == 2) Check(input[1] == reasoning, "stateless continuation restores only the encrypted reasoning item");
    Check(input.size() == text_index + 5 && input[text_index]["role"] == "assistant" &&
        input[text_index]["content"] == display, "the authorized local transcript was replayed once");
    Check(input[text_index + 1]["call_id"] == "call_a" && input[text_index + 2]["call_id"] == "call_b" &&
        input[text_index + 3]["type"] == "function_call_output" && input[text_index + 3]["call_id"] == "call_a" &&
        input[text_index + 4]["call_id"] == "call_b" &&
        Json::parse(input[text_index + 3]["output"].get<std::string>())["value"] == 42 &&
        Json::parse(input[text_index + 4]["output"].get<std::string>())["ok"] == false,
        "Responses sent results by call_id and preserved failed-tool data");
    return ResponseItem("added", 0, ResponseMessage("")) + ResponseText("Done") +
        ResponseCompleted(Json::array({ResponseMessage("Done")}));
  };
  const auto model = Take(kit::detail::MakeResponsesModel(ResponsesOptions(), tasks, server.Open()));
  Check(model.endpoint.protocol == "responses" && !model.allow_server_state && !model.allow_persist_resume_state,
        "Responses registered its actual protocol and default state policy");
  auto request = Request();
  request.allow_server_state = true;
  request.tools = {{{{"test.confirm"}, 1, {}}, "preview.confirm", "Confirm", {kit::JsonValue{true}}},
      {{{"test.inspect"}, 1, {}}, "agentkit_tool_1", "Inspect", {kit::JsonValue{true}}}};
  AddFixtureSkill(request);
  auto stream = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*stream);
  Check(std::get<kit::ModelTextDelta>(events.front()).utf8 == display, "UTF-8 fragments decoded");
  Check(std::get<kit::ModelToolStarted>(events[1]).provider_call_id == "call_a" &&
      std::get<kit::ModelToolStarted>(events[1]).name == "preview.confirm" &&
      std::get<kit::ModelToolStarted>(events[2]).provider_call_id == "call_b",
      "output indices preserved call order and mapped names");
  std::string arguments;
  for (const auto& event : events) {
    if (const auto* part = std::get_if<kit::ModelToolArguments>(&event); part && part->provider_call_id == "call_a")
      arguments += part->bytes;
  }
  Check(arguments == "{\"value\":42}" && std::get<kit::ModelUsage>(events[events.size() - 4]).usage.output_tokens == 7 &&
      std::get<kit::ModelToolFinished>(events[events.size() - 3]).provider_call_id == "call_a" &&
      std::get<kit::ModelToolFinished>(events[events.size() - 2]).provider_call_id == "call_b",
      "completed calls preserve fragmented arguments, order and usage");
  auto finish = std::get<kit::ModelFinished>(std::move(events.back()));
  Check(finish.reason == kit::ModelFinishReason::ToolCalls && finish.resume && finish.resume->provider == Ref(),
      "encrypted reasoning is returned as state bound to this provider");
  const auto value = kit::detail::FromWireJson(Json{{"value", 42}});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Assistant, display});
  request.items.push_back(kit::ModelToolCall{"call_a", "preview.confirm", value});
  request.items.push_back(kit::ModelToolCall{"call_b", "agentkit_tool_1", kit::JsonValue{kit::JsonValue::Object{}}});
  request.items.push_back(kit::ModelToolResult{"call_a", {}, value, {}});
  request.items.push_back(kit::ModelToolResult{"call_b", {}, {}, kit::AgentError{
      .code = kit::ErrorCode::PermissionDenied, .message = "Tool denied"}});
  request.continuation = kit::ModelContinuation{std::move(*finish.resume), 0};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  Check(server.requests == 1, "private reasoning outside the authorized prefix never reached the endpoint");
  request.continuation->prefix_items = 4;
  auto next = Take(co_await model.provider->StartTurnAsync(request));
  auto completed = co_await Collect(*next);
  Check(std::get<kit::ModelTextDelta>(completed.front()).utf8 == "Done", "Responses tool follow-up finished");
  request.continuation.reset();
  auto rebuilt = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  co_await Collect(*rebuilt);
}

Task<void> CompatibleResponsesProtocol(TaskScope tasks, kit::ResponsesDialect dialect, bool store) {
  const bool glm = dialect == kit::ResponsesDialect::GlmCompatible;
  const Json thought{{"type", "reasoning_text"}, {"text", "Private test reasoning"}};
  const Json reasoning{{"type", "reasoning"}, {"id", "rs-private"}, {"status", "completed"},
      {"content", glm ? thought : Json::array({thought})}};
  const Json call{{"type", "function_call"}, {"id", "fc-1"}, {"call_id", "call-1"}, {"name", "inspect"},
      {"arguments", "{\"value\":42}"}, {"status", "completed"}};
  const auto create = [](std::string_view id) {
    return ResponseFrame("response.created", {{"response", {{"id", id}, {"status", "in_progress"}}}});
  };
  Script server;
  server.fragment_bytes = 23;
  server.reply = [&](const HttpRequest& request, const Json& body) {
    Check(request.url == "https://provider.example/v1/responses" && body["stream"] == true &&
        !body.contains("include") && !body.contains("parallel_tool_calls") &&
        body["tools"][0]["name"] == "inspect" && !body["tools"][0].contains("strict") &&
        body["tools"][0]["parameters"]["type"] == "object",
        "Compatible Responses omits OpenAI-only requests and sends the typed function");
    const auto& format = body["text"]["format"];
    Check(glm ? format["type"] == "json_object" && !format.contains("schema")
              : format["type"] == "json_schema" && format["schema"]["type"] == "object" && !format.contains("strict"),
        "Compatible Responses uses the dialect's structured output format");
    const auto& input = body["input"];
    const std::size_t first = glm ? 1 : 0;
    if (glm) {
      Check(input.front()["type"] == "message" && input.front()["role"] == "system" &&
          kit::detail::String(input.front(), "content").find("JSON") != std::string::npos,
          "GLM JSON mode sends its format instruction in the input messages");
    }
    if (server.requests == 1) {
      Check(!body.contains("previous_response_id") && (glm ? body["store"] == store : !body.contains("store")) &&
          input.size() == first + 1 && input[first]["type"] == "message" && input[first]["content"] == "Hello",
          "Compatible Responses sends a typed input and only supported storage settings");
      auto pending = call;
      pending["status"] = "in_progress";
      pending["arguments"] = "";
      auto pending_reasoning = reasoning;
      pending_reasoning.erase("content");
      return create("resp-first") + ResponseItem("added", 0, pending_reasoning) +
          ResponseFrame("response.reasoning_text.delta", {{"output_index", 0}, {"item_id", "rs-private"}, {"delta", "Private"}}) +
          ResponseItem("done", 0, reasoning) + ResponseItem("added", 1, pending) +
          ResponseFrame("response.function_call_arguments.delta", {{"output_index", 1}, {"item_id", "fc-1"}, {"delta", "{\"value\":"}}) +
          ResponseFrame("response.function_call_arguments.delta", {{"output_index", 1}, {"item_id", "fc-1"}, {"delta", "42}"}}) +
          ResponseItem("done", 1, call) + ResponseCompleted(Json::array({reasoning, call}), "resp-first");
    }
    const bool linked = store && (server.requests == 2 || server.requests == 3);
    Check(glm ? body["store"] == linked : !body.contains("store"), "Compatible Responses retained the current storage policy");
    if (linked) {
      Check(body["previous_response_id"] == "resp-first" && input.size() == first + 1,
          "GLM continuation preserved the JSON instruction and sent only the new tool result and previous response ID");
    } else {
      const Json replay{{"type", "reasoning"}, {"content", reasoning.at("content")}};
      Check(!body.contains("previous_response_id") && input.size() == first + 4 && input[first]["type"] == "message" &&
          input[first + 1] == replay && input[first + 2]["call_id"] == "call-1",
          "Compatible Responses rebuilt the complete authorized input with the dialect's private reasoning shape");
    }
    Check(input.back()["type"] == "function_call_output" && input.back()["call_id"] == "call-1" &&
        Json::parse(input.back()["output"].get<std::string>())["value"] == 42,
        "Compatible Responses addressed the original function call ID");
    server.status = linked && server.requests == 3 ? 410 : 200;
    const std::string final_text = R"({"title":"Done"})";
    auto message = ResponseMessage(final_text);
    if (glm) message["content"] = Json(message["content"][0]);
    return create("resp-next") + ResponseItem("added", 0, ResponseMessage("")) + ResponseText(final_text) +
        ResponseItem("done", 0, message) + ResponseCompleted(Json::array({message}), "resp-next");
  };
  auto options = ResponsesOptions();
  options.dialect = dialect;
  options.allow_server_state = store;
  const auto model = Take(kit::detail::MakeResponsesModel(std::move(options), tasks, server.Open()));
  Check(model.endpoint.provider == (glm ? "glm" : "deepseek") && model.allow_server_state == store,
      "Compatible Responses registered the actual provider and storage policy");
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(Json{{"type", "object"}})};
  request.allow_server_state = true;
  request.tools = {{{{"test.inspect"}, 1, {}}, "inspect", "Inspect", {kit::JsonValue{true}}}};
  auto stream = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*stream);
  Check(std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelTextDelta>(event); }) &&
      std::get<kit::ModelToolStarted>(events.front()).provider_call_id == "call-1",
      "Compatible Responses kept reasoning private and delivered the original call");
  const auto finish = std::get<kit::ModelFinished>(std::move(events.back()));
  Check(finish.reason == kit::ModelFinishReason::ToolCalls && finish.resume, "Compatible Responses retained same-run continuation state");
  const auto value = kit::detail::FromWireJson(Json{{"value", 42}});
  request.items.push_back(kit::ModelToolCall{"call-1", "inspect", value});
  request.items.push_back(kit::ModelToolResult{"call-1", {}, value, {}});
  request.continuation = kit::ModelContinuation{*finish.resume, 1};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  request.continuation->prefix_items = 2;
  const auto original_run = request.run;
  request.run = {"other-run"};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  Check(server.requests == 1, "Compatible Responses rejected a changed run or input boundary before sending");
  request.run = original_run;
  const auto verify = [&](const auto& output) {
    const auto& structured = std::get<kit::ModelStructuredOutput>(output[output.size() - 2]);
    Check(kit::detail::ToWireJson(structured.value)["title"] == "Done", "Compatible Responses parsed the final structured output");
  };
  auto next = Take(co_await model.provider->StartTurnAsync(request));
  verify(co_await Collect(*next));
  if (store) {
    auto expired = Take(co_await model.provider->StartTurnAsync(request));
    verify(co_await Collect(*expired));
    Check(server.requests == 4, "GLM retried the expired reference once with its stateless transcript");
    request.allow_server_state = false;
    auto revoked = Take(co_await model.provider->StartTurnAsync(request));
    verify(co_await Collect(*revoked));
    Check(server.requests == 5, "GLM did not send its server state reference after revocation");
  }
}

Task<void> ResponsesStructuredAndFailures(TaskScope tasks) {
  const Json schema{{"type", "object"}, {"properties", {{"title", {{"type", "string"}}}}},
      {"required", Json::array({"title"})}, {"additionalProperties", false}};
  {
    Script json_mode;
    json_mode.reply = [&](const auto&, const Json& body) {
      const auto& input = body["input"];
      Check(body["text"]["format"]["type"] == "json_object" && input.size() == 2 &&
          input[0]["role"] == "system" && kit::detail::String(input[0], "content").find("JSON") != std::string::npos &&
          input[1]["role"] == "user" && input[1]["content"] == "Hello" &&
          kit::detail::String(body, "instructions").find(schema.dump()) != std::string::npos,
          "Responses JSON mode sent its format hint in the input and retained the schema and original user message");
      constexpr auto output = R"({"title":"Release"})";
      return ResponseItem("added", 0, ResponseMessage("")) + ResponseText(output) +
          ResponseCompleted(Json::array({ResponseMessage(output)}));
    };
    auto options = ResponsesOptions();
    options.output_format = kit::ResponsesOutputFormat::JsonObject;
    auto model = Take(kit::detail::MakeResponsesModel(std::move(options), tasks, json_mode.Open()));
    auto request = Request();
    request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)};
    auto stream = Take(co_await model.provider->StartTurnAsync(std::move(request)));
    const auto events = co_await Collect(*stream);
    Check(std::ranges::any_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelStructuredOutput>(event); }),
        "Responses JSON mode assembled its final structured output");
  }
  Script server;
  auto model = Take(kit::detail::MakeResponsesModel(ResponsesOptions(), tasks, server.Open()));
  server.reply = [&](const auto&, const Json& body) {
    const auto& format = body["text"]["format"];
    Check(format["type"] == "json_schema" && format["strict"] == true && format["schema"] == schema,
        "Responses sent the host schema at text.format");
    const auto output = server.requests == 1 ? "{\"title\":\"Release\"}" : "Declined";
    const bool refused = server.requests == 2;
    return ResponseItem("added", 0, ResponseMessage("")) + ResponseText(output, 0, refused) +
        ResponseCompleted(Json::array({ResponseMessage(output, refused)}));
  };
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)};
  auto structured = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*structured);
  CheckStructuredDeltas(events);
  Check(kit::detail::ToWireJson(std::get<kit::ModelStructuredOutput>(events[events.size() - 2]).value)["title"] == "Release",
        "Responses emitted the parsed structured result");
  auto refused = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  events = co_await Collect(*refused);
  Check(std::get<kit::ModelTextDelta>(events.front()).show_in_transcript &&
      std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::Refused &&
      std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelStructuredOutput>(event); }),
      "refusal text was preserved without attempting to parse it as JSON");

  const Json pending{{"type", "function_call"}, {"id", "fc_pending"}, {"call_id", "call_pending"},
      {"name", "inspect"}, {"arguments", "{\"value\":"}, {"status", "in_progress"}};
  server.reply = [&](const auto&, const auto&) {
    const auto tool = ResponseItem("added", 1, pending);
    const auto prefix = ResponseItem("added", 0, ResponseMessage("")) + ResponseText("Partial") + tool;
    if (server.requests == 3) {
      return prefix + ResponseFrame("response.incomplete", {{"response", {{"status", "incomplete"},
          {"incomplete_details", {{"reason", "max_output_tokens"}}}}}});
    }
    if (server.requests == 4) return prefix;
    return prefix + ResponseFrame("response.function_call_arguments.done",
        {{"item_id", "call_pending"}, {"output_index", 1}, {"arguments", "{\"value\":42}"}});
  };
  auto with_tool = Request();
  with_tool.tools = {{{{"test.inspect"}, 1, {}}, "inspect", "Inspect", {kit::JsonValue{true}}}};
  auto incomplete = Take(co_await model.provider->StartTurnAsync(with_tool));
  events = co_await Collect(*incomplete);
  const auto end = std::get<kit::ModelFinished>(events.back());
  Check(end.reason == kit::ModelFinishReason::Length && !end.resume &&
      std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelToolFinished>(event); }),
      "a provider-reported incomplete turn kept its text and did not complete the tool");
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto invalid = Take(co_await model.provider->StartTurnAsync(with_tool));
    Check(std::holds_alternative<kit::ModelTextDelta>(*Take(co_await invalid->ReadAsync())),
        "partially received text remained visible");
    Check(std::holds_alternative<kit::ModelToolStarted>(*Take(co_await invalid->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolArguments>(*Take(co_await invalid->ReadAsync())),
        "partially received tool call was tracked before termination");
    ErrorIs(co_await invalid->ReadAsync(), kit::ErrorCode::ProtocolError);
  }
  auto no_media = Request();
  no_media.attachments.push_back({{{"image"}, 1}, "image/png", {}});
  ErrorIs(co_await model.provider->StartTurnAsync(std::move(no_media)), kit::ErrorCode::Unsupported);
  Check(server.requests == 5, "unsupported capability failed before opening the transport");
  co_await VerifyCancellation(tasks, server, *model.provider);
}

kit::MessagesOptions MessagesOptions(kit::MessagesDialect dialect = kit::MessagesDialect::Anthropic) {
  return {.ref = Ref(), .model = "fixture-model", .base_url = "https://provider.example/v1/", .dialect = dialect};
}

std::string MessageStart() {
  return ResponseFrame("message_start", {{"message", {{"role", "assistant"}, {"content", Json::array()},
      {"usage", {{"input_tokens", 17}, {"cache_creation_input_tokens", 4}, {"cache_read_input_tokens", 3}, {"output_tokens", 1}}}}}});
}

std::string MessageBlock(unsigned index, Json block) {
  return ResponseFrame("content_block_start", {{"index", index}, {"content_block", std::move(block)}});
}

std::string MessageDelta(unsigned index, Json delta) {
  return ResponseFrame("content_block_delta", {{"index", index}, {"delta", std::move(delta)}});
}

std::string CloseMessageBlock(unsigned index) {
  return ResponseFrame("content_block_stop", {{"index", index}});
}

std::string MessageStop(std::string_view reason) {
  return ResponseFrame("message_delta", {{"delta", {{"stop_reason", reason}}}, {"usage", {{"output_tokens", 11}}}}) +
      ResponseFrame("message_stop", Json::object());
}

std::string MessageText(unsigned index, const std::string& text) {
  return MessageBlock(index, {{"type", "text"}, {"text", ""}}) +
      MessageDelta(index, {{"type", "text_delta"}, {"text", text}}) + CloseMessageBlock(index);
}

Task<void> MessagesToolProtocol(TaskScope tasks, kit::MessagesDialect dialect) {
  const bool native = dialect == kit::MessagesDialect::Anthropic;
  const std::string display = "\xe5\xae\xa1\xe9\x98\x85";
  const auto display_suffix = native ? std::string(" confirmed") : std::string{};
  const Json thought{{"type", "thinking"}, {"thinking", ""}};
  const Json redacted{{"type", "redacted_thinking"}, {"data", "opaque-redacted-test"}};
  const Json first{{"type", "tool_use"}, {"id", "toolu_a"}, {"name", "agentkit_tool_2"}, {"input", Json::object()}};
  const Json second{{"type", "tool_use"}, {"id", "toolu_b"}, {"name", "agentkit_tool_1"}, {"input", Json::object()}};
  Script server;
  server.reply = [&](const HttpRequest& call, const Json& body) {
    const auto header = [&](std::string_view name, std::string_view value) {
      return std::ranges::any_of(call.headers, [&](const auto& header) { return header.name == name && header.value == value; });
    };
    Check(call.url == "https://provider.example/v1/messages" && header("x-api-key", "fixture-api-key") &&
        header("anthropic-version", native ? "2026-10-21" : "2023-06-01"), "Messages adapter used native HTTP headers");
    Check(body["max_tokens"] == 8192 && body["stream"] == true && !body.contains("store") &&
        HasFixtureSkill(body["system"]) &&
        body["thinking"]["type"] == (native ? "enabled" : "adaptive"), "Messages request carried instructions and configured thinking");
    const auto alias = server.requests == 1 ? "agentkit_tool_2" : "agentkit_tool_3";
    Check(body["tools"][0]["name"] == alias && body["tools"][0]["input_schema"]["type"] == "object" &&
        body["tools"][1]["name"] == "agentkit_tool_1" && !body["tools"][1].contains("type"),
        "Messages tool definitions preserved the host aliases and schema");
    const auto& messages = body["messages"];
    Check(messages[0]["role"] == "user" && messages[0]["content"][0]["text"] == "Hello", "Messages initial user data was preserved");
    if (server.requests == 1) {
      auto stream = MessageStart() + ResponseFrame("ping", Json::object()) + MessageBlock(0, thought) +
          MessageDelta(0, {{"type", "thinking_delta"}, {"thinking", "private "}}) +
          MessageDelta(0, {{"type", "thinking_delta"}, {"thinking", "thought"}});
      if (native) stream += MessageDelta(0, {{"type", "signature_delta"}, {"signature", "fixture-signed"}});
      stream += CloseMessageBlock(0) + MessageText(1, display) + MessageBlock(2, first) +
          MessageDelta(2, {{"type", "input_json_delta"}, {"partial_json", "{\"value\":"}}) +
          MessageDelta(2, {{"type", "input_json_delta"}, {"partial_json", "42}"}}) + CloseMessageBlock(2);
      if (native) stream += MessageBlock(3, redacted) + CloseMessageBlock(3) + MessageText(4, display_suffix);
      const auto index = native ? 5U : 3U;
      return stream + MessageBlock(index, second) + CloseMessageBlock(index) + MessageStop("tool_use");
    }
    const auto& assistant = messages[1]["content"];
    const auto& output = messages[2]["content"];
    Check(messages.size() == 3 && messages[1]["role"] == "assistant" && messages[2]["role"] == "user",
        "assistant tool calls and next user results were grouped");
    if (server.requests == 2) {
      auto retained = thought;
      retained["thinking"] = "private thought";
      if (native) retained["signature"] = "fixture-signed";
      Check(assistant[0] == retained && assistant[1]["text"] == display && assistant[2]["input"]["value"] == 42 &&
          assistant[2]["name"] == "agentkit_tool_2", "continuation preserved original state, block order and tool alias");
      if (native) {
        Check(assistant.size() == 6 && assistant[3] == redacted && assistant[4]["text"] == display_suffix &&
            assistant[5] == second, "redacted state and post-tool visible text were replayed in place");
      } else Check(assistant.size() == 4 && assistant[3] == second, "unsigned compatibility state was replayed");
    } else {
      Check(assistant.size() == 3 && assistant[0]["type"] == "text" &&
          assistant[0]["text"] == display + display_suffix && assistant[1]["name"] == alias,
          "local history was rebuilt without private state");
    }
    Check(output.size() == 3 && output[0]["tool_use_id"] == "toolu_a" && output[0]["type"] == "tool_result" &&
        Json::parse(output[0]["content"].get<std::string>())["value"] == 42 &&
        output[1]["tool_use_id"] == "toolu_b" && output[1]["is_error"] == true &&
        output[2]["text"] == "Summarize results", "matching tool results precede user input and retain errors");
    return MessageStart() + MessageText(0, "Done") + MessageStop("end_turn");
  };
  auto options = MessagesOptions(dialect);
  options.api_key = []() -> kit::Async<std::string> { co_return kit::AgentResult<std::string>::Success("fixture-api-key"); };
  if (native) {
    options.api_version = "2026-10-21";
    options.thinking = kit::MessagesThinking{kit::MessagesThinking::Mode::Budgeted, 2048};
  } else options.thinking = kit::MessagesThinking{kit::MessagesThinking::Mode::Adaptive};
  const auto model = Take(kit::detail::MakeMessagesModel(std::move(options), tasks, server.Open()));
  Check(model.endpoint.protocol == "messages" && model.endpoint.provider == (native ? "anthropic" : "deepseek") &&
      !model.allow_persist_resume_state && !model.allow_server_state, "Messages registered the selected dialect");
  auto request = Request();
  request.tools = {{{{"test.confirm"}, 1, {}}, "preview.confirm", "Confirm", {kit::JsonValue{true}}},
      {{{"test.inspect"}, 1, {}}, "agentkit_tool_1", "Inspect", {kit::JsonValue{true}}}};
  AddFixtureSkill(request);
  auto opened = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*opened);
  const auto usage = std::ranges::find_if(events, [](const auto& event) { return std::holds_alternative<kit::ModelUsage>(event); });
  Check(usage != events.end() && std::get<kit::ModelUsage>(*usage).usage.input_tokens == 24 &&
      std::get<kit::ModelUsage>(*usage).usage.output_tokens == 11, "Messages normalized final tokens without duplicating interim usage");
  std::string text;
  std::string arguments;
  std::vector<std::string> calls;
  for (const auto& event : events) {
    if (const auto* delta = std::get_if<kit::ModelTextDelta>(&event)) text += delta->utf8;
    if (const auto* delta = std::get_if<kit::ModelToolArguments>(&event); delta && delta->provider_call_id == "toolu_a")
      arguments += delta->bytes;
    if (const auto* call = std::get_if<kit::ModelToolFinished>(&event)) calls.push_back(call->provider_call_id);
  }
  Check(text == display + display_suffix && arguments == "{\"value\":42}" &&
      calls == std::vector<std::string>{"toolu_a", "toolu_b"}, "only visible text and complete tool calls reached the runtime");
  auto finish = std::get<kit::ModelFinished>(std::move(events.back()));
  Check(finish.reason == kit::ModelFinishReason::ToolCalls && finish.resume && finish.resume->provider == Ref(),
      "Messages returned private state bound to its model revision");
  const auto value = kit::detail::FromWireJson(Json{{"value", 42}});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Assistant, text});
  request.items.push_back(kit::ModelToolCall{"toolu_a", "preview.confirm", value});
  request.items.push_back(kit::ModelToolCall{"toolu_b", "agentkit_tool_1", kit::JsonValue{kit::JsonValue::Object{}}});
  request.items.push_back(kit::ModelToolResult{"toolu_a", {}, value, {}});
  request.items.push_back(kit::ModelToolResult{"toolu_b", {}, {}, kit::AgentError{.code = kit::ErrorCode::PermissionDenied, .message = "Tool denied"}});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::User, std::string("Summarize results")});
  request.tools.push_back({{{"test.added"}, 1, {}}, "aa.new", "Added", {kit::JsonValue{true}}});
  request.continuation = kit::ModelContinuation{std::move(*finish.resume), 2};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  Check(server.requests == 1, "private Messages state outside its allowed prefix did not reach the endpoint");
  request.continuation->prefix_items = 4;
  auto next = Take(co_await model.provider->StartTurnAsync(request));
  auto continued = co_await Collect(*next);
  Check(std::get<kit::ModelTextDelta>(continued.front()).utf8 == "Done", "Messages tool follow-up completed");
  request.continuation.reset();
  auto rebuilt = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  co_await Collect(*rebuilt);
}

Task<void> MessagesStructuredAndFailures(TaskScope tasks) {
  const Json schema{{"type", "object"}, {"properties", {{"title", {{"type", "string"}}}}},
      {"required", Json::array({"title"})}, {"additionalProperties", false}};
  Script server;
  auto options = MessagesOptions();
  options.output_format = kit::MessagesOutputFormat::JsonSchema;
  const auto model = Take(kit::detail::MakeMessagesModel(std::move(options), tasks, server.Open()));
  server.reply = [&](const auto&, const Json& body) {
    Check(body["output_config"]["format"]["type"] == "json_schema" && body["output_config"]["format"]["schema"] == schema,
        "Messages sent the native structured response schema");
    return MessageStart() + MessageText(0, "{\"title\":\"Release\"}") + MessageStop("stop_sequence");
  };
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)};
  auto opened = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*opened);
  CheckStructuredDeltas(events);
  Check(kit::detail::ToWireJson(std::get<kit::ModelStructuredOutput>(events[events.size() - 2]).value)["title"] == "Release",
      "Messages emitted the native structured output");

  server.reply = [&](const auto&, const auto&) {
    if (server.requests == 5) {
      return ResponseFrame("error", {{"error", {{"type", "rate_limit_error"}}}});
    }
    const auto start = MessageStart() + MessageText(0, "Partial") +
        MessageBlock(1, {{"type", "tool_use"}, {"id", "toolu_pending"}, {"name", "inspect"}, {"input", Json::object()}}) +
        MessageDelta(1, {{"type", "input_json_delta"}, {"partial_json", "{\"value\":"}});
    if (server.requests == 2) return start + MessageStop("max_tokens");
    if (server.requests == 3) return start;
    return start + CloseMessageBlock(1) + MessageStop("tool_use");
  };
  auto with_tool = Request();
  with_tool.tools = {{{{"test.inspect"}, 1, {}}, "inspect", "Inspect", {kit::JsonValue{true}}}};
  auto partial = Take(co_await model.provider->StartTurnAsync(with_tool));
  events = co_await Collect(*partial);
  Check(std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::Length &&
      std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelToolFinished>(event); }),
      "Messages kept partial text and stopped before scheduling an incomplete tool call");
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto invalid = Take(co_await model.provider->StartTurnAsync(with_tool));
    Check(std::holds_alternative<kit::ModelTextDelta>(*Take(co_await invalid->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolStarted>(*Take(co_await invalid->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolArguments>(*Take(co_await invalid->ReadAsync())), "incomplete Messages preserved streamed parts");
    ErrorIs(co_await invalid->ReadAsync(), kit::ErrorCode::ProtocolError);
  }
  auto failed = Take(co_await model.provider->StartTurnAsync(Request()));
  ErrorIs(co_await failed->ReadAsync(), kit::ErrorCode::RateLimited);
  auto unavailable = MessagesOptions(kit::MessagesDialect::DeepSeek);
  unavailable.output_format = kit::MessagesOutputFormat::JsonSchema;
  ErrorIs(kit::detail::MakeMessagesModel(std::move(unavailable), tasks, server.Open()), kit::ErrorCode::Unsupported);
  Check(server.requests == 5, "compatibility endpoints cannot declare unsupported native schemas");
  co_await VerifyCancellation(tasks, server, *model.provider);
}

kit::GeminiGenerateContentOptions GeminiOptions() {
  return {.ref = Ref(), .model = "models/fixture.8-model", .base_url = "https://provider.example/v1beta/"};
}

kit::GeminiInteractionsOptions InteractionsOptions() {
  return {.ref = Ref(), .model = "fixture-interactions", .base_url = "https://provider.example/v1/"};
}

std::string InteractionEvent(std::string_view type, Json body) {
  body["event_type"] = type;
  return "event: " + std::string(type) + "\n" + Frame(body, "\n");
}

std::string InteractionStart(std::string_view id) {
  return InteractionEvent("interaction.created", {{"interaction", {{"id", id}, {"status", "in_progress"}}}});
}

std::string StartStep(unsigned index, Json step) {
  return InteractionEvent("step.start", {{"index", index}, {"step", std::move(step)}});
}

std::string StepDelta(unsigned index, Json delta) {
  return InteractionEvent("step.delta", {{"index", index}, {"delta", std::move(delta)}});
}

std::string StopStep(unsigned index) {
  return InteractionEvent("step.stop", {{"index", index}});
}

std::string TextStep(unsigned index, std::string_view text) {
  return StartStep(index, {{"type", "model_output"}}) + StepDelta(index, {{"type", "text"}, {"text", text}}) + StopStep(index);
}

std::string InteractionEnd(std::string_view id, std::string_view status) {
  return InteractionEvent("interaction.completed", {{"interaction", {{"id", id}, {"status", status},
      {"usage", {{"total_input_tokens", 13}, {"total_output_tokens", 7}, {"total_thought_tokens", 3}}}}}});
}

std::string InteractionText(std::string_view text) {
  return InteractionStart("fixture-text") + TextStep(0, text) + InteractionEnd("fixture-text", "completed");
}

Task<void> InteractionsToolProtocol(TaskScope tasks, bool stateful) {
  const std::string visible = "\xe5\xae\xa1\xe9\x98\x85";
  const Json output_step{{"type", "model_output"}, {"content", Json::array({
      {{"type", "text"}, {"text", visible.substr(0, 3)}},
      {{"type", "text"}, {"text", ""}},
      {{"type", "text"}, {"text", visible.substr(3)}}})}};
  const Json thought{{"type", "thought"}, {"signature", "signed-thought"},
      {"summary", Json::array({{{"type", "text"}, {"text", "private reasoning"}}})}};
  const Json first{{"type", "function_call"}, {"id", "int-call-a"}, {"name", "agentkit_tool_2"}, {"arguments", {{"value", 42}}}};
  const Json second{{"type", "function_call"}, {"id", "int-call-b"}, {"name", "agentkit_tool_1"}, {"arguments", Json::object()}};
  Script server;
  server.reply = [&](const HttpRequest& call, const Json& body) {
    const auto key = std::ranges::find_if(call.headers, [](const auto& entry) { return entry.name == "x-goog-api-key"; });
    Check(call.url == "https://provider.example/v1/interactions" && key != call.headers.end() &&
        key->value == "fixture-interactions-key" && body["model"] == "fixture-interactions" && body["stream"] == true,
        "Interactions used native URL, body and key header");
    Check(HasFixtureSkill(body["system_instruction"]) &&
        body["tools"][0]["type"] == "function" && body["tools"][0]["parameters"]["type"] == "object" &&
        body["tools"][0]["name"] == (server.requests == 1 ? "agentkit_tool_2" : "agentkit_tool_3"),
        "Interactions repeated the latest model instructions and offered tool aliases each turn");
    const auto& input = body["input"];
    if (server.requests == 1) {
      Check(body["store"] == stateful && !body.contains("previous_interaction_id") && input.size() == 1 &&
          input[0]["type"] == "user_input" && input[0]["content"][0]["text"] == "Hello",
          "Interactions saved only explicit server state on the first request");
      auto pending = first;
      pending["arguments"] = Json::object();
      auto output_start = output_step;
      output_start["content"].back()["text"] = "";
      return InteractionStart("int-first") + InteractionEvent("interaction.status_update", {
          {"interaction_id", "int-first"}, {"status", "in_progress"}}) +
          StartStep(0, {{"type", "thought"}}) +
          StepDelta(0, {{"type", "thought_summary"}, {"content", thought["summary"][0]}}) +
          StepDelta(0, {{"type", "thought_signature"}, {"signature", "signed-thought"}}) + StopStep(0) +
          StartStep(1, output_start) + StepDelta(1, {{"type", "text"}, {"text", visible.substr(3)}}) + StopStep(1) +
          StartStep(2, pending) + StartStep(3, second) +
          StepDelta(2, {{"type", "arguments_delta"}, {"arguments", "{\"value\":"}}) +
          StepDelta(2, {{"type", "arguments_delta"}, {"arguments", "42}"}}) +
          StopStep(2) + StopStep(3) + InteractionEnd("int-first", "requires_action");
    }
    const auto linked = stateful && (server.requests == 2 || server.requests == 3);
    const std::size_t result_offset = linked ? 0 : 5;
    Check(body["store"] == linked && input.size() == result_offset + 2, "Interactions sent the correct authorized input range");
    if (linked) {
      Check(body["previous_interaction_id"] == "int-first", "stateful continuation pinned the original interaction ID");
    } else {
      Check(!body.contains("previous_interaction_id") && input[0]["type"] == "user_input" && input[1] == thought &&
          input[2] == output_step &&
          input[3] == first && input[4] == second, "stateless replay preserved original steps, signature and old tool alias");
    }
    Check(input[result_offset]["type"] == "function_result" && input[result_offset]["call_id"] == "int-call-a" &&
        input[result_offset]["name"] == "agentkit_tool_2" &&
        Json::parse(input[result_offset]["result"][0]["text"].get<std::string>())["value"] == 42 &&
        input[result_offset + 1]["call_id"] == "int-call-b" && input[result_offset + 1]["is_error"] == true &&
        Json::parse(input[result_offset + 1]["result"][0]["text"].get<std::string>())["ok"] == false,
        "Interactions returned the results and errors keyed to the original calls");
    server.status = stateful && server.requests == 3 ? 404 : 200;
    return InteractionText("Done");
  };
  auto options = InteractionsOptions();
  options.allow_server_state = stateful;
  options.allow_persist_resume_state = stateful;
  options.api_key = []() -> kit::Async<std::string> { co_return kit::AgentResult<std::string>::Success("fixture-interactions-key"); };
  const auto model = Take(kit::detail::MakeGeminiInteractionsModel(std::move(options), tasks, server.Open()));
  Check(model.endpoint.provider == "google" && model.endpoint.protocol == "interactions" &&
      model.allow_server_state == stateful && model.allow_persist_resume_state == stateful,
      "Interactions reflected opt-in model state policy");
  auto request = Request();
  request.allow_server_state = true;
  request.tools = {{{{"test.confirm"}, 1, {}}, "preview.confirm", "Confirm", {kit::JsonValue{true}}},
      {{{"test.inspect"}, 1, {}}, "agentkit_tool_1", "Inspect", {kit::JsonValue{true}}}};
  AddFixtureSkill(request);
  auto opened = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*opened);
  Check(std::get<kit::ModelTextDelta>(events[0]).utf8 == visible.substr(0, 3) &&
      std::get<kit::ModelTextDelta>(events[1]).utf8 == visible.substr(3) &&
      std::get<kit::ModelToolStarted>(events[2]).provider_call_id == "int-call-a" &&
      std::get<kit::ModelToolStarted>(events[2]).name == "preview.confirm" &&
      std::get<kit::ModelToolStarted>(events[3]).provider_call_id == "int-call-b",
      "Interactions emitted authorized tools in original step order and hid thought text");
  const auto usage = std::ranges::find_if(events, [](const auto& event) { return std::holds_alternative<kit::ModelUsage>(event); });
  Check(usage != events.end() && std::get<kit::ModelUsage>(*usage).usage.input_tokens == 13 &&
      std::get<kit::ModelUsage>(*usage).usage.output_tokens == 10, "Interactions accounted for final thought and output usage");
  auto finished = std::get<kit::ModelFinished>(std::move(events.back()));
  Check(finished.reason == kit::ModelFinishReason::ToolCalls && finished.resume && finished.resume->provider == Ref(),
      "Interactions sealed calls and returned model-specific continuation");
  const auto value = kit::detail::FromWireJson(Json{{"value", 42}});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Assistant, visible});
  request.items.push_back(kit::ModelToolCall{"int-call-a", "preview.confirm", value});
  request.items.push_back(kit::ModelToolCall{"int-call-b", "agentkit_tool_1", kit::JsonValue{kit::JsonValue::Object{}}});
  request.items.push_back(kit::ModelToolResult{"int-call-a", {}, value, {}});
  request.items.push_back(kit::ModelToolResult{"int-call-b", {}, {}, kit::AgentError{
      .code = kit::ErrorCode::PermissionDenied, .message = "Access denied"}});
  request.tools.push_back({{{"test.added"}, 1, {}}, "aa.new", "Added", {kit::JsonValue{true}}});
  request.continuation = kit::ModelContinuation{std::move(*finished.resume), 2};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  request.continuation->prefix_items = 4;
  auto original_run = request.run;
  request.run = {"different-run"};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  Check(server.requests == 1, "Interactions did not send a stale prefix or a cross-run state handle");
  request.run = original_run;
  auto next = Take(co_await model.provider->StartTurnAsync(request));
  auto continued = co_await Collect(*next);
  Check(std::get<kit::ModelTextDelta>(continued.front()).utf8 == "Done", "Interactions completed after returning the tool results");
  if (stateful) {
    auto expired = Take(co_await model.provider->StartTurnAsync(request));
    auto fallback = co_await Collect(*expired);
    Check(server.requests == 4 && std::get<kit::ModelTextDelta>(fallback.front()).utf8 == "Done",
        "Expired Interactions reference recovered with one stateless retry");
    request.allow_server_state = false;
    auto revoked = Take(co_await model.provider->StartTurnAsync(std::move(request)));
    auto rebuilt = co_await Collect(*revoked);
    Check(server.requests == 5 && std::get<kit::ModelTextDelta>(rebuilt.front()).utf8 == "Done",
        "Revoked server state rebuilt only the currently authorized local input");
  }
}

Task<void> InteractionsStructuredAndFailures(TaskScope tasks) {
  const Json schema{{"type", "object"}, {"properties", {{"title", {{"type", "string"}}}}},
      {"required", Json::array({"title"})}, {"additionalProperties", false}};
  Script server;
  const auto model = Take(kit::detail::MakeGeminiInteractionsModel(InteractionsOptions(), tasks, server.Open()));
  server.reply = [&](const auto&, const Json& body) {
    Check(body["response_format"]["type"] == "text" && body["response_format"]["mime_type"] == "application/json" &&
        body["response_format"]["schema"] == schema, "Interactions sent native structured output configuration");
    return InteractionStart("typed") + StartStep(0, {{"type", "model_output"}}) +
        StepDelta(0, {{"type", "text"}, {"text", "{\"title\":"}}) +
        StepDelta(0, {{"type", "text"}, {"text", "\"Release\"}"}}) + StopStep(0) + InteractionEnd("typed", "completed");
  };
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)};
  auto opened = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  auto events = co_await Collect(*opened);
  CheckStructuredDeltas(events);
  Check(kit::detail::ToWireJson(std::get<kit::ModelStructuredOutput>(events[events.size() - 2]).value)["title"] == "Release",
      "Interactions emitted complete structured output");
  server.reply = [&](const auto&, const auto&) {
    const auto prefix = InteractionStart("int-partial") + TextStep(0, "Partial") +
        StartStep(1, {{"type", "function_call"}, {"id", "pending"}, {"name", "inspect"}, {"arguments", Json::object()}}) +
        StepDelta(1, {{"type", "arguments_delta"}, {"arguments", "{\"value\":"}});
    if (server.requests == 2) return prefix + InteractionEnd("int-partial", "incomplete");
    if (server.requests == 3) return prefix + StopStep(1);
    if (server.requests == 4) return prefix + InteractionEnd("int-partial", "requires_action");
    return prefix + StopStep(1) + InteractionEnd("int-partial", "requires_action");
  };
  auto tool = Request();
  tool.tools = {{{{"test.inspect"}, 1, {}}, "inspect", "Inspect", {kit::JsonValue{true}}}};
  auto incomplete = Take(co_await model.provider->StartTurnAsync(tool));
  events = co_await Collect(*incomplete);
  Check(std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::Length &&
      std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelToolFinished>(event); }),
      "Incomplete Interactions does not finish an unverified tool");
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto streamed = Take(co_await model.provider->StartTurnAsync(tool));
    Check(std::holds_alternative<kit::ModelTextDelta>(*Take(co_await streamed->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolStarted>(*Take(co_await streamed->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolArguments>(*Take(co_await streamed->ReadAsync())),
        "Interactions preserved received output without scheduling the pending call");
    ErrorIs(co_await streamed->ReadAsync(), kit::ErrorCode::ProtocolError);
  }
  co_await VerifyCancellation(tasks, server, *model.provider);
}

std::string GeminiFrame(Json parts = Json::array(), std::string_view reason = {}) {
  Json candidate{{"index", 0}};
  if (!parts.empty()) candidate["content"] = {{"role", "model"}, {"parts", std::move(parts)}};
  if (!reason.empty()) candidate["finishReason"] = reason;
  return Frame(Json{{"candidates", Json::array({std::move(candidate)})}});
}

std::string GeminiText(std::string_view text) {
  return GeminiFrame(Json::array({Json{{"text", text}}}));
}

Task<void> GeminiToolProtocol(TaskScope tasks) {
  const std::string display = "\xe5\xae\xa1\xe9\x98\x85";
  const Json thought{{"thought", true}, {"text", "private gemini thought"}, {"thoughtSignature", "private-signed"}};
  const Json first{{"functionCall", {{"name", "agentkit_tool_2"}, {"id", "gemini-call-a"},
      {"args", {{"value", 42}}}}}, {"thoughtSignature", "function-signature"}};
  const Json second{{"functionCall", {{"name", "agentkit_tool_1"}, {"args", Json::object()}}}};
  const Json trailer{{"text", ""}, {"thoughtSignature", "signed-empty-part"}};
  Script server;
  server.reply = [&](const HttpRequest& call, const Json& body) {
    const auto alias = server.requests == 1 ? "agentkit_tool_2" : "agentkit_tool_3";
    const auto key = std::ranges::find_if(call.headers, [](const auto& header) { return header.name == "x-goog-api-key"; });
    Check(call.url == "https://provider.example/v1beta/models/fixture.8-model:streamGenerateContent?alt=sse" &&
        key != call.headers.end() && key->value == "fixture-gemini-key", "Gemini streamed the configured model using header authentication");
    Check(body["tools"][0]["functionDeclarations"][0]["name"] == alias &&
        body["tools"][0]["functionDeclarations"][0]["parametersJsonSchema"]["type"] == "object" &&
        body["tools"][0]["functionDeclarations"][1]["name"] == "agentkit_tool_1",
        "Gemini function aliases respected declaration constraints");
    const auto& contents = body["contents"];
    Check(contents[0]["role"] == "user" && contents[0]["parts"][0]["text"] == "Hello" &&
        HasFixtureSkill(body["systemInstruction"]["parts"][0]["text"]),
        "Gemini preserved instructions, activated Skill and authorized user input");
    if (server.requests == 1) {
      return GeminiFrame(Json::array({thought})) + GeminiText(display) +
          GeminiFrame(Json::array({first, second})) + GeminiFrame(Json::array({trailer}), "STOP") +
          Frame(Json{{"usageMetadata", {{"promptTokenCount", 13}, {"candidatesTokenCount", 7}, {"thoughtsTokenCount", 3}}}});
    }
    const auto& model = contents[1];
    const auto& results = contents[2];
    Check(contents.size() == 3 && model["role"] == "model" && results["role"] == "user", "Gemini grouped parallel calls and results");
    const auto& parts = model["parts"];
    if (server.requests == 2) {
      Check(parts.size() == 5 && parts[0] == thought && parts[1]["text"] == display &&
          parts[2] == first && parts[3] == second && parts[4] == trailer, "Gemini replayed each signature on its original Part");
    } else {
      Check(parts.size() == 3 && parts[0]["text"] == display && parts[1]["functionCall"]["name"] == alias &&
          !parts[1].contains("thoughtSignature"), "Gemini reconstructed only authorized local history without a continuation");
    }
    const auto& outputs = results["parts"];
    Check(outputs.size() == 2 && outputs[0]["functionResponse"]["id"] == "gemini-call-a" &&
        outputs[0]["functionResponse"]["name"] == (server.requests == 2 ? "agentkit_tool_2" : alias) &&
        outputs[0]["functionResponse"]["response"]["output"]["value"] == 42 &&
        !outputs[1]["functionResponse"].contains("id") && outputs[1]["functionResponse"]["response"].contains("error"),
        "Gemini returned ordered results by exact call ID and omitted synthetic legacy IDs");
    return GeminiText("Done") + GeminiFrame(Json::array(), "STOP");
  };
  auto options = GeminiOptions();
  options.api_key = []() -> kit::Async<std::string> { co_return kit::AgentResult<std::string>::Success("fixture-gemini-key"); };
  const auto model = Take(kit::detail::MakeGeminiGenerateContentModel(std::move(options), tasks, server.Open()));
  Check(model.endpoint.provider == "google" && model.endpoint.protocol == "generate-content" &&
      model.endpoint.model == "fixture.8-model" && !model.allow_server_state && !model.allow_persist_resume_state,
      "Gemini registered its normalized model and stateless protocol");
  auto request = Request();
  request.tools = {{{{"test.inspect"}, 1, {}}, "preview.lookup", "Lookup", {kit::JsonValue{true}}},
      {{{"test.second"}, 1, {}}, "agentkit_tool_1", "Inspect", {kit::JsonValue{true}}}};
  AddFixtureSkill(request);
  auto opened = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*opened);
  Check(std::get<kit::ModelTextDelta>(events[0]).utf8 == display, "Gemini exposed only the visible text");
  const auto id_a = std::get<kit::ModelToolStarted>(events[1]);
  const auto id_b = std::get<kit::ModelToolStarted>(events[3]).provider_call_id;
  Check(id_a.provider_call_id == "gemini-call-a" && id_a.name == "preview.lookup" && !id_b.empty() && id_b != id_a.provider_call_id,
      "Gemini mapped provider and legacy calls to unique runtime IDs");
  const auto& usage = std::get<kit::ModelUsage>(events[events.size() - 4]).usage;
  Check(usage.input_tokens == 13 && usage.output_tokens == 10 &&
      std::get<kit::ModelToolFinished>(events[events.size() - 3]).provider_call_id == id_a.provider_call_id &&
      std::get<kit::ModelToolFinished>(events[events.size() - 2]).provider_call_id == id_b,
      "Gemini waited through trailing usage metadata before completing calls");
  auto finish = std::get<kit::ModelFinished>(std::move(events.back()));
  Check(finish.reason == kit::ModelFinishReason::ToolCalls && finish.resume && finish.resume->provider == Ref(),
      "Gemini returned signature state bound to the model revision");
  const auto value = kit::detail::FromWireJson(Json{{"value", 42}});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Assistant, display});
  request.items.push_back(kit::ModelToolCall{id_a.provider_call_id, "preview.lookup", value});
  request.items.push_back(kit::ModelToolCall{id_b, "agentkit_tool_1", kit::JsonValue{kit::JsonValue::Object{}}});
  request.items.push_back(kit::ModelToolResult{id_b, {}, {}, kit::AgentError{.code = kit::ErrorCode::PermissionDenied, .message = "Tool denied"}});
  request.items.push_back(kit::ModelToolResult{id_a.provider_call_id, {}, value, {}});
  request.tools.push_back({{{"test.added"}, 1, {}}, "2added", "Added", {kit::JsonValue{true}}});
  request.continuation = kit::ModelContinuation{std::move(*finish.resume), 2};
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  Check(server.requests == 1, "Gemini refused to replay private data outside its authorized prefix");
  request.continuation->prefix_items = 4;
  auto next = Take(co_await model.provider->StartTurnAsync(request));
  auto continued = co_await Collect(*next);
  Check(std::get<kit::ModelTextDelta>(continued.front()).utf8 == "Done", "Gemini model received the tool outputs");
  request.continuation.reset();
  auto no_state = Take(co_await model.provider->StartTurnAsync(std::move(request)));
  co_await Collect(*no_state);
}

Task<void> GeminiStructuredAndFailures(TaskScope tasks) {
  const Json schema{{"type", "object"}, {"properties", {{"title", {{"type", "string"}}}}},
      {"required", Json::array({"title"})}, {"additionalProperties", false}};
  Script server;
  const auto model = Take(kit::detail::MakeGeminiGenerateContentModel(GeminiOptions(), tasks, server.Open()));
  server.reply = [&](const auto&, const Json& body) {
    Check(body["generationConfig"]["responseMimeType"] == "application/json" &&
        body["generationConfig"]["responseJsonSchema"] == schema, "Gemini used the native response JSON schema");
    return GeminiText("{\"title\":") + GeminiText("\"Release\"}") + GeminiFrame(Json::array(), "STOP");
  };
  auto request = Request();
  request.output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)};
  auto structured = Take(co_await model.provider->StartTurnAsync(request));
  auto events = co_await Collect(*structured);
  CheckStructuredDeltas(events);
  Check(kit::detail::ToWireJson(std::get<kit::ModelStructuredOutput>(events[events.size() - 2]).value)["title"] == "Release",
      "Gemini parsed complete structured output");
  const kit::ModelToolSpec tool{{{"test.inspect"}, 1, {}}, "inspect", "Inspect", {kit::JsonValue{true}}};
  request.tools = {tool};
  ErrorIs(co_await model.provider->StartTurnAsync(std::move(request)), kit::ErrorCode::Unsupported);
  Check(server.requests == 1, "unsupported Gemini model combinations did not reach the endpoint");

  server.reply = [&](const auto&, const auto&) {
    if (server.requests == 2) return Frame(Json{{"promptFeedback", {{"blockReason", "SAFETY"}}}});
    const Json pending{{"functionCall", {{"id", "pending"}, {"name", "inspect"}, {"args", Json::object()}}}};
    const auto prefix = GeminiText("Partial") + GeminiFrame(Json::array({pending}));
    if (server.requests == 3) return prefix + GeminiFrame(Json::array(), "MAX_TOKENS");
    if (server.requests == 4) return prefix;
    return prefix + GeminiFrame(Json::array(), "STOP") + "data: {\"usageMetadata\":\n";
  };
  auto blocked = Take(co_await model.provider->StartTurnAsync(Request()));
  events = co_await Collect(*blocked);
  Check(events.size() == 1 && std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::Filtered,
      "Gemini prompt safety block finished without a candidate");
  auto with_tool = Request();
  with_tool.tools = {tool};
  auto limited = Take(co_await model.provider->StartTurnAsync(with_tool));
  events = co_await Collect(*limited);
  Check(std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::Length &&
      std::ranges::none_of(events, [](const auto& event) { return std::holds_alternative<kit::ModelToolFinished>(event); }),
      "Gemini preserved partial text and suppressed a call on token exhaustion");
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto incomplete = Take(co_await model.provider->StartTurnAsync(with_tool));
    Check(std::holds_alternative<kit::ModelTextDelta>(*Take(co_await incomplete->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolStarted>(*Take(co_await incomplete->ReadAsync())) &&
        std::holds_alternative<kit::ModelToolArguments>(*Take(co_await incomplete->ReadAsync())),
        "Gemini preserved received parts before abandoning the incomplete stream");
    ErrorIs(co_await incomplete->ReadAsync(), kit::ErrorCode::ProtocolError);
  }
  co_await VerifyCancellation(tasks, server, *model.provider);
}

kit::ModelAttachment GeminiMedia(kit::ArtifactRef ref, std::string mime, File file, int& opens) {
  return {std::move(ref), std::move(mime),
      [file = std::move(file), &opens]() -> kit::Async<AsyncInputStream> {
        ++opens;
        auto source = co_await file.OpenReadAsync();
        Check(source.Succeeded(), "could not open the attachment fixture");
        co_return kit::AgentResult<AsyncInputStream>::Success(std::move(source).Value());
      }};
}

Task<void> GeminiInlineAttachments(TaskScope tasks, bool interactions = false) {
  const struct Fixture {
    std::filesystem::path path;
    ~Fixture() {
      std::error_code error;
      std::filesystem::remove(path / "fixture.png", error);
      std::filesystem::remove(path / "fixture.pdf", error);
      std::filesystem::remove(path, error);
    }
  } fixture{std::filesystem::temp_directory_path() / ("agentkit-gemini-media-" + kit::NewCommandId().value)};
  Check(std::filesystem::create_directory(fixture.path), "could not create attachment fixtures");
  const File image_file((fixture.path / "fixture.png").u8string());
  const File document_file((fixture.path / "fixture.pdf").u8string());
  const Bytes image_bytes{std::byte{0}, std::byte{1}, std::byte{0xfe}, std::byte{0xff}};
  const Bytes document_bytes{std::byte{0x41}, std::byte{0x42}};
  Check(image_file.WriteBytes(image_bytes) && document_file.WriteBytes(document_bytes), "could not write attachment fixtures");
  Script server;
  server.reply = [interactions](const auto&, const Json& body) {
    if (interactions) {
      const auto& input = body["input"];
      Check(input.size() == 3 && input[1]["content"][0]["type"] == "image" &&
          input[1]["content"][0]["mime_type"] == "image/png" && input[1]["content"][0]["data"] == "AAH+/w==" &&
          input[2]["content"][0]["type"] == "document" && input[2]["content"][0]["mime_type"] == "application/pdf" &&
          input[2]["content"][0]["data"] == "QUI=", "Interactions encoded each authorized media version as the correct Content type");
      return InteractionText("Media accepted");
    }
    const auto& history = body["contents"];
    Check(history.size() == 3 && history[1]["parts"][0]["inlineData"]["mimeType"] == "image/png" &&
        history[1]["parts"][0]["inlineData"]["data"] == "AAH+/w==" &&
        history[2]["parts"][0]["inlineData"]["mimeType"] == "application/pdf" &&
        history[2]["parts"][0]["inlineData"]["data"] == "QUI=", "Gemini encoded exact authorized image and file bytes in content order");
    return GeminiText("Media accepted") + GeminiFrame(Json::array(), "STOP");
  };
  const auto configured = [](auto options) {
    options.image_input = true;
    options.file_input = true;
    options.max_inline_bytes = 6;
    return options;
  };
  const auto model = interactions
      ? Take(kit::detail::MakeGeminiInteractionsModel(configured(InteractionsOptions()), tasks, server.Open()))
      : Take(kit::detail::MakeGeminiGenerateContentModel(configured(GeminiOptions()), tasks, server.Open()));
  Check(model.provider->Capabilities().image_input && model.provider->Capabilities().file_input, "Gemini declared configured media support");
  auto request = Request();
  const kit::ArtifactRef image{{"test.image"}, 2};
  const kit::ArtifactRef document{{"test.document"}, 1};
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Context, image});
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::User, document});
  int opens = 0;
  request.attachments.push_back(GeminiMedia(document, "application/pdf", document_file, opens));
  request.attachments.push_back(GeminiMedia(image, "image/png", image_file, opens));
  auto opened = Take(co_await model.provider->StartTurnAsync(request));
  const auto events = co_await Collect(*opened);
  Check(opens == 2 && std::get<kit::ModelTextDelta>(events.front()).utf8 == "Media accepted", "Gemini opened each authorized version once");
  request.attachments.back().artifact.version = 3;
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::InvalidArgument);
  request.attachments.back().artifact = image;
  request.attachments.back().media_type = "application/zip";
  ErrorIs(co_await model.provider->StartTurnAsync(request), kit::ErrorCode::Unsupported);
  Check(opens == 2 && server.requests == 1, "unmatched versions and unsupported media were rejected before reading or sending bytes");
  request.attachments.back().media_type = "image/png";
  request.items.push_back(kit::ModelContent{kit::ModelContent::Role::Context, image});
  ErrorIs(co_await model.provider->StartTurnAsync(std::move(request)), kit::ErrorCode::BudgetExceeded);
  Check(server.requests == 1, "Gemini counted repeated attachment bytes before opening the network");
}

void NativeHttp(std::string base_url) {
  std::exception_ptr failure;
  bool completed = false;
  Application app{
      []() -> View { return Text("AgentKit native HTTP acceptance"); },
      {.application_hooks = {[&](ApplicationContext&) {
        const auto application = UseApplication();
        UseApplicationTaskScope().Launch([&, application]() -> Task<void> {
          try {
            auto options = Options(kit::ChatCompletionsDialect::Compatible);
            options.base_url = base_url;
            options.timeout = 5s;
            auto model = Take(kit::MakeChatCompletionsModel(std::move(options)));
            auto stream = Take(co_await model.provider->StartTurnAsync(Request()));
            auto events = co_await Collect(*stream);
            Check(events.size() == 2 && std::get<kit::ModelTextDelta>(events.front()).utf8 == "Native HTTP stream" &&
                  std::get<kit::ModelFinished>(events.back()).reason == kit::ModelFinishReason::Completed,
                  "native transport did not deliver a valid SSE response");
            completed = true;
          } catch (...) { failure = std::current_exception(); }
          application.Quit();
        });
      }}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  Check(completed, "native HTTP test did not complete");
}

struct LiveResponseRecovery {
  File database;
  bool resume;
};

struct LiveResponseReplay {
  std::size_t requests = 0;
  bool replayed = false;
  Json expected = Json::array();
};

Task<Json> StoredEncryptedReasoning(const std::shared_ptr<kit::SessionStore>& store) {
  const auto image = Take(co_await store->LoadAsync());
  const auto session = std::ranges::find(image.records, "agentkit.session", &kit::StoreValue::collection);
  Check(session != image.records.end() && std::ranges::count(image.records, "agentkit.session", &kit::StoreValue::collection) == 1,
      "Expected one persisted Responses conversation");
  const auto& data = session->data;
  const auto* begin = reinterpret_cast<const std::uint8_t*>(data.data());
  const auto document = Json::from_cbor(begin, begin + data.size());
  const auto& stored = document.at("session").at("runs").at(0).at("continuation");
  Check(stored.is_object() && stored.at("prefix_items").get<std::size_t>() > 0,
      "The stored run did not retain its authorized input boundary");
  const auto bytes = stored.at("state").at("opaque").get<std::vector<std::uint8_t>>();
  const auto positions = Json::parse(bytes.begin(), bytes.end());
  auto reasoning = Json::array();
  for (const auto& items : positions) {
    for (const auto& item : items) {
      Check(kit::detail::String(item, "type") == "reasoning" &&
          !kit::detail::String(item, "encrypted_content").empty(), "The stored Responses state was not encrypted reasoning");
      reasoning.push_back(item);
    }
  }
  Check(!reasoning.empty(), "The stored Responses state did not contain encrypted reasoning");
  co_return reasoning;
}

kit::Async<kit::RunCompletion> ExerciseStructuredOutput(kit::RunContext run, kit::RunInput) {
  const auto schema = Json::parse(R"({
    "type": "object",
    "properties": {
      "name": {"type": "string"},
      "verification": {
        "type": "object",
        "properties": {
          "passed": {"type": "boolean"},
          "verification_count": {"type": "integer"}
        },
        "required": ["passed", "verification_count"],
        "additionalProperties": false
      }
    },
    "required": ["name", "verification"],
    "additionalProperties": false
  })");
  auto result = co_await run.CompleteAsync({
      .instruction = "Return JSON for a release named relay-test whose verification passed with a count of 2. "
          "Use only the requested fields.",
      .output_schema = kit::JsonSchema{kit::detail::FromWireJson(schema)}});
  if (!result.Succeeded()) co_return kit::AgentResult<kit::RunCompletion>::Failure(result.Error());
  const Json expected{{"name", "relay-test"}, {"verification", {{"passed", true}, {"verification_count", 2}}}};
  Check(result.Value().structured && kit::detail::ToWireJson(*result.Value().structured) == expected,
      "The live structured response did not contain the expected verification fields");
  co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Structured verification accepted"});
}

Task<void> ExerciseAgent(TaskScope tasks, kit::ModelRegistration model,
    std::optional<LiveResponseRecovery> recovery, std::shared_ptr<LiveResponseReplay> replay, bool structured) {
  using kit::JsonValue;
  auto require = [](kit::AgentResult<void> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  };
  const auto model_id = model.ref.id;
  std::shared_ptr<kit::SessionStore> store;
  if (recovery) store = Take(co_await agentkit_preview::OpenSessionStoreAsync(recovery->database));
  if (recovery && recovery->resume) replay->expected = co_await StoredEncryptedReasoning(store);
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = tasks,
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"live-provider-test"}); },
      .access = [persisted = recovery.has_value()](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const auto decision = persisted && request.capability == kit::Capability::ExecuteTool
            ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow;
        co_return kit::AgentResult<kit::PolicyDecision>::Success({decision, {}});
      }, .store = store}));
  require(runtime->RegisterModel(std::move(model)));
  const kit::ToolId lookup{"lookup_test_marker"};
  const auto calls = std::make_shared<unsigned>(0);
  require(runtime->RegisterTool({
      .id = lookup, .version = 1, .description = "Get the current verification marker from this test host.",
      .input_schema = {JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}},
          {"additionalProperties", JsonValue{false}}}}},
      .effect = kit::ToolEffect::Read,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest tool) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.execution_data = std::move(tool.arguments)});
      },
      .execute = [calls](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        ++*calls;
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({
            .outcome = kit::ToolOutcome::Succeeded, .model_value = JsonValue{std::string("host-marker-M7Q2")}});
      }}));
  const kit::AgentId agent{"live-test"};
  kit::AgentRegistration registration{.definition = {.ref = {agent, 1}, .display_name = "Live agent test", .model = model_id,
      .requires_model = {.text_stream = true, .tool_calls = !structured, .structured_output = structured}}};
  if (structured) registration.run = ExerciseStructuredOutput;
  else registration.definition.tools = {lookup};
  require(runtime->RegisterAgent(std::move(registration)));
  require(co_await runtime->StartAsync());
  kit::SessionId session;
  if (recovery && recovery->resume) {
    const auto listed = Take(co_await runtime->Sessions().ListAsync({}, {}));
    Check(listed.sessions.size() == 1, "Could not find the persisted Responses conversation");
    session = listed.sessions.front().id;
  } else {
    kit::CreateSessionRequest request{
        .options = {.agent = agent, .retention = recovery ? kit::SessionRetention::Stored : kit::SessionRetention::Ephemeral},
        .initial = kit::SendInput{.text = structured ? "Check the sample release." :
            "Call lookup_test_marker exactly once. Reply with the marker returned by the tool."}};
    const auto created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request)));
    session = std::get<kit::SessionCreated>(created.outcome).session;
  }
  auto conversation = Take(co_await runtime->Sessions().OpenAsync(session));
  std::shared_ptr<const kit::SessionSnapshot> snapshot;
  auto watching = Take(co_await conversation->ObserveAsync([&](auto change) { if (change.Succeeded()) snapshot = change.Value(); }));
  if (recovery) {
    const auto until_approval = std::chrono::steady_clock::now() + 60s;
    while ((!snapshot || (snapshot->summary.active_run && snapshot->summary.pending_interactions == 0)) &&
           std::chrono::steady_clock::now() < until_approval) co_await Delay(30ms);
    if (snapshot && !snapshot->runs.empty() && snapshot->runs.back().failure)
      throw std::runtime_error(snapshot->runs.back().failure->message);
    Check(snapshot && snapshot->runs.size() == 1 && snapshot->interactions.size() == 1 &&
        snapshot->interactions.front().state == kit::InteractionState::Pending && snapshot->interactions.front().run &&
        snapshot->summary.active_run == snapshot->interactions.front().run && *calls == 0 &&
        replay->requests == (recovery->resume ? 0U : 1U), "The tool did not keep its original approval before the continuation");
    const auto approval = snapshot->interactions.front();
    const auto& plan = std::get<kit::ToolApproval>(approval.body);
    std::cout << (recovery->resume ? "Recovered" : "Pending") << " approval " << approval.id.value <<
        " version " << approval.version << " run " << approval.run->value << " call " << plan.call.value <<
        " plan " << plan.plan.value << '\n';
    if (!recovery->resume) {
      watching.Reset();
      require(co_await runtime->CloseAsync());
      static_cast<void>(co_await StoredEncryptedReasoning(store));
      co_return;
    }
    kit::InteractionReply response{approval.id, approval.version, kit::ApprovalDecision::Approve};
    Take(co_await conversation->SubmitAsync(kit::NewCommandId(), std::move(response)));
  }
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  while ((!snapshot || snapshot->summary.active_run) && std::chrono::steady_clock::now() < deadline) co_await Delay(30ms);
  if (snapshot && !snapshot->runs.empty() && snapshot->runs.back().failure) {
    throw std::runtime_error(snapshot->runs.back().failure->message);
  }
  Check(snapshot && snapshot->summary.run_state == kit::RunState::Succeeded, "The live model run did not succeed");
  std::string output;
  for (const auto& message : snapshot->messages) {
    for (const auto& block : message.blocks) {
      if (const auto* body = std::get_if<kit::TextBlock>(&block.body)) output += body->text;
    }
  }
  if (structured) {
    Check(*calls == 0 && output.find("Structured verification accepted") != std::string::npos &&
        output.find("verification_count") == std::string::npos, "The structured output did not remain with the host workflow");
  } else {
    Check(*calls == 1 && output.find("host-marker-M7Q2") != std::string::npos,
        "The live model did not complete exactly one host tool call and return its result");
  }
  watching.Reset();
  require(co_await runtime->CloseAsync());
}

void ConfiguredProvider(std::optional<LiveResponseRecovery> recovery = {}, bool structured = false) {
  const auto* url = std::getenv("HUXERUI_AGENTKIT_BASE_URL");
  const auto* model = std::getenv("HUXERUI_AGENTKIT_MODEL");
  const auto* credential = std::getenv("HUXERUI_AGENTKIT_API_KEY");
  const auto* selected = std::getenv("HUXERUI_AGENTKIT_PROTOCOL");
  const auto* replay_option = std::getenv("HUXERUI_AGENTKIT_REQUIRE_ENCRYPTED_REPLAY");
  const bool require_encrypted_replay = recovery || (replay_option && std::string_view(replay_option) == "1");
  Check(url && *url && model && *model && credential && *credential, "Configure the live model endpoint, model and credential");
  const std::string_view protocol = selected && *selected ? selected : (recovery ? "responses" : "chat-completions");
  Check(protocol == "responses" || protocol == "chat-completions" || protocol == "messages" ||
      protocol == "generate-content" || protocol == "interactions", "Unknown model protocol");
  Check(!require_encrypted_replay || protocol == "responses", "Encrypted replay requires the OpenAI Responses dialect");
  Check(!structured || (protocol == "responses" && !require_encrypted_replay), "The structured test requires Responses");
  std::exception_ptr failure;
  bool completed = false;
  Application app{
      []() -> View { return Text("AgentKit live provider acceptance"); },
      {.application_hooks = {[&](ApplicationContext&) {
        auto scope = UseApplicationTaskScope();
        auto application = UseApplication();
        scope.Launch([&, scope, application]() -> Task<void> {
          try {
            auto token = [key = std::string(credential)]() -> kit::Async<std::string> {
              co_return kit::AgentResult<std::string>::Success(key);
            };
            kit::ModelRegistration registration;
            const auto replay = require_encrypted_replay ? std::make_shared<LiveResponseReplay>() : nullptr;
            if (protocol == "responses") {
              auto options = ResponsesOptions();
              const auto* provider = std::getenv("HUXERUI_AGENTKIT_PROVIDER");
              const auto name = provider ? std::string_view(provider) : std::string_view("openai");
              Check(name == "openai" || name == "deepseek" || name == "glm", "Unknown Responses provider");
              options.dialect = name == "deepseek" ? kit::ResponsesDialect::DeepSeek :
                  name == "glm" ? kit::ResponsesDialect::GlmCompatible : kit::ResponsesDialect::OpenAI;
              if (const auto* state = std::getenv("HUXERUI_AGENTKIT_ALLOW_SERVER_STATE"))
                options.allow_server_state = name == "glm" && std::string_view(state) == "1";
              options.base_url = url;
              options.model = model;
              options.timeout = 30s;
              options.bearer_token = token;
              options.allow_persist_resume_state = recovery.has_value();
              if (recovery) options.ref.id = {std::string("live.responses/") + model};
              if (require_encrypted_replay) {
                Check(name == "openai", "Encrypted replay requires the OpenAI Responses dialect");
                const auto native = kit::detail::NativeModelTransport({});
                auto check_replay = [native, replay](HttpRequest call)
                    -> kit::Async<kit::detail::ModelHttpResponse> {
                  ++replay->requests;
                  const auto* begin = reinterpret_cast<const char*>(call.body.data());
                  const auto body = Json::parse(begin, begin + call.body.size());
                  const auto input = body.find("input");
                  if (input != body.end() && input->is_array() && !body.value("store", true) &&
                      !body.contains("previous_response_id")) {
                    const auto has_tool_result = std::ranges::any_of(*input, [](const auto& item) {
                      return kit::detail::String(item, "type") == "function_call_output";
                    });
                    auto reasoning = Json::array();
                    for (const auto& item : *input) {
                      if (kit::detail::String(item, "type") == "reasoning" &&
                          !kit::detail::String(item, "encrypted_content").empty()) reasoning.push_back(item);
                    }
                    replay->replayed |= has_tool_result && !reasoning.empty() &&
                        (replay->expected.empty() || reasoning == replay->expected);
                  }
                  co_return co_await native(std::move(call));
                };
                registration = Take(kit::detail::MakeResponsesModel(std::move(options), scope, std::move(check_replay)));
              } else {
                registration = Take(kit::MakeResponsesModel(std::move(options)));
              }
            } else if (protocol == "interactions") {
              auto options = InteractionsOptions();
              options.base_url = url;
              options.model = model;
              options.timeout = 30s;
              options.api_key = token;
              if (const auto* state = std::getenv("HUXERUI_AGENTKIT_ALLOW_SERVER_STATE"))
                options.allow_server_state = std::string_view(state) == "1";
              registration = Take(kit::MakeGeminiInteractionsModel(std::move(options)));
            } else if (protocol == "generate-content") {
              auto options = GeminiOptions();
              options.base_url = url;
              options.model = model;
              options.timeout = 30s;
              options.api_key = token;
              registration = Take(kit::MakeGeminiGenerateContentModel(std::move(options)));
            } else if (protocol == "messages") {
              const auto* provider = std::getenv("HUXERUI_AGENTKIT_PROVIDER");
              const auto name = provider ? std::string_view(provider) : std::string_view("anthropic");
              Check(name == "anthropic" || name == "deepseek" || name == "glm", "Unknown Messages provider");
              auto options = MessagesOptions(name == "deepseek" ? kit::MessagesDialect::DeepSeek :
                  name == "glm" ? kit::MessagesDialect::GlmCompatible : kit::MessagesDialect::Anthropic);
              options.base_url = url;
              options.model = model;
              options.timeout = 30s;
              options.api_key = token;
              if (const auto* version = std::getenv("HUXERUI_AGENTKIT_ANTHROPIC_VERSION"); version && *version)
                options.api_version = version;
              registration = Take(kit::MakeMessagesModel(std::move(options)));
            } else {
              auto options = Options(kit::ChatCompletionsDialect::Compatible);
              options.base_url = url;
              options.model = model;
              options.timeout = 30s;
              options.bearer_token = token;
              registration = Take(kit::MakeChatCompletionsModel(std::move(options)));
            }
            co_await ExerciseAgent(scope, std::move(registration), recovery, replay, structured);
            if (recovery && !recovery->resume) {
              std::cout << "Responses encrypted reasoning stored with pending approval\n";
            } else if (replay) {
              Check(replay->replayed && (!recovery || replay->requests == 1),
                  "The stateless Responses continuation did not send the expected encrypted reasoning with the tool result");
              std::cout << "Responses sent encrypted reasoning with the tool result in a stateless request\n";
            }
          } catch (...) { failure = std::current_exception(); }
          completed = true;
          application.Quit();
        });
      }}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  Check(completed, "The live model test timed out");
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--configured-responses-recovery") {
    try {
      const std::string_view phase = argv[3];
      Check(phase == "prepare" || phase == "resume", "Select the prepare or resume phase");
      const bool resume = phase == "resume";
      const File database(argv[2]);
      Check(database.Exists() == resume, "Use a new database for prepare or the same database for resume");
      ConfiguredProvider(LiveResponseRecovery{database, resume});
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 2 && (std::string_view(argv[1]) == "--configured-provider" ||
                    std::string_view(argv[1]) == "--configured-structured")) {
    try {
      const bool structured = std::string_view(argv[1]) == "--configured-structured";
      ConfiguredProvider({}, structured);
      std::cout << (structured ? "Configured Responses provider delivered structured output to the AgentKit workflow\n" :
          "Configured provider streamed a tool call, received the result and completed the AgentKit run\n");
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--native-http") {
    try {
      NativeHttp(argv[2]);
      std::cout << "Native HTTP stream passed\n";
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  bool done = false;
  std::exception_ptr failure;
  Application app{
      []() -> View { return Text("AgentKit model protocol"); },
      {.application_hooks = {[&](ApplicationContext&) {
        auto scope = UseApplicationTaskScope();
        scope.Launch([&, scope]() -> Task<void> {
          try {
            co_await ToolProtocol(scope);
            co_await PrivateState(scope);
            co_await StructuredOutput(scope, kit::ChatOutputFormat::JsonSchema);
            co_await StructuredOutput(scope, kit::ChatOutputFormat::JsonObject);
            co_await ErrorsAndCancellation(scope);
            co_await ResponsesToolProtocol(scope);
            co_await CompatibleResponsesProtocol(scope, kit::ResponsesDialect::DeepSeek, false);
            co_await CompatibleResponsesProtocol(scope, kit::ResponsesDialect::GlmCompatible, false);
            co_await CompatibleResponsesProtocol(scope, kit::ResponsesDialect::GlmCompatible, true);
            co_await ResponsesStructuredAndFailures(scope);
            co_await MessagesToolProtocol(scope, kit::MessagesDialect::Anthropic);
            co_await MessagesToolProtocol(scope, kit::MessagesDialect::DeepSeek);
            co_await MessagesStructuredAndFailures(scope);
            co_await GeminiToolProtocol(scope);
            co_await GeminiStructuredAndFailures(scope);
            co_await GeminiInlineAttachments(scope);
            co_await InteractionsToolProtocol(scope, false);
            co_await InteractionsToolProtocol(scope, true);
            co_await InteractionsStructuredAndFailures(scope);
            co_await GeminiInlineAttachments(scope, true);
          } catch (...) { failure = std::current_exception(); }
          done = true;
        });
      }}}};
  try {
    testing::UiTest ui(app);
    ui.PumpUntil([&] { return done; }, {.timeout = 40s, .step = 2ms, .maximum_frames = 16000});
    if (failure) std::rethrow_exception(failure);
    std::cout << "Chat Completions, Responses, Messages and Gemini protocol fixtures passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
