#include "sqlite_session_store.h"

#include <huxerui/agentkit/runtime.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

namespace kit = huxerui::agentkit;
using namespace std::chrono_literals;
const kit::ContentTypeRef report_card{"fork.report.card", 1};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(kit::AgentResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void Take(kit::AgentResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

huxerui::Task<void> Eventually(std::function<bool()> condition, const char* message) {
  for (int attempt = 0; attempt < 800 && !condition(); ++attempt) co_await huxerui::Delay(5ms);
  Check(condition(), message);
}

class HistoryTurn final : public kit::ModelTurnStream {
public:
  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    using Result = kit::AgentResult<std::optional<kit::ModelEvent>>;
    switch (index_++) {
    case 0: co_return Result::Success(kit::ModelTextDelta{"Ready"});
    case 1: co_return Result::Success(kit::ModelFinished{kit::ModelFinishReason::Completed});
    default: co_return Result::Success(std::nullopt);
    }
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    index_ = 2;
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  int index_ = 0;
};

struct ArtifactForkProbe {
  kit::SessionId source;
  kit::SessionId branch;
  kit::SessionId review_session;
  const kit::ArtifactRef original{{"fork.report"}, 1};
  const kit::ArtifactRef latest{{"fork.report"}, 2};
  bool deny_artifact = false;
  bool deny_model_artifact = false;
  bool deny_source = false;
  bool deny_attachment_context = false;
  bool hold_preview = false;
  bool preview_started = false;
  bool selection_started = false;
  bool deny_card_action = false;
  int card_preparations = 0;
  int exports = 0;
  std::filesystem::path export_file;
  kit::CommandId pending_export;
  kit::CommandId pending_steer;
  kit::RunId export_run;
  kit::CustomBlockAction export_action;
  std::vector<std::string> model_text;
};

kit::Async<kit::RunCompletion> RunCardExport(kit::RunContext run) {
  const auto command = [&](std::string_view step) { return kit::CommandId{run.Id().value + "/" + std::string(step)}; };
  Take(co_await run.SaveCheckpointAsync({.stage = "report-export", .schema_version = 1}));
  auto form = Take(co_await run.RequestInputAsync(command("form"), {
      .title = "Export report chart", .schema = {kit::JsonValue{true}}}));
  const auto interaction = std::get<kit::InputRequested>(form.outcome).id;
  auto card = Take(co_await run.StartToolAsync(command("card"), {{"fork.card"}, kit::JsonValue{interaction.value}}));
  Take(co_await run.WaitForToolAsync(std::get<kit::ToolCallAccepted>(card.outcome).call));
  auto reply = Take(co_await run.WaitForInputAsync(interaction));
  if (reply.state != kit::InteractionState::Submitted)
    co_return kit::AgentResult<kit::RunCompletion>::Failure({.code = kit::ErrorCode::AlreadyClosed, .message = "Export form closed"});
  auto supplements = Take(co_await run.ReadPendingSteersAsync());
  Check(supplements.size() == 1 && supplements[0].input.text == "Include the Operations category",
      "report export did not restore exactly one approved supplement");
  Take(co_await run.SaveCheckpointAsync({.stage = "report-export", .schema_version = 1,
      .steer_through = supplements[0].message}));
  auto tool = Take(co_await run.StartToolAsync(command("export"), {{"fork.export"}, *reply.value}));
  auto result = Take(co_await run.WaitForToolAsync(std::get<kit::ToolCallAccepted>(tool.outcome).call));
  if (result.outcome != kit::ToolOutcome::Succeeded) co_return kit::AgentResult<kit::RunCompletion>::Failure(*result.reason);
  co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Report exported"});
}

class ForkHistoryModel final : public kit::ModelProvider {
public:
  explicit ForkHistoryModel(ArtifactForkProbe& probe) : probe_(probe) {}

  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    probe_.model_text.clear();
    for (const auto& item : request.items) {
      const auto* content = std::get_if<kit::ModelContent>(&item);
      if (content) {
        if (const auto* text = std::get_if<std::string>(&content->body)) probe_.model_text.push_back(*text);
      }
    }
    Check(!request.continuation && !request.allow_server_state, "fork reused provider state");
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<HistoryTurn>());
  }
private:
  ArtifactForkProbe& probe_;
};

std::shared_ptr<kit::AgentRuntime> ArtifactForkRuntime(std::shared_ptr<kit::SessionStore> store, ArtifactForkProbe& probe) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"fork.user"}); },
      .access = [&probe](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        if (request.capability == kit::Capability::UseContentAction && request.content_action &&
            request.content_action->action == "export") {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({
              probe.deny_card_action ? kit::AccessDecision::Deny : kit::AccessDecision::RequireConfirmation, "Review report export"});
        }
        if (request.capability == kit::Capability::UseContentAction && request.content_action &&
            request.content_action->action == "refine") {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::RequireConfirmation, "Confirm report scope"});
        }
        if (request.capability == kit::Capability::ExecuteTool && request.tool && request.tool->id == kit::ToolId{"fork.export"}) {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::RequireConfirmation, "Review file write"});
        }
        if (request.capability == kit::Capability::ExecuteTool && request.tool && request.tool->id == kit::ToolId{"fork.external-review"}) {
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::RequireConfirmation, "Review the source report"});
        }
        const bool is_report = std::ranges::any_of(request.sources, [&](const kit::SourceSlice& source) {
          return source.source == kit::SourceRef{probe.original};
        });
        const bool denied = (request.capability == kit::Capability::ReadSession &&
                request.target.session == probe.source && probe.deny_source) ||
            (is_report && request.capability == kit::Capability::ReadArtifact && probe.deny_artifact &&
                request.actor.kind == kit::ActionParty::Kind::User) ||
            (is_report && request.capability == kit::Capability::ReadContext &&
                request.target.session == probe.review_session && probe.deny_attachment_context) ||
            (is_report && request.capability == kit::Capability::SendToModel && probe.deny_model_artifact) ||
            (is_report && request.capability == kit::Capability::ReadContext && request.project == kit::ProjectId{"restricted"});
        co_return kit::AgentResult<kit::PolicyDecision>::Success({
            denied ? kit::AccessDecision::Deny : kit::AccessDecision::Allow, denied ? "Report access was revoked" : ""});
      },
      .tool_resources = [](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
        std::vector<kit::ResourceAccess> grants;
        if (invocation.tool.id == kit::ToolId{"fork.export"}) grants.push_back({{"fork.export.file"}, {},
            invocation.phase == kit::ToolAccessPhase::Execute ? kit::ResourceAccessMode::Write : kit::ResourceAccessMode::Read});
        co_return kit::AgentResult<std::vector<kit::ResourceAccess>>::Success(std::move(grants));
      },
      .artifact_store = {.preview = [&probe](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        probe.preview_started = true;
        while (probe.hold_preview) co_await huxerui::Delay(2ms);
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact,
            .structured = kit::JsonValue{request.storage_handle}, .summary = request.storage_handle});
      }, .selection = [&probe](kit::ArtifactSelectionRequest request) -> kit::Async<std::string> {
        Check(request.parts == std::vector<kit::PartId>{{"body"}} && request.byte_limit > request.resource.storage_handle.size(),
            "the resolver did not receive the exact leaf");
        probe.selection_started = true;
        while (probe.hold_preview) co_await huxerui::Delay(2ms);
        co_return kit::AgentResult<std::string>::Success(request.resource.storage_handle);
      }},
      .store = std::move(store)}));
  Take(runtime->RegisterModel({.ref = {{"fork.history"}, 1}, .endpoint = {"test", "local", "script", {}},
      .provider = std::make_shared<ForkHistoryModel>(probe), .allow_server_state = true}));
  const kit::JsonSchema flag{kit::JsonValue{kit::JsonValue::Object{{"type", kit::JsonValue{std::string("boolean")}}}}};
  Take(runtime->RegisterContentComponent({report_card, {kit::JsonValue{true}}, {
      {"export", flag, [&probe](kit::ContentActionContext context, kit::JsonValue value) -> kit::Async<kit::ContentActionPlan> {
        ++probe.card_preparations;
        Check(!context.block->interactions.empty(), "export action received no interaction binding");
        co_return kit::AgentResult<kit::ContentActionPlan>::Success(
            kit::InteractionReply{context.block->interactions.front(), 1, std::move(value)});
      }},
      {"summarize", flag, [](kit::ContentActionContext context, kit::JsonValue value) -> kit::Async<kit::ContentActionPlan> {
        co_return kit::AgentResult<kit::ContentActionPlan>::Success(
            kit::ArtifactAction{context.block->artifacts.front(), "summarize", std::move(value)});
      }},
      {"refine", flag, [&probe](kit::ContentActionContext, kit::JsonValue) -> kit::Async<kit::ContentActionPlan> {
        co_return kit::AgentResult<kit::ContentActionPlan>::Success(
            kit::SteerRequest{probe.export_run, {.text = "Include the Operations category"}});
      }},
  }}));
  Take(runtime->RegisterArtifactAction({"test/report", "summarize", flag,
      [](kit::ArtifactActionContext context, kit::JsonValue) -> kit::Async<kit::SendInput> {
        co_return kit::AgentResult<kit::SendInput>::Success({.text = "Summarize the bound report", .context = {{context.artifact}}});
      }}));
  Take(runtime->RegisterTool({
      .id = {"fork.publish"}, .version = 1, .description = "Publish the test report",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest args) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.execution_data = std::move(args.arguments)});
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto update = std::get<bool>(call.change.execution_data.value);
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
            .new_artifacts = {{.id = kit::ArtifactId{"fork.report"}, .expected_latest = update ? std::optional<kit::Revision>{1} : std::nullopt,
                .type = "test/report", .name = update ? "Updated report" : "Original report",
                .storage_handle = update ? "published-payload-v2" : "published-payload-v1",
                .parts = {{{"body"}, "Report text"}}}}});
      }}));
  Take(runtime->RegisterTool({
      .id = {"fork.card"}, .version = 1, .description = "Attach the report summary card",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::Read,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({});
      },
      .execute = [&probe](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto& id = std::get<std::string>(call.request.arguments.value);
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
            .blocks = {{{}, kit::CustomBlock{report_card, kit::JsonValue{std::string("Versioned summary data")},
                "Original report summary", {{id}}, {probe.original}}}}});
      }}));
  Take(runtime->RegisterTool({
      .id = {"fork.export"}, .version = 1, .description = "Export the approved report chart",
      .input_schema = flag, .effect = kit::ToolEffect::WriteResource,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({
            .preview = {.title = "Save report chart CSV"}, .targets = {{{"fork.export.file"}, {}}}});
      },
      .execute = [&probe](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        Check(std::get<bool>(call.request.arguments.value) && context.Resources().size() == 1 &&
            context.Resources()[0].mode == kit::ResourceAccessMode::Write, "export did not receive the approved decision and resource");
        std::ofstream output(probe.export_file, std::ios::binary);
        output << "category,count\nOperations,3\n";
        output.close();
        Check(static_cast<bool>(output), "failed to write chart CSV");
        ++probe.exports;
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
            .effects = {{{"fork.export.file"}, kit::ResourceEffectState::Applied}}});
      }}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"fork.author"}, 1}, .display_name = "Author", .model = {"fork.history"},
          .tools = {{"fork.publish"}, {"fork.card"}, {"fork.export"}}, .content_types = {report_card}},
      .run = [](kit::RunContext run, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        if (input.input.text == "Export the report chart") co_return co_await RunCardExport(std::move(run));
        auto tool = Take(co_await run.CallToolAsync({{"fork.publish"}, kit::JsonValue{input.input.text == "Update report"}}));
        if (input.input.text == "Sealed source note") {
          auto form = Take(co_await run.RequestInputAsync(kit::NewCommandId(), {
              .title = "Report export", .schema = {kit::JsonValue{true}}}));
          const auto interaction = std::get<kit::InputRequested>(form.outcome).id;
          Take(co_await run.CallToolAsync({{"fork.card"}, kit::JsonValue{interaction.value}}));
        }
        co_return kit::AgentResult<kit::RunCompletion>::Success({.artifacts = std::move(tool.published_artifacts)});
      },
      .restore = [](kit::RunContext run, kit::RunInput, kit::RunResume) { return RunCardExport(std::move(run)); }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"fork.reader"}, 1}, .display_name = "Reader", .model = {"fork.history"}}}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"fork.workflow"}, 1}, .display_name = "Workflow reader", .model = {"fork.history"}},
      .run = [](kit::RunContext run, kit::RunInput) -> kit::Async<kit::RunCompletion> {
        auto response = Take(co_await run.CompleteAsync({.instruction = "Use retained context"}));
        co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = std::move(response.text)});
      }}));
  Take(runtime->RegisterTool({
      .id = {"fork.external-review"}, .version = 1, .description = "Review the source report",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::Read,
      .prepare = [&probe](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({
            .preview = {.title = "Review the source report", .artifacts = {probe.original}}});
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded});
      }}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"fork.approver"}, 1}, .display_name = "Reviewer", .model = {"fork.history"},
          .tools = {{"fork.external-review"}}},
      .run = [](kit::RunContext run, kit::RunInput) -> kit::Async<kit::RunCompletion> {
        Take(co_await run.CallToolAsync({{"fork.external-review"}, {}}));
        co_return kit::AgentResult<kit::RunCompletion>::Success({});
      }}));
  return runtime;
}

const kit::Message* MessageWithArtifact(const std::vector<kit::Message>& messages, const kit::ArtifactRef& ref) {
  auto found = std::ranges::find_if(messages, [&](const auto& message) {
    return std::ranges::any_of(message.blocks, [&](const auto& block) {
      const auto* item = std::get_if<kit::ArtifactBlock>(&block.body);
      return item && item->artifact == ref;
    });
  });
  return found == messages.end() ? nullptr : &*found;
}

const kit::Message* MessageWithReportCard(const std::vector<kit::Message>& messages) {
  auto found = std::ranges::find_if(messages, [](const auto& message) {
    return std::ranges::any_of(message.blocks, [](const auto& block) {
      const auto* custom = std::get_if<kit::CustomBlock>(&block.body);
      return custom && custom->type == report_card;
    });
  });
  return found == messages.end() ? nullptr : &*found;
}

kit::Async<kit::CommandProgress> WaitForCommand(const std::shared_ptr<kit::AgentRuntime>& runtime,
                                               kit::CommandId id, kit::CommandState state, bool with_result = false) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    auto progress = co_await runtime->Commands().ReadAsync(id);
    if (progress.Succeeded()) {
      if (progress.Value().state == state && (!with_result || progress.Value().result)) co_return progress;
      if (progress.Value().failure) co_return kit::AgentResult<kit::CommandProgress>::Failure(*progress.Value().failure);
    }
    co_await huxerui::Delay(5ms);
  }
  co_return kit::AgentResult<kit::CommandProgress>::Failure({.code = kit::ErrorCode::InvalidState, .message = "Unexpected action state"});
}

huxerui::Task<void> StoreArtifactFork(huxerui::File file, ArtifactForkProbe& probe) {
  auto runtime = ArtifactForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
      .options = {.agent = {"fork.author"}, .project = kit::ProjectId{"source"}}, .initial = kit::SendInput{.text = "Sealed source note"}}));
  probe.source = std::get<kit::SessionCreated>(created.outcome).session;
  auto source = Take(co_await runtime->Sessions().OpenAsync(probe.source));
  co_await Eventually([&] { return !source->Snapshot()->summary.active_run; }, "report did not publish");
  source = Take(co_await runtime->Sessions().OpenAsync(probe.source));
  auto sealed = source->Snapshot();
  const auto* original = MessageWithArtifact(sealed->messages, probe.original);
  Check(original, "original report did not appear in sealed history");
  const auto* original_card = MessageWithReportCard(sealed->messages);
  Check(original_card && std::get<kit::CustomBlock>(original_card->blocks.front().body).interactions.size() == 1,
      "original custom block did not retain its authorized interaction");
  const kit::HistoryBoundary boundary{probe.source, original_card->ref.message, original_card->sequence};
  Take(co_await source->SendAsync(kit::NewCommandId(), {.text = "Update report"}));
  co_await Eventually([&] { return !source->Snapshot()->summary.active_run; }, "report update did not publish");
  Check(MessageWithArtifact(source->Snapshot()->messages, probe.latest), "source report revision was not advanced");
  std::vector<kit::ArtifactRef> registered;
  auto events = Take(co_await source->ObserveEventsAsync(0, [&](auto update) {
    const auto batch = Take(std::move(update));
    for (const auto& event : batch->events) {
      if (const auto* artifact = std::get_if<kit::ArtifactRegistered>(&event.body))
        registered.push_back(artifact->artifact);
    }
  }));
  co_await Eventually([&] { return registered.size() >= 2; }, "artifact registrations were not published");
  Check(registered == std::vector{probe.original, probe.latest}, "artifact events changed the original version or order");
  events.Reset();
  auto copied = Take(co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), {
      .from = boundary, .options = {.agent = {"fork.reader"}, .project = kit::ProjectId{"copy"}}}));
  probe.branch = std::get<kit::SessionCreated>(copied.outcome).session;
  auto target = Take(co_await runtime->Sessions().OpenAsync(probe.branch));
  auto view = target->Snapshot();
  const auto* inherited = MessageWithArtifact(view->messages, probe.original);
  Check(inherited && inherited->ref == original->ref && !MessageWithArtifact(view->messages, probe.latest) && view->artifacts.empty(),
        "fork did not retain the original reference without copying ownership");
  const auto* inherited_card = MessageWithReportCard(view->messages);
  Check(inherited_card && inherited_card->ref == original_card->ref &&
      std::get<kit::CustomBlock>(inherited_card->blocks.front().body).interactions.empty(),
      "fork exposed an interaction bound to the source run");
  auto denied = Take(co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), {
      .from = boundary, .options = {.agent = {"fork.reader"}, .project = kit::ProjectId{"restricted"}}}));
  auto hidden = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(denied.outcome).session));
  auto hidden_view = hidden->Snapshot();
  Check(!MessageWithArtifact(hidden_view->messages, probe.original) && !MessageWithReportCard(hidden_view->messages),
      "fork persisted a reference denied in the target");
  auto worker = Take(co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), {
      .from = boundary, .options = {.agent = {"fork.workflow"}}}));
  auto host = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(worker.outcome).session));
  Take(co_await host->SendAsync(kit::NewCommandId(), {.text = "Use original report"}));
  co_await Eventually([&] { return !host->Snapshot()->summary.active_run; }, "host workflow did not submit the inherited history");
  Check(std::ranges::any_of(probe.model_text, [](const auto& text) { return text.find("Original report") != text.npos; }),
        "host model did not receive the authorized version reference");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> StageCardExport(huxerui::File file, ArtifactForkProbe& probe) {
  auto runtime = ArtifactForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto source = Take(co_await runtime->Sessions().OpenAsync(probe.source));
  auto sent = Take(co_await source->SendAsync(kit::NewCommandId(), {.text = "Export the report chart"}));
  probe.export_run = std::get<kit::RunAccepted>(sent.outcome).run;
  std::optional<kit::CustomBlockRef> origin;
  co_await Eventually([&] {
    const auto snapshot = source->Snapshot();
    for (const auto& message : snapshot->messages) {
      if (!snapshot->summary.active_run || message.run != snapshot->summary.active_run) continue;
      const auto found = std::ranges::find_if(message.blocks, [](const auto& block) {
        return std::holds_alternative<kit::CustomBlock>(block.body);
      });
      if (found != message.blocks.end()) origin = kit::CustomBlockRef{message.ref, found->id};
    }
    return origin.has_value();
  }, "the export run did not publish a bound card");
  probe.export_action = {*origin, "export", kit::JsonValue{true}};
  auto stale = probe.export_action;
  ++stale.target.message.version;
  auto stale_reply = co_await source->DispatchContentActionAsync(kit::NewCommandId(), stale);
  Check(!stale_reply.Succeeded() && stale_reply.Error().code == kit::ErrorCode::Conflict, "stale card dispatched an action");
  auto invalid = probe.export_action;
  invalid.payload = kit::JsonValue{std::string("invalid")};
  auto rejected = co_await source->DispatchContentActionAsync(kit::NewCommandId(), invalid);
  Check(!rejected.Succeeded() && rejected.Error().code == kit::ErrorCode::InvalidArgument && !probe.card_preparations,
      "invalid action payload reached the prepare handler");

  const auto revoked = kit::NewCommandId();
  auto first = Take(co_await source->DispatchContentActionAsync(revoked, probe.export_action));
  auto same = Take(co_await source->DispatchContentActionAsync(revoked, probe.export_action));
  auto first_approval = std::get<kit::PendingApproval>(first.outcome);
  Check(same.stamp.commit == first.stamp.commit && probe.card_preparations == 1, "action replay prepared another request");
  probe.deny_card_action = true;
  Take(co_await runtime->RefreshAccessAsync());
  Take(co_await source->SubmitAsync(kit::NewCommandId(), {
      first_approval.interaction, first_approval.version, kit::ApprovalDecision::Approve}));
  auto failed = Take(co_await WaitForCommand(runtime, revoked, kit::CommandState::Failed));
  Check(failed.failure && failed.failure->code == kit::ErrorCode::PermissionDenied && !probe.exports,
      "approved action executed after permission was revoked");
  probe.deny_card_action = false;
  Take(co_await runtime->RefreshAccessAsync());
  probe.pending_steer = kit::NewCommandId();
  const kit::CustomBlockAction steer{*origin, "refine", kit::JsonValue{true}};
  auto steering = Take(co_await source->DispatchContentActionAsync(probe.pending_steer, steer));
  const auto steer_approval = std::get<kit::PendingApproval>(steering.outcome);
  Take(co_await source->SubmitAsync(kit::NewCommandId(), {
      steer_approval.interaction, steer_approval.version, kit::ApprovalDecision::Approve}));
  auto staged_steer = Take(co_await WaitForCommand(runtime, probe.pending_steer, kit::CommandState::Scheduled, true));
  Check(staged_steer.result && std::get<kit::RunSteered>(*staged_steer.result).run == probe.export_run,
      "approved component supplement did not register for the original run");
  probe.pending_export = kit::NewCommandId();
  auto pending = Take(co_await source->DispatchContentActionAsync(probe.pending_export, probe.export_action));
  Check(std::holds_alternative<kit::PendingApproval>(pending.outcome) && probe.card_preparations == 2 && !probe.exports,
      "export action did not wait for approval");
  auto conflict = co_await source->DispatchContentActionAsync(probe.pending_export, invalid);
  Check(!conflict.Succeeded() && conflict.Error().code == kit::ErrorCode::Conflict, "same action command accepted different data");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> FinishCardExport(huxerui::File file, ArtifactForkProbe& probe) {
  auto runtime = ArtifactForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto source = Take(co_await runtime->Sessions().OpenAsync(probe.source));
  auto pending = Take(co_await runtime->Commands().ReadAsync(probe.pending_export));
  Check(pending.state == kit::CommandState::AwaitingApproval && pending.approval, "component action approval did not survive restart");
  auto replay = Take(co_await source->DispatchContentActionAsync(probe.pending_export, probe.export_action));
  Check(replay.stamp.commit == pending.accepted.commit && probe.card_preparations == 2, "restarting reran the prepare handler");
  Take(co_await source->SubmitAsync(kit::NewCommandId(), {
      pending.approval->interaction, pending.approval->version, kit::ApprovalDecision::Approve}));
  auto applied = Take(co_await WaitForCommand(runtime, probe.pending_export, kit::CommandState::Applied));
  Check(applied.resolution && applied.result && std::holds_alternative<kit::InteractionConsumed>(*applied.result),
      "component command did not record the form consumption");
  auto steered = Take(co_await WaitForCommand(runtime, probe.pending_steer, kit::CommandState::Applied));
  Check(steered.result && std::get<kit::RunSteered>(*steered.result).run == probe.export_run,
      "supplement resolution lost its original run");
  std::optional<kit::InteractionSnapshot> approval;
  co_await Eventually([&] {
    auto snapshot = source->Snapshot();
    for (const auto& item : snapshot->interactions) {
      const auto* tool = std::get_if<kit::ToolApproval>(&item.body);
      if (tool && tool->preview.title == "Save report chart CSV" && item.state == kit::InteractionState::Pending) approval = item;
    }
    return approval.has_value();
  }, "bound form did not invoke the regular export tool approval");
  Check(!std::filesystem::exists(probe.export_file) && !probe.exports, "component action bypassed approval for the file write");
  Take(co_await source->SubmitAsync(kit::NewCommandId(), {approval->id, approval->version, kit::ApprovalDecision::Approve}));
  co_await Eventually([&] { return !source->Snapshot()->summary.active_run; }, "the approved file write did not close the run");
  std::ifstream csv(probe.export_file, std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(csv)), {});
  Check(contents == "category,count\nOperations,3\n" && probe.exports == 1 && probe.card_preparations == 2,
      "the exported CSV differed from the approved payload or was repeated");
  Take(co_await source->DispatchContentActionAsync(probe.pending_export, probe.export_action));
  Check(probe.exports == 1, "completed action replay repeated the write");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ReopenArtifactFork(huxerui::File file, ArtifactForkProbe& probe) {
  auto runtime = ArtifactForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto branch = Take(co_await runtime->Sessions().OpenAsync(probe.branch));
  auto history = Take(co_await runtime->Sessions().ReadHistoryAsync(probe.branch, {}));
  const auto* preserved = MessageWithReportCard(history.messages);
  Check(preserved, "custom block was not restored in paginated history");
  const auto& block = preserved->blocks.front();
  const auto& card = std::get<kit::CustomBlock>(block.body);
  Check(card.data == kit::JsonValue{std::string("Versioned summary data")} && card.artifacts == std::vector{probe.original} &&
      card.interactions.empty(),
      "stored card lost its versioned bindings or failed to use the fallback");
  auto nested = kit::NewCommandId();
  Take(co_await branch->DispatchContentActionAsync(nested, {{preserved->ref, block.id}, "summarize", kit::JsonValue{true}}));
  co_await Eventually([&] { return !branch->Snapshot()->summary.active_run; }, "component's artifact action did not start a run");
  auto nested_action = Take(co_await runtime->Commands().ReadAsync(nested));
  Check(nested_action.result && std::holds_alternative<kit::RunAccepted>(*nested_action.result), "component lost its artifact action result");
  auto direct = Take(co_await branch->DispatchArtifactActionAsync(kit::NewCommandId(), {probe.original, "summarize", kit::JsonValue{true}}));
  Check(std::holds_alternative<kit::RunAccepted>(direct.outcome), "standalone artifact action did not return the run");
  co_await Eventually([&] { return !branch->Snapshot()->summary.active_run; }, "standalone artifact action did not finish");
  const auto has_text = [&](std::string_view text) {
    return std::ranges::any_of(probe.model_text, [&](const auto& item) { return item.find(text) != item.npos; });
  };
  auto view = Take(co_await branch->PreviewArtifactAsync(probe.original));
  Check(view.summary == "published-payload-v1" && view.artifact == probe.original, "reopened fork did not resolve the original payload");
  auto newer = co_await branch->PreviewArtifactAsync(probe.latest);
  Check(!newer.Succeeded() && newer.Error().code == kit::ErrorCode::NotFound, "branch preview escaped the sealed boundary");
  const kit::SourceSlice text{probe.original, {}, kit::SourceTextRange{kit::PartId{"body"}, 10, 20}};
  auto selected = Take(co_await branch->ReadSelectionAsync(text));
  Check(selected == "payload-v1", "inherited selection resolved a different version or byte range");
  const kit::SourceSlice outside{probe.latest, {}, text.text_range};
  auto denied_selection = co_await branch->ReadSelectionAsync(outside);
  Check(!denied_selection.Succeeded() && denied_selection.Error().code == kit::ErrorCode::NotFound,
        "artifact selection escaped the sealed boundary");
  Take(co_await branch->SendAsync(kit::NewCommandId(), {.text = "Use the original report explicitly", .context = {{probe.original}}}));
  co_await Eventually([&] { return !branch->Snapshot()->summary.active_run; }, "the branch did not submit the explicit citation");
  Check(has_text("published-payload-v1") && !has_text("published-payload-v2"), "explicit source input used a different version");
  probe.deny_model_artifact = true;
  kit::SendInput forbidden{.text = "Reference an excerpt denied to the model", .context = {text}};
  auto forbidden_send = co_await branch->SendAsync(kit::NewCommandId(), std::move(forbidden));
  Check(!forbidden_send.Succeeded() && forbidden_send.Error().code == kit::ErrorCode::PermissionDenied,
        "the model received an artifact excerpt denied to it");
  Take(co_await branch->SendAsync(kit::NewCommandId(), {.text = "Ask with model artifact access denied"}));
  co_await Eventually([&] { return !branch->Snapshot()->summary.active_run; }, "normal input with restricted context did not finish");
  Check(!has_text("Original report") && !has_text("published-payload-v1") && has_text("Sealed source note"),
        "model recipient received a reference that was allowed only in history");
  Take(co_await branch->PreviewArtifactAsync(probe.original));

  std::shared_ptr<const kit::SessionSnapshot> expanded;
  auto observed = Take(co_await branch->ObserveAsync([&](auto result) {
    expanded = result.Succeeded() ? std::move(result).Value() : nullptr;
  }, kit::SessionHistoryWindow{0}));
  co_await Eventually([&] { return expanded && MessageWithReportCard(expanded->messages); },
      "expanded observation did not deliver the inherited card");
  probe.hold_preview = true;
  probe.preview_started = false;
  probe.selection_started = false;
  auto answer = std::make_shared<std::optional<kit::AgentResult<kit::ArtifactPreview>>>();
  auto excerpt = std::make_shared<std::optional<kit::AgentResult<std::string>>>();
  huxerui::UseApplicationTaskScope().Launch([answer, branch, ref = probe.original]() -> huxerui::Task<void> {
    answer->emplace(co_await branch->PreviewArtifactAsync(ref));
  });
  huxerui::UseApplicationTaskScope().Launch([excerpt, branch, text]() -> huxerui::Task<void> {
    excerpt->emplace(co_await branch->ReadSelectionAsync(text));
  });
  co_await Eventually([&] { return probe.preview_started && probe.selection_started; }, "the reads did not reach artifact storage");
  probe.deny_artifact = true;
  Take(co_await runtime->RefreshAccessAsync());
  probe.hold_preview = false;
  co_await Eventually([&] {
    auto snapshot = branch->Snapshot();
    return answer->has_value() && excerpt->has_value() && snapshot && expanded &&
        !MessageWithArtifact(expanded->messages, probe.original) && !MessageWithReportCard(expanded->messages) &&
        !MessageWithArtifact(snapshot->messages, probe.original) && !MessageWithReportCard(snapshot->messages);
  }, "revocation did not update the pending preview and history projection");
  Check(!answer->value().Succeeded() && answer->value().Error().code == kit::ErrorCode::PermissionDenied,
        "preview returned a payload after a concurrent revocation");
  Check(!excerpt->value().Succeeded() && excerpt->value().Error().code == kit::ErrorCode::PermissionDenied,
        "artifact selection returned a payload after a concurrent revocation");
  auto page = Take(co_await runtime->Sessions().ReadHistoryAsync(probe.branch, {}));
  Check(!MessageWithArtifact(page.messages, probe.original) && !MessageWithReportCard(page.messages),
      "revoked reference survived in paginated history");
  kit::Sequence replayed = 0;
  auto events = Take(co_await branch->ObserveEventsAsync(0, [&](auto update) {
    const auto batch = Take(std::move(update));
    for (const auto& event : batch->events) {
      if (const auto* message = std::get_if<kit::Message>(&event.body)) {
        Check(std::ranges::none_of(message->blocks, [&](const auto& block) {
          const auto* artifact = std::get_if<kit::ArtifactBlock>(&block.body);
          const auto* custom = std::get_if<kit::CustomBlock>(&block.body);
          return (artifact && artifact->artifact == probe.original) ||
                 (custom && std::ranges::find(custom->artifacts, probe.original) != custom->artifacts.end());
        }), "event replay exposed revoked inherited content");
      }
    }
    replayed = batch->through_sequence;
  }));
  co_await Eventually([&] { return replayed > 0 && replayed == branch->Snapshot()->event_sequence; },
      "authorized replay did not reach the branch cursor");
  events.Reset();
  auto submission = co_await branch->SendAsync(kit::NewCommandId(), {.text = "Reference revoked report", .context = {{probe.original}}});
  Check(!submission.Succeeded() && submission.Error().code == kit::ErrorCode::PermissionDenied, "revoked reference entered a new run");
  probe.deny_model_artifact = false;
  Take(co_await branch->SendAsync(kit::NewCommandId(), {.text = "Continue after report revocation"}));
  co_await Eventually([&] { return !branch->Snapshot()->summary.active_run; }, "model did not continue after report revocation");
  Check(!has_text("Original report") && has_text("Sealed source note"),
        "model recipient bypassed the user's revoked report permission");
  probe.deny_source = true;
  Take(co_await runtime->RefreshAccessAsync());
  co_await Eventually([&] { auto snapshot = branch->Snapshot(); return snapshot && !snapshot->origin->source_session; },
      "source identity was not hidden after revocation");
  Take(co_await branch->SendAsync(kit::NewCommandId(), {.text = "Continue with local history"}));
  co_await Eventually([&] { return !branch->Snapshot()->summary.active_run; }, "new run did not continue with the visible history");
  Check(!has_text("Sealed source note") && !has_text("Original report") && has_text("Continue with local history"),
        "new model turn reused withdrawn source history");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> VerifyApprovalAttachment(huxerui::File file, ArtifactForkProbe& probe) {
  probe.deny_artifact = false;
  probe.deny_source = false;
  auto runtime = ArtifactForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  const auto create = [&](kit::ProjectId project) {
    return runtime->Sessions().CreateAsync(kit::NewCommandId(), {
        .options = {.agent = {"fork.approver"}, .project = std::move(project)},
        .initial = kit::SendInput{.text = "Review the report in this project"}});
  };
  auto created = Take(co_await create({"source"}));
  probe.review_session = std::get<kit::SessionCreated>(created.outcome).session;
  auto review = Take(co_await runtime->Sessions().OpenAsync(probe.review_session));
  auto outside = Take(co_await create({"copy"}));
  auto foreign = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(outside.outcome).session));
  std::shared_ptr<const kit::SessionSnapshot> current;
  auto observed = Take(co_await review->ObserveAsync([&](auto result) {
    current = result.Succeeded() ? std::move(result).Value() : nullptr;
  }));
  const auto projected = [&]() -> std::optional<kit::ToolApproval> {
    if (!current || current->interactions.size() != 1 || current->interactions.front().state != kit::InteractionState::Pending)
      return {};
    return std::get<kit::ToolApproval>(current->interactions.front().body);
  };
  co_await Eventually([&] {
    const auto approval = projected();
    return approval && approval->preview.artifacts == std::vector{probe.original};
  }, "same-project approval did not expose the authorized version");
  const auto interaction = current->interactions.front();
  Check(current->artifacts.empty(), "a source attachment became an owned artifact");
  auto own_preview = Take(co_await review->PreviewArtifactAsync(probe.original));
  Check(own_preview.artifact == probe.original && own_preview.summary == "published-payload-v1",
        "approval review returned a different version");
  Check(!(co_await review->PreviewArtifactAsync(probe.latest)).Succeeded(), "approval preview accepted an unbound version");
  co_await Eventually([&] {
    const auto snapshot = foreign->Snapshot();
    return snapshot && snapshot->summary.pending_interactions == 1;
  }, "cross-project approval fixture did not prepare its tool");
  foreign = Take(co_await runtime->Sessions().OpenAsync(foreign->Snapshot()->summary.id));
  Check(std::get<kit::ToolApproval>(foreign->Snapshot()->interactions.front().body).preview.artifacts.empty() &&
        !(co_await foreign->PreviewArtifactAsync(probe.original)).Succeeded(), "approval preview crossed a project boundary");

  probe.hold_preview = true;
  probe.preview_started = false;
  auto reading = std::make_shared<std::optional<kit::AgentResult<kit::ArtifactPreview>>>();
  huxerui::UseApplicationTaskScope().Launch([reading, review, ref = probe.original]() -> huxerui::Task<void> {
    reading->emplace(co_await review->PreviewArtifactAsync(ref));
  });
  co_await Eventually([&] { return probe.preview_started; }, "the approval preview did not reach the source store");
  probe.deny_attachment_context = true;
  Take(co_await runtime->RefreshAccessAsync());
  probe.hold_preview = false;
  co_await Eventually([&] {
    const auto approval = projected();
    return approval && approval->preview.artifacts.empty() && reading->has_value();
  }, "revocation did not hide the source attachment from the active approval");
  Check(!reading->value().Succeeded() && reading->value().Error().code == kit::ErrorCode::PermissionDenied,
        "revocation returned an in-flight approval payload");
  const kit::InteractionFilter filter{.session = probe.review_session};
  const auto pending = Take(co_await runtime->Interactions().ListPendingAsync(filter, std::nullopt));
  Check(pending.items.size() == 1 && std::get<kit::ToolApproval>(pending.items.front().interaction.body).preview.artifacts.empty(),
        "the pending directory retained a revoked source attachment");
  auto source = Take(co_await runtime->Artifacts().PreviewAsync(probe.original));
  Check(source.artifact == probe.original && current->interactions.front().id == interaction.id &&
        current->interactions.front().version == interaction.version, "projection changed the source or the approval binding");
  Take(co_await runtime->CloseAsync());
}

} // namespace

huxerui::Task<void> VerifyArtifactFork(huxerui::File file) {
  ArtifactForkProbe probe;
  const auto export_path = file.Path() + ".report.csv";
  probe.export_file = std::filesystem::path(std::u8string(export_path.begin(), export_path.end()));
  co_await StoreArtifactFork(file, probe);
  co_await StageCardExport(file, probe);
  co_await FinishCardExport(file, probe);
  co_await ReopenArtifactFork(file, probe);
  co_await VerifyApprovalAttachment(file, probe);
}
