#pragma once

#include "model_sse.h"

#include <huxerui/agentkit/providers.h>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace huxerui::agentkit::detail {

class PositionedModelProvider : public ModelProvider {
public:
  enum class Layout { Flat, Responses, Interactions };

  virtual Layout ResumeLayout() const noexcept { return Layout::Flat; }
};

bool RebaseModelContinuation(const ModelProvider& provider, ModelContinuation& continuation,
    std::size_t removed, std::size_t inserted);

struct ModelHttpResponse {
  int status;
  std::vector<HttpHeader> headers;
  ModelBodyReader read;
};

using ModelHttpOpen = std::function<Async<ModelHttpResponse>(HttpRequest)>;

const nlohmann::json* Member(const nlohmann::json& object, std::string_view name);

std::string String(const nlohmann::json& object, std::string_view name);

std::uint64_t Count(const nlohmann::json& value);

Bytes ByteString(std::string_view bytes);

std::string ToolContent(const ModelToolResult& tool);

std::string ModelInstructions(const ModelTurnRequest& request);

class ToolNames {
public:
  explicit ToolNames(const ModelTurnRequest& request, bool require_identifier_start = false);

  const std::string& Alias(const std::string& name) const { return aliases_.at(name); }

  const std::string* Offered(std::string_view alias) const;

private:
  std::map<std::string, std::string> aliases_;
  std::map<std::string, std::string, std::less<>> outbound_;
};

AgentResult<std::string> ModelUrl(std::string_view base, bool authenticated, std::string_view path);

AgentResult<HttpRequest> MakeModelPost(std::string url, Duration timeout, std::optional<TimePoint> deadline);
bool ValidModelTimeouts(Duration total, ModelStreamTimeouts timeouts);

std::optional<AgentError> ModelContextError(const nlohmann::json& payload);

enum class ModelCredential { Bearer, ApiKey, GoogleApiKey };
Async<std::unique_ptr<ModelTurnStream>> OpenModelStream(HttpRequest call, TaskScope tasks,
    const ModelHttpOpen& open, const std::function<Async<std::string>()>& credential,
    std::stop_token stop, std::unique_ptr<SseModelProtocol> protocol, ModelStreamTimeouts timeouts,
    ModelCredential method = ModelCredential::Bearer);

ModelHttpOpen NativeModelTransport(std::shared_ptr<HttpClient> client);

AgentResult<ModelRegistration> MakeChatCompletionsModel(
    ChatCompletionsOptions options, TaskScope tasks, ModelHttpOpen transport);

AgentResult<ModelRegistration> MakeResponsesModel(
    ResponsesOptions options, TaskScope tasks, ModelHttpOpen transport);

AgentResult<ModelRegistration> MakeMessagesModel(
    MessagesOptions options, TaskScope tasks, ModelHttpOpen transport);

AgentResult<ModelRegistration> MakeGeminiGenerateContentModel(
    GeminiGenerateContentOptions options, TaskScope tasks, ModelHttpOpen transport);

AgentResult<ModelRegistration> MakeGeminiInteractionsModel(
    GeminiInteractionsOptions options, TaskScope tasks, ModelHttpOpen transport);

} // namespace huxerui::agentkit::detail
