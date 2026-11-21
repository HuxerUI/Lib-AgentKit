#pragma once

/// @file
/// @brief Factories for streaming HTTP model providers.
///
/// Call the selected factory on the HuxerUI application thread and pass its ModelRegistration to
/// AgentRuntime::RegisterModel(). Each factory describes one exact model, dialect and base URL.
/// Use a host-supplied HttpClient to share transport ownership within the same application.

#include <huxerui/agentkit/model.h>
#include <huxerui/http.h>

namespace huxerui::agentkit {

/// @brief Wire conventions for Chat Completions endpoints.
enum class ChatCompletionsDialect {
  /// OpenAI Chat Completions conventions.
  OpenAI,
  /// DeepSeek-compatible request and continuation conventions.
  DeepSeek,
  /// Google's Chat Completions compatibility conventions.
  GeminiCompatible,
  /// GLM compatibility conventions.
  GlmCompatible,
  /// Generic Chat Completions conventions for compatible endpoints.
  Compatible
};
/// @brief Format selected when a Chat Completions turn requests structured output.
enum class ChatOutputFormat {
  /// Text transport without a configured JSON output mode.
  Text,
  /// JSON-object mode with schema constraints supplied in the prompt.
  JsonObject,
  /// Provider-native JSON-schema output mode.
  JsonSchema
};

/// @brief Configuration for one Chat Completions model registration.
struct ChatCompletionsOptions {
  /// Positive registration version and stable model ID for AgentRuntime::RegisterModel().
  ModelProviderRef ref;
  /// Provider-facing model name.
  std::string model;
  /// API base URL. The factory appends /chat/completions.
  std::string base_url;
  /// Wire conventions for the selected endpoint.
  ChatCompletionsDialect dialect = ChatCompletionsDialect::OpenAI;
  /// Optional asynchronous credential source used for the HTTP Authorization header.
  std::function<Async<std::string>()> bearer_token;
  /// Positive request timeout in milliseconds, including response consumption.
  Duration timeout{120000};
  /// Advertises and enables tool calling for this model configuration.
  bool tool_calls = true;
  /// Allows multiple calls per turn when tool_calls is enabled.
  bool multi_tool_calls = true;
  /// Requests a streaming usage report and advertises usage capability.
  bool request_usage = false;
  /// Uses the developer role for host instructions when supported.
  bool developer_role = false;
  /// Optional provider thinking switch for dialects that recognize it.
  std::optional<bool> thinking;
  /// Opt-in to durable storage of compatible adapter continuation bytes.
  bool allow_persist_resume_state = false;
  /// Output mode available to schema-bound model turns.
  ChatOutputFormat output_format = ChatOutputFormat::Text;
};

/// @brief Creates the Chat Completions adapter on the application thread.
/// @param options Model identity, dialect and feature configuration.
/// @param client Optional application-bound HTTP client. Empty creates a client for the adapter.
/// @return Registration suitable for AgentRuntime::RegisterModel(), or a configuration error.
AgentResult<ModelRegistration> MakeChatCompletionsModel(
    ChatCompletionsOptions options, std::shared_ptr<huxerui::HttpClient> client = {});

/// @brief Wire conventions for Responses endpoints.
enum class ResponsesDialect {
  /// OpenAI Responses with optional encrypted reasoning continuation.
  OpenAI,
  /// DeepSeek Responses conventions.
  DeepSeek,
  /// GLM Responses conventions, with optional provider-stored continuation.
  GlmCompatible
};
/// @brief Output mode for schema-bound Responses turns.
enum class ResponsesOutputFormat {
  /// Regular text mode without structured-output capability.
  Text,
  /// JSON-object output with the schema supplied in prompt instructions.
  JsonObject,
  /// Provider-native JSON-schema output.
  JsonSchema,
  /// Selects JSON-object mode for GLM and JSON-schema mode for the other supported dialects.
  Automatic
};

/// @brief Configuration for one Responses model registration.
struct ResponsesOptions {
  /// Positive registration version and stable model ID for AgentRuntime::RegisterModel().
  ModelProviderRef ref;
  /// Provider-facing model name.
  std::string model;
  /// API base URL. The factory appends /responses.
  std::string base_url;
  /// Optional asynchronous credential source used for the HTTP Authorization header.
  std::function<Async<std::string>()> bearer_token;
  /// Positive request timeout in milliseconds, including response consumption.
  Duration timeout{120000};
  /// Advertises and enables tool calling for this model configuration.
  bool tool_calls = true;
  /// Allows multiple calls per turn when tool_calls is enabled.
  bool multi_tool_calls = true;
  /// Requests encrypted reasoning items when using the OpenAI dialect.
  bool encrypted_reasoning = true;
  /// Opt-in to durable storage of compatible adapter continuation bytes.
  bool allow_persist_resume_state = false;
  /// Structured-output mode to advertise and encode.
  ResponsesOutputFormat output_format = ResponsesOutputFormat::Automatic;
  /// Responses dialect used to validate and frame requests.
  ResponsesDialect dialect = ResponsesDialect::OpenAI;
  /// Opt-in for GLM server-side continuation, subject to current Run authorization.
  bool allow_server_state = false;
  /// Requests displayable reasoning summaries on a supporting OpenAI Responses model.
  /// The summary is published in RunSnapshot and follows the Run's retention.
  bool reasoning_summary = false;
};

/// @brief Creates the Responses adapter on the application thread.
/// @param options Model identity, dialect and feature configuration.
/// @param client Optional application-bound HTTP client. Empty creates a client for the adapter.
/// @return Registration suitable for AgentRuntime::RegisterModel(), or a configuration error.
AgentResult<ModelRegistration> MakeResponsesModel(
    ResponsesOptions options, std::shared_ptr<huxerui::HttpClient> client = {});

/// @brief Wire conventions for the Messages streaming protocol.
enum class MessagesDialect {
  /// Anthropic Messages conventions.
  Anthropic,
  /// DeepSeek Messages compatibility conventions.
  DeepSeek,
  /// GLM Messages compatibility conventions.
  GlmCompatible
};
/// @brief Mode used for schema-bound Messages requests.
enum class MessagesOutputFormat {
  /// Text transport without a configured JSON output mode.
  Text,
  /// Requests JSON in the prompt and validates it locally.
  PromptedJson,
  /// Uses Anthropic-native structured output.
  JsonSchema
};

/// @brief Optional reasoning mode requested from a Messages provider.
struct MessagesThinking {
  /// @brief Thinking request mode.
  enum class Mode {
    /// Requests that thinking be disabled.
    Disabled,
    /// Lets the provider decide the thinking budget or enable reasoning using its dialect.
    Adaptive,
    /// Requests a numeric thinking budget on Anthropic.
    Budgeted
  };
  /// Mode encoded when thinking options are present.
  Mode mode = Mode::Disabled;
  /// Numeric thinking budget for Budgeted mode. At least 1024 and below max_output_tokens.
  std::uint32_t budget_tokens = 0;
};

/// @brief Configuration for one Messages model registration.
struct MessagesOptions {
  /// Positive registration version and stable model ID for AgentRuntime::RegisterModel().
  ModelProviderRef ref;
  /// Provider-facing model name.
  std::string model;
  /// API base URL. The factory appends /messages.
  std::string base_url;
  /// Messages dialect used to select fields and supported modes.
  MessagesDialect dialect = MessagesDialect::Anthropic;
  /// Optional asynchronous credential source used for the provider's API-key header.
  std::function<Async<std::string>()> api_key;
  /// API version header in YYYY-MM-DD form.
  std::string api_version = "2023-06-01";
  /// Positive request timeout in milliseconds, including response consumption.
  Duration timeout{120000};
  /// Positive maximum output token count for a turn.
  std::uint32_t max_output_tokens = 8192;
  /// Advertises and enables tool calling for this model configuration.
  bool tool_calls = true;
  /// Allows multiple calls per turn when tool_calls is enabled.
  bool multi_tool_calls = true;
  /// Optional reasoning settings. Empty leaves the provider default in effect.
  std::optional<MessagesThinking> thinking;
  /// Opt-in to durable storage of compatible adapter continuation bytes.
  bool allow_persist_resume_state = false;
  /// Structured-output mode to advertise and use.
  MessagesOutputFormat output_format = MessagesOutputFormat::Text;
};

/// @brief Creates the Messages adapter on the application thread.
/// @param options Model identity, dialect and feature configuration.
/// @param client Optional application-bound HTTP client. Empty creates a client for the adapter.
/// @return Registration suitable for AgentRuntime::RegisterModel(), or a configuration error.
AgentResult<ModelRegistration> MakeMessagesModel(
    MessagesOptions options, std::shared_ptr<huxerui::HttpClient> client = {});

/// @brief Configuration for one streaming Gemini GenerateContent model.
struct GeminiGenerateContentOptions {
  /// Positive registration version and stable model ID for AgentRuntime::RegisterModel().
  ModelProviderRef ref;
  /// Provider-facing model name.
  std::string model;
  /// API base URL. The factory appends the model's streamGenerateContent route.
  std::string base_url;
  /// Optional asynchronous credential source used for the provider's API-key header.
  std::function<Async<std::string>()> api_key;
  /// Positive request timeout in milliseconds, including response consumption.
  Duration timeout{120000};
  /// Advertises and enables tool calling for this model configuration.
  bool tool_calls = true;
  /// Allows multiple calls per turn when tool_calls is enabled.
  bool multi_tool_calls = true;
  /// Advertises schema-bound output for the selected model.
  bool structured_output = true;
  /// Allows a request to combine tools with schema-bound output.
  bool tools_with_structured_output = false;
  /// Allows inline images when the selected model supports them.
  bool image_input = false;
  /// Allows supported inline file types when the selected model supports them.
  bool file_input = false;
  /// Positive limit for the raw inline-media byte budget. At most 14,000,000 bytes.
  std::size_t max_inline_bytes = 8 * 1024 * 1024;
  /// Opt-in to durable storage of compatible adapter continuation bytes.
  bool allow_persist_resume_state = false;
};

/// @brief Creates the Gemini GenerateContent adapter on the application thread.
/// @param options Model identity, dialect and feature configuration.
/// @param client Optional application-bound HTTP client. Empty creates a client for the adapter.
/// @return Registration suitable for AgentRuntime::RegisterModel(), or a configuration error.
AgentResult<ModelRegistration> MakeGeminiGenerateContentModel(
    GeminiGenerateContentOptions options, std::shared_ptr<huxerui::HttpClient> client = {});

/// @brief Configuration for one streaming Gemini Interactions model.
struct GeminiInteractionsOptions {
  /// Positive registration version and stable model ID for AgentRuntime::RegisterModel().
  ModelProviderRef ref;
  /// Provider-facing model name.
  std::string model;
  /// API base URL. The factory appends /interactions.
  std::string base_url;
  /// Optional asynchronous credential source used for the provider's API-key header.
  std::function<Async<std::string>()> api_key;
  /// Positive request timeout in milliseconds, including response consumption.
  Duration timeout{120000};
  /// Advertises and enables tool calling for this model configuration.
  bool tool_calls = true;
  /// Allows multiple calls per turn when tool_calls is enabled.
  bool multi_tool_calls = true;
  /// Advertises schema-bound output for the selected model.
  bool structured_output = true;
  /// Allows a request to combine tools with schema-bound output.
  bool tools_with_structured_output = false;
  /// Allows inline images when the selected model supports them.
  bool image_input = false;
  /// Allows supported inline file types when the selected model supports them.
  bool file_input = false;
  /// Positive limit for the raw inline-media byte budget. At most 14,000,000 bytes.
  std::size_t max_inline_bytes = 8 * 1024 * 1024;
  /// Opt-in for continuation by previous interaction ID when the runtime permits it.
  bool allow_server_state = false;
  /// Opt-in to durable storage of compatible adapter continuation bytes.
  bool allow_persist_resume_state = false;
};

/// @brief Creates the Gemini Interactions adapter on the application thread.
/// @param options Model identity, dialect and feature configuration.
/// @param client Optional application-bound HTTP client. Empty creates a client for the adapter.
/// @return Registration suitable for AgentRuntime::RegisterModel(), or a configuration error.
AgentResult<ModelRegistration> MakeGeminiInteractionsModel(
    GeminiInteractionsOptions options, std::shared_ptr<huxerui::HttpClient> client = {});

} // namespace huxerui::agentkit
