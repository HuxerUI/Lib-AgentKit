#include <huxerui/agentkit.h>
#include <huxerui/agentkit/mcp.h>
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <tuple>

#if defined(AGENTKIT_TEST_MCP_TRANSPORTS)
#include "json_wire.h"
#include <huxerui/http.h>
#include <nlohmann/json.hpp>
#include <array>
#include <charconv>
#include <cstdio>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#endif

using namespace huxerui;
using namespace huxerui::agentkit;

namespace {
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(AgentResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void Take(AgentResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

template<class T>
void CheckError(const AgentResult<T>& result, ErrorCode code, const char* message) {
  if (result.Succeeded()) throw std::runtime_error(message);
  if (result.Error().code != code) throw std::runtime_error(std::string(message) + ": " + result.Error().message);
}

class ScriptStream final : public ModelTurnStream {
public:
  explicit ScriptStream(std::vector<ModelEvent> events) : events_(std::move(events)) {}

  Async<std::optional<ModelEvent>> ReadAsync() override {
    co_await Delay(15ms);
    if (stopped_ || next_ == events_.size()) co_return AgentResult<std::optional<ModelEvent>>::Success(std::nullopt);
    co_return AgentResult<std::optional<ModelEvent>>::Success(std::move(events_[next_++]));
  }

  Async<ModelStopOutcome> StopAsync() override {
    stopped_ = true;
    co_return AgentResult<ModelStopOutcome>::Success(ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<ModelEvent> events_;
  std::size_t next_ = 0;
  bool stopped_ = false;
};

class ScriptModel final : public ModelProvider {
public:
  ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    std::string input;
    std::vector<std::string> context;
    std::vector<ModelContent::Role> content_order;
    auto continuation = false;
    for (const auto& item : request.items) {
      const auto* content = std::get_if<ModelContent>(&item);
      if (content) content_order.push_back(content->role);
      if (content && content->role == ModelContent::Role::User) input = std::get<std::string>(content->body);
      if (content && content->role == ModelContent::Role::Context) context.push_back(std::get<std::string>(content->body));
      if (const auto* result = std::get_if<ModelToolResult>(&item)) {
        Check(result->provider_call_id == "set-request", "tool result must preserve the provider's call ID");
        continuation = true;
      }
    }
    inputs.push_back(input);
    contexts.push_back(std::move(context));
    roles.push_back(std::move(content_order));
    instructions.push_back(std::move(request.instructions));
    std::vector<ModelEvent> events;
    if (!continuation && (input == "write" || input == "partial")) {
      Check(request.tools.size() == 1 && request.tools.front().name == "test.set", "tool must be offered");
      events = {ModelTextDelta{"Preparing a change"}, ModelToolStarted{"set-request", "test.set"},
                ModelToolArguments{"set-request", "{\"value\":"}, ModelToolArguments{"set-request", "\"tested\"}"},
                ModelToolFinished{"set-request"}};
      if (input != "partial") events.push_back(ModelFinished{ModelFinishReason::ToolCalls, {}});
    } else if (!continuation && input.starts_with("chart ")) {
      Check(request.tools.size() == 1 && request.tools.front().name == "test.chart", "chart publisher must be offered");
      const auto arguments = input == "chart invalid" ? R"({"count":"invalid","version":1})" :
          input == "chart fallback" ? R"({"count":3,"version":2})" : R"({"count":3,"version":1})";
      events = {ModelToolStarted{"set-request", "test.chart"}, ModelToolArguments{"set-request", arguments},
                ModelToolFinished{"set-request"}, ModelFinished{ModelFinishReason::ToolCalls, {}}};
    } else {
      events = {ModelTextDelta{"Done"}, ModelFinished{ModelFinishReason::Completed, {}}};
    }
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<ScriptStream>(std::move(events)));
  }

  std::vector<std::string> inputs;
  std::vector<std::vector<std::string>> contexts;
  std::vector<std::vector<ModelContent::Role>> roles;
  std::vector<std::string> instructions;
};

struct HostState {
  std::string value = "original";
  Revision revision = 1;
  int writes = 0;
  bool allow_write = true;
  std::optional<SourceSlice> withdrawn_selection;
  AccessDecision write_policy = AccessDecision::RequireConfirmation;
  std::shared_ptr<ScriptModel> model = std::make_shared<ScriptModel>();
};

std::shared_ptr<AgentRuntime> MakeRuntime(const std::shared_ptr<HostState>& host, std::optional<ToolReviewer> reviewer = {}) {
  RuntimeOptions options{
      .tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"operator"}); },
      .access = [host](PolicyRequest request) -> Async<PolicyDecision> {
        if (host->withdrawn_selection && std::ranges::find(request.sources, *host->withdrawn_selection) != request.sources.end() &&
            (request.capability == Capability::ReadContext || request.capability == Capability::SendToModel))
          co_return AgentResult<PolicyDecision>::Success({AccessDecision::Deny, "Source permission withdrawn"});
        if (request.capability == Capability::ExecuteTool) {
          co_return AgentResult<PolicyDecision>::Success(
              {host->allow_write ? host->write_policy : AccessDecision::Deny, "Write permission"});
        }
        co_return AgentResult<PolicyDecision>::Success({AccessDecision::Allow, {}});
      },
      .generate_session_title = [](SessionTitleRequest) -> Async<std::optional<std::string>> {
        co_await Delay(400ms);
        co_return AgentResult<std::optional<std::string>>::Success("Suggested title");
      },
      .tool_reviewer = std::move(reviewer),
      .tool_resources = [host](ToolInvocation invocation) -> Async<std::vector<ResourceAccess>> {
        const auto mode = invocation.phase == ToolAccessPhase::Prepare ? ResourceAccessMode::Read : ResourceAccessMode::Write;
        co_return AgentResult<std::vector<ResourceAccess>>::Success(
            {{{"test.value"}, ResourceRef{{"test.value"}, std::to_string(host->revision)}, mode, {}}});
      }};
  auto runtime = Take(AgentRuntime::Create(std::move(options)));
  Take(runtime->RegisterModel({{{"fake"}, 1}, {"test", "test", "script", {}}, host->model}));
  Take(runtime->RegisterTool(ToolDefinition{
      .id = {"test.set"}, .version = 1, .description = "Update the test value",
      .input_schema = {JsonValue{JsonValue::Object{
          {"type", JsonValue{std::string("object")}},
          {"required", JsonValue{JsonValue::Array{JsonValue{std::string("value")}}}}}}},
      .effect = ToolEffect::WriteResource,
      .prepare = [](ToolPrepareContext context, ToolRequest request) -> Async<PreparedChange> {
        Check(context.readable.size() == 1 && context.readable.front().mode == ResourceAccessMode::Read, "read-only prepare");
        co_return AgentResult<PreparedChange>::Success({
            .preview = {.title = "Update test value"},
            .targets = {{{"test.value"}, context.readable.front().current}},
            .execution_data = std::move(request.arguments), .undo_support = UndoSupport::None});
      },
      .execute = [host](ToolContext context, PreparedToolCall plan) -> Async<ToolExecutionResult> {
        auto access = context.Resources();
        Check(access.size() == 1 && access.front().mode == ResourceAccessMode::Write, "exact write grant");
        Check(access.front().current == plan.change.targets.front().expected, "execution baseline");
        host->value = std::get<std::string>(std::get<JsonValue::Object>(plan.request.arguments.value).at("value").value);
        ++host->writes;
        ++host->revision;
        co_return AgentResult<ToolExecutionResult>::Success(
            {.outcome = ToolOutcome::Succeeded, .model_value = JsonValue{host->value}});
      }}));
  Take(runtime->RegisterAgent({{.ref = {{"test.general"}, 1}, .display_name = "General", .model = {"fake"},
                                .tools = {{"test.set"}}}}));
  Take(runtime->RegisterAgent({{.ref = {{"test.selection"}, 1}, .display_name = "Side chat", .model = {"fake"}}}));
  return runtime;
}

CreateSessionRequest Request(std::string text) {
  return {.options = {.agent = {"test.general"}, .retention = SessionRetention::Ephemeral},
          .initial = SendInput{.text = std::move(text)}};
}

Task<void> WaitUntil(std::function<bool()> condition, const char* message) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (condition()) co_return;
    co_await Delay(10ms);
  }
  throw std::runtime_error(message);
}

class AttachmentModel final : public ModelProvider {
public:
  struct Turn {
    RunId run;
    std::vector<ArtifactRef> references;
    std::vector<std::pair<ArtifactRef, std::string>> media;
  };

  ModelCapabilities Capabilities() const override { return {.text_stream = true, .image_input = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    Turn seen{.run = request.run};
    for (const auto& item : request.items) {
      const auto* content = std::get_if<ModelContent>(&item);
      if (content) {
        if (const auto* ref = std::get_if<ArtifactRef>(&content->body)) seen.references.push_back(*ref);
      }
    }
    for (const auto& attachment : request.attachments) {
      Check(attachment.media_type == "image/png", "The runtime did not preserve the approved media type");
      auto stream = Take(co_await attachment.open());
      std::string bytes;
      for (;;) {
        auto read = co_await stream.ReadAsync(2);
        Check(read.Succeeded(), "The authorized model stream could not be read");
        if (read.Value().empty()) break;
        bytes.append(reinterpret_cast<const char*>(read.Value().data()), read.Value().size());
      }
      seen.media.emplace_back(attachment.artifact, std::move(bytes));
    }
    turns.push_back(std::move(seen));
    const auto index = turns.size();
    while (hold_turn == index && !request.stop.stop_requested()) co_await Delay(5ms);
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<ScriptStream>(
        std::vector<ModelEvent>{ModelTextDelta{"Done"}, ModelFinished{ModelFinishReason::Completed, {}}}));
  }

  std::size_t hold_turn = 0;
  std::vector<Turn> turns;
};

Task<void> VerifyModelAttachments() {
  const ModelId media_model{"attachment.image"};
  const ModelId text_model{"attachment.text"};
  const ProjectId project{"media-project"};
  const ArtifactRef first{{"preview-image"}, 1};
  const ArtifactRef second{first.id, 2};
  const ArtifactRef imported{{"picked-image"}, 1};
  const ToolId publish{"attachment.publish"};
  auto model = std::make_shared<AttachmentModel>();
  bool allow_send = true;
  bool hold_read = false;
  unsigned opens = 0;
  unsigned imports = 0;
  auto runtime = Take(AgentRuntime::Create({
      .tasks = UseApplicationTaskScope(), .principal = [] { return AgentResult<PrincipalId>::Success({"operator"}); },
      .access = [&](PolicyRequest request) -> Async<PolicyDecision> {
        const bool denied = !allow_send && request.capability == Capability::SendToModel && !request.sources.empty();
        co_return AgentResult<PolicyDecision>::Success({denied ? AccessDecision::Deny : AccessDecision::Allow, "Fixture policy"});
      },
      .artifact_store = {.open = [&](ArtifactResolveRequest request) -> Async<AsyncInputStream> {
        ++opens;
        Check(request.actor.kind == ActionParty::Kind::Agent && request.storage_handle ==
              (request.artifact == imported ? "3" : std::to_string(request.artifact.version)),
              "Opening did not use the authorized artifact version");
        co_return AgentResult<AsyncInputStream>::Success(AsyncInputStream::FromReader(
            [&, bytes = "v" + request.storage_handle, offset = std::size_t{0}](std::stop_token stop, std::size_t limit) mutable
                -> Task<IoResult<Bytes>> {
              while (hold_read && !stop.stop_requested()) co_await Delay(5ms);
              const auto chunk = std::string_view(bytes).substr(offset, limit);
              offset += chunk.size();
              const auto data = std::as_bytes(std::span(chunk));
              co_return IoResult<Bytes>::Success(Bytes(data.begin(), data.end()));
            }));
      }, .resolve_staged_attachment = [&](ActionParty actor, SessionId session, ArtifactId id) -> Async<ArtifactDraft> {
        ++imports;
        Check(actor.principal == PrincipalId{"operator"} && actor.kind == ActionParty::Kind::User &&
              !session.value.empty() && id == imported.id, "Staged attachment resolution lost the user submission");
        co_return AgentResult<ArtifactDraft>::Success({.id = std::move(id), .type = "image/png",
            .name = "Picked image", .storage_handle = "3", .size_bytes = 2});
      }}}));
  Take(runtime->RegisterModel({{media_model, 1}, {"test", "media", "script", {}}, model}));
  Take(runtime->RegisterModel({{text_model, 1}, {"test", "text", "script", {}}, std::make_shared<ScriptModel>()}));
  Take(runtime->RegisterTool({.id = publish, .version = 1, .description = "Publish an image fixture",
      .input_schema = {JsonValue{true}}, .effect = ToolEffect::StageArtifact,
      .prepare = [](ToolPrepareContext, ToolRequest call) -> Async<PreparedChange> {
        co_return AgentResult<PreparedChange>::Success({.execution_data = std::move(call.arguments)});
      },
      .execute = [&](ToolContext, PreparedToolCall call) -> Async<ToolExecutionResult> {
        const bool revision = std::get<bool>(call.change.execution_data.value);
        co_return AgentResult<ToolExecutionResult>::Success({.outcome = ToolOutcome::Succeeded,
            .new_artifacts = {{.id = first.id, .expected_latest = revision ? std::optional<Revision>{1} : std::nullopt,
                .type = "image/png", .name = "Original image", .storage_handle = revision ? "2" : "1", .size_bytes = 2}}});
      }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"attachment.source"}, 1}, .display_name = "Publisher",
      .model = media_model, .tools = {publish}}, .run = [&](RunContext context, RunInput input) -> Async<RunCompletion> {
        auto published = co_await context.CallToolAsync({publish, JsonValue{input.input.text == "Revise"}});
        if (!published.Succeeded()) co_return AgentResult<RunCompletion>::Failure(published.Error());
        co_return AgentResult<RunCompletion>::Success({.summary = "Published"});
      }}));
  Take(runtime->RegisterAgent({{.ref = {{"attachment.reader"}, 1}, .display_name = "Reader", .model = media_model}}));
  Take(co_await runtime->StartAsync());
  const auto start = [&](AgentId agent, SendInput input) -> Task<std::shared_ptr<Conversation>> {
    CreateSessionRequest request{.options = {.agent = std::move(agent), .project = project, .retention = SessionRetention::Ephemeral},
        .initial = std::move(input)};
    const auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request))).outcome);
    co_return Take(co_await runtime->Sessions().OpenAsync(created.session));
  };
  const auto done = [](const std::shared_ptr<Conversation>& session) {
    return WaitUntil([session] { return !session->Snapshot()->summary.active_run; }, "Attachment fixture run did not finish");
  };
  auto source = co_await start({"attachment.source"}, {.text = "Publish"});
  co_await done(source);
  Take(co_await source->SendAsync(NewCommandId(), {.text = "Revise"}));
  co_await done(source);
  Check(source->Snapshot()->artifacts == std::vector{first, second}, "The fixture did not publish two image versions");

  auto reader = co_await start({"attachment.reader"}, {.text = "Read the old image", .attachments = {first, first}});
  co_await done(reader);
  const auto reader_id = reader->Snapshot()->summary.id;
  reader = Take(co_await runtime->Sessions().OpenAsync(reader_id));
  const auto transcript = reader->Snapshot();
  Check(std::ranges::count_if(transcript->messages.front().blocks, [&](const ContentBlock& block) {
          const auto* item = std::get_if<ArtifactBlock>(&block.body);
          return item && item->artifact == first;
        }) == 2, "User attachment versions were not retained in the transcript");
  Check(model->turns.size() == 1 && model->turns.back().references == std::vector{first, first} &&
        model->turns.back().media == std::vector{std::pair{first, std::string("v1")}} && opens == 1,
        "Duplicate references did not pin the original version and share its read");
  CheckError(co_await reader->SendAsync(NewCommandId(), {.text = "Text-only model", .attachments = {first}},
      {.model = ModelProviderRef{text_model, 1}}), ErrorCode::Unsupported, "Text-only model accepted an image");

  model->hold_turn = 2;
  const auto run = std::get<RunAccepted>(Take(co_await reader->SendAsync(NewCommandId(), {.text = "Another task"})).outcome).run;
  co_await WaitUntil([&] { return model->turns.size() == 2; }, "The second request did not start");
  Check(model->turns.back().references.empty(), "A later task resent historical media");
  const auto queued = std::get<InputQueued>(Take(co_await reader->QueueAsync(NewCommandId(),
      {.text = "Queued image", .attachments = {first}})).outcome);
  Take(co_await reader->SteerAsync(NewCommandId(), run, {.text = "Use the new image", .attachments = {second}}));
  co_await WaitUntil([&] { return reader->Snapshot()->summary.queued_inputs == 1; }, "The attachment was not queued");
  model->hold_turn = 3;
  co_await WaitUntil([&] { return model->turns.size() >= 3; }, "The steered attachment did not reach the active run");
  Check(model->turns[2].run == run && model->turns[2].media == std::vector{std::pair{second, std::string("v2")}},
        "Steering replaced the Run or resolved the wrong image");
  allow_send = false;
  Take(co_await runtime->RefreshAccessAsync());
  reader = Take(co_await runtime->Sessions().OpenAsync(reader_id));
  model->hold_turn = 0;
  co_await WaitUntil([&] { return reader->Snapshot()->queue.front().state == QueueItemState::Rejected; },
      "Queued attachment was not rejected after loss of model authorization");
  auto recovered = Take(co_await reader->ReadQueuedDraftAsync(queued.item));
  Check(model->turns.size() == 3 && recovered.references.size() == 1 && recovered.references.front().reference &&
        std::get<ArtifactRef>(*recovered.references.front().reference) == first, "The rejected attachment lost its recoverable reference");
  CheckError(co_await reader->RetryAsync(NewCommandId(), run), ErrorCode::PermissionDenied,
      "Retry omitted the previously applied attachment and bypassed its authorization check");

  allow_send = true;
  hold_read = true;
  Take(co_await runtime->RefreshAccessAsync());
  reader = Take(co_await runtime->Sessions().OpenAsync(reader_id));
  Take(co_await reader->SendAsync(NewCommandId(), {.text = "Read while authorized", .attachments = {first}}));
  co_await WaitUntil([&] { return opens == 3; }, "The blocked byte read was not entered");
  allow_send = false;
  Take(co_await runtime->RefreshAccessAsync());
  reader = Take(co_await runtime->Sessions().OpenAsync(reader_id));
  hold_read = false;
  co_await done(reader);
  Check(reader->Snapshot()->runs.back().state == RunState::Failed && model->turns.size() == 3,
        "Bytes read during revocation reached the provider");

  const auto import_command = NewCommandId();
  const CreateSessionRequest draft{.options = {.agent = {"attachment.reader"}, .project = project,
      .retention = SessionRetention::Ephemeral}, .initial = SendInput{.staged_attachments = {imported.id}}};
  CheckError(co_await runtime->Sessions().CreateAsync(import_command, draft), ErrorCode::PermissionDenied,
      "A staged image bypassed model authorization");
  Check(imports == 0, "Host persisted an attachment before model authorization");
  allow_send = true;
  Take(co_await runtime->RefreshAccessAsync());
  const auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(import_command, draft)).outcome);
  const auto replayed = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(import_command, draft)).outcome);
  auto picked = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_await done(picked);
  const auto sent = picked->Snapshot();
  Check(replayed.session == created.session && imports == 1 && sent->artifacts == std::vector{imported} &&
        sent->summary.title_source == SessionTitleSource::Placeholder &&
        model->turns.size() == 4 && model->turns.back().media == std::vector{std::pair{imported, std::string("v3")}},
        "First-message attachment was not published and sent once by the original command");
  Check(std::ranges::any_of(sent->messages.front().blocks, [&](const ContentBlock& block) {
    const auto* artifact = std::get_if<ArtifactBlock>(&block.body);
    return artifact && artifact->artifact == imported && artifact->name == "Picked image";
  }), "First-message attachment was not visible at its committed version");
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifyHostResourceContext() {
  const SourceSlice editor{ResourceRef{{"workspace/settings.cpp"}, "editor-7"}, {},
      SourceTextRange{PartId{"buffer"}, 7, 16}};
  const SourceSlice disk{ResourceRef{{"workspace/README.md"}, "file-1"}};
  const ModelId recipient{"context.model"};
  const std::uint64_t budget = 96;
  const auto model = std::make_shared<ScriptModel>();
  bool permit_send = true;
  bool pause_read = false;
  bool reading = false;
  enum class Reply { Current, WrongVersion, Missing, Oversized } reply = Reply::Current;
  std::size_t opens = 0;
  std::size_t editor_reads = 0;
  auto runtime = Take(AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"editor-user"}); },
      .access = [&](PolicyRequest request) -> Async<PolicyDecision> {
        const bool outbound = request.capability == Capability::SendToModel && !request.sources.empty();
        if (outbound) Check(request.recipient_model == recipient, "Context authorization lost the model recipient");
        co_return AgentResult<PolicyDecision>::Success({outbound && !permit_send ? AccessDecision::Deny : AccessDecision::Allow, {}});
      }}));
  Take(runtime->RegisterModel({{recipient, 1}, {"test", "editor context", "script", {}}, model}));
  Take(runtime->RegisterContextProvider({"disk"}, [&](ContextRequest request) -> Async<ContextSnapshot> {
    ++opens;
    Check(request.actor.principal == PrincipalId{"editor-user"} && request.actor.run && request.recipient == recipient &&
        request.byte_limit == budget, "Context request lost its Run, principal, model, or quota");
    reading = true;
    while (pause_read && !request.stop.stop_requested()) co_await Delay(5ms);
    ContextSnapshot snapshot;
    if (std::ranges::find(request.selected, disk) != request.selected.end())
      snapshot.fragments.push_back({disk, {ModelContent::Role::Context, std::string("disk")}});
    co_return AgentResult<ContextSnapshot>::Success(std::move(snapshot));
  }));
  Take(runtime->RegisterContextProvider({"editor"}, [&](ContextRequest request) -> Async<ContextSnapshot> {
    ++editor_reads;
    Check(request.selected == std::vector<SourceSlice>{editor}, "Provider did not receive the exact remaining editor slice");
    if (reply == Reply::Missing) co_return AgentResult<ContextSnapshot>::Success({});
    auto echoed = editor;
    if (reply == Reply::WrongVersion) std::get<ResourceRef>(echoed.source).version = "editor-8";
    ModelContent content{ModelContent::Role::Context, std::string("editor")};
    if (reply == Reply::Oversized) content.body = JsonValue{std::string(budget, 'X')};
    ContextSnapshot snapshot;
    snapshot.fragments.push_back({std::move(echoed), std::move(content)});
    co_return AgentResult<ContextSnapshot>::Success(std::move(snapshot));
  }));
  const AgentDefinition agent{.ref = {{"context.default"}, 1}, .display_name = "Context test", .model = recipient,
      .context = {{"disk"}, {"editor"}}, .budget = {.context_bytes = budget}};
  Take(runtime->RegisterAgent({agent}));
  auto hosted = agent;
  hosted.ref.id = {"context.hosted"};
  Take(runtime->RegisterAgent({.definition = hosted,
      .run = [&](RunContext run, RunInput) -> Async<RunCompletion> {
        ModelInput input{.instruction = "Read scoped sources", .context = {editor, disk}};
        auto answer = co_await run.CompleteAsync(std::move(input));
        if (!answer.Succeeded()) co_return AgentResult<RunCompletion>::Failure(answer.Error());
        co_return AgentResult<RunCompletion>::Success({.summary = answer.Value().text});
      }}));
  Take(co_await runtime->StartAsync());
  const auto create = [&](AgentId agent_id, std::vector<SourceSlice> context) -> Task<std::shared_ptr<Conversation>> {
    CreateSessionRequest request{.options = {.agent = std::move(agent_id), .retention = SessionRetention::Ephemeral},
        .initial = SendInput{.text = "Use these sources", .context = std::move(context)}};
    auto accepted = Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request)));
    co_return Take(co_await runtime->Sessions().OpenAsync(std::get<SessionCreated>(accepted.outcome).session));
  };
  std::vector<SourceSlice> selected{editor, disk};
  auto chat = co_await create(agent.ref.id, selected);
  co_await WaitUntil([&] { return chat->Snapshot()->summary.run_state == RunState::Succeeded; }, "Editor resource run did not finish");
  const auto workflow = co_await create(hosted.ref.id, {});
  co_await WaitUntil([&] { return workflow->Snapshot()->summary.run_state == RunState::Succeeded; }, "Hosted context run did not finish");
  Check(model->contexts == std::vector<std::vector<std::string>>{{"editor", "disk"}, {"editor", "disk"}},
      "Model did not receive only the requested context in input order");
  const auto send = [&]() -> Task<void> {
    SendInput input{.text = "Use these sources", .context = selected};
    Take(co_await chat->SendAsync(NewCommandId(), std::move(input)));
  };
  const auto failed = [&](ErrorCode expected) -> Task<void> {
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "Invalid resource context did not settle");
    const auto snapshot = chat->Snapshot();
    const auto& run = snapshot->runs.back();
    Check(run.state == RunState::Failed && run.failure && run.failure->code == expected && model->contexts.size() == 2,
        "Invalid or unauthorized resource context reached the model, or returned the wrong error");
  };
  for (const auto [condition, code] : {std::pair{Reply::WrongVersion, ErrorCode::ProtocolError},
      {Reply::Missing, ErrorCode::NotFound}, {Reply::Oversized, ErrorCode::BudgetExceeded}}) {
    reply = condition;
    co_await send();
    co_await failed(code);
  }
  reply = Reply::Current;
  const auto earlier_opens = opens;
  permit_send = false;
  co_await send();
  co_await failed(ErrorCode::PermissionDenied);
  Check(opens == earlier_opens, "Resource provider was called without model-send permission");
  permit_send = true;
  pause_read = true;
  reading = false;
  const auto before_revocation = editor_reads;
  co_await send();
  co_await WaitUntil([&] { return reading; }, "Resource provider did not begin its read");
  const auto session = chat->Snapshot()->summary.id;
  permit_send = false;
  Take(co_await runtime->RefreshAccessAsync());
  chat = Take(co_await runtime->Sessions().OpenAsync(session));
  pause_read = false;
  co_await failed(ErrorCode::Conflict);
  Check(editor_reads == before_revocation, "Revocation did not stop later context providers");
  Take(co_await runtime->CloseAsync());
}

void VerifyContentApplication() {
  using VisibleConversation = State<std::shared_ptr<Conversation>>;
  std::shared_ptr<Conversation> rendered_chat;
  std::exception_ptr error;
  bool complete = false;
  const ContentTypeRef chart_type{"reporting.breakdown", 1};
  const ContentTypeRef fallback_type{chart_type.id, 2};
  const JsonValue integer_schema{JsonValue::Object{{"type", JsonValue{std::string("integer")}}}};
  const JsonSchema chart_schema{JsonValue{JsonValue::Object{
      {"type", JsonValue{std::string("object")}},
      {"required", JsonValue{JsonValue::Array{JsonValue{std::string("count")}}}},
      {"properties", JsonValue{JsonValue::Object{{"count", integer_schema}}}}}}};
  Application application{
      []() -> View {
        if (auto state = UseService<VisibleConversation>(); state && state->Get()) return AgentChat(state->Get());
        return Text("Opening a report");
      }, {.application_hooks = {[&](ApplicationContext& context) {
        auto visible = std::make_shared<VisibleConversation>(std::shared_ptr<Conversation>{});
        State<bool> show_legend{true};
        context.Provide(visible);
        auto selections = std::make_shared<AgentChatSelection>();
        context.Provide(selections);
        auto tasks = UseApplicationTaskScope();
        tasks.Launch([&, tasks, visible, show_legend, selections]() -> Task<void> {
          try {
            auto runtime = Take(AgentRuntime::Create({
                .tasks = tasks,
                .principal = [] { return AgentResult<PrincipalId>::Success({"report-user"}); },
                .access = [](PolicyRequest) -> Async<PolicyDecision> {
                  co_return AgentResult<PolicyDecision>::Success({AccessDecision::Allow, {}});
                }}));
            Take(runtime->RegisterModel({{{"report.model"}, 1}, {"test", "reporting", "script", {}},
                std::make_shared<ScriptModel>()}));
            auto render = [show_legend](RenderInput<CustomBlock> input) -> View {
              Check(input.origin && input.origin->message.version && !input.origin->block.value.empty(),
                  "renderer did not receive a versioned origin");
              const auto& data = std::get<JsonValue::Object>(input.value->data.value);
              const auto count = std::get<std::int64_t>(data.at("count").value);
              const auto tasks = UseApplicationTaskScope();
              auto follow_up = [tasks, dispatch = std::move(input.dispatch), target = *input.origin] {
                tasks.Launch([dispatch, target]() -> Task<void> {
                  Take(co_await dispatch(NewCommandId(), CustomBlockAction{target, "follow-up", JsonValue{true}}));
                });
              };
              return Column{
                  Button(show_legend.Get() ? "Hide operations" : "Show operations")
                      .OnClick([show_legend] { show_legend = !show_legend.Get(); }),
                  Button("Review this report").OnClick(std::move(follow_up)),
                  Text(show_legend.Get() ? "Operations: " + std::to_string(count) : "Operations hidden")};
            };
            Take(runtime->RegisterContentComponent({chart_type, chart_schema, render, {{
                "follow-up", {JsonValue{true}}, [](ContentActionContext, JsonValue) -> Async<ContentActionPlan> {
                  co_return AgentResult<ContentActionPlan>::Success(SendInput{.text = "Review the selected report"});
                }}}}));
            Take(runtime->RegisterContentComponent({fallback_type, chart_schema, {}}));
            CheckError(runtime->RegisterContentComponent({chart_type, chart_schema, render}),
                ErrorCode::Conflict, "component schema version was overwritten");
            const ToolId publisher{"test.chart"};
            Take(runtime->RegisterTool({
                .id = publisher, .version = 1, .description = "Publish a report breakdown",
                .input_schema = {JsonValue{true}}, .effect = ToolEffect::Read,
                .prepare = [](ToolPrepareContext, ToolRequest) -> Async<PreparedChange> {
                  co_return AgentResult<PreparedChange>::Success({.preview = {.title = "Draw report"}});
                },
                .execute = [chart_type](ToolContext, PreparedToolCall plan) -> Async<ToolExecutionResult> {
                  const auto& arguments = std::get<JsonValue::Object>(plan.request.arguments.value);
                  auto type = chart_type;
                  type.schema_version = std::get<std::int64_t>(arguments.at("version").value);
                  co_return AgentResult<ToolExecutionResult>::Success({
                      .outcome = ToolOutcome::Succeeded,
                      .blocks = {{{}, CustomBlock{type, JsonValue{JsonValue::Object{{"count", arguments.at("count")}}},
                          "Report breakdown without a renderer"}}}});
                }}));
            const AgentId authorized{"report.editor"};
            const AgentId unapproved{"report.reader"};
            Take(runtime->RegisterAgent({{.ref = {authorized, 1}, .display_name = "Report editor",
                .model = {"report.model"}, .tools = {publisher}, .content_types = {chart_type, fallback_type}}}));
            Take(runtime->RegisterAgent({{.ref = {unapproved, 1}, .display_name = "Report reader",
                .model = {"report.model"}, .tools = {publisher}}}));
            const AgentId selection_agent{"report.selection"};
            Take(runtime->RegisterAgent({{.ref = {selection_agent, 1}, .display_name = "Report selection",
                .model = {"report.model"}}}));
            Take(co_await runtime->StartAsync());
            *selections = {runtime, selection_agent};
            const auto publish = [&](AgentId agent, std::string text) -> Async<std::shared_ptr<Conversation>> {
              auto created = Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), {
                  .options = {.agent = agent, .retention = SessionRetention::Ephemeral},
                  .initial = SendInput{.text = std::move(text)}}));
              auto opened = Take(co_await runtime->Sessions().OpenAsync(std::get<SessionCreated>(created.outcome).session));
              co_await WaitUntil([&] { return !opened->Snapshot()->summary.active_run; }, "chart run did not finish");
              co_return AgentResult<std::shared_ptr<Conversation>>::Success(std::move(opened));
            };
            const auto invalid = Take(co_await publish(authorized, "chart invalid"))->Snapshot();
            Check(invalid->tools.back().reason && invalid->tools.back().reason->code == ErrorCode::InvalidArgument &&
                !invalid->tools.back().reason->fields.empty(), "invalid component data was not checked against the schema");
            const auto denied = Take(co_await publish(unapproved, "chart restricted"))->Snapshot();
            Check(denied->tools.back().reason && denied->tools.back().reason->code == ErrorCode::PermissionDenied,
                "an agent published an undeclared component");
            auto rendered = Take(co_await publish(authorized, "chart report"));
            Take(co_await rendered->SendAsync(NewCommandId(), {.text = "chart fallback"}));
            co_await WaitUntil([&] { return !rendered->Snapshot()->summary.active_run; }, "fallback chart run did not finish");
            rendered_chat = std::move(rendered);
            *visible = rendered_chat;
          } catch (...) { error = std::current_exception(); }
          complete = true;
        });
      }}}};
  testing::UiTest ui(application, {.viewport = {900.0F, 820.0F}});
  ui.PumpUntil([&] { return complete; }, {.timeout = 5s, .step = 5ms});
  if (error) std::rethrow_exception(error);
  const auto has_operations = [&] { return ui.Find(testing::UiSelector::Text("Operations: 3")).Exists(); };
  ui.PumpUntil(has_operations);
  Check(ui.Find(testing::UiSelector::Text("Report breakdown without a renderer")).Exists(),
      "unavailable renderer did not fall back to text");
  ui.Find(testing::UiSelector::Text("Hide operations")).Tap();
  ui.PumpUntil([&] { return ui.Find(testing::UiSelector::Text("Operations hidden")).Exists(); });
  Check(!has_operations() && rendered_chat->Snapshot()->runs.size() == 2,
      "local chart interaction changed the conversation");
  ui.Find(testing::UiSelector::Text("Review this report")).Tap();
  ui.PumpUntil([&] { return rendered_chat->Snapshot()->runs.size() == 3 && !rendered_chat->Snapshot()->summary.active_run; });
  const auto snapshot = rendered_chat->Snapshot();
  Check(std::ranges::any_of(snapshot->messages, [](const Message& message) {
    return message.author == MessageAuthor::User && std::ranges::any_of(message.blocks, [](const auto& block) {
      const auto* text = std::get_if<TextBlock>(&block.body);
      return text && text->text == "Review the selected report";
    });
  }), "the renderer action did not submit its prepared user input");
}

std::optional<InteractionSnapshot> Approval(const Conversation& session) {
  auto snapshot = session.Snapshot();
  auto found = std::ranges::find(snapshot->interactions, huxerui::agentkit::InteractionState::Pending,
                                 &InteractionSnapshot::state);
  return found == snapshot->interactions.end() ? std::nullopt : std::optional<InteractionSnapshot>(*found);
}

Task<void> VerifyFormConstraints(std::shared_ptr<AgentRuntime> runtime) {
  using Object = JsonValue::Object;
  using Array = JsonValue::Array;
  const JsonValue label{Object{{"type", JsonValue{std::string("string")}},
      {"minLength", JsonValue{std::int64_t{2}}}, {"maxLength", JsonValue{std::int64_t{2}}}}};
  const JsonValue number{Object{{"type", JsonValue{std::string("number")}},
      {"minimum", JsonValue{-0.125}}, {"maximum", JsonValue{9007199254740992.0}}}};
  const JsonValue values{Object{{"type", JsonValue{std::string("array")}}, {"items", number},
      {"minItems", JsonValue{std::int64_t{2}}}, {"maxItems", JsonValue{std::int64_t{2}}}}};
  const JsonSchema schema{JsonValue{Object{{"type", JsonValue{std::string("object")}},
      {"required", JsonValue{Array{JsonValue{std::string("label")}, JsonValue{std::string("values")}}}},
      {"properties", JsonValue{Object{{"label", label}, {"values", values}}}}}}};
  const AgentId agent{"test.bounded-form"};
  Take(runtime->RegisterAgent({{.ref = {agent, 1}, .display_name = "Bounded form", .model = {"fake"}},
      [schema](RunContext run, RunInput) -> Async<RunCompletion> {
        auto answer = co_await run.AskAsync({.title = "Submit two values", .schema = schema});
        if (!answer.Succeeded()) co_return AgentResult<RunCompletion>::Failure(answer.Error());
        co_return AgentResult<RunCompletion>::Success({.structured = std::move(answer.Value().value)});
      }}));
  Take(co_await runtime->StartAsync());
  auto request = Request("Check field constraints");
  request.options.agent = agent;
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request))).outcome);
  auto chat = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "bounded form did not open");
  const auto form = *Approval(*chat);
  const auto submit = [chat, form](std::string name, Array items) {
    return chat->SubmitAsync(NewCommandId(), {form.id, form.version, JsonValue{Object{
        {"label", JsonValue{std::move(name)}}, {"values", JsonValue{std::move(items)}}}}});
  };
  const auto check_fields = [](const auto& result, const char* length, const char* count, const char* bound) {
    CheckError(result, ErrorCode::InvalidArgument, "out-of-range form was accepted");
    const auto has = [&](std::string_view path, std::string_view code) {
      return std::ranges::any_of(result.Error().fields, [&](const FieldError& field) {
        return field.path == path && field.code == code;
      });
    };
    Check(has("/label", length) && has("/values", count) && has("/values/0", bound),
        "bounds did not identify the invalid field and constraint");
  };
  check_fields(co_await submit("A", Array{JsonValue{std::int64_t{9007199254740993LL}}}),
      "minLength", "minItems", "maximum");
  check_fields(co_await submit("ABC", Array{JsonValue{-0.25}, JsonValue{0.0}, JsonValue{1.0}}),
      "maxLength", "maxItems", "minimum");
  Check(Approval(*chat) && Approval(*chat)->version == form.version, "field errors closed or changed the pending form");
  Take(co_await submit("\xe6\x96\xb9\xe6\xa1\x88", Array{JsonValue{-0.125}, JsonValue{std::int64_t{9007199254740992LL}}}));
  co_await WaitUntil([&] { return chat->Snapshot()->summary.run_state == RunState::Succeeded; },
      "UTF-8 text and exact boundary values did not settle the form");
  Take(co_await runtime->CloseAsync());
}

enum class ReviewCase { Approve, Deny, Defer, Failed, Timeout, NoReviewer, HumanRequired, Revoked, Superseded, Cancelled, Stale };

Task<void> VerifyToolReview(ReviewCase scenario) {
  struct Probe {
    unsigned calls = 0;
    std::optional<ToolReviewRequest> reviewed;
  };
  auto probe = std::make_shared<Probe>();
  auto host = std::make_shared<HostState>();
  host->write_policy = AccessDecision::AllowReview;
  std::optional<ToolReviewer> reviewer;
  if (scenario != ReviewCase::NoReviewer) {
    reviewer = ToolReviewer{"test.reviewer", 3, scenario == ReviewCase::Timeout ? 80ms : 30s,
        [probe, host, scenario](ToolReviewRequest request) -> Async<ToolReviewResult> {
      ++probe->calls;
      Check(request.invocation.phase == ToolAccessPhase::Execute && !request.plan.value.empty() &&
            request.invocation.prepared_targets.size() == 1 && request.preview.targets.size() == 1 &&
            request.preview.targets.front().target == request.invocation.prepared_targets.front().target &&
            request.preview.targets.front().expected == request.invocation.prepared_targets.front().expected &&
            request.preview.title == "Update test value" && host->writes == 0,
            "review received the frozen plan before execution");
      probe->reviewed = request;
      if (scenario == ReviewCase::Timeout || scenario == ReviewCase::Cancelled || scenario == ReviewCase::Revoked ||
          scenario == ReviewCase::Superseded) co_await Delay(1h);
      if (scenario == ReviewCase::HumanRequired) host->write_policy = AccessDecision::RequireConfirmation;
      if (scenario == ReviewCase::Stale) ++host->revision;
      if (scenario == ReviewCase::Failed) {
        co_return AgentResult<ToolReviewResult>::Failure({.code = ErrorCode::ProviderFailure, .message = "Unavailable"});
      }
      const auto decision = scenario == ReviewCase::Deny ? ToolReviewDecision::Deny :
          scenario == ReviewCase::Defer ? ToolReviewDecision::DeferToUser : ToolReviewDecision::Approve;
      co_return AgentResult<ToolReviewResult>::Success({decision, "Local reviewer decision"});
    }};
  }
  auto runtime = MakeRuntime(host, std::move(reviewer));
  Take(co_await runtime->StartAsync());
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), Request("write"))).outcome);
  auto session = Take(co_await runtime->Sessions().OpenAsync(created.session));
  if (scenario == ReviewCase::Cancelled || scenario == ReviewCase::Revoked || scenario == ReviewCase::Superseded) {
    co_await WaitUntil([&] { return probe->reviewed.has_value(); }, "review did not start");
    const auto awaiting = session->Snapshot();
    Check(awaiting->tools.front().state == ToolCallState::Reviewing && awaiting->interactions.empty() &&
          awaiting->tools.front().review->outcome == ToolReviewOutcome::Pending,
          "reviewing was projected without a human interaction");
    if (scenario == ReviewCase::Cancelled) {
      Take(co_await session->CancelAsync(NewCommandId(), *created.initial_run));
    } else if (scenario == ReviewCase::Superseded) {
      Take(co_await session->SteerAsync(NewCommandId(), *created.initial_run, {.text = "Use a safe alternative"}));
    } else {
      host->allow_write = false;
      Take(co_await runtime->RefreshAccessAsync());
      session = Take(co_await runtime->Sessions().OpenAsync(created.session));
    }
    Check(probe->reviewed->stop.stop_requested(), "plan invalidation did not signal reviewer cancellation");
  }
  const bool manual = scenario == ReviewCase::Defer || scenario == ReviewCase::Failed || scenario == ReviewCase::Timeout ||
      scenario == ReviewCase::NoReviewer || scenario == ReviewCase::HumanRequired;
  if (manual) {
    co_await WaitUntil([&] { return Approval(*session).has_value(); }, "review did not fall back to manual approval");
    const auto pending = *Approval(*session);
    const auto tool = session->Snapshot()->tools.front();
    const auto& binding = std::get<ToolApproval>(pending.body);
    Check(binding.plan == tool.plan && binding.call == tool.id && host->writes == 0,
          "fallback preserved the plan and held its effect");
    Take(co_await session->SubmitAsync(NewCommandId(), {pending.id, pending.version, ApprovalDecision::Approve}));
  }
  co_await WaitUntil([&] { return !session->Snapshot()->summary.active_run; }, "tool review run did not finish");
  const auto final = session->Snapshot();
  const auto& tool = final->tools.front();
  Check(host->writes == (scenario == ReviewCase::Approve || manual ? 1 : 0), "review changed the number of tool writes");
  Check(final->interactions.size() == (manual ? 1 : 0), "unexpected approval interaction for tool review");
  if (scenario == ReviewCase::NoReviewer) {
    Check(!tool.review && probe->calls == 0, "no reviewer produced an audit entry");
  } else {
    const auto state = scenario == ReviewCase::Approve ? ToolReviewOutcome::Approved :
        scenario == ReviewCase::Deny ? ToolReviewOutcome::Denied :
        (manual && scenario != ReviewCase::HumanRequired) ? ToolReviewOutcome::Escalated : ToolReviewOutcome::Invalidated;
    Check(probe->calls == 1 && tool.review && tool.review->outcome == state &&
          tool.plan == tool.review->plan && tool.review->plan == probe->reviewed->plan &&
          tool.review->reviewer == "test.reviewer" && tool.review->reviewer_version == 3 &&
          probe->reviewed->stop.stop_requested(), "tool review did not retain the final decision scoped to the original plan");
  }
  if (scenario == ReviewCase::Stale) {
    Check(tool.state == ToolCallState::Conflict && tool.reason->code == ErrorCode::Conflict,
          "an approved review did not enforce the plan baseline");
  }
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifyRuntime(std::shared_ptr<AgentRuntime> runtime, std::shared_ptr<HostState> host) {
  Take(co_await runtime->StartAsync());
  std::shared_ptr<const SessionPage> directory;
  auto list_subscription = Take(co_await runtime->Sessions().ObserveListAsync(
      {.include_ephemeral = true}, [&](auto page) { directory = Take(std::move(page)); }));
  auto command = NewCommandId();
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(command, Request("write"))).outcome);
  auto repeat = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(command, Request("write"))).outcome);
  Check(repeat.session == created.session && repeat.initial_run == created.initial_run, "create is atomic and idempotent");
  CheckError(co_await runtime->Sessions().CreateAsync(command, Request("different")), ErrorCode::Conflict,
             "different commands must not reuse the original ID");
  auto session = Take(co_await runtime->Sessions().OpenAsync(created.session));
  auto snapshot = session->Snapshot();
  Check(snapshot->summary.title == "write" && snapshot->summary.title_source == SessionTitleSource::FirstInput,
        "first message supplies the title in the create transaction");
  auto saw_streaming = false;
  auto subscription = Take(co_await session->ObserveAsync([&](auto update) {
    auto latest = Take(std::move(update));
    saw_streaming |= std::ranges::any_of(latest->messages,
        [](const auto& message) { return message.state == MessageState::Streaming; });
  }));
  const auto rename = NewCommandId();
  const auto event_cursor = snapshot->event_sequence;
  auto observed_rename = false;
  auto events = Take(co_await session->ObserveEventsAsync(event_cursor, [&](auto update) {
    const auto batch = Take(std::move(update));
    observed_rename |= std::ranges::any_of(batch->events, [&](const auto& event) {
      const auto* summary = std::get_if<SessionSummary>(&event.body);
      return event.sequence > event_cursor && event.command == rename && summary && summary->title == "Manual name";
    });
  }));
  Take(co_await runtime->Sessions().UpdateMetadataAsync(rename, created.session,
      snapshot->summary.metadata_revision, RenameSession{"Manual name"}));
  co_await WaitUntil([&] { return Approval(*session).has_value(); }, "approval pending");
  co_await WaitUntil([&] { return observed_rename; }, "the in-memory event tail did not observe the rename");
  events.Reset();
  Check(saw_streaming && directory && directory->sessions.size() == 1, "session and catalog observations delivered");
  Check(host->writes == 0 && session->Snapshot()->summary.run_state == RunState::WaitingForInput, "write waits for approval");
  auto queued = std::get<InputQueued>(Take(co_await session->QueueAsync(NewCommandId(), SendInput{.text = "queued"})).outcome);
  auto pending = *Approval(*session);
  auto approve = NewCommandId();
  auto accepted = Take(co_await session->SubmitAsync(approve, {pending.id, pending.version, ApprovalDecision::Approve}));
  auto second = Take(co_await session->SubmitAsync(approve, {pending.id, pending.version, ApprovalDecision::Approve}));
  Check(accepted.stamp.commit == second.stamp.commit, "approval replay returns the original revision");
  CheckError(co_await session->SubmitAsync(NewCommandId(), {pending.id, pending.version, ApprovalDecision::Approve}),
             ErrorCode::AlreadyClosed, "second approval cannot write");
  co_await WaitUntil([&] { return !session->Snapshot()->summary.active_run && session->Snapshot()->runs.size() == 2; },
                     "tool and queued continuation finished");
  snapshot = session->Snapshot();
  Check(host->writes == 1 && host->value == "tested", "tool applied exactly once");
  Check(snapshot->queue.front().id == queued.item && snapshot->queue.front().state == QueueItemState::Started &&
        snapshot->queue.front().run == snapshot->runs.back().id, "queued input used its own run");

  const auto pending_run = std::get<RunAccepted>(Take(co_await session->SendAsync(NewCommandId(), {.text = "write"})).outcome).run;
  co_await WaitUntil([&] { return Approval(*session).has_value(); }, "second write pending");
  auto stale = *Approval(*session);
  auto steer = std::get<RunSteered>(Take(co_await session->SteerAsync(NewCommandId(), pending_run,
                                                                  {.text = "Use a safe alternative"})).outcome);
  CheckError(co_await session->SubmitAsync(NewCommandId(), {stale.id, stale.version, ApprovalDecision::Approve}),
             ErrorCode::AlreadyClosed, "steer invalidated the prepared plan");
  co_await WaitUntil([&] { return !session->Snapshot()->summary.active_run; }, "steer applied before next turn");
  snapshot = session->Snapshot();
  Check(snapshot->runs.back().steering.front().message == steer.message &&
        snapshot->runs.back().steering.front().state == SteerState::Applied &&
        host->model->inputs.back() == "Use a safe alternative" && host->writes == 1, "steer kept original run and skipped write");

  auto cancelling = std::get<RunAccepted>(Take(co_await session->SendAsync(NewCommandId(), {.text = "write"})).outcome).run;
  co_await WaitUntil([&] { return Approval(*session).has_value(); }, "third write pending");
  Take(co_await session->CancelAsync(NewCommandId(), cancelling));
  co_await WaitUntil([&] { return session->Snapshot()->summary.run_state == RunState::Cancelled; }, "run cancelled");
  Check(!Approval(*session) && host->writes == 1, "cancellation closed approval without executing");
  CheckError(co_await session->SteerAsync(NewCommandId(), cancelling, {.text = "Keep this draft"}),
             ErrorCode::AlreadyClosed, "late steer stays outside the completed run");

  Take(co_await session->SendAsync(NewCommandId(), {.text = "partial"}));
  co_await WaitUntil([&] { return session->Snapshot()->summary.run_state == RunState::Failed; }, "incomplete stream rejected");
  snapshot = session->Snapshot();
  Check(snapshot->runs.back().failure->code == ErrorCode::ProtocolError &&
        snapshot->messages.back().state == MessageState::Interrupted &&
        snapshot->summary.title == "Manual name" && host->writes == 1, "truncated stream and late title handled");
  subscription.Reset();
  list_subscription.Reset();
  Take(co_await runtime->CloseAsync());
}

Task<void> VerifySideChat(std::shared_ptr<AgentRuntime> runtime, std::shared_ptr<HostState> host) {
  Take(co_await runtime->StartAsync());
  const std::string selected = "\xe6\x96\xb9\xe6\xa1\x88";
  const auto parent = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(
      NewCommandId(), Request("before " + selected + " after"))).outcome);
  auto main = Take(co_await runtime->Sessions().OpenAsync(parent.session));
  co_await WaitUntil([&] { return !main->Snapshot()->summary.active_run; }, "source conversation did not finish");
  const auto message = main->Snapshot()->messages.front();
  const SourceSlice source{message.ref, {}, SourceTextRange{message.blocks.front().id, 7, 13}};
  const auto response = main->Snapshot()->messages.back();
  const SourceSlice second{response.ref, {}, SourceTextRange{response.blocks.front().id, 0, 2}};
  const std::vector sources{source, second};
  const std::vector excerpts{selected, std::string("Do")};
  Check(Take(co_await main->ReadSelectionsAsync(sources)) == excerpts, "reading a selection changed its fragment order");
  CreateSessionRequest side{
      .options = {.agent = {"test.selection"}, .retention = SessionRetention::Ephemeral},
      .initial = SendInput{.text = "Explain the selection"},
      .side_chat = SideChatSeed{parent.session, sources}};
  auto invalid = side;
  invalid.side_chat->selected.front().text_range->begin_utf8 = 8;
  CheckError(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(invalid)),
             ErrorCode::InvalidArgument, "a UTF-8 character was split");
  auto stale = side;
  ++std::get<MessageRef>(stale.side_chat->selected.back().source).version;
  CheckError(co_await main->ReadSelectionsAsync(stale.side_chat->selected), ErrorCode::Conflict,
             "reading returned a partial selection after a stale fragment");
  CheckError(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(stale)),
             ErrorCode::Conflict, "selection used a different message version");
  auto unsafe = side;
  unsafe.options.agent = {"test.general"};
  CheckError(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(unsafe)),
             ErrorCode::PermissionDenied, "a write-capable agent entered the side chat");
  const auto command = NewCommandId();
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(command, side)).outcome);
  auto replayed = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(command, side)).outcome);
  Check(created.session == replayed.session, "side chat was recreated on replay");
  auto changed = side;
  changed.side_chat->selected.front().text_range->end_utf8 = 10;
  CheckError(co_await runtime->Sessions().CreateAsync(command, std::move(changed)), ErrorCode::Conflict,
             "a side chat command was reused for a different selection");
  auto chat = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "side chat did not finish");
  const auto snapshot = chat->Snapshot();
  const std::vector<std::string> quoted_excerpts{"Selected passage:\n" + selected, "Selected passage:\nDo"};
  Check(snapshot->origin->kind == SessionOriginKind::SideChat && snapshot->origin->source_session == parent.session &&
      snapshot->origin->side_chat_selection == sources && host->model->contexts.back() == quoted_excerpts,
      "the model or session received a broader selection");
  using Role = ModelContent::Role;
  Check(host->model->roles.back() == std::vector{Role::Context, Role::Context, Role::User},
        "selection context should precede the latest question");
  const auto reply = snapshot->messages.back();
  const SourceSlice conclusion{reply.ref, {}, SourceTextRange{reply.blocks.front().id, 0, 2}};
  auto input = SendInput{.text = "Bring back the selected conclusion", .side_chat_conclusions = {conclusion}};
  auto send = NewCommandId();
  auto attached = Take(co_await main->SendAsync(send, input));
  auto attached_again = Take(co_await main->SendAsync(send, input));
  Check(attached.stamp.commit == attached_again.stamp.commit, "replaying a conclusion started another run");
  co_await WaitUntil([&] { return !main->Snapshot()->summary.active_run; }, "the conclusion run did not finish");
  Check(host->model->contexts.back() == std::vector<std::string>{"Attached side-chat conclusion:\nDo"},
        "the main model got the wrong conclusion");
  Check(host->model->roles.back().back() == Role::User, "the active question should follow the conclusion");
  Check(host->model->instructions.back().find("Attached side-chat conclusions") != std::string::npos,
        "the model did not get a description of the attached conclusion");
  auto visible = Take(co_await runtime->Sessions().OpenAsync(parent.session));
  const auto view = visible->Snapshot();
  const auto& blocks = view->messages[2].blocks;
  const auto recorded = std::get<SideChatConclusion>(blocks.back().body);
  Check(recorded.text == "Do" && recorded.sources == sources, "incorrect conclusion provenance");

  const auto steering_run = std::get<RunAccepted>(Take(co_await main->SendAsync(NewCommandId(), {.text = "write"})).outcome).run;
  co_await WaitUntil([&] { return Approval(*main).has_value(); }, "the run to steer did not prepare its approval");
  auto supplement = Take(co_await main->SteerAsync(NewCommandId(), steering_run, input));
  const auto steering = std::get<RunSteered>(supplement.outcome);
  co_await WaitUntil([&] { return !main->Snapshot()->summary.active_run; }, "the steered run did not finish");
  const auto steered = main->Snapshot();
  Check(steering.run == steering_run && steered->runs.back().steering.back().state == SteerState::Applied &&
      host->model->contexts.back().size() == 2 && host->writes == 0, "steering did not apply the conclusion to the selected run");

  const auto active = std::get<RunAccepted>(Take(co_await main->SendAsync(NewCommandId(), {.text = "write"})).outcome).run;
  co_await WaitUntil([&] { return Approval(*main).has_value(); }, "main approval was not independently pending");
  const auto queued = std::get<InputQueued>(Take(co_await main->QueueAsync(NewCommandId(), input)).outcome);
  host->withdrawn_selection = second;
  Take(co_await runtime->RefreshAccessAsync());
  main = Take(co_await runtime->Sessions().OpenAsync(parent.session));
  chat = Take(co_await runtime->Sessions().OpenAsync(created.session));
  Check(Take(co_await main->ReadSelectionAsync(source)) == selected, "an unrelated fragment lost access");
  CheckError(co_await main->ReadSelectionsAsync(sources), ErrorCode::PermissionDenied,
             "reading returned a partial selection after one fragment was revoked");
  Check(chat->Snapshot()->origin->side_chat_selection == std::vector{source} && chat->Snapshot()->messages.size() == 1 &&
      main->Snapshot()->messages[2].blocks.size() == 1, "revoked source or conclusion remained visible");
  Take(co_await main->CancelAsync(NewCommandId(), active));
  co_await WaitUntil([&] {
    auto current = main->Snapshot();
    auto entry = std::ranges::find(current->queue, queued.item, &QueuedInput::id);
    return entry != current->queue.end() && entry->state == QueueItemState::Rejected;
  }, "queue did not reject a revoked conclusion");
  Take(co_await main->SendAsync(NewCommandId(), {.text = "Summarize the open task"}));
  co_await WaitUntil([&] { return !main->Snapshot()->summary.active_run; }, "main conversation did not resume");
  Check(host->model->contexts.back().empty(), "revoked conclusion reached a later model round");
  Check(host->model->instructions.back().find("Attached side-chat conclusions") == std::string::npos,
        "a revoked conclusion remained in the model's instructions");
  Take(co_await runtime->CloseAsync());
}
const ToolCatalogId external_catalog{"test.external"};
const ToolId catalog_tool{"test.external/report"};

class CatalogModel final : public ModelProvider {
public:
  ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true, .multi_tool_calls = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    std::vector<ToolRef> offered;
    for (const auto& spec : request.tools) offered.push_back(spec.tool);
    turns.push_back(std::move(offered));
    std::vector<ModelEvent> events;
    const auto result = std::ranges::find_if(request.items, [](const auto& item) { return std::holds_alternative<ModelToolResult>(item); });
    if (result != request.items.end()) {
      for (auto item = result; item != request.items.end(); ++item) {
        const auto* tool = std::get_if<ModelToolResult>(&*item);
        if (!tool) continue;
        Check(tool->provider_call_id == "external-report" ||
              (parallel_reports && (tool->provider_call_id == "first-report" || tool->provider_call_id == "second-report")),
              "Catalog tool reply lost its provider call identity");
        results.push_back(*tool);
        if (!parallel_reports) break;
      }
      events = {ModelTextDelta{"Report completed"}, ModelFinished{ModelFinishReason::Completed, {}}};
    } else {
      Check(request.tools.size() == 1 && request.tools.front().tool.id == catalog_tool, "Invalid catalog entries reached the model");
      while (hold_round && !request.stop.stop_requested()) co_await Delay(2ms);
      const auto append_report = [&](const std::string& id, const std::string& arguments) {
        events.emplace_back(ModelToolStarted{id, request.tools.front().name});
        events.emplace_back(ModelToolArguments{id, arguments});
        events.emplace_back(ModelToolFinished{id});
      };
      if (parallel_reports) {
        append_report("first-report", R"({"section":"first"})");
        append_report("second-report", R"({"section":"second"})");
      } else {
        append_report("external-report", "{}");
      }
      events.emplace_back(ModelFinished{ModelFinishReason::ToolCalls, {}});
    }
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<ScriptStream>(std::move(events)));
  }

  bool hold_round = false;
  bool parallel_reports = false;
  std::vector<std::vector<ToolRef>> turns;
  std::vector<ModelToolResult> results;
};

struct CatalogEffects {
  std::vector<Revision> started;
  std::vector<Revision> finished;
  bool hold = false;
  unsigned poisoned = 0;
};

ToolDefinition CatalogTool(const std::shared_ptr<CatalogEffects>& effects, Revision version, bool poisoned = false) {
  return {.id = catalog_tool, .version = version, .description = "Prepare the requested report",
      .input_schema = {JsonValue{true}}, .effect = ToolEffect::Read,
      .prepare = [](ToolPrepareContext, ToolRequest) -> Async<PreparedChange> {
        co_return AgentResult<PreparedChange>::Success({.preview = {.title = "Catalog report"}});
      },
      .execute = [effects, version, poisoned](ToolContext context, PreparedToolCall call) -> Async<ToolExecutionResult> {
        Check(call.tool == ToolRef{catalog_tool, version, external_catalog}, "Catalog tool received a different version");
        if (poisoned) ++effects->poisoned;
        effects->started.push_back(version);
        while (effects->hold && !context.StopToken().stop_requested()) co_await Delay(2ms);
        effects->finished.push_back(version);
        co_return AgentResult<ToolExecutionResult>::Success({.outcome = ToolOutcome::Succeeded, .model_value = JsonValue{true}});
      }};
}

Task<void> VerifyToolCatalog() {
  auto model = std::make_shared<CatalogModel>();
  auto effects = std::make_shared<CatalogEffects>();
  auto runtime = Take(AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"operator"}); },
      .access = [](PolicyRequest request) -> Async<PolicyDecision> {
        auto allowed = AccessDecision::Allow;
        if (request.capability == Capability::ExecuteTool) allowed = AccessDecision::RequireConfirmation;
        if (request.capability == Capability::DiscoverTool && request.tool->id.value.ends_with("/private"))
          allowed = AccessDecision::Deny;
        co_return AgentResult<PolicyDecision>::Success({allowed, {}});
      }}));
  Take(runtime->RegisterModel({{{"test.catalog.model"}, 1}, {"test", "catalog", "script", {}}, model}));
  Take(runtime->RegisterToolCatalog(external_catalog));
  CheckError(runtime->RegisterToolCatalog({"test.external/nested"}), ErrorCode::Conflict, "Catalog prefixes must be exclusive");
  CheckError(runtime->RegisterTool(CatalogTool(effects, 1)), ErrorCode::Conflict, "Fixed tools cannot use a reserved prefix");
  Take(runtime->RegisterAgent({{.ref = {{"test.catalog.agent"}, 1}, .display_name = "Catalog test", .model = {"test.catalog.model"},
      .tool_catalogs = {external_catalog}}}));
  auto publish = [runtime, effects](Revision revision, Revision version, bool poisoned = false) {
    ToolCatalogSnapshot descriptor{revision, {}};
    if (version) {
      descriptor.tools.push_back(CatalogTool(effects, version, poisoned));
      auto hidden = CatalogTool(effects, version);
      hidden.id = {"test.external/private"};
      descriptor.tools.push_back(std::move(hidden));
    }
    return runtime->ReplaceToolCatalogAsync(external_catalog, std::move(descriptor));
  };
  Take(co_await publish(1, 1));
  Take(co_await runtime->StartAsync());
  model->hold_round = true;
  CreateSessionRequest request{.options = {.agent = {"test.catalog.agent"}, .retention = SessionRetention::Ephemeral},
      .initial = SendInput{.text = "Old model snapshot"}};
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request))).outcome);
  auto chat = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_await WaitUntil([&] { return model->turns.size() == 1; }, "Model did not see the first catalog");
  Take(co_await publish(2, 2));
  model->hold_round = false;
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "Catalog change interrupted the complete round");
  Check(model->results.size() == 1 && model->results.back().error && model->results.back().error->code == ErrorCode::Conflict &&
        model->turns[0] == std::vector<ToolRef>{{catalog_tool, 1, external_catalog}} &&
        model->turns[1] == std::vector<ToolRef>{{catalog_tool, 2, external_catalog}} &&
        effects->started.empty(), "Stale binding executed or the new catalog did not reach the next round");

  Take(co_await chat->SendAsync(NewCommandId(), {.text = "Approve the report"}));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "Catalog approval did not reach the user");
  const auto stale = *Approval(*chat);
  Take(co_await publish(3, 3));
  const auto rejected = co_await chat->SubmitAsync(NewCommandId(), {stale.id, stale.version, ApprovalDecision::Approve});
  CheckError(rejected, ErrorCode::AlreadyClosed, "Replaced tool's approval must be closed");
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "Stale catalog approval did not settle");
  const auto invalidated = chat->Snapshot();
  Check(invalidated->interactions.back().state == huxerui::agentkit::InteractionState::Invalidated &&
        invalidated->tools.back().state == ToolCallState::Conflict && effects->started.empty(),
        "Catalog replacement did not invalidate the pending plan");

  Take(co_await chat->SendAsync(NewCommandId(), {.text = "Approve the current report"}));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "Current catalog approval did not appear");
  const auto current = *Approval(*chat);
  Take(co_await publish(4, 3, true));
  auto invalid = CatalogTool(effects, 3, true);
  invalid.description = "Changed while keeping the same version";
  ToolCatalogSnapshot invalid_snapshot{5, {}};
  invalid_snapshot.tools.push_back(std::move(invalid));
  CheckError(co_await runtime->ReplaceToolCatalogAsync(external_catalog, std::move(invalid_snapshot)),
             ErrorCode::Conflict, "Changed metadata must not reuse an active version");
  CheckError(co_await publish(4, 4), ErrorCode::Conflict, "Catalog publications must be strictly ordered");
  effects->hold = true;
  Take(co_await chat->SubmitAsync(NewCommandId(), {current.id, current.version, ApprovalDecision::Approve}));
  co_await WaitUntil([&] { return effects->started == std::vector<Revision>{3}; }, "Unchanged catalog request did not execute");
  Take(co_await publish(5, 0));
  CheckError(co_await publish(6, 3), ErrorCode::Conflict, "A removed version must not be reintroduced");
  Take(co_await publish(6, 4));
  effects->hold = false;
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "In-flight call did not settle after replacement");
  Check(effects->finished == std::vector<Revision>{3} && !effects->poisoned &&
        model->turns.back() == std::vector<ToolRef>{{catalog_tool, 4, external_catalog}} &&
        model->results.back().value == JsonValue{true}, "Replacement changed an already issued invocation or its original outcome");
  Take(co_await runtime->CloseAsync());
}

struct McpFixture {
  using Object = JsonValue::Object;
  using Array = JsonValue::Array;

  static JsonValue String(std::string value) { return JsonValue{std::move(value)}; }

  Async<JsonValue> Exchange(McpToolRequest exchange) {
    const auto& request = std::get<Object>(exchange.message.value);
    const auto& method = std::get<std::string>(request.at("method").value);
    const auto& id = request.at("id");
    const auto& params = std::get<Object>(request.at("params").value);
    const auto& meta = std::get<Object>(params.at("_meta").value);
    Check(request.at("jsonrpc") == String("2.0") && ids.insert(std::get<std::string>(id.value)).second &&
          meta.at("io.modelcontextprotocol/protocolVersion") == String("2026-07-28") &&
          meta.at("io.modelcontextprotocol/clientCapabilities") == JsonValue{Object{}},
          "MCP request omitted metadata or reused an RPC ID");
    const auto respond = [&](Object result) {
      return AgentResult<JsonValue>::Success(JsonValue{Object{{"jsonrpc", String("2.0")},
          {"id", id}, {"result", JsonValue{std::move(result)}}}});
    };
    if (method == "tools/call") {
      ++calls;
      Check(exchange.tool && exchange.tool->description == description && params.at("name") == String("report") &&
            params.at("arguments") == JsonValue{Object{}} && !exchange.stop.stop_requested(),
            "MCP execution used the wrong definition, arguments or cancellation signal");
      co_return respond({{"resultType", String("complete")}, {"content", JsonValue{Array{}}},
                         {"structuredContent", JsonValue{Object{{"pages", JsonValue{std::int64_t{3}}}}}}});
    }
    Check(method == "tools/list" && !exchange.tool, "MCP catalog sent an unexpected request");
    if (revoked) co_return AgentResult<JsonValue>::Failure({.code = ErrorCode::PermissionDenied, .message = "Test scope revoked"});
    ++pages;
    const auto cursor = params.find("cursor");
    const bool first = cursor == params.end();
    Check(first || cursor->second == String("page-two"), "MCP catalog supplied a wrong pagination cursor");
    Object tool{{"name", String(first ? "report" : "private")},
                {"description", String(first ? description : "Private tool")},
                {"inputSchema", JsonValue{Object{{"type", String("object")}}}},
                {"outputSchema", JsonValue{Object{{"type", String("object")}}}},
                {"annotations", JsonValue{Object{{"readOnlyHint", JsonValue{true}}}}}};
    Object result{{"resultType", String("complete")}, {"tools", JsonValue{Array{JsonValue{std::move(tool)}}}}};
    if (first || repeat_cursor) result.emplace("nextCursor", String("page-two"));
    co_return respond(std::move(result));
  }

  JsonValue ListChanged() const {
    return JsonValue{Object{{"jsonrpc", String("2.0")}, {"method", String("notifications/tools/list_changed")}}};
  }

  Revision version = 1;
  std::string description = "Initial report";
  bool revoked = false;
  bool repeat_cursor = false;
  unsigned pages = 0;
  unsigned calls = 0;
  std::set<std::string> ids;
};

Task<void> VerifyMcpCatalog() {
  auto model = std::make_shared<CatalogModel>();
  auto fixture = std::make_shared<McpFixture>();
  auto runtime = Take(AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"operator"}); },
      .access = [](PolicyRequest request) -> Async<PolicyDecision> {
        const bool approval = request.capability == Capability::ExecuteTool && request.invocation &&
            request.invocation->effect == ToolEffect::ExternalSubmit;
        co_return AgentResult<PolicyDecision>::Success(
            {approval ? AccessDecision::RequireConfirmation : AccessDecision::Allow, {}});
      }}));
  auto catalog = Take(McpToolCatalog::Register(runtime, {.catalog = external_catalog, .client_name = "AgentKit Test",
      .client_version = "1", .exchange = [fixture](McpToolRequest request) { return fixture->Exchange(std::move(request)); },
      .bind = [fixture](const McpToolDescription& tool) {
        using Binding = AgentResult<std::optional<McpToolBinding>>;
        if (tool.name != "report") return Binding::Success({});
        McpToolBinding binding{
            .version = fixture->version, .effect = ToolEffect::ExternalSubmit,
            .prepare = [](ToolPrepareContext, ToolRequest) -> Async<PreparedChange> {
              co_return AgentResult<PreparedChange>::Success({.preview = {.title = "Submit the report"}});
            }};
        return Binding::Success(std::move(binding));
      }}));
  Take(runtime->RegisterModel({{{"test.mcp.model"}, 1}, {"test", "mcp", "script", {}}, model}));
  Take(runtime->RegisterAgent({{.ref = {{"test.mcp.agent"}, 1}, .display_name = "MCP test", .model = {"test.mcp.model"},
      .tool_catalogs = {external_catalog}}}));
  Take(co_await catalog->RefreshAsync());
  Check(fixture->pages == 2, "MCP catalog did not fetch all pages");
  Take(co_await runtime->StartAsync());
  CreateSessionRequest request{.options = {.agent = {"test.mcp.agent"}, .retention = SessionRetention::Ephemeral},
      .initial = SendInput{.text = "Submit a report"}};
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), request)).outcome);
  auto chat = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "MCP tool did not use its host-approved effect");
  fixture->version = 2;
  fixture->description = "Updated report";
  Check(Take(co_await catalog->HandleNotificationAsync(fixture->ListChanged())), "MCP list change was not handled");
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "MCP list change did not close the old approval");
  Check(fixture->calls == 0 && model->results.back().error && model->results.back().error->code == ErrorCode::Conflict,
        "An outdated MCP tool reached the server");

  fixture->description = "Invalid reuse of revision";
  CheckError(co_await catalog->RefreshAsync(), ErrorCode::Conflict, "MCP metadata must change together with its host version");
  Take(co_await chat->SendAsync(NewCommandId(), {.text = "Use the unverified catalog"}));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "The last published MCP binding disappeared");
  const auto unverified = *Approval(*chat);
  Take(co_await chat->SubmitAsync(NewCommandId(), {unverified.id, unverified.version, ApprovalDecision::Approve}));
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "Unverified MCP binding did not settle");
  Check(fixture->calls == 0 && model->results.back().error && model->results.back().error->code == ErrorCode::Conflict,
        "An approval sent an MCP request after the server definition became unverified");
  fixture->description = "Updated report";
  fixture->repeat_cursor = true;
  CheckError(co_await catalog->RefreshAsync(), ErrorCode::ProtocolError, "MCP catalog must reject cyclic pagination");
  fixture->repeat_cursor = false;
  Take(co_await catalog->RefreshAsync());

  Take(co_await chat->SendAsync(NewCommandId(), {.text = "Submit the new report"}));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "MCP replaced tool did not request approval");
  const auto approval = *Approval(*chat);
  Take(co_await chat->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve}));
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "MCP tool did not return a result");
  const JsonValue expected{McpFixture::Object{{"pages", JsonValue{std::int64_t{3}}}}};
  Check(fixture->calls == 1 && model->results.back().value == expected && !model->results.back().error,
        "MCP structured result did not reach the original model call");

  Take(co_await chat->SendAsync(NewCommandId(), {.text = "Submit another report"}));
  co_await WaitUntil([&] { return Approval(*chat).has_value(); }, "MCP pending approval did not appear");
  fixture->revoked = true;
  CheckError(co_await catalog->RefreshAsync(), ErrorCode::PermissionDenied, "MCP revoked server scope must remain visible");
  co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "MCP permission withdrawal did not settle the call");
  Check(fixture->calls == 1 && model->turns.back().empty() && model->results.back().error &&
        model->results.back().error->code == ErrorCode::Conflict, "Revoked MCP tool stayed visible or reached the server");
  Take(co_await runtime->CloseAsync());
}

#if defined(AGENTKIT_TEST_MCP_TRANSPORTS)
using WireJson = nlohmann::json;
using huxerui::agentkit::detail::FromWireJson;
using huxerui::agentkit::detail::ToWireJson;

WireJson McpSubscriptionRequest(std::string id, std::string client) {
  return {{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", "subscriptions/listen"}, {"params", {
      {"_meta", {{"io.modelcontextprotocol/protocolVersion", McpToolProtocolVersion},
                 {"io.modelcontextprotocol/clientInfo", {{"name", std::move(client)}, {"version", "1"}}},
                 {"io.modelcontextprotocol/clientCapabilities", WireJson::object()}}},
      {"notifications", {{"toolsListChanged", true}}}}}};
}

void CheckMcpSubscriptionEvent(const WireJson& event, std::string_view method, std::string_view subscription) {
  Check(event.at("jsonrpc") == "2.0" && event.at("method").get_ref<const std::string&>() == method &&
        event.at("params").at("_meta").at("io.modelcontextprotocol/subscriptionId").get_ref<const std::string&>() == subscription,
        "MCP message did not belong to the current subscription");
}

class McpHttpEvents {
public:
  explicit McpHttpEvents(HttpResponseStream stream) : stream_(std::move(stream)) {}

  Task<WireJson> Read() {
    std::string data;
    while (true) {
      const auto end = pending_.find('\n');
      if (end == std::string::npos) {
        auto bytes = co_await stream_.Body().ReadAsync(4096);
        Check(bytes.Succeeded() && !bytes.Value().empty(), "MCP event stream ended before its next message");
        pending_.append(reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size());
        Check(pending_.size() + data.size() < 64 * 1024, "MCP test event is too large");
        continue;
      }
      auto line = pending_.substr(0, end);
      pending_.erase(0, end + 1);
      if (line.ends_with('\r')) line.pop_back();
      if (line.empty() && !data.empty()) co_return WireJson::parse(data);
      if (!line.starts_with("data:")) continue;
      auto value = std::string_view(line).substr(5);
      if (value.starts_with(' ')) value.remove_prefix(1);
      data.append(value);
      data.push_back('\n');
    }
  }

private:
  HttpResponseStream stream_;
  std::string pending_;
};

struct McpHttpPeer {
  explicit McpHttpPeer(std::string url) : endpoint(std::move(url)) {}

  std::string endpoint;
  HttpClient http;

  HttpRequest Post(const WireJson& message) const {
    const auto data = message.dump();
    const auto bytes = std::as_bytes(std::span(data));
    const auto method = message.at("method").get<std::string>();
    HttpRequest request{.url = endpoint, .method = HttpMethod::Post,
        .headers = {{"Accept", "application/json, text/event-stream"}, {"Content-Type", "application/json"},
                    {"Authorization", "Bearer agentkit-test"}, {"MCP-Protocol-Version", std::string(McpToolProtocolVersion)},
                    {"Mcp-Method", method}}, .body = Bytes(bytes.begin(), bytes.end()), .timeout = 8s};
    if (method == "tools/call") request.headers.push_back({"Mcp-Name", message.at("params").at("name").get<std::string>()});
    return request;
  }

  static bool IsEventStream(const HttpResponseStream& stream) {
    return std::ranges::any_of(stream.Headers(), [](const HttpHeader& header) {
      auto name = header.name;
      for (char& ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
      return name == "content-type" && header.value.starts_with("text/event-stream");
    });
  }

  Async<JsonValue> Exchange(McpToolRequest request) {
    using Result = AgentResult<JsonValue>;
    auto message = ToWireJson(request.message);
    if (request.tool) message["params"]["_meta"]["progressToken"] = message.at("id");
    auto opened = co_await http.SendStreamAsync(Post(message));
    if (request.stop.stop_requested()) co_return Result::Failure({.code = ErrorCode::AlreadyClosed, .message = "MCP request cancelled"});
    if (!opened.Succeeded()) co_return Result::Failure({.code = ErrorCode::ProviderFailure, .message = opened.Error().message});
    auto response = std::move(opened).Value();
    if (response.StatusCode() == 403) co_return Result::Failure({.code = ErrorCode::PermissionDenied, .message = "MCP scope revoked"});
    if (response.StatusCode() != 200) co_return Result::Failure({.code = ErrorCode::ProtocolError, .message = "MCP test server rejected the request"});
    if (IsEventStream(response)) {
      McpHttpEvents events(std::move(response));
      auto progress = co_await events.Read();
      Check(progress.at("method") == "notifications/progress" &&
            progress.at("params").at("progressToken") == message.at("id"), "MCP tool stream lost its progress token");
      co_return Result::Success(FromWireJson(co_await events.Read()));
    }
    std::string body;
    while (true) {
      auto bytes = co_await response.Body().ReadAsync(4096);
      Check(bytes.Succeeded(), "could not read the MCP catalog response");
      if (bytes.Value().empty()) break;
      body.append(reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size());
      Check(body.size() < 64 * 1024, "MCP test response is too large");
    }
    co_return Result::Success(FromWireJson(WireJson::parse(body)));
  }

  Task<McpHttpEvents> Subscribe(std::string id) {
    auto opened = co_await http.SendStreamAsync(Post(McpSubscriptionRequest(id, "AgentKit HTTP Test")));
    if (!opened.Succeeded()) throw std::runtime_error(opened.Error().message);
    auto response = std::move(opened).Value();
    Check(response.StatusCode() == 200 && IsEventStream(response), "MCP subscription did not open an event stream");
    McpHttpEvents events(std::move(response));
    const auto ack = co_await events.Read();
    CheckMcpSubscriptionEvent(ack, "notifications/subscriptions/acknowledged", id);
    Check(ack.at("params").at("notifications").at("toolsListChanged") == true, "MCP tools filter was not acknowledged");
    co_return events;
  }

  Task<void> ReleaseFirstReport() {
    auto result = co_await http.SendAsync({.url = endpoint + "/test/release-first", .timeout = 8s});
    Check(result.Succeeded() && result.Value().status_code == 200, "MCP fixture did not release the first report");
  }
};

struct McpConnectionTest {
  std::shared_ptr<CatalogModel> model = std::make_shared<CatalogModel>();
  std::shared_ptr<AgentRuntime> runtime = Take(AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"mcp-test-operator"}); },
      .access = [](PolicyRequest request) -> Async<PolicyDecision> {
        const auto decision = request.capability == Capability::ExecuteTool ? AccessDecision::RequireConfirmation : AccessDecision::Allow;
        co_return AgentResult<PolicyDecision>::Success({decision, {}});
      }, .max_parallel_tools_per_turn = 2}));
  std::shared_ptr<McpToolCatalog> catalog;

  McpConnectionTest(McpToolExchange exchange, std::string client, bool parallel = false) {
    catalog = Take(McpToolCatalog::Register(runtime, {.catalog = external_catalog, .client_name = std::move(client),
      .client_version = "1", .exchange = std::move(exchange), .bind = [parallel](const McpToolDescription& tool) {
        using Binding = AgentResult<std::optional<McpToolBinding>>;
        if (tool.name != "report") return Binding::Success({});
        constexpr std::array<std::string_view, 3> descriptions{"Report revision 1", "Report revision 2", "Report revision 3"};
        const auto selected = std::ranges::find(descriptions, std::string_view(tool.description));
        Check(selected != descriptions.end(), "unexpected MCP tool revision");
        const auto version = static_cast<Revision>(selected - descriptions.begin() + 1);
        return Binding::Success(McpToolBinding{.version = version, .effect = ToolEffect::ExternalSubmit,
            .concurrency = parallel ? ToolConcurrency::DisjointResources : ToolConcurrency::Serial,
            .prepare = [](ToolPrepareContext, ToolRequest request) -> Async<PreparedChange> {
              PreparedChange plan{.preview = {.title = "Submit report"}};
              const auto* args = std::get_if<JsonValue::Object>(&request.arguments.value);
              if (args && args->contains("section")) {
                const auto& section = std::get<std::string>(args->at("section").value);
                plan.preview.title = "Submit " + section + " report";
                plan.conflict_keys = std::vector<ToolResourceClaim>{{{"report/" + section}, ResourceAccessMode::Write}};
              }
              co_return AgentResult<PreparedChange>::Success(std::move(plan));
            }});
      }}));
    Take(runtime->RegisterModel({{{"test.mcp.connection.model"}, 1}, {"test", "mcp", "script", {}}, model}));
    Take(runtime->RegisterAgent({{.ref = {{"test.mcp.connection.agent"}, 1}, .display_name = "MCP connection test",
        .model = {"test.mcp.connection.model"}, .tool_catalogs = {external_catalog}}}));
  }

  Task<void> Start() {
    Take(co_await catalog->RefreshAsync());
    Take(co_await runtime->StartAsync());
  }

  Task<std::shared_ptr<Conversation>> OpenApproval(std::string prompt) {
    CreateSessionRequest input{.options = {.agent = {"test.mcp.connection.agent"}, .retention = SessionRetention::Ephemeral},
        .initial = SendInput{.text = std::move(prompt)}};
    const auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(input))).outcome);
    auto chat = Take(co_await runtime->Sessions().OpenAsync(created.session));
    co_await WaitUntil([&] { return Approval(*chat).has_value() || !chat->Snapshot()->summary.active_run; },
        "MCP report approval was not offered");
    if (!Approval(*chat)) {
      const auto snapshot = chat->Snapshot();
      if (!snapshot->runs.empty() && snapshot->runs.back().failure)
        throw std::runtime_error(snapshot->runs.back().failure->message);
      throw std::runtime_error("MCP report run ended before approval");
    }
    co_return chat;
  }

  Task<void> RequireInvalidated(const std::shared_ptr<Conversation>& chat, InteractionSnapshot approval, Revision version) {
    const auto rejected = co_await chat->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve});
    CheckError(rejected, ErrorCode::AlreadyClosed, "MCP approval remained usable after the catalog changed");
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "the stale MCP plan did not settle");
    const auto after = chat->Snapshot();
    const auto& original = std::get<ToolApproval>(approval.body);
    Check(after->tools.size() == 1 && after->tools.front().tool == ToolRef{catalog_tool, version, external_catalog} &&
          after->tools.front().id == original.call && after->tools.front().plan == original.plan &&
          after->tools.front().state == ToolCallState::Conflict && after->interactions.back().state == huxerui::agentkit::InteractionState::Invalidated &&
          !model->results.empty() && model->results.back().error && model->results.back().error->code == ErrorCode::Conflict,
          "the MCP catalog update did not invalidate the original plan and return its result");
  }

  Task<void> ApproveReport(const std::shared_ptr<Conversation>& chat, Revision version) {
    const auto approval = *Approval(*chat);
    const auto decision = NewCommandId();
    const InteractionReply reply{approval.id, approval.version, ApprovalDecision::Approve};
    const auto receipt = Take(co_await chat->SubmitAsync(decision, reply));
    Check(Take(co_await chat->SubmitAsync(decision, reply)).stamp.commit == receipt.stamp.commit, "MCP approval was not idempotent");
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "MCP tool call did not finish");
    const auto completed = chat->Snapshot();
    const auto expected = FromWireJson(WireJson{{"pages", 3}, {"revision", version}});
    Check(completed->tools.size() == 1 && completed->tools.front().tool == ToolRef{catalog_tool, version, external_catalog} &&
          completed->tools.front().state == ToolCallState::Succeeded && !model->results.empty() &&
          model->results.back().value == expected && !model->results.back().error, "approved MCP output did not return to the model");
  }
};

Task<void> ExerciseMcpHttp(std::string endpoint) {
  auto peer = std::make_shared<McpHttpPeer>(std::move(endpoint));
  McpConnectionTest fixture([peer](McpToolRequest request) { return peer->Exchange(std::move(request)); }, "AgentKit HTTP Test", true);
  co_await fixture.Start();
  const auto publish_notification = [&](std::string id, bool revoked) -> Task<void> {
    auto events = co_await peer->Subscribe(id);
    const auto notification = co_await events.Read();
    CheckMcpSubscriptionEvent(notification, "notifications/tools/list_changed", id);
    const auto updated = co_await fixture.catalog->HandleNotificationAsync(FromWireJson(notification));
    if (revoked) CheckError(updated, ErrorCode::PermissionDenied, "MCP scope withdrawal was not propagated");
    else Check(Take(updated), "the received MCP notification did not refresh the catalog");
    const auto end = co_await events.Read();
    Check(end.at("id") == id && end.at("result").at("resultType") == "complete", "MCP subscription completion lost its request ID");
  };
  const auto old = co_await fixture.OpenApproval("Use the old report");
  const auto old_approval = *Approval(*old);
  co_await publish_notification("first-listener", false);
  co_await fixture.RequireInvalidated(old, old_approval, 1);

  const auto fresh = co_await fixture.OpenApproval("Use the current report");
  co_await fixture.ApproveReport(fresh, 2);

  fixture.model->parallel_reports = true;
  const auto previous_results = fixture.model->results.size();
  const auto batch = co_await fixture.OpenApproval("Submit both reports in one turn");
  co_await WaitUntil([&] { return batch->Snapshot()->summary.pending_interactions == 2; },
      "The two reports did not receive separate approvals");
  const auto review = batch->Snapshot();
  const auto approve = [&](std::string_view title) -> Task<ToolCallId> {
    const auto approval = std::ranges::find_if(review->interactions, [&](const InteractionSnapshot& interaction) {
      const auto* body = std::get_if<ToolApproval>(&interaction.body);
      return body && body->preview.title == title;
    });
    Check(approval != review->interactions.end(), "Missing approval for a parallel report");
    Take(co_await batch->SubmitAsync(NewCommandId(), {approval->id, approval->version, ApprovalDecision::Approve}));
    co_return std::get<ToolApproval>(approval->body).call;
  };
  const auto state = [&](const ToolCallId& id) {
    const auto snapshot = batch->Snapshot();
    const auto tool = std::ranges::find(snapshot->tools, id, &ToolCallSnapshot::id);
    Check(tool != snapshot->tools.end(), "The original report call was lost");
    return tool->state;
  };
  const auto first = co_await approve("Submit first report");
  co_await WaitUntil([&] { return state(first) == ToolCallState::Executing; }, "The first approval did not start its request");
  Check(batch->Snapshot()->summary.pending_interactions == 1, "Approving the first report consumed another approval");
  const auto second = co_await approve("Submit second report");
  co_await WaitUntil([&] { return state(second) == ToolCallState::Succeeded; }, "The second MCP report did not complete independently");
  Check(state(first) == ToolCallState::Executing && fixture.model->results.size() == previous_results,
        "MCP results reached the model before the tool turn ended");
  co_await peer->ReleaseFirstReport();
  co_await WaitUntil([&] { return !batch->Snapshot()->summary.active_run; }, "The parallel MCP tool turn did not finish");
  const auto& results = fixture.model->results;
  Check(results.size() == previous_results + 2, "The model did not receive both reports");
  const auto matches = [&](const ModelToolResult& result, const ToolCallId& call, std::string section) {
    return result.call == call && result.provider_call_id == section + "-report" && !result.error &&
        result.value == FromWireJson(WireJson{{"pages", 3}, {"revision", 2}, {"section", std::move(section)}});
  };
  Check(matches(results[previous_results], first, "first") && matches(results[previous_results + 1], second, "second"),
        "The MCP tool results were not associated with the original ordered calls");
  fixture.model->parallel_reports = false;

  const auto revoked = co_await fixture.OpenApproval("Use the report before revocation");
  const auto revoked_approval = *Approval(*revoked);
  co_await publish_notification("second-listener", true);
  co_await fixture.RequireInvalidated(revoked, revoked_approval, 2);
  Check(fixture.model->turns.back().empty(), "MCP tools remained visible after the server denied access");
  Take(co_await fixture.runtime->CloseAsync());
}

void VerifyMcpHttp(std::string endpoint) {
  std::exception_ptr failure;
  bool completed = false;
  Application app{[]() -> View { return Text("AgentKit MCP HTTP test"); }, {.application_hooks = {[&](ApplicationContext&) {
    const auto application = UseApplication();
    UseApplicationTaskScope().Launch([&, application]() -> Task<void> {
      try {
        co_await ExerciseMcpHttp(std::move(endpoint));
        completed = true;
      } catch (...) {
        failure = std::current_exception();
      }
      application.Quit();
    });
  }}}};
  static_cast<void>(RunApplication());
  if (failure) std::rethrow_exception(failure);
  Check(completed, "MCP native HTTP test did not complete");
}

std::shared_ptr<std::FILE> OpenInheritedPipe(const std::string& argument, bool write) {
  std::uintptr_t value{};
  const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
  Check(parsed.ec == std::errc{} && parsed.ptr == argument.data() + argument.size(), "invalid inherited MCP pipe");
#if defined(_WIN32)
  const auto fd = _open_osfhandle(static_cast<std::intptr_t>(value), _O_BINARY | (write ? _O_WRONLY : _O_RDONLY));
  Check(fd >= 0, "could not adopt the inherited MCP pipe");
  auto* stream = _fdopen(fd, write ? "wb" : "rb");
  if (!stream) _close(fd);
#else
  const auto fd = static_cast<int>(value);
  auto* stream = fdopen(fd, write ? "wb" : "rb");
  if (!stream) close(fd);
#endif
  Check(stream != nullptr, "could not open the inherited MCP stream");
  return {stream, std::fclose};
}

struct McpStdioPipe {
  std::shared_ptr<std::FILE> input;
  std::shared_ptr<std::FILE> output;

  Task<void> Send(WireJson message) const {
    return RunWorker([](std::shared_ptr<std::FILE> stream, std::string line) {
      Check(std::fwrite(line.data(), 1, line.size(), stream.get()) == line.size() && std::fflush(stream.get()) == 0,
            "could not write to the MCP child process");
    }, output, message.dump() + "\n");
  }

  Task<std::optional<WireJson>> Read() const {
    return RunWorker([](std::shared_ptr<std::FILE> stream) -> std::optional<WireJson> {
      std::string line;
      for (int ch; (ch = std::fgetc(stream.get())) != EOF;) {
        if (ch == '\n') return WireJson::parse(line);
        Check(line.size() < 64 * 1024, "MCP child process sent an oversized record");
        line.push_back(static_cast<char>(ch));
      }
      Check(line.empty() && !std::ferror(stream.get()), "MCP child process stream ended in an incomplete message");
      return {};
    }, input);
  }
};

struct McpStdioPeer {
  McpStdioPipe pipe;

  Async<JsonValue> Exchange(McpToolRequest request) const {
    using Result = AgentResult<JsonValue>;
    if (request.stop.stop_requested()) co_return Result::Failure({.code = ErrorCode::AlreadyClosed, .message = "MCP request cancelled"});
    auto message = ToWireJson(request.message);
    const auto id = message.at("id");
    if (request.tool) message["params"]["_meta"]["progressToken"] = id;
    co_await pipe.Send(std::move(message));
    if (request.tool) {
      const auto progress = co_await pipe.Read();
      Check(progress && progress->at("method") == "notifications/progress" &&
            progress->at("params").at("progressToken") == id, "MCP stdio tool progress was misrouted");
    }
    const auto result = co_await pipe.Read();
    if (!result) co_return Result::Failure({.code = ErrorCode::ProviderFailure, .message = "MCP child process closed its output"});
    Check(result->at("id") == id, "MCP stdio response was misrouted");
    co_return Result::Success(FromWireJson(*result));
  }

  Task<WireJson> Subscribe(std::string id) const {
    co_await pipe.Send(McpSubscriptionRequest(id, "AgentKit stdio Test"));
    const auto ack = co_await pipe.Read();
    Check(ack.has_value(), "MCP child process exited before acknowledging the subscription");
    CheckMcpSubscriptionEvent(*ack, "notifications/subscriptions/acknowledged", id);
    Check(ack->at("params").at("notifications").at("toolsListChanged") == true, "MCP tools filter was not acknowledged");
    auto event = co_await pipe.Read();
    Check(event.has_value(), "MCP child process exited before reporting the directory change");
    CheckMcpSubscriptionEvent(*event, "notifications/tools/list_changed", id);
    co_return std::move(*event);
  }
};

Task<void> ExerciseMcpStdio(std::array<std::string, 4> handles) {
  auto peer = std::make_shared<McpStdioPeer>(McpStdioPeer{
      {OpenInheritedPipe(handles[0], false), OpenInheritedPipe(handles[1], true)}});
  McpConnectionTest fixture([peer](McpToolRequest request) { return peer->Exchange(std::move(request)); }, "AgentKit stdio Test");
  co_await fixture.Start();

  const auto old = co_await fixture.OpenApproval("Use the initial directory");
  const auto old_approval = *Approval(*old);
  const auto initial_change = co_await peer->Subscribe("original-listener");
  Check(Take(co_await fixture.catalog->HandleNotificationAsync(FromWireJson(initial_change))), "MCP stdio change did not update the catalog");
  co_await fixture.RequireInvalidated(old, old_approval, 1);
  const auto current = co_await fixture.OpenApproval("Use the replacement directory");
  co_await fixture.ApproveReport(current, 2);

  const auto pending = co_await fixture.OpenApproval("Leave an approval awaiting the old server");
  const auto awaiting = *Approval(*pending);
  const auto interrupted = co_await fixture.OpenApproval("Start the call interrupted by child exit");
  const auto approval = *Approval(*interrupted);
  Take(co_await interrupted->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve}));
  co_await WaitUntil([&] { return !interrupted->Snapshot()->summary.active_run; }, "MCP child exit did not settle the interrupted call");
  const auto failed = interrupted->Snapshot();
  Check(failed->tools.size() == 1 && failed->tools.front().id == std::get<ToolApproval>(approval.body).call &&
        failed->tools.front().state == ToolCallState::ResultUnknown &&
        failed->runs.back().end_reason == RunEndReason::ToolOutcomeUnknown, "MCP child exit lost the original unknown result");
  CheckError(co_await interrupted->RetryAsync(NewCommandId(), failed->runs.back().id), ErrorCode::ResultUnknown,
             "the ambiguous MCP invocation could be retried");
  Take(co_await fixture.catalog->WithdrawAsync());
  co_await fixture.RequireInvalidated(pending, awaiting, 2);

  peer->pipe = {OpenInheritedPipe(handles[2], false), OpenInheritedPipe(handles[3], true)};
  const auto restored_change = co_await peer->Subscribe("restarted-listener");
  Check(Take(co_await fixture.catalog->HandleNotificationAsync(FromWireJson(restored_change))), "MCP stdio did not refresh after restart");
  const auto fresh = co_await fixture.OpenApproval("Use the restarted MCP process");
  co_await fixture.ApproveReport(fresh, 3);
  const WireJson cancel{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
      {"params", {{"requestId", "restarted-listener"}, {"reason", "Test finished"}}}};
  co_await peer->pipe.Send(cancel);
  peer->pipe.output.reset();
  Check(!(co_await peer->pipe.Read()), "MCP child process did not exit quietly after stdin closed");
  Take(co_await fixture.runtime->CloseAsync());
}

void VerifyMcpStdio(std::array<std::string, 4> handles) {
  bool complete = false;
  std::exception_ptr failure;
  Application app{[]() -> View { return Text("AgentKit MCP stdio test"); }, {.application_hooks = {[&](ApplicationContext&) {
    UseApplicationTaskScope().Launch([&]() -> Task<void> {
      try { co_await ExerciseMcpStdio(std::move(handles)); } catch (...) { failure = std::current_exception(); }
      complete = true;
    });
  }}}};
  testing::UiTest ui(app);
  ui.PumpUntil([&] { return complete; }, {.timeout = 20s, .step = 5ms});
  if (failure) std::rethrow_exception(failure);
  Check(complete, "MCP stdio test did not complete");
}
#endif

class ScheduleModel final : public ModelProvider {
public:
  explicit ScheduleModel(std::vector<std::string> tools) : tools(std::move(tools)) {}

  ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true, .multi_tool_calls = true}; }

  Async<std::unique_ptr<ModelTurnStream>> StartTurnAsync(ModelTurnRequest request) override {
    std::vector<ModelEvent> events;
    for (const auto& item : request.items) {
      if (const auto* result = std::get_if<ModelToolResult>(&item)) results.push_back(*result);
    }
    if (results.empty()) {
      for (const auto& tool : tools) {
        events.emplace_back(ModelToolStarted{tool, "schedule." + tool});
        events.emplace_back(ModelToolArguments{tool, "{}"});
        events.emplace_back(ModelToolFinished{tool});
      }
      events.emplace_back(ModelFinished{ModelFinishReason::ToolCalls, {}});
    } else {
      events = {ModelTextDelta{"Tool turn completed"}, ModelFinished{ModelFinishReason::Completed, {}}};
    }
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<ScriptStream>(std::move(events)));
  }

  std::vector<std::string> tools;
  std::vector<ModelToolResult> results;
};

struct ScheduledTool {
  std::string name;
  huxerui::agentkit::ResourceId resource;
  ToolConcurrency concurrency = ToolConcurrency::ReadOnly;
  ToolEffect effect = ToolEffect::Read;
  bool approval = false;
  bool form = false;
  bool unknown_keys = false;
  bool invalid_claim = false;
};

struct ScheduleProbe {
  std::vector<std::string> prepared;
  std::vector<std::string> started;
  std::vector<std::string> finished;
  std::set<std::string> released;
  std::map<huxerui::agentkit::ResourceId, std::string> revisions;
  std::optional<ErrorCode> form_error;
  std::size_t active = 0;
  std::size_t peak = 0;
};

using ScheduleFixture = std::tuple<std::shared_ptr<AgentRuntime>, std::shared_ptr<Conversation>,
                                   std::shared_ptr<ScheduleModel>, std::shared_ptr<ScheduleProbe>>;

Task<ScheduleFixture> StartSchedule(std::vector<ScheduledTool> tools, std::size_t limit) {
  std::vector<std::string> names;
  for (const auto& tool : tools) names.push_back(tool.name);
  auto model = std::make_shared<ScheduleModel>(std::move(names));
  auto probe = std::make_shared<ScheduleProbe>();
  auto runtime = Take(AgentRuntime::Create({.tasks = UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"scheduler"}); },
      .access = [tools](PolicyRequest request) -> Async<PolicyDecision> {
        const bool confirms = request.capability == Capability::ExecuteTool && request.tool &&
            std::ranges::any_of(tools, [&](const auto& tool) {
              return tool.approval && request.tool->id.value == "schedule." + tool.name;
            });
        co_return AgentResult<PolicyDecision>::Success({confirms ? AccessDecision::RequireConfirmation : AccessDecision::Allow, {}});
      },
      .tool_resources = [tools, probe](ToolInvocation invocation) -> Async<std::vector<ResourceAccess>> {
        const auto tool = std::ranges::find_if(tools, [&](const auto& tool) {
          return invocation.tool.id.value == "schedule." + tool.name;
        });
        const auto mode = invocation.phase == ToolAccessPhase::Execute && tool->effect == ToolEffect::WriteResource
            ? ResourceAccessMode::Write : ResourceAccessMode::Read;
        co_return AgentResult<std::vector<ResourceAccess>>::Success(
            {{tool->resource, ResourceRef{tool->resource, probe->revisions.at(tool->resource)}, mode, {}}});
      }, .max_parallel_tools_per_turn = limit}));
  Take(runtime->RegisterModel({{{"schedule.model"}, 1}, {"test", "scheduler", "script", {}}, model}));
  std::vector<ToolId> ids;
  for (const auto& tool : tools) {
    ids.push_back({"schedule." + tool.name});
    probe->revisions.try_emplace(tool.resource, "initial");
    Take(runtime->RegisterTool({.id = ids.back(), .version = 1, .description = tool.name,
        .input_schema = {JsonValue{true}}, .effect = tool.effect, .concurrency = tool.concurrency,
        .prepare = [tool, probe](ToolPrepareContext context, ToolRequest) -> Async<PreparedChange> {
          probe->prepared.push_back(tool.name);
          PreparedChange prepared{.preview = {.title = tool.name}};
          const bool writes = tool.effect == ToolEffect::WriteResource;
          if (writes) prepared.targets.push_back({tool.resource, context.readable.front().current});
          if (!tool.unknown_keys) {
            prepared.conflict_keys = std::vector<ToolResourceClaim>{
                {tool.resource, writes || tool.invalid_claim ? ResourceAccessMode::Write : ResourceAccessMode::Read}};
          }
          co_return AgentResult<PreparedChange>::Success(std::move(prepared));
        },
        .execute = [tool, probe](ToolContext context, PreparedToolCall) -> Async<ToolExecutionResult> {
          probe->started.push_back(tool.name);
          probe->peak = std::max(probe->peak, ++probe->active);
          Take(context.ReportProgress(tool.name));
          if (tool.form) {
            FormRequest form{.title = "Ask in tool", .schema = {JsonValue{true}}};
            auto asked = co_await context.AskAsync(std::move(form));
            if (!asked.Succeeded()) probe->form_error = asked.Error().code;
          }
          while (!probe->released.contains(tool.name) && !context.StopToken().stop_requested()) co_await Delay(2ms);
          --probe->active;
          const bool writes = tool.effect == ToolEffect::WriteResource;
          if (context.StopToken().stop_requested()) {
            co_return AgentResult<ToolExecutionResult>::Success(
                {.outcome = writes ? ToolOutcome::ResultUnknown : ToolOutcome::Cancelled});
          }
          if (writes) probe->revisions.at(tool.resource) = tool.name;
          probe->finished.push_back(tool.name);
          co_return AgentResult<ToolExecutionResult>::Success(
              {.outcome = ToolOutcome::Succeeded, .model_value = JsonValue{tool.name}});
        }}));
  }
  Take(runtime->RegisterAgent({{.ref = {{"schedule.agent"}, 1}, .display_name = "Scheduler", .model = {"schedule.model"},
      .tools = std::move(ids)}}));
  Take(co_await runtime->StartAsync());
  CreateSessionRequest request{.options = {.agent = {"schedule.agent"}, .retention = SessionRetention::Ephemeral},
                               .initial = SendInput{.text = "Exercise tool scheduling"}};
  auto created = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request))).outcome);
  auto conversation = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_return ScheduleFixture{runtime, conversation, model, probe};
}

Task<void> VerifyToolScheduler() {
  {
    std::vector<ScheduledTool> tools{{.name = "a", .resource = {"shared"}, .form = true}, {"b", {"shared"}},
        {"write", {"shared"}, ToolConcurrency::DisjointResources, ToolEffect::WriteResource}, {"other", {"disjoint"}}};
    auto [runtime, chat, model, probe] = co_await StartSchedule(std::move(tools), 2);
    co_await WaitUntil([&] { return probe->started.size() == 2; }, "two readers of the same key could not overlap");
    probe->released.insert("b");
    co_await WaitUntil([&] { return probe->finished.size() == 1; }, "out-of-order read did not finish");
    co_await Delay(25ms);
    Check(probe->started == std::vector<std::string>{"a", "b"} && probe->form_error == ErrorCode::InvalidState &&
        chat->Snapshot()->interactions.empty(), "conflicting writer let a later tool bypass it, or a parallel form was accepted");
    probe->released.insert("a");
    co_await WaitUntil([&] { return probe->started.size() == 4; }, "disjoint writer and reader did not overlap");
    probe->released.insert("other");
    co_await WaitUntil([&] { return probe->finished.size() == 3; }, "disjoint reader did not finish before writer");
    probe->released.insert("write");
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "parallel tool turn did not finish");
    const auto snapshot = chat->Snapshot();
    Check(probe->peak == 2 && probe->finished == std::vector<std::string>{"b", "a", "other", "write"} &&
          snapshot->runs.back().usage.tool_calls == 4 && model->results.size() == model->tools.size(),
          "tool concurrency limit, budget or completion order changed");
    for (std::size_t i = 0; i < model->tools.size(); ++i) {
      Check(model->results[i].provider_call_id == model->tools[i] && model->results[i].call == snapshot->tools[i].id &&
            model->results[i].value == JsonValue{model->tools[i]}, "model result did not keep its original identity and order");
    }
    Take(co_await runtime->CloseAsync());
  }
  for (const auto mode : {std::string{"default"}, std::string{"serial"}, std::string{"unknown"}}) {
    ScheduledTool barrier{"barrier", {"first"}};
    if (mode == "serial") barrier.concurrency = ToolConcurrency::Serial;
    if (mode == "unknown") barrier.unknown_keys = true;
    std::vector<ScheduledTool> tools{std::move(barrier), {"next", {"second"}}};
    auto [runtime, chat, model, probe] = co_await StartSchedule(std::move(tools), mode == "default" ? 1 : 2);
    co_await WaitUntil([&] { return !probe->started.empty(); }, "serial barrier did not start");
    co_await Delay(25ms);
    Check(probe->started == std::vector<std::string>{"barrier"}, "later tool crossed a serial barrier");
    probe->released.insert("barrier");
    probe->released.insert("next");
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "serial tool turn did not finish");
    Check(probe->peak == 1 && probe->finished.size() == 2, "serial calls overlapped or were skipped");
    Take(co_await runtime->CloseAsync());
  }
  {
    std::vector<ScheduledTool> tools{
        {"first", {"shared"}, ToolConcurrency::DisjointResources, ToolEffect::WriteResource},
        {"stale", {"shared"}, ToolConcurrency::DisjointResources, ToolEffect::WriteResource}};
    auto [runtime, chat, model, probe] = co_await StartSchedule(std::move(tools), 2);
    co_await WaitUntil([&] { return probe->prepared.size() == 2 && probe->started.size() == 1; },
                       "conflicting writers did not prepare on the same version");
    probe->released = {"first", "stale"};
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "conflicting writers did not settle");
    Check(probe->started == std::vector<std::string>{"first"} && chat->Snapshot()->tools.back().state == ToolCallState::Conflict &&
          model->results.size() == 2 && model->results.back().error && model->results.back().error->code == ErrorCode::Conflict,
          "a conflicting write was issued without rechecking the prepared baseline");
    Take(co_await runtime->CloseAsync());
  }
  {
    std::vector<ScheduledTool> tools{{"before", {"first"}}, {.name = "approve", .resource = {"second"}, .approval = true},
        {"later", {"third"}}, {.name = "bad", .resource = {"bad"}, .approval = true, .invalid_claim = true}};
    auto [runtime, chat, model, probe] = co_await StartSchedule(std::move(tools), 2);
    co_await WaitUntil([&] { return Approval(*chat).has_value() && !probe->started.empty(); }, "approval did not wait alongside a running tool");
    const auto approval = *Approval(*chat);
    Check(std::get<ToolApproval>(approval.body).preview.title == "approve" &&
          probe->started == std::vector<std::string>{"before"}, "an unready call was bypassed or invalid claims reached approval");
    Take(co_await chat->SubmitAsync(NewCommandId(), {approval.id, approval.version, ApprovalDecision::Approve}));
    co_await WaitUntil([&] { return probe->started.size() == 2; }, "approved tool did not acquire the unused slot");
    probe->released = {"before", "approve", "later"};
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "approved tool turn did not finish");
    Check(probe->peak == 2 && probe->started.back() == "later" && model->results.size() == 4 &&
          model->results.back().error && model->results.back().error->code == ErrorCode::InvalidArgument,
          "claim validation or approval arbitration was lost");
    Take(co_await runtime->CloseAsync());
  }
  {
    std::vector<ScheduledTool> tools{{"write", {"first"}, ToolConcurrency::DisjointResources, ToolEffect::WriteResource},
        {"read", {"second"}}, {"pending", {"third"}}};
    auto [runtime, chat, model, probe] = co_await StartSchedule(std::move(tools), 2);
    co_await WaitUntil([&] { return probe->started.size() == 2; }, "cancellation fixture did not reach two active tools");
    Take(co_await chat->CancelAsync(NewCommandId(), *chat->Snapshot()->summary.active_run));
    co_await WaitUntil([&] { return !chat->Snapshot()->summary.active_run; }, "parallel cancellation did not settle");
    const auto snapshot = chat->Snapshot();
    Check(probe->started.size() == 2 && snapshot->tools[0].state == ToolCallState::ResultUnknown &&
          snapshot->tools[1].state == ToolCallState::Cancelled && snapshot->tools[2].state == ToolCallState::Cancelled &&
          model->results.empty(), "cancellation launched a new call or lost a started call's effect classification");
    Take(co_await runtime->CloseAsync());
  }
}

} // namespace

int main(int argc, char** argv) {
#if defined(AGENTKIT_TEST_MCP_TRANSPORTS)
  const bool mcp_http = argc == 3 && std::string_view(argv[1]) == "--mcp-http";
  const bool mcp_stdio = argc == 6 && std::string_view(argv[1]) == "--mcp-stdio";
  if (mcp_http || mcp_stdio) {
    try {
      if (mcp_http) {
        VerifyMcpHttp(argv[2]);
        std::cout << "AgentKit MCP Streamable HTTP acceptance passed\n";
      } else {
        VerifyMcpStdio({argv[2], argv[3], argv[4], argv[5]});
        std::cout << "AgentKit MCP stdio acceptance passed\n";
      }
      return 0;
    } catch (const std::exception& failure) {
      std::cerr << failure.what() << '\n';
      return 1;
    }
  }
#endif
  const bool scheduling = argc == 2 && std::string_view(argv[1]) == "--tool-schedule";
  bool complete = false;
  std::exception_ptr error;
  Application application{
      []() -> View { return Text("AgentKit runtime test"); },
      {.application_hooks = {[&](ApplicationContext&) {
        auto host = std::make_shared<HostState>();
        auto runtime = MakeRuntime(host);
        UseApplicationTaskScope().Launch([&, host, runtime]() -> Task<void> {
          try {
            if (scheduling) {
              co_await VerifyToolScheduler();
              complete = true;
              co_return;
            }
            co_await VerifyRuntime(runtime, host);
            co_await VerifyFormConstraints(MakeRuntime(std::make_shared<HostState>()));
            for (auto review : {ReviewCase::Approve, ReviewCase::Deny, ReviewCase::Defer, ReviewCase::Failed,
                                ReviewCase::Timeout, ReviewCase::NoReviewer, ReviewCase::HumanRequired,
                                ReviewCase::Revoked, ReviewCase::Superseded, ReviewCase::Cancelled, ReviewCase::Stale}) {
              co_await VerifyToolReview(review);
            }
            auto side_host = std::make_shared<HostState>();
            co_await VerifySideChat(MakeRuntime(side_host), side_host);
            co_await VerifyToolCatalog();
            co_await VerifyMcpCatalog();
            co_await VerifyHostResourceContext();
            co_await VerifyModelAttachments();
          } catch (...) { error = std::current_exception(); }
          complete = true;
        });
      }}}};
  try {
    {
      testing::UiTest ui(application);
      ui.PumpUntil([&] { return complete; }, {.timeout = 10s, .step = 5ms});
      if (error) std::rethrow_exception(error);
    }
    if (!scheduling) VerifyContentApplication();
    std::cout << "AgentKit runtime acceptance passed\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
