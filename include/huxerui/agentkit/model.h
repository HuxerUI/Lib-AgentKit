#pragma once

/// @file
/// @brief Model provider, streaming turn and versioned context-provider contracts.
/// Implement ModelProvider to adapt a custom protocol, or use the factories in <huxerui/agentkit/providers.h>.
/// AgentRuntime assembles authorized inputs. A ModelTurnStream returns events for one turn and has one reader.

#include <huxerui/agentkit/session.h>
#include <huxerui/stream.h>

#include <stop_token>

namespace huxerui::agentkit {

/// @brief Cancellation capability reported by a provider.
enum class ModelCancellation {
  /// The adapter can stop local reading and release its connection.
  StopReading,
  /// The adapter can request cancellation of provider-side work.
  ProviderCancel
};

/// @brief Features supplied by a provider or required by an AgentDefinition.
struct ModelCapabilities {
  /// Supports incremental text events.
  bool text_stream = false;
  /// Supports tool requests and continuation with their outcomes.
  bool tool_calls = false;
  /// Supports schema-bound structured output.
  bool structured_output = false;
  /// Accepts supported inline image attachments.
  bool image_input = false;
  /// Reports provider usage events.
  bool usage = false;
  /// Accepts supported inline document or file attachments.
  bool file_input = false;
  /// Can issue more than one call in a single turn.
  bool multi_tool_calls = false;
  /// Strongest cancellation behavior the adapter can offer.
  ModelCancellation cancellation = ModelCancellation::StopReading;
};

/// @brief Descriptive provider, model and protocol information associated with a registration.
struct ModelEndpoint {
  /// Provider or compatibility family label.
  std::string provider;
  /// Provider-facing model name.
  std::string model;
  /// Protocol identifier describing the registration.
  std::string protocol;
  /// Optional endpoint descriptor. The provider owns the actual transport configuration.
  std::optional<std::string> base_url;
};

class ModelProvider;

/// @brief Token limits for the model and endpoint bound to this registration.
struct ModelContextLimits {
  /// Combined input and output window. Empty if the endpoint has not reported it.
  std::optional<std::uint64_t> context_window;
  /// Independent input ceiling, if the endpoint specifies one.
  std::optional<std::uint64_t> max_input_tokens;
  /// Independent output ceiling, if the endpoint specifies one.
  std::optional<std::uint64_t> max_output_tokens;

  bool operator==(const ModelContextLimits&) const = default;
};

/// @brief Versioned provider implementation and continuation permissions registered with AgentRuntime.
struct ModelRegistration {
  /// Stable model ID and registration version.
  ModelProviderRef ref;
  /// Descriptive endpoint metadata.
  ModelEndpoint endpoint;
  /// Provider implementation that starts and reads model turns.
  std::shared_ptr<ModelProvider> provider;
  /// Host opt-in for eligible provider-stored continuation.
  bool allow_server_state = false;
  /// Host opt-in to durably store provider-private resume bytes.
  bool allow_persist_resume_state = false;
  /// Known limits enable budgeting and compaction during request assembly.
  ModelContextLimits context_limits;
};

/// @brief Opaque continuation data understood by one exact provider registration.
struct ModelResumeState {
  /// Model registration that produced and can consume the bytes.
  ModelProviderRef provider;
  /// Version of the adapter's private continuation encoding.
  Revision format_version;
  /// Owned private bytes. Keep them scoped to the matching provider and authorized prefix.
  std::vector<std::byte> opaque;
};

/// @brief Provider-private state with the number of historical items it covers.
struct ModelContinuation {
  /// Private state to decode with the bound registration.
  ModelResumeState state;
  /// Length of the covered prefix of ModelTurnRequest::items.
  std::size_t prefix_items;
};

/// @brief Input to a host-controlled completion through RunContext::CompleteAsync().
struct ModelInput {
  /// UTF-8 task instruction for this completion.
  std::string instruction;
  /// Additional source selections to resolve and authorize.
  std::vector<SourceSlice> context;
  /// Explicit tool IDs for an unstructured completion, including built-in Skill tools.
  /// An empty list with include_catalog_tools set to false offers no tools.
  std::vector<ToolId> allowed_tools;
  /// Expected result schema for a structured completion.
  std::optional<JsonSchema> output_schema;
  /// Also offer every authorized tool in the running agent's AgentDefinition::tool_catalogs, including MCP catalogs.
  /// The runtime discovers the current catalog entries each model round. Run restrictions, current permissions,
  /// and any required approvals still apply. Individual catalog tool IDs need not appear in allowed_tools.
  /// Use with a model that supports tool calls and leave output_schema unset. Recovered steps keep their saved
  /// catalog opt-in and recheck current permissions.
  bool include_catalog_tools = false;
};

/// @brief One content item to encode in a model turn.
struct ModelContent {
  /// @brief Origin and intended role of model content.
  enum class Role {
    /// User or host task input.
    User,
    /// Assistant content from a previous completed turn.
    Assistant,
    /// Authorized contextual data such as a selection or verified report.
    Context,
    /// Tool-originated content for a custom provider. Built-in providers require ModelToolResult with a call ID.
    Tool
  };
  /// Role to preserve or explicitly represent in the provider protocol.
  Role role;
  /// UTF-8 text, structured JSON, or a versioned artifact reference.
  std::variant<std::string, JsonValue, ArtifactRef> body;
};

/// @brief Outcome associated with a tool call in a model continuation.
struct ModelToolResult {
  /// Exact provider call ID received from the original ModelToolCall.
  std::string provider_call_id;
  /// Local logical call ID when a tool was accepted.
  std::optional<ToolCallId> call;
  /// Structured result or provider-visible value, when available.
  std::optional<JsonValue> value;
  /// Failure to report for the original call, when applicable.
  std::optional<AgentError> error;
};

/// @brief Complete tool request assembled from a model turn.
struct ModelToolCall {
  /// Unique call identity within the provider's turn.
  std::string provider_call_id;
  /// Tool name from the offered ModelToolSpec.
  std::string name;
  /// Decoded JSON arguments, validated before execution.
  JsonValue arguments;
};

/// @brief Ordered model history entry: content, a tool request, or its result.
using ModelItem = std::variant<ModelContent, ModelToolCall, ModelToolResult>;

/// @brief Authorized reader for the bytes of one exact artifact version.
struct ModelAttachment {
  /// Artifact version bound to this attachment.
  ArtifactRef artifact;
  /// Declared MIME type the provider must support.
  std::string media_type;
  /// Opens the verified bytes without resolving a different artifact version.
  std::function<Async<huxerui::AsyncInputStream>()> open;
};

/// @brief Tool declaration exposed for one model round.
struct ModelToolSpec {
  /// Exact tool version and dynamic catalog binding, if applicable.
  ToolRef tool;
  /// Logical name the provider maps to its protocol's tool identifier.
  std::string name;
  /// Description supplied to the model.
  std::string description;
  /// Schema for JSON tool arguments.
  JsonSchema parameters;
};

/// @brief Authorized instructions loaded for one active skill version.
struct ModelSkillInstruction {
  /// Activated SkillRef.
  SkillRef skill;
  /// UTF-8 Markdown instructions to include in the model request.
  std::string markdown;
};

/// @brief Owned request to ModelProvider::StartTurnAsync().
/// Adapters must preserve tool-call identity, honor cancellation and keep private continuation state within its offered prefix.
struct ModelTurnRequest {
  /// Unique ID of this provider turn.
  TurnId turn;
  /// Run that owns the turn.
  RunId run;
  /// Exact version of the chosen provider registration.
  ModelProviderRef provider;
  /// Optional state corresponding to an authorized prefix of items.
  std::optional<ModelContinuation> continuation;
  /// Whether provider-stored continuation is authorized for this request.
  bool allow_server_state;
  /// Host instructions for the active agent.
  std::string instructions;
  /// Authorized history and current input, including calls and results in order.
  std::vector<ModelItem> items;
  /// Authorized versioned attachments for this turn.
  std::vector<ModelAttachment> attachments;
  /// Tool declarations permitted for this turn.
  std::vector<ModelToolSpec> tools;
  /// Authorized summaries available for activation.
  std::vector<SkillSummary> available_skills;
  /// Versioned instructions for skills already active.
  std::vector<ModelSkillInstruction> skill_instructions;
  /// Expected structured result, when requested.
  std::optional<JsonSchema> output_schema;
  /// Run deadline for participating adapters, when set.
  std::optional<TimePoint> deadline;
  /// Cancellation signal for this Run.
  std::stop_token stop;
};

/// @brief A UTF-8 text increment from the provider.
struct ModelTextDelta {
  /// Complete UTF-8 bytes for this increment.
  std::string utf8;
  /// Whether to publish this increment. Set false for internal structured data.
  bool show_in_transcript = true;
};
/// @brief Opens a provider call whose arguments may arrive in subsequent events.
struct ModelToolStarted {
  /// Unique identifier linking argument, finish and result events.
  std::string provider_call_id;
  /// Offered tool name requested by the model.
  std::string name;
};
/// @brief One byte fragment of a call's JSON argument payload.
struct ModelToolArguments {
  /// Previously opened call to append to.
  std::string provider_call_id;
  /// Argument bytes. A fragment may end before a complete JSON value or UTF-8 character.
  std::string bytes;
};
/// @brief Seals the arguments of one provider call.
struct ModelToolFinished {
  /// Opened call with complete arguments.
  std::string provider_call_id;
};
/// @brief Schema-bound value received for the turn.
struct ModelStructuredOutput {
  /// Provider result validated against the requested schema.
  JsonValue value;
};
/// @brief Usage update for the active model turn.
struct ModelUsage {
  /// Cumulative usage reported so far. The runtime settles the last known totals once.
  Usage usage;
};
/// @brief Provider-reported reason the stream ended.
enum class ModelFinishReason {
  /// The provider completed the response.
  Completed,
  /// The turn finished after issuing calls to be handled before continuation.
  ToolCalls,
  /// The provider reached an output limit.
  Length,
  /// The provider filtered the response.
  Filtered,
  /// The provider declined the request.
  Refused,
  /// The provider exhausted the active model context window.
  ContextLimit
};
/// @brief Terminal event for one model turn.
struct ModelFinished {
  /// Completion, tool-use or interruption reason.
  ModelFinishReason reason;
  /// Optional private state for continuing a compatible, authorized prefix.
  std::optional<ModelResumeState> resume;
};

/// @brief User-visible reasoning summary explicitly supplied by the provider.
/// Adapters only emit text meant for display. Continuation data, signatures and private reasoning remain in ModelResumeState.
struct ModelReasoningSummary {
  /// Non-empty UTF-8 text suitable for display in a collapsible activity section.
  std::string text;
};

/// @brief Event returned in provider order by a ModelTurnStream.
using ModelEvent = std::variant<ModelTextDelta, ModelToolStarted, ModelToolArguments,
    ModelToolFinished, ModelStructuredOutput, ModelUsage, ModelFinished, ModelReasoningSummary>;

/// @brief Final output of a host-controlled completion.
struct ModelReply {
  /// Collected model text, including text withheld from transcript publication.
  std::string text;
  /// Validated structured output, when present.
  std::optional<JsonValue> structured;
  /// Usage attributed to the completion.
  Usage usage;
  /// Readable versions newly published by the tool calls in this completion, in model call and publication order.
  /// References to earlier outputs and approval previews stay with the corresponding ToolResult.
  std::vector<ArtifactRef> artifacts;
};

/// @brief Adapter report of how far a cancellation request reached.
enum class ModelStopOutcome {
  /// Local consumption stopped.
  LocalStopped,
  /// The provider acknowledged cancellation. Custom adapters can report this outcome.
  ProviderConfirmed,
  /// Provider-side state could not be determined.
  ProviderUnknown
};

/// @brief Single-reader stream for one model turn.
/// Finish with one ModelFinished before end of stream. Argument fragments must be associated by provider_call_id.
class ModelTurnStream {
public:
  /// @brief Releases the provider stream and any retained resources.
  virtual ~ModelTurnStream() = default;

  /// @brief Reads the next event, or an empty result at the end of the stream.
  /// @return The next provider event, an empty value for end of stream, or a provider error.
  virtual Async<std::optional<ModelEvent>> ReadAsync() = 0;

  /// @brief Requests shutdown of active work and reports the observed cancellation outcome.
  /// @return The provider's stop outcome for this turn.
  /// @note Implementations should distinguish stopping local delivery from confirmed cancellation at the provider.
  virtual Async<ModelStopOutcome> StopAsync() = 0;
};

/// @brief Host- or library-provided implementation of a model protocol.
class ModelProvider {
public:
  /// @brief Releases the provider implementation.
  virtual ~ModelProvider() = default;

  /// @brief Returns features of the exact registered endpoint/model/protocol combination.
  /// @return Supported features for this registered endpoint, used to validate agent bindings before a turn.
  virtual ModelCapabilities Capabilities() const = 0;

  /// @brief Starts a turn from an owned request and returns its single-reader stream.
  /// @param request Owned model input, conversation prefix and turn cancellation context.
  /// @return The stream that emits this turn's events.
  /// @note Deliver ModelFinished before reporting normal end of stream.
  virtual Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) = 0;
};

/// @brief Authorized resource slices offered to a registered ContextProvider.
struct ContextRequest {
  /// User or agent on whose behalf the context is read.
  ActionParty actor;
  /// Model receiving the authorized data.
  ModelId recipient;
  /// Unresolved slices offered to this provider in input order.
  std::vector<SourceSlice> selected;
  /// Remaining budget for returned context data in bytes.
  std::uint64_t byte_limit;
  /// Cancellation signal for the current context read.
  std::stop_token stop;
};

/// @brief Resolved model content for one exact requested selection.
struct ContextFragment {
  /// Original versioned slice, including requested positions.
  SourceSlice source;
  /// Context-role text or JSON to submit after reauthorization.
  ModelContent content;
};

/// @brief One ContextProvider response to a ContextRequest.
struct ContextSnapshot {
  /// Resolved slices. Omit selections for which the provider is not responsible.
  std::vector<ContextFragment> fragments;
};

/// @brief Host reader for versioned ResourceRef context. The runtime validates contents and preserves input order.
using ContextProvider = std::function<Async<ContextSnapshot>(ContextRequest)>;

} // namespace huxerui::agentkit
