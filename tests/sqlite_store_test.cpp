#include "sqlite_session_store.h"

#include <huxerui/agentkit.h>
#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>
#include <huxerui/testing/ui_test.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <source_location>
#include <thread>

huxerui::Task<void> VerifyArtifactFork(huxerui::File file);

namespace {
namespace kit = huxerui::agentkit;
namespace db = huxerui::sqlite;
using namespace std::chrono_literals;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(kit::AgentResult<T> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return std::move(result).Value();
}

void ExpectError(const kit::AgentResult<kit::Revision>& result, kit::ErrorCode code) {
  Check(!result.Succeeded(), "transaction unexpectedly committed");
  if (result.Error().code != code) throw std::runtime_error(result.Error().message);
}

class LostReceiptStore final : public kit::SessionStore {
public:
  explicit LostReceiptStore(std::shared_ptr<kit::SessionStore> store, bool multi_record_only = false, bool recover = true)
      : store_(std::move(store)), multi_record_only_(multi_record_only), recover_(recover) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return store_->LoadAsync(); }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction transaction) override {
    const bool inject = !multi_record_only_ || std::ranges::count_if(transaction.writes,
        [](const kit::StoreWrite& write) { return write.collection != "agentkit.session-events"; }) > 1;
    auto result = co_await store_->CommitAsync(std::move(transaction));
    if (result.Succeeded() && inject && std::exchange(drop_, false)) {
      co_return kit::AgentResult<kit::Revision>::Failure(
          {.code = kit::ErrorCode::CommitUncertain, .message = "Injected lost reply"});
    }
    co_return result;
  }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId id) override {
    ++lookups;
    if (!recover_) return MissingReceipt();
    return store_->FindCommitAsync(std::move(id));
  }

  int lookups = 0;

private:
  static kit::Async<std::optional<kit::Revision>> MissingReceipt() {
    co_return kit::AgentResult<std::optional<kit::Revision>>::Success(std::nullopt);
  }

  std::shared_ptr<kit::SessionStore> store_;
  bool multi_record_only_;
  bool recover_;
  bool drop_ = true;
};

const std::vector<std::byte> private_model_state{std::byte{0x5a}, std::byte{0x00}, std::byte{0xc3}};

class OneTurn final : public kit::ModelTurnStream {
public:
  explicit OneTurn(std::vector<kit::ModelEvent> events, bool pause_after_first = false)
      : events_(std::move(events)), pause_after_first_(pause_after_first) {}

  explicit OneTurn(bool tool = false, std::optional<kit::ModelResumeState> resume = {},
                   std::string tool_name = "test.effect")
      : events_(tool ? std::vector<kit::ModelEvent>{
            kit::ModelToolStarted{"write-call", std::move(tool_name)}, kit::ModelToolArguments{"write-call", "{}"},
            kit::ModelToolFinished{"write-call"}, kit::ModelFinished{kit::ModelFinishReason::ToolCalls, std::move(resume)}}
          : std::vector<kit::ModelEvent>{
            kit::ModelTextDelta{"Ready"}, kit::ModelFinished{kit::ModelFinishReason::Completed, {}}}) {}

  kit::Async<std::optional<kit::ModelEvent>> ReadAsync() override {
    if (pause_after_first_ && index_ == 1) co_await huxerui::Delay(1h);
    std::optional<kit::ModelEvent> event;
    if (index_ < events_.size()) event = std::move(events_[index_++]);
    co_return kit::AgentResult<std::optional<kit::ModelEvent>>::Success(std::move(event));
  }

  kit::Async<kit::ModelStopOutcome> StopAsync() override {
    co_return kit::AgentResult<kit::ModelStopOutcome>::Success(kit::ModelStopOutcome::LocalStopped);
  }

private:
  std::vector<kit::ModelEvent> events_;
  std::size_t index_ = 0;
  bool pause_after_first_ = false;
};

class CountingModel final : public kit::ModelProvider {
public:
  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    ++starts;
    const auto result = std::ranges::find_if(request.items, [](const auto& item) {
      const auto* result = std::get_if<kit::ModelToolResult>(&item);
      return result && result->provider_call_id == "write-call";
    });
    const bool calling_tool = write && result == request.items.end();
    if (result != request.items.end()) received_result = std::get<kit::ModelToolResult>(*result);
    auto tool = request.tools.empty() ? std::string{"test.effect"} : request.tools.front().name;
    std::optional<kit::ModelResumeState> resume;
    if (calling_tool && with_private_state) resume = kit::ModelResumeState{request.provider, 1, private_model_state};
    received_state = std::move(request.continuation);
    if (split_text && !calling_tool) {
      co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<OneTurn>(
          std::vector<kit::ModelEvent>{kit::ModelTextDelta{"Ready"}, kit::ModelTextDelta{" \xe4\xbd\xa0\xe5\xa5\xbd"},
                                      kit::ModelFinished{kit::ModelFinishReason::Completed, {}}}, pause_text));
    }
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(
        std::make_unique<OneTurn>(calling_tool, std::move(resume), std::move(tool)));
  }
  int starts = 0;
  bool write = false;
  bool with_private_state = false;
  bool split_text = false;
  bool pause_text = false;
  std::optional<kit::ModelContinuation> received_state;
  std::optional<kit::ModelToolResult> received_result;
};

struct EffectProbe {
  kit::ToolCallId started;
  kit::ToolCallId queried;
  unsigned executions = 0;
  unsigned preparations = 0;
  unsigned execute_checks = 0;
  bool outcome_known = false;
  bool ask = false;
  bool review = false;
  bool hold_before_effect = false;
  bool hold_effect = true;
  bool deny = false;
  bool deny_reconciliation = false;
  kit::CommandId reconciliation;
  kit::CommitId reconciliation_commit;
};

void Take(kit::AgentResult<void> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

const kit::ToolCatalogId test_catalog{"test.catalog"};

std::shared_ptr<kit::AgentRuntime> RegisterRuntime(std::shared_ptr<kit::SessionStore> store,
                                                  std::shared_ptr<CountingModel> model,
                                                  std::shared_ptr<EffectProbe> probe = {},
                                                  bool persist_model_state = false,
                                                  std::optional<kit::ToolReviewer> reviewer = {},
                                                  kit::AccessPolicy extra_policy = {}, kit::PrincipalResolver principal = {},
                                                  std::optional<kit::ToolDefinition>* dynamic_tool = nullptr) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = principal ? std::move(principal) :
          [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [probe, extra_policy = std::move(extra_policy)](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        if (extra_policy) co_return co_await extra_policy(std::move(request));
        auto decision = kit::AccessDecision::Allow;
        if (probe && request.capability == kit::Capability::ExecuteTool) {
          if (++probe->execute_checks > 1 && probe->hold_before_effect) co_await huxerui::Delay(1h);
          if (probe->deny) decision = kit::AccessDecision::Deny;
          else if (probe->ask) decision = kit::AccessDecision::RequireConfirmation;
          else if (probe->review) decision = kit::AccessDecision::AllowReview;
        }
        if (probe && request.capability == kit::Capability::ReconcileTool) {
          Check(request.actor.kind == kit::ActionParty::Kind::Host && request.actor.call == probe->started,
              "reconciliation was authorized as a host against the original call");
          if (probe->deny_reconciliation) decision = kit::AccessDecision::Deny;
        }
        co_return kit::AgentResult<kit::PolicyDecision>::Success({decision, {}});
      },
      .tool_reviewer = std::move(reviewer),
      .store = std::move(store)}));
  Take(runtime->RegisterModel({.ref = {{"test.model"}, 1}, .endpoint = {"test", "test", "script", {}}, .provider = std::move(model),
      .allow_persist_resume_state = persist_model_state}));
  const auto agent = probe ? "test.writer" : "test.agent";
  if (probe) {
    kit::ToolDefinition effect{
        .id = {dynamic_tool ? "test.catalog/effect" : "test.effect"}, .version = 1, .description = "Record a test effect",
        .input_schema = {kit::JsonValue{kit::JsonValue::Object{}}},
        .effect = kit::ToolEffect::ExternalSubmit,
        .prepare = [probe](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
          ++probe->preparations;
          co_return kit::AgentResult<kit::PreparedChange>::Success(
              {.preview = {.title = "Effect"}, .undo_support = kit::UndoSupport::None});
        },
        .execute = [probe](kit::ToolContext context, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
          probe->started = context.CallId();
          ++probe->executions;
          if (probe->hold_effect) co_await huxerui::Delay(1h);
          co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded});
        },
        .lookup_result = [probe](kit::ToolCallId id) -> kit::Async<std::optional<kit::ToolExecutionResult>> {
          probe->queried = id;
          auto result = probe->outcome_known
              ? std::optional<kit::ToolExecutionResult>{{.outcome = kit::ToolOutcome::Succeeded}}
              : std::nullopt;
          co_return kit::AgentResult<std::optional<kit::ToolExecutionResult>>::Success(std::move(result));
        }};
    if (dynamic_tool) {
      Take(runtime->RegisterToolCatalog(test_catalog));
      *dynamic_tool = std::move(effect);
    } else {
      Take(runtime->RegisterTool(std::move(effect)));
    }
  }
  Take(runtime->RegisterAgent({{.ref = {{agent}, 1}, .display_name = "Test", .model = {"test.model"},
                               .tools = probe && !dynamic_tool ? std::vector<kit::ToolId>{{"test.effect"}} : std::vector<kit::ToolId>{},
                               .tool_catalogs = dynamic_tool ? std::vector{test_catalog} : std::vector<kit::ToolCatalogId>{}}}));
  return runtime;
}

huxerui::Task<void> Eventually(std::function<bool()> condition, const char* message);

huxerui::Task<void> VerifyRuntimeReopen(huxerui::File file) {
  auto command = kit::NewCommandId();
  kit::CreateSessionRequest request{
      .options = {.agent = {"test.agent"}, .retention = kit::SessionRetention::Stored},
      .initial = kit::SendInput{.text = "Persistent title"}};
  kit::Receipt<kit::SessionCreated> first;
  kit::Sequence cursor = 0;
  std::vector<kit::SessionEvent> live;
  {
    auto store = std::make_shared<LostReceiptStore>(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)));
    auto model = std::make_shared<CountingModel>();
    model->split_text = true;
    auto runtime = RegisterRuntime(store, model);
    Take(co_await runtime->StartAsync());
    first = Take(co_await runtime->Sessions().CreateAsync(command, request));
    Check(store->lookups == 1, "lost acknowledgement resolved by original commit ID");
    auto session = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(first.outcome).session));
    auto events = Take(co_await session->ObserveEventsAsync(0, [&](auto update) {
      const auto batch = Take(std::move(update));
      for (const auto& event : batch->events) {
        Check(event.sequence == ++cursor && event.session == session->Snapshot()->summary.id, "live event order changed");
        live.push_back(event);
      }
      Check(cursor == batch->through_sequence, "unfiltered event batch has a gap");
    }));
    for (int tries = 0; tries < 100 && session->Snapshot()->summary.active_run; ++tries) {
      co_await huxerui::Delay(10ms);
    }
    Check(session->Snapshot()->summary.run_state == kit::RunState::Succeeded && model->starts == 1,
          "original model turn completed once");
    co_await Eventually([&] { return cursor == session->Snapshot()->event_sequence && cursor > 0; }, "live journal did not reach the snapshot");
    const auto streaming = std::ranges::find_if(live, [](const auto& event) {
      const auto* message = std::get_if<kit::Message>(&event.body);
      return message && message->state == kit::MessageState::Streaming;
    });
    const auto delta = std::ranges::find_if(live, [](const auto& event) { return std::holds_alternative<kit::MessageDelta>(event.body); });
    Check(streaming < delta && delta != live.end(), "streamed text was not published as an opening message followed by a delta");
    const auto& opened = std::get<kit::Message>(streaming->body);
    const auto& increment = std::get<kit::MessageDelta>(delta->body);
    const auto final = session->Snapshot();
    const auto& complete = final->messages.back();
    Check(increment.message == opened.ref && increment.block == opened.blocks.front().id &&
          std::get<kit::TextBlock>(opened.blocks.front().body).text + increment.utf8 ==
              std::get<kit::TextBlock>(complete.blocks.front().body).text &&
          std::ranges::any_of(live, [&](const auto& event) {
            const auto* message = std::get_if<kit::Message>(&event.body);
            return message && message->ref == complete.ref && message->state == kit::MessageState::Complete;
          }), "text delta or sealed message lost its original version and block");
    Check(live.front().commit == first.stamp.commit && live.front().command == command, "creation event lost its receipt binding");
    events.Reset();
    Take(co_await runtime->CloseAsync());
  }
  {
    auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
    auto model = std::make_shared<CountingModel>();
    bool may_read = true;
    auto runtime = RegisterRuntime(store, model, {}, false, {}, [&](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
      co_return kit::AgentResult<kit::PolicyDecision>::Success({
          !may_read && request.capability == kit::Capability::ReadSession ? kit::AccessDecision::Deny : kit::AccessDecision::Allow});
    });
    Take(co_await runtime->StartAsync());
    auto original = Take(co_await runtime->Commands().ReadAsync(command));
    auto replay = Take(co_await runtime->Sessions().CreateAsync(command, request));
    Check(original.accepted.commit == replay.stamp.commit && replay.stamp.commit == first.stamp.commit,
          "reopen returned the original command receipt");
    const auto created = std::get<kit::SessionCreated>(replay.outcome);
    auto catalog = Take(co_await runtime->Sessions().ListAsync({}, {}));
    auto history = Take(co_await runtime->Sessions().ReadHistoryAsync(created.session, {}));
    Check(catalog.sessions.size() == 1 && catalog.sessions.front().title == "Persistent title" &&
          history.messages.size() == 2 && history.messages.back().run == created.initial_run && model->starts == 0,
          "restored catalog and history, with no duplicate run");
    auto session = Take(co_await runtime->Sessions().OpenAsync(created.session));
    Check(session->Snapshot()->event_sequence == cursor, "restart changed the committed event cursor");
    std::size_t replayed = 0;
    auto journal = Take(co_await session->ObserveEventsAsync(0, [&](auto update) {
      const auto batch = Take(std::move(update));
      for (const auto& event : batch->events) {
        const auto& expected = live.at(replayed++);
        Check(event.sequence == expected.sequence && event.commit == expected.commit &&
              event.command == expected.command && event.body.index() == expected.body.index(),
              "SQLite replay changed a committed event");
      }
    }));
    co_await Eventually([&] { return replayed == live.size(); }, "SQLite did not replay the complete journal");
    journal.Reset();

    std::vector<kit::SessionEvent> changes;
    std::optional<kit::ErrorCode> ended;
    auto tail = Take(co_await session->ObserveEventsAsync(cursor, [&](auto update) {
      if (!update.Succeeded()) { ended = update.Error().code; return; }
      for (const auto& event : update.Value()->events) changes.push_back(event);
    }));
    const auto rename = kit::NewCommandId();
    const auto receipt = Take(co_await runtime->Sessions().UpdateMetadataAsync(rename, created.session,
        session->Snapshot()->summary.metadata_revision, kit::RenameSession{"After replay"}));
    co_await Eventually([&] { return !changes.empty(); }, "resumed event tail did not deliver a new commit");
    Check(changes.size() == 1 && changes.front().sequence == cursor + 1 && changes.front().command == rename &&
          changes.front().commit == receipt.stamp.commit && std::get<kit::SessionSummary>(changes.front().body).title == "After replay",
          "resume repeated old events or missed the first new commit");
    may_read = false;
    Take(co_await runtime->RefreshAccessAsync());
    co_await Eventually([&] { return ended.has_value(); }, "event observer did not end after access was revoked");
    Check(ended == kit::ErrorCode::PermissionDenied, "access revocation did not end with PermissionDenied");
    Take(co_await runtime->CloseAsync());
  }
  {
    auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
    const auto image = Take(co_await store->LoadAsync());
    const auto missing = std::ranges::find_if(image.records, [&](const auto& row) {
      return row.collection == "agentkit.session-events" && row.key == first.stamp.commit.value;
    });
    Check(missing != image.records.end(), "the journal was not persisted with the creation commit");
    kit::StoreTransaction remove{{kit::NewCommandId().value}, image.revision,
        {{missing->collection, missing->key, missing->revision, std::nullopt}}};
    Take(co_await store->CommitAsync(std::move(remove)));
    auto runtime = RegisterRuntime(store, std::make_shared<CountingModel>());
    Take(co_await runtime->StartAsync());
    auto session = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(first.outcome).session));
    std::optional<kit::ErrorCode> gap;
    auto journal = Take(co_await session->ObserveEventsAsync(0, [&](auto update) {
      Check(!update.Succeeded(), "an incomplete journal was replayed as contiguous");
      gap = update.Error().code;
    }));
    co_await Eventually([&] { return gap.has_value(); }, "missing journal range did not expire the cursor");
    const auto snapshot = session->Snapshot();
    Check(gap == kit::ErrorCode::CursorExpired && snapshot->messages.size() == 2 && snapshot->event_sequence == cursor + 1,
          "the committed snapshot did not provide a recovery cursor");
    std::optional<kit::Sequence> resumed;
    auto tail = Take(co_await session->ObserveEventsAsync(snapshot->event_sequence, [&](auto update) {
      resumed = Take(std::move(update))->through_sequence;
    }));
    Take(co_await runtime->Sessions().UpdateMetadataAsync(kit::NewCommandId(), snapshot->summary.id,
        snapshot->summary.metadata_revision, kit::RenameSession{"After gap"}));
    co_await Eventually([&] { return resumed == snapshot->event_sequence + 1; }, "snapshot cursor did not recover a journal gap");
    tail.Reset();
    Take(co_await runtime->CloseAsync());
  }
}

struct StreamingCheckpoint {
  kit::SessionId session;
  kit::MessageRef message;
  kit::Sequence cursor;
};

huxerui::Task<void> SaveStreamingCheckpoint(huxerui::File file, StreamingCheckpoint& saved) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  model->split_text = model->pause_text = true;
  auto runtime = RegisterRuntime(store, model);
  Take(co_await runtime->StartAsync());
  auto created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
      .options = {.agent = {"test.agent"}}, .initial = kit::SendInput{.text = "Interrupted stream"}}));
  auto session = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(created.outcome).session));
  co_await Eventually([&] { return session->Snapshot()->messages.back().state == kit::MessageState::Streaming; },
      "the streaming checkpoint was not committed");
  const auto snapshot = session->Snapshot();
  saved = {snapshot->summary.id, snapshot->messages.back().ref, snapshot->event_sequence};
}

huxerui::Task<void> RecoverStreamingCheckpoint(huxerui::File file, const StreamingCheckpoint& saved) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  auto runtime = RegisterRuntime(store, model);
  Take(co_await runtime->StartAsync());
  auto session = Take(co_await runtime->Sessions().OpenAsync(saved.session));
  std::optional<kit::Message> interrupted;
  auto journal = Take(co_await session->ObserveEventsAsync(saved.cursor, [&](auto update) {
    const auto batch = Take(std::move(update));
    for (const auto& event : batch->events) {
      if (const auto* message = std::get_if<kit::Message>(&event.body)) interrupted = *message;
    }
  }));
  co_await Eventually([&] { return interrupted.has_value(); }, "the recovery commit did not publish the interrupted message");
  Check(interrupted->state == kit::MessageState::Interrupted && interrupted->ref.message == saved.message.message &&
        interrupted->ref.version == saved.message.version + 1 &&
        std::get<kit::TextBlock>(interrupted->blocks.front().body).text == "Ready" && model->starts == 0,
        "recovery replaced the streamed prefix or ran the model again");
  journal.Reset();
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> StartInterruptedEffect(huxerui::File file, std::shared_ptr<EffectProbe> probe,
                                          kit::SessionCreated& created) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  auto runtime = RegisterRuntime(store, model, probe);
  Take(co_await runtime->StartAsync());
  kit::CreateSessionRequest request{
      .options = {.agent = {"test.writer"}, .retention = kit::SessionRetention::Stored},
      .initial = kit::SendInput{.text = "Write once"}};
  created = std::get<kit::SessionCreated>(Take(
      co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request))).outcome);
  for (int attempts = 0; !probe->executions && attempts < 150; ++attempts) co_await huxerui::Delay(10ms);
  Check(probe->executions == 1 && !probe->started.value.empty(), "the tool actually entered its handler");
  auto session = Take(co_await runtime->Sessions().OpenAsync(created.session));
  auto snapshot = session->Snapshot();
  Check(snapshot->tools.size() == 1 && snapshot->tools.front().state == kit::ToolCallState::Executing,
        "the effect started after its executing marker was committed");
}

huxerui::Task<void> RecoverInterruptedEffect(huxerui::File file, std::shared_ptr<EffectProbe> probe,
                                            kit::SessionCreated created) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  auto runtime = RegisterRuntime(store, model, probe);
  Take(co_await runtime->StartAsync());
  auto session = Take(co_await runtime->Sessions().OpenAsync(created.session));
  auto snapshot = session->Snapshot();
  const auto expected = probe->outcome_known ? kit::ToolCallState::Succeeded : kit::ToolCallState::ResultUnknown;
  Check(probe->started == probe->queried && probe->executions == 1 && model->starts == 0,
        "recovery checked the original invocation without redispatch");
  Check(snapshot->tools.size() == 1 && snapshot->tools.front().state == expected && snapshot->tools.front().plan,
        "recovery retained original plan and classified the outcome");
  if (!probe->outcome_known) {
    auto retried = co_await session->RetryAsync(kit::NewCommandId(), *created.initial_run);
    Check(!retried.Succeeded() && retried.Error().code == kit::ErrorCode::ResultUnknown,
          "retry must wait for the original effect to be reconciled");
    const kit::ToolExecutionResult applied{.outcome = kit::ToolOutcome::Succeeded};
    probe->reconciliation = kit::NewCommandId();
    probe->deny_reconciliation = true;
    auto denied = co_await runtime->ReconcileToolAsync(probe->reconciliation, created.session, probe->started, applied);
    Check(!denied.Succeeded() && denied.Error().code == kit::ErrorCode::PermissionDenied &&
          session->Snapshot()->tools.front().state == kit::ToolCallState::ResultUnknown, "denied evidence changed the tool outcome");
    probe->deny_reconciliation = false;
    auto forged = applied;
    forged.effects.push_back({.resource = {"unrelated"}, .state = kit::ResourceEffectState::Applied,
        .after = kit::ResourceRef{{"unrelated"}, "v2"}});
    auto rejected = co_await runtime->ReconcileToolAsync(probe->reconciliation, created.session, probe->started, forged);
    Check(!rejected.Succeeded() && rejected.Error().code == kit::ErrorCode::Conflict, "the host recorded an unrelated write");
    auto received = Take(co_await runtime->ReconcileToolAsync(probe->reconciliation, created.session, probe->started, applied));
    probe->reconciliation_commit = received.stamp.commit;
    snapshot = session->Snapshot();
    Check(snapshot->tools.front().state == kit::ToolCallState::Succeeded && snapshot->tools.front().reconciliation == probe->reconciliation &&
          snapshot->runs.front().state == kit::RunState::Failed && snapshot->runs.front().end_reason == kit::RunEndReason::ToolOutcomeUnknown &&
          probe->executions == 1 && model->starts == 0, "reconciliation changed Run history or executed the original call again");
    retried = co_await session->RetryAsync(kit::NewCommandId(), *created.initial_run);
    Check(!retried.Succeeded() && retried.Error().code == kit::ErrorCode::Conflict, "retry could repeat a confirmed external effect");
  }
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> VerifyReconciledEffect(huxerui::File file, std::shared_ptr<EffectProbe> probe, kit::SessionCreated created) {
  auto runtime = RegisterRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), std::make_shared<CountingModel>(), probe);
  Take(co_await runtime->StartAsync());
  const kit::ToolExecutionResult applied{.outcome = kit::ToolOutcome::Succeeded};
  auto replay = Take(co_await runtime->ReconcileToolAsync(probe->reconciliation, created.session, probe->started, applied));
  const auto command = Take(co_await runtime->Commands().ReadAsync(probe->reconciliation));
  Check(command.accepted.commit == probe->reconciliation_commit && replay.stamp.commit == command.accepted.commit,
        "reopening changed the original reconciliation receipt");
  const kit::ToolExecutionResult missing{.outcome = kit::ToolOutcome::Failed};
  auto conflict = co_await runtime->ReconcileToolAsync(probe->reconciliation, created.session, probe->started, missing);
  Check(!conflict.Succeeded() && conflict.Error().code == kit::ErrorCode::Conflict, "reconciliation accepted a different outcome under the same command");
  auto closed = co_await runtime->ReconcileToolAsync(kit::NewCommandId(), created.session, probe->started, missing);
  Check(!closed.Succeeded() && closed.Error().code == kit::ErrorCode::AlreadyClosed && probe->executions == 1,
        "later evidence replaced the settled tool outcome");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> Eventually(std::function<bool()> condition, const char* message) {
  for (int attempt = 0; attempt < 180 && !condition(); ++attempt) co_await huxerui::Delay(10ms);
  Check(condition(), message);
}

enum class ApprovalRestart { Pending, Approved, Revoked, StoredContinuation, VolatileContinuation };

enum class ReviewRestart { Pending, Approved, ReviewerChanged };

struct SavedReview {
  kit::SessionCreated session;
  kit::ToolCallId call;
  kit::PlanId plan;
  unsigned reviews = 0;
  std::stop_token stop;
};

class ReviewExecutionFaultStore final : public kit::SessionStore {
public:
  ReviewExecutionFaultStore(std::shared_ptr<kit::SessionStore> store, const SavedReview& state)
      : store_(std::move(store)), state_(state) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return store_->LoadAsync(); }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction transaction) override {
    if (state_.reviews && ++after_review_ >= 2) {
      failed = true;
      co_return kit::AgentResult<kit::Revision>::Failure(
          {.code = kit::ErrorCode::PersistenceFailure, .message = "Interrupted after storing the automatic decision"});
    }
    co_return co_await store_->CommitAsync(std::move(transaction));
  }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId id) override {
    return store_->FindCommitAsync(std::move(id));
  }

  bool failed = false;

private:
  std::shared_ptr<kit::SessionStore> store_;
  const SavedReview& state_;
  unsigned after_review_ = 0;
};

huxerui::Task<void> SaveToolReview(huxerui::File file, std::shared_ptr<EffectProbe> probe, SavedReview& saved,
                                 ReviewRestart scenario) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto fault = std::make_shared<ReviewExecutionFaultStore>(store, saved);
  const bool pending = scenario == ReviewRestart::Pending;
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  auto runtime = RegisterRuntime(pending ? store : fault, model, probe, false,
      kit::ToolReviewer{"test.reviewer", 2, 30s, [&, pending](kit::ToolReviewRequest request) -> kit::Async<kit::ToolReviewResult> {
        ++saved.reviews;
        saved.call = request.invocation.call;
        saved.plan = request.plan;
        saved.stop = request.stop;
        if (pending) co_await huxerui::Delay(1h);
        co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::Approve, "Approved original plan"});
      }});
  Take(co_await runtime->StartAsync());
  kit::CreateSessionRequest request{.options = {.agent = {"test.writer"}}, .initial = kit::SendInput{.text = "Review write"}};
  saved.session = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request))).outcome);
  auto chat = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
  co_await Eventually([&] { return pending ? saved.reviews != 0 : fault->failed; }, "review did not reach the restart boundary");
  const auto before = chat->Snapshot();
  const auto& call = before->tools.front();
  Check(before->interactions.empty() && call.id == saved.call && call.plan == saved.plan &&
        call.review && call.review->outcome == (pending ? kit::ToolReviewOutcome::Pending : kit::ToolReviewOutcome::Approved) &&
        probe->preparations == 1 && probe->executions == 0, "review state was not saved ahead of its effect");
  auto closed = co_await runtime->CloseAsync();
  if (pending) Take(std::move(closed));
  else Check(!closed.Succeeded() && closed.Error().code == kit::ErrorCode::PersistenceFailure,
             "tool execution marker fault did not surface");
  Check(saved.stop.stop_requested(), "shut down kept the tool review alive");
}

huxerui::Task<void> ReopenToolReview(huxerui::File file, std::shared_ptr<EffectProbe> probe, SavedReview& saved,
                                   ReviewRestart scenario) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  auto runtime = RegisterRuntime(std::move(store), model, probe, false,
      kit::ToolReviewer{"test.reviewer", scenario == ReviewRestart::ReviewerChanged ? 3U : 2U, 30s,
          [&](kit::ToolReviewRequest) -> kit::Async<kit::ToolReviewResult> {
        ++saved.reviews;
        co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::Deny, "Unexpected replay"});
      }});
  Take(co_await runtime->StartAsync());
  auto chat = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
  if (scenario != ReviewRestart::Approved) {
    co_await Eventually([&] { return chat->Snapshot()->summary.pending_interactions == 1; }, "review did not reopen as a manual action");
    const auto pending = chat->Snapshot()->interactions.front();
    const auto& approval = std::get<kit::ToolApproval>(pending.body);
    Check(approval.call == saved.call && approval.plan == saved.plan && probe->executions == 0,
          "recovery changed the reviewed plan before human approval");
    Take(co_await chat->SubmitAsync(kit::NewCommandId(), {pending.id, pending.version, kit::ApprovalDecision::Approve}));
  }
  co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "restored review did not finish");
  const auto snapshot = chat->Snapshot();
  const auto& call = snapshot->tools.front();
  const auto expected = scenario == ReviewRestart::Pending ? kit::ToolReviewOutcome::Escalated :
      scenario == ReviewRestart::Approved ? kit::ToolReviewOutcome::Approved : kit::ToolReviewOutcome::Invalidated;
  Check(snapshot->tools.size() == 1 && snapshot->runs.size() == 1 && snapshot->runs.front().id == saved.session.initial_run &&
        snapshot->runs.front().usage.tool_calls == 1 && call.state == kit::ToolCallState::Succeeded &&
        call.plan == saved.plan && call.review && call.review->outcome == expected &&
        snapshot->interactions.size() == (scenario == ReviewRestart::Approved ? 0 : 1) &&
        saved.reviews == 1 && probe->preparations == 1 && probe->executions == 1 && probe->started == saved.call && model->starts == 1,
        "restored review repeated preparation, decision, effect or model continuation");
  Take(co_await runtime->CloseAsync());
}

class ReleaseSkillSource final : public kit::SkillSource {
public:
  explicit ReleaseSkillSource(kit::AgentId agent = {"test.release-skill"}) : agent_(std::move(agent)) {}

  static kit::SkillRef Ref(kit::Revision version) { return {{"test.packaged"}, {"release-brief"}, version}; }

  kit::Async<std::vector<kit::SkillSummary>> ListAsync(kit::SkillSourceRequest) override {
    co_return kit::AgentResult<std::vector<kit::SkillSummary>>::Success({
        {Ref(current), "Release brief", "Prepare release evidence using a stable template"},
        {{{"test.packaged"}, {"hidden"}, current}, "Hidden", "Unavailable to this model and user"}});
  }

  kit::Async<kit::SkillPackage> LoadAsync(kit::SkillSourceRequest request, kit::SkillRef skill) override {
    Check(request.actor.run.has_value() && request.agent.id == agent_, "Skill source lost its scope");
    loads.push_back(skill);
    if (skill == Ref(1) && !old_available)
      co_return kit::AgentResult<kit::SkillPackage>::Failure({kit::ErrorCode::NotFound, "Old package was removed"});
    co_return kit::AgentResult<kit::SkillPackage>::Success({skill,
        "Compose a release brief with references/release.txt from v" + std::to_string(skill.version),
        {{"references/release.txt", "text/plain", std::nullopt}}});
  }

  kit::Async<huxerui::AsyncInputStream> OpenResourceAsync(kit::SkillSourceRequest, kit::SkillRef skill, std::string path) override {
    using namespace huxerui;
    Check(path == "references/release.txt", "Undeclared Skill path reached the source");
    opens.push_back(skill);
    co_return kit::AgentResult<AsyncInputStream>::Success(AsyncInputStream::FromReader(
        [data = std::string("release-template-v") + std::to_string(skill.version), index = std::size_t{0}](
            std::stop_token, std::size_t count) mutable -> Task<IoResult<Bytes>> {
          const auto size = std::min(count, data.size() - index);
          const auto* begin = reinterpret_cast<const std::byte*>(data.data() + index);
          index += size;
          co_return IoResult<Bytes>::Success(Bytes(begin, begin + size));
        }));
  }

  kit::Revision current = 1;
  bool old_available = true;
  std::vector<kit::SkillRef> loads;
  std::vector<kit::SkillRef> opens;

private:
  kit::AgentId agent_;
};

class ReleaseSkillModel final : public kit::ModelProvider {
public:
  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true, .multi_tool_calls = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    using namespace kit;
    ++starts;
    Check(request.available_skills.size() == 1 && request.available_skills.front().ref == ReleaseSkillSource::Ref(catalog_version),
          "Skill discovery exposed a hidden entry or the wrong catalog version");
    Check(!request.allow_server_state, "Skill run used provider-side state");
    std::vector<ModelEvent> events;
    const auto call = [&](const char* id, const char* tool, const char* arguments) {
      Check(std::ranges::find(request.tools, std::string_view(tool), &ModelToolSpec::name) != request.tools.end(), "Skill tool was not offered");
      events.emplace_back(ModelToolStarted{id, tool});
      events.emplace_back(ModelToolArguments{id, arguments});
      events.emplace_back(ModelToolFinished{id});
    };
    if (request.skill_instructions.empty()) {
      Check(catalog_version == 1 && starts == 1, "The model lost the active Skill");
      call("activate", "agentkit_activate_skill", R"({"source":"test.packaged","id":"release-brief","version":"1"})");
      events.emplace_back(ModelFinished{ModelFinishReason::ToolCalls, {}});
    } else {
      const auto& instructions = request.skill_instructions;
      Check(instructions.size() == 1 && instructions.front().skill == ReleaseSkillSource::Ref(1) &&
            instructions.front().markdown.ends_with("from v1"), "Skill instructions drifted to the latest version");
      bool read = false;
      for (const auto& item : request.items) {
        const auto* result = std::get_if<ModelToolResult>(&item);
        if (!result) continue;
        if (result->provider_call_id == "activation-outside-catalog")
          Check(result->error && result->error->code == ErrorCode::PermissionDenied, "Model activated an unoffered version");
        if (result->provider_call_id == "unsafe-path")
          Check(result->error && result->error->code == ErrorCode::InvalidArgument, "Model read a traversal path");
        if (result->provider_call_id == "template") {
          Check(result->value && std::get<std::string>(result->value->value) == "release-template-v1",
                "Model got the wrong release template");
          read = true;
        }
      }
      if (read) {
        events.emplace_back(ModelTextDelta{"Release brief ready"});
        events.emplace_back(ModelFinished{ModelFinishReason::Completed, {}});
      } else {
        call("activation-outside-catalog", "agentkit_activate_skill", R"({"source":"test.packaged","id":"hidden","version":"2"})");
        call("unsafe-path", "agentkit_read_skill",
             R"({"source":"test.packaged","id":"release-brief","version":"1","path":"references/../release.txt"})");
        call("template", "agentkit_read_skill",
             R"({"source":"test.packaged","id":"release-brief","version":"1","path":"references/release.txt"})");
        events.emplace_back(ModelFinished{ModelFinishReason::ToolCalls, {}});
      }
    }
    co_return AgentResult<std::unique_ptr<ModelTurnStream>>::Success(std::make_unique<OneTurn>(std::move(events)));
  }

  int starts = 0;
  kit::Revision catalog_version = 1;
};

struct SavedSkill {
  kit::SessionCreated session;
  kit::InteractionSnapshot approval;
};

huxerui::Task<void> VerifySkillApproval(huxerui::File file, SavedSkill& saved, bool reopen,
                                      kit::SkillActivationTrigger trigger, bool old_available = true,
                                      bool revoke_resource = false) {
  using namespace kit;
  auto source = std::make_shared<ReleaseSkillSource>();
  source->current = reopen ? 2 : 1;
  source->old_available = old_available;
  auto model = std::make_shared<ReleaseSkillModel>();
  model->catalog_version = source->current;
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto runtime = Take(AgentRuntime::Create({.tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return AgentResult<PrincipalId>::Success({"skill-user"}); },
      .access = [source, trigger, revoke_resource](PolicyRequest request) -> Async<PolicyDecision> {
        auto decision = AccessDecision::Allow;
        if (request.skill && request.skill->id.value == "hidden") decision = AccessDecision::Deny;
        if (revoke_resource && request.capability == Capability::SendToModel && request.skill_resource && !source->opens.empty())
          decision = AccessDecision::Deny;
        if (request.capability == Capability::ActivateSkill && decision != AccessDecision::Deny) {
          Check(request.actor.kind == (trigger == SkillActivationTrigger::User ? ActionParty::Kind::User : ActionParty::Kind::Agent),
                "Activation policy saw the wrong initiator");
          decision = AccessDecision::RequireConfirmation;
        }
        co_return AgentResult<PolicyDecision>::Success({decision, {}});
      }, .store = store}));
  Take(runtime->RegisterModel({{{"test.skill-model"}, 1}, {"test", "skills", "script", {}}, model, true}));
  Take(runtime->RegisterSkillSource({"test.packaged"}, source));
  Take(runtime->RegisterAgent({{.ref = {{"test.release-skill"}, 1}, .display_name = "Skill test",
      .model = {"test.skill-model"}, .skill_sources = {{"test.packaged"}}}}));
  Take(co_await runtime->StartAsync());
  auto listed = Take(co_await runtime->ListSkillsAsync({AgentId{"test.release-skill"}}));
  Check(listed.size() == 1 && listed.front().ref == ReleaseSkillSource::Ref(source->current) && source->loads.empty(),
        "Discovering a Skill loaded its body or exposed a hidden entry");

  if (!reopen) {
    SendInput initial{.text = "Prepare a release brief"};
    if (trigger == SkillActivationTrigger::User) initial.skills = {ReleaseSkillSource::Ref(1)};
    CreateSessionRequest request{.options = {.agent = {"test.release-skill"}}, .initial = std::move(initial)};
    saved.session = std::get<SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(NewCommandId(), std::move(request))).outcome);
  }
  auto chat = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
  co_await Eventually([&] { return chat->Snapshot()->summary.run_state == RunState::WaitingForInput; },
                      "Skill activation did not wait for approval");
  const auto pending = chat->Snapshot()->interactions.front();
  Check(chat->Snapshot()->interactions.size() == 1 && source->loads.empty() &&
        std::get<OperationApproval>(pending.body).preview.title == "Activate Skill",
        "Skill body was loaded before confirmation");
  if (!reopen) {
    saved.approval = pending;
    Check(model->starts == (trigger == SkillActivationTrigger::User ? 0 : 1), "Skill changed the pre-approval model scope");
  } else {
    Check(pending.id == saved.approval.id && pending.version == saved.approval.version && pending.run == saved.session.initial_run,
          "Reopened Skill approval changed its original binding");
    Take(co_await chat->SubmitAsync(NewCommandId(), {pending.id, pending.version, ApprovalDecision::Approve}));
    co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "The restored Skill run did not finish");
    const auto view = chat->Snapshot();
    const auto& run = view->runs.front();
    const auto command = std::get<OperationApproval>(pending.body).pending_command;
    auto resolved = Take(co_await runtime->Commands().ReadAsync(command));
    Check(resolved.state == (old_available ? CommandState::Applied : CommandState::Failed) && resolved.resolution &&
        Take(co_await store->FindCommitAsync(resolved.resolution->commit)).has_value(),
        "Skill approval reported an uncommitted resolution");
    Check(run.id == saved.session.initial_run && view->interactions.size() == 1, "Skill execution replaced the run or duplicated its approval");
    if (revoke_resource) {
      Check(run.state == RunState::Failed && run.failure && run.failure->code == ErrorCode::PermissionDenied &&
            source->opens == std::vector<SkillRef>{ReleaseSkillSource::Ref(1)} && model->starts == 1,
            "Revoked Skill resource reached a later model turn");
    } else if (old_available) {
      Check(run.state == RunState::Succeeded && run.active_skills == std::vector<SkillActivation>{{ReleaseSkillSource::Ref(1), trigger}} &&
            source->opens == std::vector<SkillRef>{ReleaseSkillSource::Ref(1)} && model->starts == 2 &&
            run.usage.tool_calls == (trigger == SkillActivationTrigger::Model ? 4 : 3),
            "Skill or resource did not maintain its version, identity and budget through restart");
    } else {
      Check(run.state == RunState::Failed && run.failure && run.failure->code == ErrorCode::Conflict &&
            run.active_skills.empty() && source->opens.empty() && model->starts == 0,
            "A missing selected Skill version fell back to the new version");
    }
  }
  Take(co_await runtime->CloseAsync());
}

struct SavedApproval {
  kit::SessionCreated session;
  kit::InteractionSnapshot interaction;
  kit::CommandId command;
};

huxerui::Task<void> UseLegacyToolPlanFields(huxerui::File file, kit::SessionId session) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto image = Take(co_await store->LoadAsync());
  const auto record = std::ranges::find(image.records, session.value, &kit::StoreValue::key);
  Check(record != image.records.end(), "approval checkpoint was not stored");
  const auto* begin = reinterpret_cast<const std::uint8_t*>(record->data.data());
  auto document = nlohmann::json::from_cbor(begin, begin + record->data.size());
  Check(document.at("schema") == 2, "parallel tool checkpoints require the new session schema");
  document["schema"] = 1;
  auto& run = document.at("session").at("runs").at(0);
  auto plan = run.at("tool_plans").at(0).at(1);
  run["prepared_tool"] = std::move(plan.at("prepared"));
  run["tool_approval"] = std::move(plan.at("approval"));
  run.erase("tool_plans");
  auto& call = run.at("pending_calls").at(0);
  call.erase("local_call");
  call.erase("result");
  call.erase("terminal_error");
  call["budget_reserved"] = false;
  const auto cbor = nlohmann::json::to_cbor(document);
  const auto* bytes = reinterpret_cast<const std::byte*>(cbor.data());
  kit::StoreTransaction transaction{{kit::NewCommandId().value}, image.revision,
      {{record->collection, record->key, record->revision, huxerui::Bytes(bytes, bytes + cbor.size())}}};
  Take(co_await store->CommitAsync(std::move(transaction)));
}

huxerui::Task<void> SaveApproval(huxerui::File file, std::shared_ptr<EffectProbe> probe, SavedApproval& saved,
                                ApprovalRestart scenario) {
  auto storage = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  model->with_private_state = scenario == ApprovalRestart::StoredContinuation || scenario == ApprovalRestart::VolatileContinuation;
  auto runtime = RegisterRuntime(std::move(storage), model, probe, scenario == ApprovalRestart::StoredContinuation);
  Take(co_await runtime->StartAsync());
  kit::CreateSessionRequest request{.options = {.agent = {"test.writer"}}, .initial = kit::SendInput{.text = "Approval"}};
  saved.session = std::get<kit::SessionCreated>(Take(
      co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request))).outcome);
  auto session = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
  co_await Eventually([&] { return session->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
                      "original run did not wait for approval");
  saved.interaction = session->Snapshot()->interactions.back();
  saved.command = kit::NewCommandId();
  if (scenario == ApprovalRestart::Approved) {
    probe->hold_before_effect = true;
    Take(co_await session->SubmitAsync(saved.command,
        {saved.interaction.id, saved.interaction.version, kit::ApprovalDecision::Approve}));
    co_await Eventually([&] { return session->Snapshot()->tools.front().state == kit::ToolCallState::Ready; },
                        "approval did not commit before write was held");
  }
  Check(probe->preparations == 1 && probe->executions == 0, "write must not run in the first application");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ResumeApproval(huxerui::File file, std::shared_ptr<EffectProbe> probe, SavedApproval saved,
                                  ApprovalRestart scenario) {
  probe->hold_before_effect = false;
  probe->hold_effect = false;
  probe->deny = scenario == ApprovalRestart::Revoked;
  auto storage = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  auto runtime = RegisterRuntime(std::move(storage), model, probe, scenario == ApprovalRestart::StoredContinuation);
  Take(co_await runtime->StartAsync());
  auto session = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
  auto restarted = session->Snapshot();
  const auto& before = std::get<kit::ToolApproval>(saved.interaction.body);
  if (scenario == ApprovalRestart::VolatileContinuation) {
    Check(restarted->summary.run_state == kit::RunState::Failed && restarted->runs.front().end_reason == kit::RunEndReason::Interrupted &&
        restarted->tools.front().id == before.call && restarted->tools.front().state == kit::ToolCallState::Cancelled &&
        restarted->interactions.front().id == saved.interaction.id &&
        restarted->interactions.front().state == kit::InteractionState::Cancelled && model->starts == 0 && probe->executions == 0,
        "an unavailable private continuation closed the original approval without executing");
    kit::InteractionReply response{saved.interaction.id, saved.interaction.version, kit::ApprovalDecision::Approve};
    const auto rejected = co_await session->SubmitAsync(saved.command, std::move(response));
    Check(!rejected.Succeeded() && probe->executions == 0, "an old approval cannot execute after loss of private state");
    Take(co_await runtime->CloseAsync());
    co_return;
  }
  Check(restarted->summary.active_run == saved.session.initial_run || scenario == ApprovalRestart::Revoked,
        "restore kept the original RunId");
  Check(restarted->tools.front().id == before.call && restarted->tools.front().plan == before.plan &&
        restarted->interactions.front().id == saved.interaction.id && restarted->interactions.size() == 1,
        "restart kept call, plan and approval identity");
  kit::InteractionReply response{saved.interaction.id, saved.interaction.version, kit::ApprovalDecision::Approve};
  if (scenario == ApprovalRestart::Pending || scenario == ApprovalRestart::StoredContinuation) {
    Check(restarted->interactions.front().version == saved.interaction.version && model->starts == 0,
          "restore displayed the original approval before continuing the model");
    auto other_view = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
    auto approved = Take(co_await session->SubmitAsync(saved.command, response));
    auto retry = Take(co_await other_view->SubmitAsync(saved.command, response));
    Check(approved.stamp.commit == retry.stamp.commit, "second view returned the original receipt");
  } else if (scenario == ApprovalRestart::Approved) {
    auto prior = Take(co_await runtime->Commands().ReadAsync(saved.command));
    auto retry = Take(co_await session->SubmitAsync(saved.command, response));
    Check(prior.accepted.commit == retry.stamp.commit, "persisted approval result replayed its receipt");
  }
  co_await Eventually([&] { return !session->Snapshot()->summary.active_run; }, "restored tool turn did not finish");
  const auto finished = session->Snapshot();
  const auto& tool = finished->tools.front();
  Check(finished->runs.size() == 1 && finished->runs.front().id == saved.session.initial_run &&
        finished->runs.front().usage.tool_calls == 1 && model->starts == 1 && probe->preparations == 1,
        "continuation returned once to the original model after its original tool plan");
  if (scenario == ApprovalRestart::StoredContinuation) {
    const auto& continuation = model->received_state;
    Check(continuation && continuation->prefix_items == 2 && continuation->state.provider == finished->runs.front().model &&
        continuation->state.format_version == 1 && continuation->state.opaque == private_model_state,
        "authorized private continuation restored original bytes, model revision and input boundary");
  }
  if (scenario == ApprovalRestart::Revoked) {
    Check(tool.state == kit::ToolCallState::Denied && probe->executions == 0 &&
          finished->interactions.front().state == kit::InteractionState::Invalidated,
          "revoked permission invalidated the pending approval");
  } else {
    Check(tool.state == kit::ToolCallState::Succeeded && probe->executions == 1 && probe->started == before.call,
          "restored approval executed exactly one call with original ID");
  }
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> VerifyCatalogApproval(huxerui::File file, SavedApproval& saved,
                                         std::shared_ptr<EffectProbe> probe, bool reopen, kit::Revision version) {
  auto storage = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  std::optional<kit::ToolDefinition> definition;
  auto runtime = RegisterRuntime(std::move(storage), model, probe, false, {}, {}, {}, &definition);
  definition->version = version;
  kit::ToolCatalogSnapshot catalog{1, {}};
  catalog.tools.push_back(std::move(*definition));
  Take(co_await runtime->ReplaceToolCatalogAsync(test_catalog, std::move(catalog)));
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    kit::CreateSessionRequest request{.options = {.agent = {"test.writer"}}, .initial = kit::SendInput{.text = "Use catalog tool"}};
    saved.session = std::get<kit::SessionCreated>(Take(
        co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request))).outcome);
    auto session = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
    co_await Eventually([&] { return session->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
                        "catalog approval was not saved");
    saved.interaction = session->Snapshot()->interactions.back();
    saved.command = kit::NewCommandId();
    Check(probe->preparations == 1 && probe->executions == 0, "catalog tool executed before approval");
  } else {
    auto session = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
    const auto old = std::get<kit::ToolApproval>(saved.interaction.body);
    const auto reply = co_await session->SubmitAsync(saved.command,
        {saved.interaction.id, saved.interaction.version, kit::ApprovalDecision::Approve});
    if (version == 1) {
      Check(reply.Succeeded(), "the unchanged catalog binding lost its restorable approval");
    } else {
      Check(!reply.Succeeded() && (reply.Error().code == kit::ErrorCode::Conflict ||
              reply.Error().code == kit::ErrorCode::AlreadyClosed), "a replaced catalog tool was approved after restart");
    }
    co_await Eventually([&] { return !session->Snapshot()->summary.active_run; }, "catalog turn did not settle after restart");
    const auto finished = session->Snapshot();
    Check(finished->runs.size() == 1 && finished->runs.front().id == saved.session.initial_run &&
          finished->tools.size() == 1 && finished->tools.front().id == old.call && finished->tools.front().plan == old.plan &&
          finished->tools.front().tool == kit::ToolRef{{"test.catalog/effect"}, 1, test_catalog} &&
          finished->interactions.front().id == saved.interaction.id && probe->preparations == 1 &&
          model->received_result && model->received_result->call == old.call,
          "catalog restoration changed the original call, plan, approval or model continuation");
    if (version == 1) {
      Check(probe->executions == 1 && probe->started == old.call && finished->tools.front().state == kit::ToolCallState::Succeeded,
            "the unchanged catalog tool failed to execute exactly once");
    } else {
      Check(probe->executions == 0 && finished->tools.front().state == kit::ToolCallState::Conflict &&
            finished->interactions.front().state == kit::InteractionState::Invalidated && model->received_result->error &&
            model->received_result->error->code == kit::ErrorCode::Conflict,
            "the old catalog binding did not settle as a conflict at the original call");
    }
  }
  Take(co_await runtime->CloseAsync());
}

struct SavedToolTurn {
  kit::SessionCreated session;
  std::vector<kit::ToolCallId> calls;
  std::vector<kit::InteractionSnapshot> approvals;
  std::vector<kit::ToolId> issued;
};

class StoredToolTurnModel final : public kit::ModelProvider {
public:
  kit::ModelCapabilities Capabilities() const override {
    return {.text_stream = true, .tool_calls = true, .multi_tool_calls = true};
  }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    for (const auto& entry : request.items) {
      if (const auto* result = std::get_if<kit::ModelToolResult>(&entry)) results.push_back(*result);
    }
    std::vector<kit::ModelEvent> events;
    if (results.empty()) {
      for (const auto* name : {"stored.read", "stored.first", "stored.second"}) {
        events.emplace_back(kit::ModelToolStarted{name, name});
        events.emplace_back(kit::ModelToolArguments{name, "{}"});
        events.emplace_back(kit::ModelToolFinished{name});
      }
      events.emplace_back(kit::ModelFinished{kit::ModelFinishReason::ToolCalls, {}});
    } else {
      events = {kit::ModelTextDelta{"Restored all calls"}, kit::ModelFinished{kit::ModelFinishReason::Completed, {}}};
    }
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<OneTurn>(std::move(events)));
  }

  std::vector<kit::ModelToolResult> results;
};

huxerui::Task<void> VerifyStoredToolTurn(huxerui::File file, SavedToolTurn& saved, bool reopen) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto model = std::make_shared<StoredToolTurnModel>();
  auto runtime = Take(kit::AgentRuntime::Create({.tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const bool confirms = request.capability == kit::Capability::ExecuteTool && request.tool &&
            request.tool->id.value != "stored.read";
        co_return kit::AgentResult<kit::PolicyDecision>::Success(
            {confirms ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow, {}});
      }, .max_parallel_tools_per_turn = 2, .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"stored.tools.model"}, 1}, {"test", "test", "script", {}}, model}));
  std::vector<kit::ToolId> ids{{"stored.read"}, {"stored.first"}, {"stored.second"}};
  for (const auto& id : ids) {
    Take(runtime->RegisterTool({.id = id, .version = 1, .description = id.value, .input_schema = {kit::JsonValue{true}},
        .effect = kit::ToolEffect::Read, .concurrency = kit::ToolConcurrency::ReadOnly,
        .prepare = [](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
          co_return kit::AgentResult<kit::PreparedChange>::Success({.preview = {.title = request.tool.value},
              .conflict_keys = std::vector<kit::ToolResourceClaim>{}});
        },
        .execute = [&saved](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
          saved.issued.push_back(call.tool.id);
          co_return kit::AgentResult<kit::ToolExecutionResult>::Success(
              {.outcome = kit::ToolOutcome::Succeeded, .model_value = kit::JsonValue{call.tool.id.value}});
        }}));
  }
  Take(runtime->RegisterAgent({{.ref = {{"stored.tools.agent"}, 1}, .display_name = "Stored tools",
      .model = {"stored.tools.model"}, .tools = ids}}));
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    kit::CreateSessionRequest request{.options = {.agent = {"stored.tools.agent"}},
                                     .initial = kit::SendInput{.text = "Save a parallel tool turn"}};
    saved.session = std::get<kit::SessionCreated>(Take(
        co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request))).outcome);
  }
  auto chat = Take(co_await runtime->Sessions().OpenAsync(saved.session.session));
  co_await Eventually([&] {
    const auto snapshot = chat->Snapshot();
    return snapshot->summary.pending_interactions == 2 && snapshot->tools.size() == 3 &&
        snapshot->tools.front().state == kit::ToolCallState::Succeeded;
  }, "one completed call and two pending approvals were not recovered together");
  const auto snapshot = chat->Snapshot();
  if (!reopen) {
    for (const auto& tool : snapshot->tools) saved.calls.push_back(tool.id);
    saved.approvals = snapshot->interactions;
    Check(saved.issued == std::vector{ids.front()} && saved.approvals.size() == 2,
          "calls escaped their approval before shutdown");
  } else {
    for (std::size_t i = 0; i < saved.approvals.size(); ++i) {
      Check(snapshot->interactions[i].id == saved.approvals[i].id &&
            snapshot->interactions[i].version == saved.approvals[i].version, "restart created a new approval");
    }
    auto approve = [&](const auto& approval) {
      return chat->SubmitAsync(kit::NewCommandId(), {approval.id, approval.version, kit::ApprovalDecision::Approve});
    };
    const auto first = std::ranges::find_if(saved.approvals, [](const auto& approval) {
      return std::get<kit::ToolApproval>(approval.body).preview.title == "stored.first";
    });
    Check(first != saved.approvals.end(), "the first tool's approval could not be identified");
    const auto second = first == saved.approvals.begin() ? std::next(first) : saved.approvals.begin();
    Take(co_await approve(*second));
    Check(saved.issued == std::vector{ids.front()}, "later restored approval bypassed an earlier call");
    Take(co_await approve(*first));
    co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "restored tool turn did not continue");
    const auto complete = chat->Snapshot();
    Check(complete->runs.back().state == kit::RunState::Succeeded && complete->runs.back().id == saved.session.initial_run &&
          complete->runs.back().usage.tool_calls == 3 && model->results.size() == saved.calls.size() && saved.issued == ids,
          "restored tool turn replayed an effect, reserved budget twice or lost its run");
    for (std::size_t i = 0; i < saved.calls.size(); ++i) {
      Check(model->results[i].call == saved.calls[i] && model->results[i].provider_call_id == ids[i].value &&
            model->results[i].value == kit::JsonValue{ids[i].value}, "restored results did not preserve original call order");
    }
  }
  Take(co_await runtime->CloseAsync());
}

const kit::ArtifactRef rework_report{{"rework.report"}, 1};
const kit::ArtifactRef rework_deck{{"rework.deck"}, 1};
const kit::PartId overview_part{"overview"};
const kit::PartId results_part{"results"};

const kit::ToolId model_publish_tool{"model.publish"};
const kit::ToolId denied_model_tool{"model.denied"};
const kit::ArtifactId model_output{"model.output"};

class PublicationModel final : public kit::ModelProvider {
public:
  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true, .tool_calls = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    if (request.tools.empty()) {
      co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<OneTurn>());
    }
    Check(request.tools.size() == 1 && request.tools.front().tool.id == model_publish_tool,
          "hosted completion offered a tool outside its explicit authorized scope");
    const auto calls = std::ranges::count_if(request.items, [](const auto& item) {
      return std::holds_alternative<kit::ModelToolResult>(item);
    });
    if (calls == 2) {
      co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<OneTurn>());
    }
    const auto phase = std::to_string(calls + 1);
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<OneTurn>(
        std::vector<kit::ModelEvent>{kit::ModelToolStarted{phase, model_publish_tool.value},
            kit::ModelToolArguments{phase, phase}, kit::ModelToolFinished{phase},
            kit::ModelFinished{kit::ModelFinishReason::ToolCalls, {}}}));
  }
};

struct ModelPublicationFixture {
  kit::SessionId session;
  kit::InteractionId approval;
  kit::ToolCallId approved_call;
  std::vector<int> executions;
};

kit::Async<kit::RunCompletion> CompletePublicationModel(kit::RunContext run, bool restoring) {
  if (restoring) {
    kit::ModelInput narrowed{.instruction = "Publish with tool", .allowed_tools = {model_publish_tool}};
    const auto conflict = co_await run.CompleteAsync(std::move(narrowed));
    Check(!conflict.Succeeded() && conflict.Error().code == kit::ErrorCode::Conflict,
          "restored completion accepted a different tool scope");
  }
  kit::ModelInput input{.instruction = "Publish with tool", .allowed_tools = {model_publish_tool, denied_model_tool}};
  if (restoring) std::ranges::reverse(input.allowed_tools);
  auto reply = Take(co_await run.CompleteAsync(std::move(input)));
  const std::vector<kit::ArtifactRef> expected{{model_output, 2}, {model_output, 3}};
  Check(reply.artifacts == expected, "model reply included an unowned output, a preview, a denied version or the wrong order");
  kit::ModelInput no_tools{.instruction = "Next completion without tools"};
  auto next = Take(co_await run.CompleteAsync(std::move(no_tools)));
  Check(next.artifacts.empty(), "next completion inherited artifacts from the previous model loop");
  co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = std::move(reply.text), .artifacts = std::move(reply.artifacts)});
}

std::shared_ptr<kit::AgentRuntime> RegisterPublicationRuntime(std::shared_ptr<kit::SessionStore> store,
                                                             ModelPublicationFixture& fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        auto decision = kit::AccessDecision::Allow;
        if (request.capability == kit::Capability::DiscoverTool && request.tool && request.tool->id == denied_model_tool) {
          decision = kit::AccessDecision::Deny;
        }
        if (request.capability == kit::Capability::ExecuteTool && request.invocation &&
            std::get<std::int64_t>(request.invocation->arguments.value) == 2) {
          decision = kit::AccessDecision::RequireConfirmation;
        }
        if (request.capability == kit::Capability::ReadArtifact && !request.sources.empty()) {
          const auto* ref = std::get_if<kit::ArtifactRef>(&request.sources.front().source);
          if (ref && ((ref->id.value == "model.user-private" && request.actor.kind == kit::ActionParty::Kind::User) ||
                      (ref->id.value == "model.agent-private" && request.actor.kind == kit::ActionParty::Kind::Agent))) {
            decision = kit::AccessDecision::Deny;
          }
        }
        co_return kit::AgentResult<kit::PolicyDecision>::Success({decision});
      },
      .artifact_store = {.preview = [](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact});
      }},
      .store = std::move(store)}));
  Take(runtime->RegisterModel({.ref = {{"model.publication"}, 1}, .endpoint = {"test", "test", "script", {}},
      .provider = std::make_shared<PublicationModel>()}));
  kit::ToolDefinition publisher{.id = model_publish_tool, .version = 1, .description = "Publish fixture versions",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        kit::PreparedChange change{.preview = {.title = "Publish fixture"}};
        if (std::get<std::int64_t>(request.arguments.value) == 1)
          change.preview_artifacts.push_back({.id = kit::ArtifactId{"model.preview"}, .type = "test",
              .name = "Preview only", .storage_handle = "preview"});
        co_return kit::AgentResult<kit::PreparedChange>::Success(std::move(change));
      },
      .execute = [&fixture](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto phase = static_cast<int>(std::get<std::int64_t>(call.request.arguments.value));
        fixture.executions.push_back(phase);
        kit::ToolExecutionResult result{.outcome = kit::ToolOutcome::Succeeded, .model_value = kit::JsonValue{true},
            .existing_artifacts = std::move(call.change.preview.artifacts)};
        if (phase) result.existing_artifacts.push_back({model_output, 1});
        result.new_artifacts.push_back({.id = model_output,
            .expected_latest = phase ? std::optional<kit::Revision>{static_cast<kit::Revision>(phase)} : std::nullopt,
            .type = "test", .name = "Published version", .storage_handle = std::to_string(phase)});
        if (phase == 1) {
          for (const auto* id : {"model.user-private", "model.agent-private"})
            result.new_artifacts.push_back({.id = kit::ArtifactId{id}, .type = "test", .name = "Hidden version", .storage_handle = id});
        }
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success(std::move(result));
      }};
  Take(runtime->RegisterTool(publisher));
  publisher.id = denied_model_tool;
  Take(runtime->RegisterTool(publisher));
  publisher.id = {"model.unlisted"};
  Take(runtime->RegisterTool(std::move(publisher)));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"model.agent"}, 1}, .display_name = "Publication agent",
      .model = {"model.publication"}, .tools = {model_publish_tool, denied_model_tool, {"model.unlisted"}}},
      .run = [](kit::RunContext run, kit::RunInput) -> kit::Async<kit::RunCompletion> {
        kit::ToolRequest request{model_publish_tool, kit::JsonValue{std::int64_t{0}}};
        const auto prior = Take(co_await run.CallToolAsync(std::move(request)));
        Check(prior.published_artifacts == std::vector<kit::ArtifactRef>{{model_output, 1}}, "seed publication failed");
        Take(co_await run.SaveCheckpointAsync({.stage = "publish", .schema_version = 1}));
        co_return co_await CompletePublicationModel(std::move(run), false);
      },
      .restore = [](kit::RunContext run, kit::RunInput, kit::RunResume resume) -> kit::Async<kit::RunCompletion> {
        Check(resume.checkpoint && resume.checkpoint->stage == "publish", "publication checkpoint did not resume");
        co_return co_await CompletePublicationModel(std::move(run), true);
      }}));
  return runtime;
}

huxerui::Task<void> VerifyModelPublications(huxerui::File file, ModelPublicationFixture& fixture, bool reopen) {
  auto runtime = RegisterPublicationRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    auto receipt = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
        .options = {.agent = {"model.agent"}}, .initial = kit::SendInput{.text = "Publish"} }));
    fixture.session = std::get<kit::SessionCreated>(receipt.outcome).session;
  }
  auto chat = Take(co_await runtime->Sessions().OpenAsync(fixture.session));
  co_await Eventually([&] { return chat->Snapshot()->summary.pending_interactions == 1; }, "model output approval was not pending");
  const auto approval = chat->Snapshot()->interactions.back();
  const auto call = std::get<kit::ToolApproval>(approval.body).call;
  Check(fixture.executions == std::vector{0, 1}, "publishing ran twice or ahead of approval");
  if (!reopen) {
    fixture.approval = approval.id;
    fixture.approved_call = call;
  } else {
    Check(approval.id == fixture.approval && call == fixture.approved_call, "recovery replaced the original tool approval");
    Take(co_await chat->SubmitAsync(kit::NewCommandId(), {approval.id, approval.version, kit::ApprovalDecision::Approve}));
    co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "model did not finish after restored approval");
    const auto completed = chat->Snapshot();
    Check(completed->summary.run_state == kit::RunState::Succeeded && fixture.executions == std::vector{0, 1, 2},
          "model publication did not finish exactly once after restart");
    const auto& run = completed->runs.back();
    Check(run.result && run.result->artifacts == std::vector<kit::ArtifactRef>{{model_output, 2}, {model_output, 3}},
          "completed run lost the versions returned by the resumed model loop");
    for (const auto& ref : run.result->artifacts) {
      const auto version = Take(co_await runtime->Artifacts().ReadAsync(ref));
      Check(version.metadata.ref == ref && version.session == fixture.session && version.run == run.id &&
            version.call == completed->tools.at(ref.version - 1).id, "reply returned a version with different publication provenance");
    }
  }
  Take(co_await runtime->CloseAsync());
}

const kit::ArtifactRef readable_v1{{"permission.output"}, 1};
const kit::ArtifactRef readable_v2{{"permission.output"}, 2};
const kit::ArtifactRef never_readable{{"permission.hidden"}, 1};

struct ResultAccessFixture {
  kit::SessionId session;
  kit::Sequence before_publication;
  kit::CommandId command = kit::NewCommandId();
  kit::ToolCallId first_call;
  kit::ToolCallId second_call;
  bool revoked = false;
  bool checkpointed = false;
  unsigned executions = 0;
};

bool HasArtifact(const std::vector<kit::ContentBlock>& blocks, const kit::ArtifactRef& ref) {
  return std::ranges::any_of(blocks, [&](const auto& block) {
    const auto* artifact = std::get_if<kit::ArtifactBlock>(&block.body);
    return artifact && artifact->artifact == ref;
  });
}

std::shared_ptr<kit::AgentRuntime> ResultAccessRuntime(std::shared_ptr<kit::SessionStore> store, ResultAccessFixture& fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [&fixture](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const auto denied = request.capability == kit::Capability::ReadArtifact &&
            std::ranges::any_of(request.sources, [&](const auto& source) {
              const auto* ref = std::get_if<kit::ArtifactRef>(&source.source);
              return ref && (*ref == never_readable || (fixture.revoked && *ref == readable_v1));
            });
        co_return kit::AgentResult<kit::PolicyDecision>::Success({
            denied ? kit::AccessDecision::Deny : kit::AccessDecision::Allow});
      },
      .artifact_store = {.preview = [](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact});
      }},
      .store = std::move(store)}));
  Take(runtime->RegisterModel({.ref = {{"permission.model"}, 1}, .endpoint = {"test", "test", "script", {}},
      .provider = std::make_shared<CountingModel>()}));
  Take(runtime->RegisterTool({.id = {"permission.publish"}, .version = 1, .description = "Publish versioned test output",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({});
      },
      .execute = [&fixture](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        ++fixture.executions;
        const bool update = std::get<bool>(call.request.arguments.value);
        kit::ToolExecutionResult result{.outcome = kit::ToolOutcome::Succeeded};
        result.new_artifacts.push_back({.id = readable_v1.id, .expected_latest = update ? std::optional<kit::Revision>{1} : std::nullopt,
            .type = "test", .name = "Report", .storage_handle = update ? "v2" : "v1"});
        if (update) {
          result.existing_artifacts.push_back(readable_v1);
          result.new_artifacts.push_back({.id = never_readable.id, .type = "test", .name = "Hidden", .storage_handle = "hidden"});
          result.model_value = kit::JsonValue{never_readable.id.value};
        }
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success(std::move(result));
      }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"permission.agent"}, 1}, .display_name = "Permissions",
      .model = {"permission.model"}, .tools = {{"permission.publish"}}},
      .run = [&fixture](kit::RunContext run, kit::RunInput) -> kit::Async<kit::RunCompletion> {
        kit::ToolRequest first{{"permission.publish"}, kit::JsonValue{false}};
        const auto accepted = Take(co_await run.StartToolAsync(fixture.command, std::move(first)));
        fixture.first_call = std::get<kit::ToolCallAccepted>(accepted.outcome).call;
        const auto initial = Take(co_await run.WaitForToolAsync(fixture.first_call));
        Check(initial.published_artifacts == std::vector{readable_v1} && HasArtifact(initial.blocks, readable_v1),
              "initial wait did not deliver the authorized version");
        kit::ToolRequest second{{"permission.publish"}, kit::JsonValue{true}};
        const auto next = Take(co_await run.CallToolAsync(std::move(second)));
        fixture.second_call = next.call;
        Check(next.published_artifacts == std::vector{readable_v2} && next.existing_artifacts == std::vector{readable_v1} &&
              HasArtifact(next.blocks, readable_v2) && !HasArtifact(next.blocks, never_readable) && !next.model_value,
              "CallToolAsync returned an unauthorized publication or lost an allowed reference");
        Take(co_await run.SaveCheckpointAsync({.stage = "replay-tool-results", .schema_version = 1}));
        fixture.checkpointed = true;
        co_await huxerui::Delay(1h);
        co_return kit::AgentResult<kit::RunCompletion>::Success({});
      },
      .restore = [&fixture](kit::RunContext run, kit::RunInput, kit::RunResume resume) -> kit::Async<kit::RunCompletion> {
        Check(resume.checkpoint && resume.checkpoint->stage == "replay-tool-results", "tool result checkpoint changed");
        kit::ToolRequest original{{"permission.publish"}, kit::JsonValue{false}};
        const auto accepted = Take(co_await run.StartToolAsync(fixture.command, std::move(original)));
        Check(std::get<kit::ToolCallAccepted>(accepted.outcome).call == fixture.first_call, "replay allocated a different tool call");
        const auto first = Take(co_await run.WaitForToolAsync(fixture.first_call));
        const auto second = Take(co_await run.WaitForToolAsync(fixture.second_call));
        Check(first.outcome == kit::ToolOutcome::Succeeded && first.published_artifacts.empty() &&
              !HasArtifact(first.blocks, readable_v1) && second.published_artifacts == std::vector{readable_v2} &&
              second.existing_artifacts.empty() && !HasArtifact(second.blocks, never_readable),
              "replayed tool results exposed a revoked version");
        co_return kit::AgentResult<kit::RunCompletion>::Success({
            .structured = kit::JsonValue{readable_v1.id.value}, .artifacts = {readable_v1, readable_v2}});
      }}));
  return runtime;
}

huxerui::Task<void> VerifyResultAccess(huxerui::File file, ResultAccessFixture& fixture, bool reopen) {
  auto runtime = ResultAccessRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    const auto created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {.options = {.agent = {"permission.agent"}}}));
    fixture.session = std::get<kit::SessionCreated>(created.outcome).session;
  }
  auto chat = Take(co_await runtime->Sessions().OpenAsync(fixture.session));
  const auto check_snapshot = [&](const kit::SessionSnapshot& view) {
    Check(view.artifacts == std::vector{readable_v2} &&
          view.tools.front().artifacts.empty() && view.tools.back().artifacts == std::vector{readable_v2} &&
          std::ranges::none_of(view.messages, [&](const auto& message) {
            return HasArtifact(message.blocks, readable_v1) || HasArtifact(message.blocks, never_readable);
          }), "snapshot exposed a denied artifact version");
  };
  if (!reopen) {
    fixture.before_publication = chat->Snapshot()->event_sequence;
    Take(co_await chat->SendAsync(kit::NewCommandId(), {.text = "Publish two versions"}));
    co_await Eventually([&] { return fixture.checkpointed && chat->Snapshot()->artifacts.size() == 2; },
        "original versions were not visible");
    std::optional<kit::ErrorCode> invalidated;
    bool delivered_before_revoke = false;
    auto events = Take(co_await chat->ObserveEventsAsync(fixture.before_publication, [&](auto update) {
      if (!update.Succeeded()) { invalidated = update.Error().code; return; }
      delivered_before_revoke = true;
    }));
    co_await Eventually([&] { return delivered_before_revoke; }, "event observer did not start");
    std::shared_ptr<const kit::SessionSnapshot> updated;
    auto snapshots = Take(co_await chat->ObserveAsync([&](auto update) { updated = Take(std::move(update)); }));
    co_await Eventually([&] { return updated && updated->artifacts.size() == 2; }, "snapshot observer did not start");
    fixture.revoked = true;
    Take(co_await runtime->RefreshAccessAsync());
    co_await Eventually([&] {
      return invalidated == kit::ErrorCode::CursorExpired && updated && updated->artifacts == std::vector{readable_v2};
    }, "revocation did not replace the snapshot or expire the old event access");
    check_snapshot(*updated);
    snapshots.Reset();
  } else {
    co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "the restored workflow did not finish");
    const auto snapshot = chat->Snapshot();
    Check(fixture.executions == 2 && snapshot->runs.back().result && !snapshot->runs.back().result->structured &&
          snapshot->runs.back().result->artifacts == std::vector{readable_v2}, "replay ran a tool again or exposed a revoked Run result");
    check_snapshot(*snapshot);
  }
  std::vector<kit::ArtifactRef> registrations;
  kit::Sequence through = 0;
  auto resumed = Take(co_await chat->ObserveEventsAsync(fixture.before_publication, [&](auto update) {
    const auto batch = Take(std::move(update));
    for (const auto& event : batch->events) {
      if (const auto* artifact = std::get_if<kit::ArtifactRegistered>(&event.body)) registrations.push_back(artifact->artifact);
      if (const auto* tool = std::get_if<kit::ToolCallSnapshot>(&event.body))
        Check(std::ranges::all_of(tool->artifacts, [](const auto& ref) { return ref == readable_v2; }), "tool event exposed a denied version");
      if (const auto* message = std::get_if<kit::Message>(&event.body))
        Check(!HasArtifact(message->blocks, readable_v1) && !HasArtifact(message->blocks, never_readable), "message event exposed a denied version");
      if (const auto* run = std::get_if<kit::RunSnapshot>(&event.body); run && run->result)
        Check(run->result->artifacts == std::vector{readable_v2} && !run->result->structured, "Run event exposed a denied version");
    }
    through = batch->through_sequence;
  }));
  co_await Eventually([&] { return through == chat->Snapshot()->event_sequence; }, "event replay did not reach the snapshot cursor");
  Check(registrations == std::vector{readable_v2}, "event replay exposed a revoked registration or skipped an allowed one");
  resumed.Reset();
  Take(co_await runtime->CloseAsync());
}

void RegisterReworkSeeder(const std::shared_ptr<kit::AgentRuntime>& runtime) {
  Take(runtime->RegisterTool({.id = {"rework.seed"}, .version = 1, .description = "Stage fixture revisions",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.preview = {.title = "Stage fixture"},
            .execution_data = std::move(request.arguments)});
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto mode = std::get<std::string>(call.change.execution_data.value);
        const bool deck = mode == "deck";
        kit::ArtifactDraft draft{.id = deck ? rework_deck.id : rework_report.id, .type = deck ? "test.deck" : "test.report",
            .name = deck ? "Report slides" : "Verification report", .storage_handle = mode};
        if (mode == "report-v2" || mode == "report-stale") {
          const kit::ResourceId resource{"test.report.resource"};
          const kit::ResourceRef before{resource, "1"};
          const kit::ResourceRef after{resource, "2"};
          draft.expected_latest = rework_report.version;
          draft.published_effect = kit::ResourceEffect{resource, kit::ResourceEffectState::Applied, before, after};
        }
        draft.parts = {{overview_part, "Overview"}, {results_part, "Results"}};
        if (deck) {
          draft.dependencies = {{overview_part, {rework_report}}, {results_part, {rework_report}}};
        }
        std::vector<kit::ArtifactDraft> drafts;
        drafts.push_back(std::move(draft));
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success(
            {.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(drafts)});
      }}));
}

enum class SourceApprovalCase { BeforeReview, Reviewing, Approved };

struct SavedSourceApproval {
  kit::SessionCreated consumer;
  kit::InteractionSnapshot approval;
  kit::CommandId decision;
  kit::CommitId decision_commit;
  kit::ToolCallId call;
  std::optional<kit::PlanId> plan;
  std::stop_token review_stop;
  bool waiting_for_execution = false;
  bool release_execution = false;
  unsigned reviews = 0;
  unsigned prepared = 0;
  unsigned executed = 0;
};

class SourcePlanFaultStore final : public kit::SessionStore {
public:
  explicit SourcePlanFaultStore(std::shared_ptr<kit::SessionStore> store) : store_(std::move(store)) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return store_->LoadAsync(); }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction transaction) override {
    if (blocked) {
      const auto write = std::ranges::find(transaction.writes, blocked->value, &kit::StoreWrite::key);
      if (write != transaction.writes.end() && write->data) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(write->data->data());
        const auto image = nlohmann::json::from_cbor(begin, begin + write->data->size());
        interrupted_state = image.at("session").at("data").at("tools").at(0).at("state").get<kit::ToolCallState>();
        co_return kit::AgentResult<kit::Revision>::Failure(
            {.code = kit::ErrorCode::PersistenceFailure, .message = "Interrupted before settling the stale plan"});
      }
    }
    co_return co_await store_->CommitAsync(std::move(transaction));
  }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId id) override {
    return store_->FindCommitAsync(std::move(id));
  }

  std::optional<kit::SessionId> blocked;
  std::optional<kit::ToolCallState> interrupted_state;

private:
  std::shared_ptr<kit::SessionStore> store_;
};

huxerui::Task<void> VerifySourceApproval(huxerui::File file, SavedSourceApproval& saved, SourceApprovalCase scenario, bool reopen) {
  const bool automatic = scenario != SourceApprovalCase::Approved;
  const bool before_review = scenario == SourceApprovalCase::BeforeReview;
  const kit::ProjectId project{"source-approval.project"};
  const kit::AgentId reader{"source-approval.reader"};
  const kit::ToolId consumer_tool{"source-approval.consume"};
  const kit::SourceSlice input{rework_report, {results_part}};
  const kit::ArtifactRef replacement{rework_report.id, 2};
  auto fault = std::make_shared<SourcePlanFaultStore>(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)));
  auto runtime = Take(kit::AgentRuntime::Create({.tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [&](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        if (!reopen && before_review && request.capability == kit::Capability::ReadArtifact &&
            request.actor.agent && request.actor.agent->id == reader) {
          saved.waiting_for_execution = true;
          while (!saved.release_execution) co_await huxerui::Delay(10ms);
        }
        const auto access = request.capability == kit::Capability::ExecuteTool && request.tool && request.tool->id == consumer_tool
            ? (automatic ? kit::AccessDecision::AllowReview : kit::AccessDecision::RequireConfirmation) : kit::AccessDecision::Allow;
        co_return kit::AgentResult<kit::PolicyDecision>::Success({access, {}});
      },
      .tool_reviewer = kit::ToolReviewer{"source-approval.reviewer", 1, 30s,
          [&](kit::ToolReviewRequest request) -> kit::Async<kit::ToolReviewResult> {
            ++saved.reviews;
            Check(request.authorized_intent == std::vector{input}, "review did not bind the original report result part");
            saved.review_stop = request.stop;
            if (!before_review) co_await huxerui::Delay(1h);
            co_return kit::AgentResult<kit::ToolReviewResult>::Success({kit::ToolReviewDecision::Approve, "Reviewed results"});
          }},
      .tool_resources = [&](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
        if (!reopen && !automatic && invocation.tool.id == consumer_tool && invocation.phase == kit::ToolAccessPhase::Execute) {
          saved.waiting_for_execution = true;
          while (!saved.release_execution) co_await huxerui::Delay(10ms);
        }
        co_return kit::AgentResult<std::vector<kit::ResourceAccess>>::Success({});
      },
      .artifact_store = {.preview = [](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact, .summary = request.storage_handle});
      }},
      .store = fault}));
  auto model = std::make_shared<CountingModel>();
  model->write = true;
  Take(runtime->RegisterModel({{{"source-approval.model"}, 1}, {"test", "test", "script", {}}, model}));
  RegisterReworkSeeder(runtime);
  Take(runtime->RegisterTool({.id = consumer_tool, .version = 1, .description = "Act on verified report results",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::ExternalSubmit,
      .prepare = [&, input](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        ++saved.prepared;
        co_return kit::AgentResult<kit::PreparedChange>::Success({.preview = {.title = "Use report results"}, .inputs = {input}});
      },
      .execute = [&](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        ++saved.executed;
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded});
      }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"source-approval.publisher"}, 1}, .display_name = "Publish report",
      .model = {"source-approval.model"}, .tools = {{"rework.seed"}}},
      .run = [](kit::RunContext context, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        kit::ToolRequest request{{"rework.seed"}, kit::JsonValue{std::move(input.input.text)}};
        auto result = Take(co_await context.CallToolAsync(std::move(request)));
        Check(result.outcome == kit::ToolOutcome::Succeeded, "report publication failed");
        co_return kit::AgentResult<kit::RunCompletion>::Success({});
      }}));
  Take(runtime->RegisterAgent({.definition = {.ref = {reader, 1}, .display_name = "Use report",
      .model = {"source-approval.model"}, .tools = {consumer_tool}}}));
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    kit::CreateSessionRequest source_request{.options = {.agent = {"source-approval.publisher"}, .project = project},
        .initial = kit::SendInput{.text = "report-v1"}};
    auto created = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(
        kit::NewCommandId(), std::move(source_request))).outcome);
    auto publisher = Take(co_await runtime->Sessions().OpenAsync(created.session));
    co_await Eventually([&] { return !publisher->Snapshot()->summary.active_run; }, "report baseline did not publish");
    Take(co_await runtime->Artifacts().ReadAsync(rework_report));
    kit::CreateSessionRequest consumer_request{.options = {.agent = reader, .project = project},
        .initial = kit::SendInput{.text = "Use the report's results"}};
    saved.consumer = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(
        kit::NewCommandId(), std::move(consumer_request))).outcome);
    auto consumer = Take(co_await runtime->Sessions().OpenAsync(saved.consumer.session));
    if (before_review) {
      co_await Eventually([&] {
        return saved.waiting_for_execution && consumer->Snapshot()->tools.size() == 1;
      }, "report validation did not reach the review boundary");
    } else if (automatic) {
      co_await Eventually([&] { return saved.reviews == 1; }, "the source-bound tool did not begin automatic review");
      const kit::SourceChange unrelated{{rework_report, {overview_part}}, {}, kit::SourceChangeKind::Modified};
      Take(co_await runtime->Artifacts().RecordSourceChangeAsync(project, kit::NewCommandId(), unrelated));
      Check(!saved.review_stop.stop_requested() && consumer->Snapshot()->tools.front().state == kit::ToolCallState::Reviewing,
            "a change to the overview interrupted review of the results");
    } else {
      co_await Eventually([&] { return consumer->Snapshot()->summary.pending_interactions == 1; }, "source plan did not request approval");
      saved.approval = consumer->Snapshot()->interactions.front();
      saved.decision = kit::NewCommandId();
      saved.decision_commit = Take(co_await consumer->SubmitAsync(saved.decision,
          {saved.approval.id, saved.approval.version, kit::ApprovalDecision::Approve})).stamp.commit;
      co_await Eventually([&] { return saved.waiting_for_execution; }, "approved plan did not reach the dispatch boundary");
      Check(consumer->Snapshot()->tools.front().state == kit::ToolCallState::Ready,
            "the approved source plan was not committed ahead of dispatch");
    }
    const auto pending = consumer->Snapshot();
    Check(pending->tools.size() == 1, "the original tool call was not published");
    const auto& call = pending->tools.front();
    saved.call = call.id;
    saved.plan = call.plan;
    Check(before_review || saved.plan.has_value(), "the pending tool plan was not persisted");
    Take(co_await publisher->SendAsync(kit::NewCommandId(), {.text = "report-v2"}));
    co_await Eventually([&] { return !publisher->Snapshot()->summary.active_run; }, "replacement report did not publish");
    Check(Take(co_await runtime->Artifacts().ReadAsync(replacement)).source_change.has_value(), "the report revision did not record its change");
    if (before_review) saved.release_execution = true;
    if (!automatic) {
      fault->blocked = saved.consumer.session;
      saved.release_execution = true;
      co_await Eventually([&] { return fault->interrupted_state.has_value(); }, "the interrupted settlement was not attempted");
      Check(fault->interrupted_state == kit::ToolCallState::Conflict &&
            consumer->Snapshot()->tools.front().state == kit::ToolCallState::Ready && saved.executed == 0,
            "the fault did not leave the original approval saved before dispatch");
      auto closed = co_await runtime->CloseAsync();
      Check(!closed.Succeeded() && closed.Error().code == kit::ErrorCode::PersistenceFailure,
            "the fault settling the source conflict was not reported");
      co_return;
    }
  }

  auto consumer = Take(co_await runtime->Sessions().OpenAsync(saved.consumer.session));
  co_await Eventually([&] { return !consumer->Snapshot()->summary.active_run; }, "the stale plan did not settle");
  const auto state = consumer->Snapshot();
  Check(state->tools.size() == 1 && state->runs.size() == 1, "the original run or tool call was lost");
  const auto& call = state->tools.front();
  Check(state->runs.front().id == saved.consumer.initial_run &&
        call.id == saved.call && (!saved.plan || call.plan == saved.plan) && call.state == kit::ToolCallState::Conflict &&
        call.reason && call.reason->code == kit::ErrorCode::Conflict && saved.prepared == 1 && saved.executed == 0,
        "the replaced report was accepted or the original tool plan was lost");
  if (before_review) {
    Check(saved.reviews == 0 && !call.review && state->interactions.empty(), "an obsolete plan was sent for automatic review");
  } else if (automatic) {
    Check(saved.reviews == 1 && saved.review_stop.stop_requested() && call.review &&
          call.review->outcome == kit::ToolReviewOutcome::Invalidated && state->interactions.empty(),
          "source invalidation did not stop the original review or opened a manual approval");
  } else {
    Check(state->interactions.size() == 1, "recovery lost or duplicated the original approval");
    const auto& approval = state->interactions.front();
    Check(saved.reviews == 0 && state->summary.pending_interactions == 0 &&
          approval.id == saved.approval.id && approval.state == kit::InteractionState::Approved,
          "recovering a stale plan changed the recorded human decision or asked again");
    const auto decision = Take(co_await runtime->Commands().ReadAsync(saved.decision));
    Check(decision.accepted.commit == saved.decision_commit && decision.state == kit::CommandState::Applied,
          "the original approval receipt was lost on recovery");
  }
  Take(co_await runtime->CloseAsync());
}

struct SavedRework {
  kit::SessionId session;
  kit::SessionId source_consumer;
  kit::InteractionId retired_approval;
  kit::CommandId command;
  kit::CommandId stale;
  kit::CommitId accepted;
  kit::InteractionSnapshot approval;
  kit::ToolCallId call;
  kit::CommandId report_change;
  kit::CommandId published_change;
  kit::CommitId recorded_at;
  bool report_visible = true;
  unsigned prepared = 0;
  unsigned applied = 0;
  unsigned used_source = 0;
};

huxerui::Task<void> UseLegacyReworkPlanField(huxerui::File file, kit::SessionId session) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto image = Take(co_await store->LoadAsync());
  const auto row = std::ranges::find(image.records, session.value, &kit::StoreValue::key);
  Check(row != image.records.end(), "rework was not stored");
  const auto* begin = reinterpret_cast<const std::uint8_t*>(row->data.data());
  auto document = nlohmann::json::from_cbor(begin, begin + row->data.size());
  unsigned converted = 0;
  for (auto& command : document.at("session").at("commands")) {
    auto& record = command.at("record");
    auto plan = std::move(record.at("artifact_change"));
    record.erase("artifact_change");
    if (plan.is_null()) continue;
    record["rework"] = std::move(plan);
    ++converted;
  }
  Check(converted == 2, "the pending and queued rework plans were not both stored");
  const auto cbor = nlohmann::json::to_cbor(document);
  const auto* bytes = reinterpret_cast<const std::byte*>(cbor.data());
  kit::StoreTransaction transaction{{kit::NewCommandId().value}, image.revision,
      {{row->collection, row->key, row->revision, huxerui::Bytes(bytes, bytes + cbor.size())}}};
  Take(co_await store->CommitAsync(std::move(transaction)));
}

huxerui::Task<void> VerifyRework(huxerui::File file, SavedRework& saved, bool reopen) {
  const kit::ArtifactRef updated_report{rework_report.id, 2};
  const kit::ProjectId project{"rework.project"};
  const kit::ToolId check_source{"rework.check-source"};
  kit::PrincipalId viewer{"test.user"};
  auto storage = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto lost = std::make_shared<LostReceiptStore>(storage, true);
  auto runtime = Take(kit::AgentRuntime::Create({.tasks = huxerui::UseApplicationTaskScope(),
      .principal = [&viewer] { return kit::AgentResult<kit::PrincipalId>::Success(viewer); },
      .access = [&saved, updated_report, check_source](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        if (!saved.report_visible && request.capability == kit::Capability::ReadArtifact &&
            std::ranges::any_of(request.sources, [&](const auto& input) { return input.source == kit::SourceRef{updated_report}; }))
          co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::Deny, "Replacement report is private"});
        const auto access = request.capability == kit::Capability::ReworkArtifact ||
            (request.capability == kit::Capability::ExecuteTool && request.tool && request.tool->id == check_source)
            ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow;
        co_return kit::AgentResult<kit::PolicyDecision>::Success({access, {}});
      },
      .artifact_store = {.preview = [](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact, .summary = request.storage_handle});
      }},
      .changes = {
          .prepare_rework = [&saved](kit::ChangePrepareContext context, kit::ReworkRequest request) -> kit::Async<kit::PreparedChange> {
            ++saved.prepared;
            Check(context.artifact && context.artifact->metadata.ref == rework_deck &&
                  request.parts == std::vector{results_part} && context.access.sources.size() == 3,
                  "rework adapter did not receive the reviewed baseline and sources");
            co_return kit::AgentResult<kit::PreparedChange>::Success({
                .preview = {.title = "Update results page", .artifacts = {rework_deck}},
                .execution_data = kit::JsonValue{std::string("preserved overview; results from the updated report")}});
          },
          .execute_rework = [&saved](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
            ++saved.applied;
            Check(context.CallId() == saved.call, "reopened rework changed its call ID");
            kit::ArtifactDraft deck{.id = rework_deck.id, .expected_latest = rework_deck.version, .type = "test.deck",
                .name = "Report slides", .storage_handle = std::get<std::string>(call.change.execution_data.value),
                .parts = {{overview_part, "Overview"}, {results_part, "Updated results"}}};
            std::vector<kit::ArtifactDraft> drafts;
            drafts.push_back(std::move(deck));
            co_return kit::AgentResult<kit::ToolExecutionResult>::Success(
                {.outcome = kit::ToolOutcome::Succeeded, .new_artifacts = std::move(drafts)});
          }},
      .store = reopen ? storage : lost}));
  Take(runtime->RegisterModel({{{"rework.model"}, 1}, {"test", "test", "script", {}}, std::make_shared<CountingModel>()}));
  Take(runtime->RegisterTool({.id = check_source, .version = 1, .description = "Use the reviewed report results",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.preview = {.title = "Use report results"},
            .inputs = {{rework_report, {results_part}}}});
      },
      .execute = [&saved](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        ++saved.used_source;
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded});
      }}));
  RegisterReworkSeeder(runtime);
  Take(runtime->RegisterAgent({.definition = {.ref = {{"rework.agent"}, 1}, .display_name = "Rework",
      .model = {"rework.model"}, .tools = {{"rework.seed"}, check_source}},
      .run = [check_source](kit::RunContext context, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        if (input.input.text == "hold") {
          kit::FormRequest form{.title = "Finish the current task", .schema = {kit::JsonValue{true}}};
          auto result = co_await context.AskAsync(std::move(form));
          if (!result.Succeeded()) co_return kit::AgentResult<kit::RunCompletion>::Failure(result.Error());
        } else {
          kit::ToolRequest request{input.input.text == "consume" ? check_source : kit::ToolId{"rework.seed"},
                                   kit::JsonValue{input.input.text}};
          auto staged = co_await context.CallToolAsync(std::move(request));
          if (!staged.Succeeded()) co_return kit::AgentResult<kit::RunCompletion>::Failure(staged.Error());
          if (staged.Value().outcome != kit::ToolOutcome::Succeeded)
            co_return kit::AgentResult<kit::RunCompletion>::Failure(*staged.Value().reason);
        }
        co_return kit::AgentResult<kit::RunCompletion>::Success({});
      }}));
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    kit::CreateSessionRequest create{.options = {.agent = {"rework.agent"}, .project = project}};
    saved.session = std::get<kit::SessionCreated>(
        Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(create))).outcome).session;
  }
  auto chat = Take(co_await runtime->Sessions().OpenAsync(saved.session));
  kit::ReworkRequest request{.session = saved.session, .baseline = rework_deck, .parts = {results_part},
      .replacements = {{{rework_report}, {updated_report}}}};
  const kit::SourceChange change{{rework_report}, kit::SourceRef{updated_report}, kit::SourceChangeKind::Modified};
  if (!reopen) {
    for (const auto* mode : {"report-v1", "deck"}) {
      Take(co_await chat->SendAsync(kit::NewCommandId(), {.text = mode}));
      co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "rework seed did not finish");
      Check(chat->Snapshot()->runs.back().state == kit::RunState::Succeeded, "rework seed failed");
    }
    kit::CreateSessionRequest consumer_request{.options = {.agent = {"rework.agent"}, .project = project}};
    saved.source_consumer = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(
        kit::NewCommandId(), std::move(consumer_request))).outcome).session;
    auto consumer = Take(co_await runtime->Sessions().OpenAsync(saved.source_consumer));
    Take(co_await consumer->SendAsync(kit::NewCommandId(), {.text = "consume"}));
    co_await Eventually([&] { return consumer->Snapshot()->summary.pending_interactions == 1; }, "report plan did not await approval");
    const auto pending = consumer->Snapshot()->interactions.back();
    saved.retired_approval = pending.id;
    const kit::SourceChange unrelated{{rework_report, {overview_part}}, {}, kit::SourceChangeKind::Modified};
    Take(co_await runtime->Artifacts().RecordSourceChangeAsync(project, kit::NewCommandId(), unrelated));
    Check(consumer->Snapshot()->interactions.back().state == kit::InteractionState::Pending,
        "a change outside the used source part invalidated the approval");
    Check(lost->lookups == 0, "the receipt probe was triggered before the report upgrade");
    Take(co_await chat->SendAsync(kit::NewCommandId(), {.text = "report-v2"}));
    co_await Eventually([&] { return !chat->Snapshot()->summary.active_run && !consumer->Snapshot()->summary.active_run; },
        "report publication or approval invalidation did not finish");
    const auto updated = Take(co_await runtime->Artifacts().ReadAsync(updated_report));
    Check(updated.source_change.has_value(), "publishing a report revision did not register a source change");
    Check(lost->lookups == 1, "the report and source change did not share a recoverable store transaction");
    saved.published_change = *updated.source_change;
    const auto retired = consumer->Snapshot();
    Check(retired->interactions.back().state == kit::InteractionState::Invalidated && saved.used_source == 0 &&
          retired->tools.back().state == kit::ToolCallState::Conflict, "publishing a report revision allowed the stale plan");
    auto ignored = co_await consumer->SubmitAsync(kit::NewCommandId(),
        {pending.id, pending.version, kit::ApprovalDecision::Approve});
    Check(!ignored.Succeeded(), "an invalidated report approval was accepted");
    saved.report_change = kit::NewCommandId();
    saved.recorded_at = Take(co_await runtime->Artifacts().RecordSourceChangeAsync(project, saved.report_change, change)).stamp.commit;
    auto changed = change;
    changed.kind = kit::SourceChangeKind::Reverted;
    const auto conflict = co_await runtime->Artifacts().RecordSourceChangeAsync(project, saved.report_change, std::move(changed));
    Check(!conflict.Succeeded() && conflict.Error().code == kit::ErrorCode::Conflict,
        "a source-change command accepted a different change");
    auto broad = request;
    broad.parts.clear();
    auto expansion = co_await runtime->Artifacts().PrepareReworkAsync(kit::NewCommandId(), std::move(broad));
    Check(!expansion.Succeeded() && expansion.Error().code == kit::ErrorCode::ScopeExpansionRequired &&
          expansion.Error().proposed_scope && expansion.Error().proposed_scope->whole_artifact && saved.prepared == 0,
          "whole document scope did not require explicit confirmation");
    Take(co_await chat->SendAsync(kit::NewCommandId(), {.text = "hold"}));
    co_await Eventually([&] { return chat->Snapshot()->summary.pending_interactions == 1; }, "original run did not wait");
    const auto form = chat->Snapshot()->interactions.back();
    saved.command = kit::NewCommandId();
    saved.stale = kit::NewCommandId();
    auto accepted = Take(co_await runtime->Artifacts().PrepareReworkAsync(saved.command, request));
    saved.accepted = accepted.stamp.commit;
    const auto retry = Take(co_await runtime->Artifacts().PrepareReworkAsync(saved.command, request));
    Check(std::get<kit::ChangePlanAccepted>(accepted.outcome).queue_item && retry.stamp.commit == saved.accepted,
          "rework was not queued once under the original command");
    Take(co_await runtime->Artifacts().PrepareReworkAsync(saved.stale, request));
    Check(chat->Snapshot()->summary.queued_inputs == 2 && saved.prepared == 2 && saved.applied == 0,
          "queued rework started before the active run could finish");
    Take(co_await chat->SubmitAsync(kit::NewCommandId(), {form.id, form.version, kit::JsonValue{true}}));
  }
  co_await Eventually([&] {
    const auto snapshot = chat->Snapshot();
    return snapshot->summary.pending_interactions == 1 && std::holds_alternative<kit::ToolApproval>(snapshot->interactions.back().body);
  }, "the rework plan did not become an approval");
  const auto approval = chat->Snapshot()->interactions.back();
  if (!reopen) {
    saved.approval = approval;
    saved.call = std::get<kit::ToolApproval>(approval.body).call;
    Check(saved.applied == 0, "rework executed before the approval");
  } else {
    auto consumer = Take(co_await runtime->Sessions().OpenAsync(saved.source_consumer));
    const auto retired = consumer->Snapshot();
    Check(retired->interactions.back().id == saved.retired_approval &&
          retired->interactions.back().state == kit::InteractionState::Invalidated && saved.used_source == 0,
          "source invalidation changed or executed on reopen");
    const auto v2 = Take(co_await runtime->Artifacts().ReadAsync(updated_report));
    Check(v2.source_change == saved.published_change, "the revision and its source-change command were not restored together");
    const kit::ArtifactImpactQuery published_query{.project = project, .change_command = saved.published_change};
    const auto automatic = Take(co_await runtime->Artifacts().InspectImpactAsync(published_query));
    Check(automatic.recorded_change == change && automatic.affected.size() == 1 &&
          automatic.affected.front().parts == std::vector{overview_part, results_part}, "automatic source change lost its lineage");
    Check(approval.id == saved.approval.id && approval.version == saved.approval.version &&
          std::get<kit::ToolApproval>(approval.body).call == saved.call, "reopen changed the rework approval");
    const auto replay = Take(co_await runtime->Artifacts().RecordSourceChangeAsync(project, saved.report_change, change));
    const auto progress = Take(co_await runtime->Commands().ReadAsync(saved.report_change));
    Check(replay.stamp.commit == saved.recorded_at && progress.state == kit::CommandState::Applied &&
          progress.accepted.project_revision == replay.stamp.project_revision, "source change did not preserve its original project commit");
    const kit::ArtifactImpactQuery query{.project = project, .change_command = saved.report_change};
    const auto impact = Take(co_await runtime->Artifacts().InspectImpactAsync(query));
    Check(impact.recorded_change == change && impact.affected.size() == 1 && impact.affected.front().affected == rework_deck &&
          impact.affected.front().parts == std::vector{overview_part, results_part}, "recorded source change did not locate the original pages");
    saved.report_visible = false;
    const auto hidden = co_await runtime->Artifacts().InspectImpactAsync(query);
    saved.report_visible = true;
    Check(!hidden.Succeeded() && hidden.Error().code == kit::ErrorCode::PermissionDenied,
        "query exposed a recorded replacement after access was withdrawn");
    Take(co_await chat->SubmitAsync(kit::NewCommandId(), {approval.id, approval.version, kit::ApprovalDecision::Approve}));
    co_await Eventually([&] {
      return !chat->Snapshot()->summary.active_run && chat->Snapshot()->summary.queued_inputs == 0;
    }, "rework or stale plan could not settle");
    Check(saved.applied == 1 && saved.prepared == 2, "restored rework ran or prepared again");
    const auto published = Take(co_await runtime->Artifacts().ReadAsync({rework_deck.id, 2}));
    const auto original = Take(co_await runtime->Artifacts().ReadAsync(rework_deck));
    Check(published.source_change.has_value(), "partial rework did not record its source change");
    const kit::ArtifactImpactQuery local_rework{.project = project, .change_command = published.source_change};
    const auto changed_deck = Take(co_await runtime->Artifacts().InspectImpactAsync(local_rework));
    Check(changed_deck.recorded_change &&
          changed_deck.recorded_change->before == kit::SourceSlice{rework_deck, {results_part}},
          "partial rework recorded changes outside the approved pages");
    Check(published.parts.size() == 2 && published.parts[0].id == original.parts[0].id &&
          published.parts[0].label == original.parts[0].label && published.parts[1].id == results_part &&
          published.dependencies[0].input.source == kit::SourceRef{rework_report} &&
          published.dependencies[1].input.source == kit::SourceRef{updated_report} &&
          original.dependencies[1].input.source == kit::SourceRef{rework_report},
          "partial rework changed an unselected part or lost a versioned source");
    const auto applied = Take(co_await runtime->Commands().ReadAsync(saved.command));
    const auto stale = Take(co_await runtime->Commands().ReadAsync(saved.stale));
    Check(applied.state == kit::CommandState::Applied && stale.state == kit::CommandState::Failed && stale.failure &&
          stale.failure->code == kit::ErrorCode::Conflict, "old-baseline rework was not rejected");
    const auto before = chat->Snapshot();
    const auto report_call = std::ranges::find_if(before->tools, [&](const auto& tool) {
      return std::ranges::find(tool.artifacts, updated_report) != tool.artifacts.end();
    });
    Check(report_call != before->tools.end() && report_call->effects.size() == 1 &&
          report_call->effects.front().state == kit::ResourceEffectState::Applied,
          "a committed artifact did not retain its resource effect after restart");
    Take(co_await chat->SendAsync(kit::NewCommandId(), {.text = "report-stale"}));
    co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "the conflicted publication did not finish");
    const auto after = chat->Snapshot();
    Check(after->artifacts == before->artifacts && after->tools.back().state == kit::ToolCallState::Conflict &&
          after->tools.back().effects.empty() && after->tools.back().source_changes.empty(),
          "a rejected artifact publication recorded an applied effect");
    Check(std::ranges::any_of(after->queue, [&](const auto& item) {
      const auto* command = std::get_if<kit::CommandId>(&item.input);
      return command && *command == saved.stale && item.state == kit::QueueItemState::Rejected;
    }), "the owner did not receive the rejected change in the reopened queue");
    viewer = {"test.other"};
    const auto other = Take(co_await runtime->Sessions().OpenAsync(saved.session));
    Check(std::ranges::none_of(other->Snapshot()->queue, [](const auto& item) {
      return std::holds_alternative<kit::CommandId>(item.input);
    }), "a reader saw someone else's queued artifact change");
    viewer = {"test.user"};
  }
  Take(co_await runtime->CloseAsync());
}

struct FormProbe {
  bool hold_before_wait = false;
  int fresh = 0;
  int restored = 0;
  kit::InteractionId form;
  kit::CommitId registration;
  std::string answer;
  bool receive_conclusion = false;
  std::vector<kit::SideChatConclusion> received;
  bool reject_saved_profile = false;
  std::optional<kit::SourceSlice> hidden_source;
  kit::PrincipalId principal{"test.user"};
  std::shared_ptr<ReleaseSkillSource> skills;
};

kit::Async<kit::RunCompletion> ReadForm(kit::RunContext context, kit::CommandId command, FormProbe& probe) {
  kit::FormRequest request{
      .title = "Audience",
      .schema = {kit::JsonValue{kit::JsonValue::Object{{"type", kit::JsonValue{std::string("string")}}}}}};
  auto receipt = Take(co_await context.RequestInputAsync(std::move(command), request));
  const auto registered = std::get<kit::InputRequested>(receipt.outcome);
  if (probe.form.value.empty()) {
    probe.form = registered.id;
    probe.registration = receipt.stamp.commit;
  } else {
    Check(probe.form == registered.id && probe.registration == receipt.stamp.commit,
          "stage command restored its original form receipt");
  }
  if (probe.hold_before_wait) co_await huxerui::Delay(1h);
  auto answer = Take(co_await context.WaitForInputAsync(registered.id));
  Check(answer.state == kit::InteractionState::Submitted && answer.value, "form reply was not retained");
  probe.answer = std::get<std::string>(answer.value->value);
  co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = probe.answer});
}

std::shared_ptr<kit::AgentRuntime> RegisterFormRuntime(std::shared_ptr<kit::SessionStore> storage,
                                                      std::shared_ptr<FormProbe> probe) {
  auto model = std::make_shared<CountingModel>();
  auto runtime = RegisterRuntime(std::move(storage), model, {}, false, {},
      [probe](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        using Decision = kit::AgentResult<kit::PolicyDecision>;
        if (probe->reject_saved_profile && request.capability == kit::Capability::UseRunConfiguration &&
            request.run_configuration && request.run_configuration->execution_profile ==
                kit::ExecutionProfileRef{{"test.old-permission"}, 1}) {
          co_return Decision::Failure({.code = kit::ErrorCode::DeadlineExceeded, .message = "Previous grant expired"});
        }
        if (probe->hidden_source && (request.capability == kit::Capability::ReadContext ||
                                    request.capability == kit::Capability::SendToModel) &&
            std::ranges::find(request.sources, *probe->hidden_source) != request.sources.end()) {
          co_return Decision::Success({kit::AccessDecision::Deny, "Source withdrawn"});
        }
        co_return Decision::Success({kit::AccessDecision::Allow, {}});
      }, [probe] { return kit::AgentResult<kit::PrincipalId>::Success(probe->principal); });
  std::vector<kit::SkillSourceId> skill_sources;
  if (probe->skills) {
    skill_sources.push_back({"test.packaged"});
    Take(runtime->RegisterSkillSource(skill_sources.front(), probe->skills));
  }
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"test.form"}, 1}, .display_name = "Form", .model = {"test.model"},
          .skill_sources = std::move(skill_sources)},
      .run = [probe](kit::RunContext context, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        if (probe->receive_conclusion && input.input.text == "Apply the conclusion") {
          Check(input.input.side_chat_conclusions.empty() && input.input.recovered_conclusions.empty() &&
                input.input.recovered_steer_conclusions.empty() && input.conclusions.size() == 1,
                "restored run lost the normalized conclusion");
          probe->received = std::move(input.conclusions);
          co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Conclusion accepted"});
        }
        ++probe->fresh;
        Check(context.Id() == input.run, "host context must be scoped to its own run");
        auto command = kit::NewCommandId();
        kit::RunCheckpoint checkpoint{.stage = "audience", .schema_version = 1, .state = kit::JsonValue{command.value}};
        Take(co_await context.SaveCheckpointAsync(std::move(checkpoint)));
        co_return co_await ReadForm(std::move(context), std::move(command), *probe);
      },
      .restore = [probe](kit::RunContext context, kit::RunInput input, kit::RunResume resume)
          -> kit::Async<kit::RunCompletion> {
        ++probe->restored;
        Check(context.Id() == input.run && resume.checkpoint && resume.checkpoint->stage == "audience" &&
              resume.checkpoint->schema_version == 1 && resume.waits.size() == 1 &&
              std::get<kit::InteractionId>(resume.waits.front().subject) == probe->form, "restore received stage and form");
        kit::CommandId command{std::get<std::string>(resume.checkpoint->state.value)};
        co_return co_await ReadForm(std::move(context), std::move(command), *probe);
      }}));
  return runtime;
}

huxerui::Task<void> FormRun(huxerui::File file, std::shared_ptr<FormProbe> probe, kit::SessionCreated& created,
                           bool reopen, bool answer_before_restart) {
  auto runtime = RegisterFormRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  if (!reopen) {
    kit::CreateSessionRequest request{.options = {.agent = {"test.form"}}, .initial = kit::SendInput{.text = "Ask"}};
    created = std::get<kit::SessionCreated>(Take(
        co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(request))).outcome);
  }
  auto session = Take(co_await runtime->Sessions().OpenAsync(created.session));
  co_await Eventually([&] { return probe->form.value.size() && session->Snapshot()->interactions.size() == 1; },
                      "form was not committed");
  auto form = session->Snapshot()->interactions.front();
  Check(form.id == probe->form && form.run == created.initial_run, "form was bound to original run");
  if (reopen != answer_before_restart) {
    kit::InteractionReply reply{form.id, form.version, kit::JsonValue{std::string("Engineers")}};
    Take(co_await session->SubmitAsync(kit::NewCommandId(), std::move(reply)));
  }
  if (reopen) {
    co_await Eventually([&] { return !session->Snapshot()->summary.active_run; }, "restored host run did not finish");
    auto snapshot = session->Snapshot();
    Check(probe->fresh == 1 && probe->restored == 1 && probe->answer == "Engineers" &&
          snapshot->runs.size() == 1 && snapshot->runs.front().state == kit::RunState::Succeeded &&
          snapshot->interactions.size() == 1, "host continued the same form and read its committed reply");
  } else {
    Check(probe->answer.empty(), "first application must stop before processing the answer");
  }
  Take(co_await runtime->CloseAsync());
}

struct StoredSideChat {
  kit::SessionId parent;
  kit::SessionId child;
  kit::QueueItemId item;
  kit::SourceSlice source;
  kit::SourceSlice quote;
  kit::CommandId steer;
};

enum class SideChatDelivery { Queue, RejectedQueue, Steer };
const kit::SkillRef recovered_skill = ReleaseSkillSource::Ref(3);

huxerui::Task<void> SaveSideChat(huxerui::File file, std::shared_ptr<FormProbe> probe, StoredSideChat& saved,
                               SideChatDelivery delivery = SideChatDelivery::Queue) {
  auto runtime = RegisterFormRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(runtime->RegisterAgent({{.ref = {{"test.selection"}, 1}, .display_name = "Selection", .model = {"test.model"}}}));
  Take(co_await runtime->StartAsync());
  kit::CreateSessionRequest root{.options = {.agent = {"test.form"}}, .initial = kit::SendInput{.text = "Stored source"}};
  auto base = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(root)));
  saved.parent = std::get<kit::SessionCreated>(base.outcome).session;
  auto main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  co_await Eventually([&] { return !main->Snapshot()->interactions.empty(); }, "the parent did not commit its form");
  const auto source = main->Snapshot()->messages.front();
  saved.source = {source.ref, {}, kit::SourceTextRange{source.blocks.front().id, 0, 6}};
  kit::CreateSessionRequest side{.options = {.agent = {"test.selection"}, .retention = kit::SessionRetention::Ephemeral},
      .initial = kit::SendInput{.text = "Explain the selection"}, .side_chat = kit::SideChatSeed{saved.parent, {saved.source}}};
  auto child = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(side)));
  saved.child = std::get<kit::SessionCreated>(child.outcome).session;
  auto conversation = Take(co_await runtime->Sessions().OpenAsync(saved.child));
  co_await Eventually([&] { return !conversation->Snapshot()->summary.active_run; }, "the side chat did not settle");
  const auto reply = conversation->Snapshot()->messages.back();
  kit::SendInput input{.text = "Apply the conclusion", .side_chat_conclusions = {{reply.ref}}};
  kit::UserRunOptions start;
  if (delivery != SideChatDelivery::Queue) {
    saved.quote = {source.ref, {}, kit::SourceTextRange{source.blocks.front().id, 7, 13}};
    input.context.push_back(saved.quote);
    start.execution_profile = kit::ExecutionProfileRef{{"test.old-permission"}, 1};
  }
  if (delivery == SideChatDelivery::Steer) {
    input.skills = {recovered_skill};
    saved.steer = kit::NewCommandId();
    const auto run = *main->Snapshot()->summary.active_run;
    Take(co_await main->SteerAsync(saved.steer, run, std::move(input)));
    const auto pending = co_await main->ReadUnappliedSteerDraftAsync(saved.steer);
    Check(!pending.Succeeded() && pending.Error().code == kit::ErrorCode::InvalidState,
          "a pending supplement could be copied into a second run");
  } else {
    auto queued = Take(co_await main->QueueAsync(kit::NewCommandId(), std::move(input), std::move(start)));
    saved.item = std::get<kit::InputQueued>(queued.outcome).item;
  }
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ReopenSideChat(huxerui::File file, std::shared_ptr<FormProbe> probe, const StoredSideChat& saved) {
  auto runtime = RegisterFormRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto child = co_await runtime->Sessions().OpenAsync(saved.child);
  Check(!child.Succeeded() && child.Error().code == kit::ErrorCode::NotFound, "the temporary side chat survived restart");
  auto main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto snapshot = main->Snapshot();
  const auto queued = std::ranges::find(snapshot->queue, saved.item, &kit::QueuedInput::id);
  Check(queued != snapshot->queue.end() && queued->state == kit::QueueItemState::Queued &&
      std::get<kit::SendInput>(queued->input).side_chat_conclusions.empty() &&
      queued->conclusions.size() == 1 && queued->conclusions.front().text == "Ready" &&
      queued->conclusions.front().sources == std::vector<kit::SourceSlice>{saved.source},
      "the queued conclusion did not restore independently of its temporary reply");
  const auto& form = snapshot->interactions.front();
  kit::InteractionReply approval{form.id, form.version, kit::JsonValue{std::string("Done")}};
  Take(co_await main->SubmitAsync(kit::NewCommandId(), std::move(approval)));
  co_await Eventually([&] { return !main->Snapshot()->summary.active_run && !probe->received.empty(); },
      "the recovered queue did not submit the stored conclusion");
  Check(probe->received.size() == 1 && probe->received.front().text == "Ready", "the host got the wrong conclusion");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> RecoverRejectedReferences(huxerui::File file, std::shared_ptr<FormProbe> probe, const StoredSideChat& saved) {
  probe->reject_saved_profile = true;
  auto runtime = RegisterFormRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto expired_child = co_await runtime->Sessions().OpenAsync(saved.child);
  Check(!expired_child.Succeeded(), "the original temporary conversation should be gone");
  auto main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto form = main->Snapshot()->interactions.front();
  kit::InteractionReply reply{form.id, form.version, kit::JsonValue{std::string("Done")}};
  Take(co_await main->SubmitAsync(kit::NewCommandId(), std::move(reply)));
  co_await Eventually([&] {
    const auto snapshot = main->Snapshot();
    return !snapshot->summary.active_run && snapshot->queue.front().state == kit::QueueItemState::Rejected;
  }, "the old queue grant was not rejected after restart");
  auto restored = Take(co_await main->ReadQueuedDraftAsync(saved.item));
  Check(restored.text == "Apply the conclusion" && restored.references.size() == 2 &&
        restored.references[0].reference && std::get<kit::SourceSlice>(*restored.references[0].reference) == saved.quote &&
        restored.references[0].text == "source" && restored.references[1].reference && restored.references[1].text == "Ready",
        "draft did not recover the exact quote and saved conclusion text");
  const auto recovered = std::get<kit::QueuedConclusionRef>(*restored.references[1].reference);
  Check(recovered.item == saved.item && recovered.index == 0, "recovery did not address the original conclusion");

  probe->principal = {"another.user"};
  Take(co_await runtime->RefreshAccessAsync());
  auto other = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  auto hidden = co_await other->ReadQueuedDraftAsync(saved.item);
  Check(!hidden.Succeeded() && hidden.Error().code == kit::ErrorCode::NotFound && other->Snapshot()->queue.empty(),
        "another user saw the rejected draft");
  kit::SendInput reapply{.text = restored.text, .context = {saved.quote}, .recovered_conclusions = {recovered}};
  auto forged = co_await other->SendAsync(kit::NewCommandId(), reapply);
  Check(!forged.Succeeded() && forged.Error().code == kit::ErrorCode::NotFound,
        "another user replayed a recovered conclusion");

  probe->principal = {"test.user"};
  probe->hidden_source = saved.source;
  Take(co_await runtime->RefreshAccessAsync());
  main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  auto stale = co_await main->SendAsync(kit::NewCommandId(), reapply);
  Check(!stale.Succeeded() && stale.Error().code == kit::ErrorCode::PermissionDenied &&
        main->Snapshot()->runs.size() == 1 && probe->received.empty(), "a revoked source reached a new run");
  const auto partial = Take(co_await main->ReadQueuedDraftAsync(saved.item));
  Check(partial.references.size() == 2 && partial.references.front() == restored.references.front() &&
        !partial.references.back().reference && partial.references.back().text == "Queued conclusion is unavailable",
        "a withdrawn conclusion exposed data or removed the remaining editable quote");

  probe->hidden_source.reset();
  Take(co_await runtime->RefreshAccessAsync());
  main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto command = kit::NewCommandId();
  const auto accepted = Take(co_await main->QueueAsync(command, reapply));
  const auto replay = Take(co_await main->QueueAsync(command, std::move(reapply)));
  Check(accepted.stamp.commit == replay.stamp.commit, "retry duplicated the recovered input");
  co_await Eventually([&] { return !main->Snapshot()->summary.active_run && !probe->received.empty(); },
      "the edited draft did not reach the new run");
  const auto final = main->Snapshot();
  Check(final->queue.size() == 2 && final->queue.front().state == kit::QueueItemState::Rejected &&
        final->queue.back().state == kit::QueueItemState::Started && final->runs.size() == 2 &&
        std::get<kit::SendInput>(final->queue.back().input).context == std::vector<kit::SourceSlice>{saved.quote} &&
        probe->received.size() == 1 && probe->received.front().text == "Ready" &&
        probe->received.front().sources == std::vector<kit::SourceSlice>{saved.source},
        "new queue command changed the original item or lost selected references");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> FinishUnappliedSteer(huxerui::File file, std::shared_ptr<FormProbe> probe, const StoredSideChat& saved) {
  auto runtime = RegisterFormRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  const auto main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto before = main->Snapshot();
  const auto& steer = before->runs.front().steering.front();
  Check(steer.state == kit::SteerState::Pending && !steer.recovery, "restart exposed an unfinished supplement as a draft");
  const auto& form = before->interactions.front();
  kit::InteractionReply reply{form.id, form.version, kit::JsonValue{std::string("Done")}};
  Take(co_await main->SubmitAsync(kit::NewCommandId(), std::move(reply)));
  co_await Eventually([&] { return !main->Snapshot()->summary.active_run; }, "the original run did not finish");
  const auto after = main->Snapshot();
  Check(after->runs.front().steering.front().state == kit::SteerState::NotApplied &&
        after->runs.front().steering.front().recovery == saved.steer && probe->received.empty(),
        "the finished run consumed or lost the unapplied supplement");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> RecoverUnappliedSteer(huxerui::File file, std::shared_ptr<FormProbe> probe, const StoredSideChat& saved) {
  auto runtime = RegisterFormRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  Check(!(co_await runtime->Sessions().OpenAsync(saved.child)).Succeeded(), "the temporary reply unexpectedly survived restart");
  auto main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto original = main->Snapshot()->runs.front().steering.front();
  const auto draft = Take(co_await main->ReadUnappliedSteerDraftAsync(saved.steer));
  const auto ref = kit::UnappliedSteerConclusionRef{saved.steer, 0};
  Check(original.recovery == saved.steer && draft.text == "Apply the conclusion" &&
        draft.skills == std::vector<kit::SkillRef>{recovered_skill} && draft.references.size() == 2 &&
        draft.references[0].reference && std::get<kit::SourceSlice>(*draft.references[0].reference) == saved.quote &&
        draft.references[0].text == "source" && draft.references[1].reference && draft.references[1].text == "Ready" &&
        std::get<kit::UnappliedSteerConclusionRef>(*draft.references[1].reference) == ref,
        "the recovery did not retain the original selection, conclusion, and Skill revision");
  const kit::SendInput input{.text = draft.text, .context = {saved.quote}, .skills = draft.skills,
      .recovered_steer_conclusions = {ref}};

  probe->principal = {"another.user"};
  Take(co_await runtime->RefreshAccessAsync());
  const auto other = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto hidden = co_await other->ReadUnappliedSteerDraftAsync(saved.steer);
  const auto forged = co_await other->SendAsync(kit::NewCommandId(), input);
  Check(!other->Snapshot()->runs.front().steering.front().recovery && !hidden.Succeeded() &&
        hidden.Error().code == kit::ErrorCode::NotFound && !forged.Succeeded() && forged.Error().code == kit::ErrorCode::NotFound,
        "another principal recovered the original draft or used its conclusion");

  probe->principal = {"test.user"};
  probe->hidden_source = saved.source;
  Take(co_await runtime->RefreshAccessAsync());
  main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto partial = Take(co_await main->ReadUnappliedSteerDraftAsync(saved.steer));
  const auto stale = co_await main->SendAsync(kit::NewCommandId(), input);
  Check(partial.references.size() == 2 && partial.references.front() == draft.references.front() &&
        !partial.references.back().reference && partial.references.back().text == "Original conclusion is unavailable" &&
        !stale.Succeeded() && stale.Error().code == kit::ErrorCode::PermissionDenied && main->Snapshot()->runs.size() == 1,
        "a revoked conclusion disclosed text, removed the permitted quote, or started a run");

  probe->hidden_source.reset();
  Take(co_await runtime->RefreshAccessAsync());
  main = Take(co_await runtime->Sessions().OpenAsync(saved.parent));
  const auto command = kit::NewCommandId();
  const auto accepted = Take(co_await main->SendAsync(command, input));
  const auto replay = Take(co_await main->SendAsync(command, input));
  Check(accepted.stamp.commit == replay.stamp.commit, "retry duplicated the explicitly recovered input");
  co_await Eventually([&] { return !main->Snapshot()->summary.active_run; },
      "the explicitly submitted draft did not reach its new run");
  const auto done = main->Snapshot();
  Check(done->runs.size() == 2 && done->runs.front().steering.front().state == kit::SteerState::NotApplied &&
        done->runs.front().steering.front().message == original.message &&
        done->runs.back().state == kit::RunState::Succeeded &&
        std::ranges::any_of(done->runs.back().active_skills, [](const auto& entry) { return entry.skill == recovered_skill; }) &&
        probe->received.size() == 1 &&
        probe->received.front().sources == std::vector<kit::SourceSlice>{saved.source},
        "explicit recovery mutated the old supplement or lost its conclusion source");
  Take(co_await runtime->CloseAsync());
}


huxerui::Task<void> VerifyStore(huxerui::File file) {
  const huxerui::Bytes first{std::byte{0x00}, std::byte{0xff}};
  const huxerui::Bytes second{std::byte{0x42}};
  auto existing = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto empty = Take(co_await existing->LoadAsync());
  Check(empty.revision == 0 && empty.records.empty(), "new store must be empty");
  kit::StoreTransaction begin{{"create"}, 0, {
      {"session", "a", {}, first}, {"command", "a", {}, second}}};
  Check(Take(co_await existing->CommitAsync(begin)) == 1, "initial commit revision");
  std::ranges::reverse(begin.writes);
  Check(Take(co_await existing->CommitAsync(begin)) == 1, "reordered identical commit must replay");
  begin.writes.back().data = second;
  ExpectError(co_await existing->CommitAsync(begin), kit::ErrorCode::Conflict);

  auto other = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  kit::StoreTransaction stale{{"stale"}, 0, {{"session", "new", {}, first}}};
  ExpectError(co_await other->CommitAsync(stale), kit::ErrorCode::Conflict);
  kit::StoreTransaction bad_key{{"bad-base"}, 1, {
      {"session", "a", 1, second}, {"session", "b", 1, second}}};
  ExpectError(co_await other->CommitAsync(bad_key), kit::ErrorCode::Conflict);

  auto fault_db = co_await db::Database::OpenAsync(file);
  if (!fault_db) throw std::runtime_error(fault_db.Error().Message());
  auto trigger = co_await fault_db->ExecuteAsync(
      "CREATE TRIGGER fail_test_record BEFORE INSERT ON agentkit_store_records "
      "WHEN NEW.record_key = 'z' BEGIN SELECT RAISE(ABORT, 'injected write failure'); END");
  if (!trigger) throw std::runtime_error(trigger.Error().Message());
  kit::StoreTransaction rollback{{"rollback"}, 1, {
      {"session", "a", 1, second}, {"session", "z", {}, second}}};
  ExpectError(co_await existing->CommitAsync(rollback), kit::ErrorCode::PersistenceFailure);
  auto after_rollback = Take(co_await other->LoadAsync());
  Check(after_rollback.revision == 1 && after_rollback.records.size() == 2, "rollback exposed no partial rows");
  Check(after_rollback.records.back().collection == "session" && after_rollback.records.back().data == first,
        "rollback restored the first write's old data");
  Check(!Take(co_await other->FindCommitAsync({"rollback"})), "rollback did not publish a commit ID");

  kit::StoreTransaction deletion{{"deletion"}, 1, {{"session", "a", 1, std::nullopt}}};
  Check(Take(co_await other->CommitAsync(deletion)) == 2, "deletion revision");
  auto closed = co_await fault_db->CloseAsync();
  if (!closed) throw std::runtime_error(closed.Error().Message());
  existing.reset();
  other.reset();

  auto reopened = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto image = Take(co_await reopened->LoadAsync());
  Check(image.revision == 2 && image.records.size() == 1 && image.records.front().collection == "command" &&
        image.records.front().revision == 1 && image.records.front().data == second, "reopened image contains only committed data");
  Check(Take(co_await reopened->FindCommitAsync({"create"})) == 1 &&
        Take(co_await reopened->FindCommitAsync({"deletion"})) == 2, "commit lookup survived reopen");
  Check(Take(co_await reopened->CommitAsync(deletion)) == 2, "duplicate deletion survived reopen");
}

struct CollaborationFixture {
  kit::SessionId worker;
  kit::SessionCreated parent;
  kit::CollaborationMessageId request;
  kit::CollaborationMessageId steer;
  unsigned replies = 0;
};

kit::Async<kit::RunCompletion> WaitForReview(kit::RunContext run, kit::CollaborationMessageId request) {
  auto reviewed = Take(co_await run.Collaboration().WaitForReplyAsync(request));
  Check(reviewed.end == kit::ReplyEnd::Received && reviewed.reply && reviewed.reply->summary == "Review existing",
        "reply did not return to the original waiting run");
  auto inputs = Take(co_await run.ReadPendingSteersAsync());
  Check(inputs.size() == 1 && inputs.front().input.text == "Focus on the confirmed version", "targeted input did not survive restart");
  kit::RunCheckpoint checkpoint{.stage = "review", .schema_version = 1,
      .state = kit::JsonValue{request.value}, .steer_through = inputs.front().message};
  Take(co_await run.SaveCheckpointAsync(std::move(checkpoint)));
  co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Reviewed"});
}

std::shared_ptr<kit::AgentRuntime> CollaborationRuntime(
    std::shared_ptr<kit::SessionStore> store, std::shared_ptr<CollaborationFixture> fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"collaboration.user"}); },
      .access = [](kit::PolicyRequest) -> kit::Async<kit::PolicyDecision> {
        co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::Allow, {}});
      },
      .receive = [](kit::ReceiveRequest incoming) -> kit::Async<kit::ReceiveDecision> {
        const auto* request = std::get_if<kit::SessionMessageRequest>(&incoming.payload);
        const auto* work = request ? std::get_if<kit::WorkRequest>(&request->body) : nullptr;
        const bool manual = work && work->input.text == "Review existing";
        co_return kit::AgentResult<kit::ReceiveDecision>::Success({
            manual ? kit::ReceiveAction::Hold : kit::ReceiveAction::Allow, !manual});
      },
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"collab.local"}, 1}, {"test", "local", "script", {}}, std::make_shared<CountingModel>()}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"reviewer"}, 1}, .display_name = "Reviewer", .model = {"collab.local"},
                     .capabilities = {kit::Capability::Reply}},
      .run = [fixture](kit::RunContext run, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        Check(input.trigger == kit::RunTrigger::Collaboration && input.work_request, "work start lost its request");
        kit::SessionReply reply{.status = kit::ReplyStatus::Succeeded, .summary = input.input.text};
        const auto command = kit::NewCommandId();
        auto first = Take(co_await run.Collaboration().ReplyAsync(command, *input.work_request, reply));
        auto repeat = Take(co_await run.Collaboration().ReplyAsync(command, *input.work_request, reply));
        Check(first.stamp.commit == repeat.stamp.commit, "reply replay generated a new commit");
        ++fixture->replies;
        co_return kit::AgentResult<kit::RunCompletion>::Success({});
      }}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"lead"}, 1}, .display_name = "Lead", .model = {"collab.local"},
                     .collaboration_targets = {{"reviewer"}, {"lead"}},
                     .capabilities = {kit::Capability::CreateSession, kit::Capability::SendMessage, kit::Capability::WaitForResult}},
      .run = [fixture](kit::RunContext run, kit::RunInput) -> kit::Async<kit::RunCompletion> {
        kit::CreateSessionRequest seed{
            .options = {.agent = {"reviewer"}},
            .initial = kit::WorkRequest{.input = {.text = "Review new"}, .request_start = true}};
        const auto command = kit::NewCommandId();
        auto created = Take(co_await run.Sessions().CreateAsync(command, seed));
        auto replay = Take(co_await run.Sessions().CreateAsync(command, seed));
        auto child = std::get<kit::SessionCreated>(created.outcome);
        Check(created.stamp.commit == replay.stamp.commit && child.initial_request && child.initial_run,
              "new session and its initial work were not committed atomically");
        for (int i = 0; i < 200 && fixture->replies == 0; ++i) co_await huxerui::Delay(2ms);
        Check(fixture->replies == 1, "the new conversation did not start its initial work");
        auto early = Take(co_await run.Collaboration().WaitForReplyAsync(*child.initial_request));
        Check(early.end == kit::ReplyEnd::Received && early.reply->summary == "Review new", "reply-before-wait was lost");

        auto self = Take(co_await run.Collaboration().SendMessageAsync(kit::NewCommandId(), {
            .to = fixture->parent.session, .body = kit::WorkRequest{.input = {.text = "Self review"}}}));
        auto cycle = co_await run.Collaboration().WaitForReplyAsync(std::get<kit::MessageAccepted>(self.outcome).message);
        Check(!cycle.Succeeded() && cycle.Error().code == kit::ErrorCode::WaitCycle, "self-queued work formed a wait cycle");

        kit::SessionMessageRequest request{
            .to = fixture->worker, .body = kit::WorkRequest{.input = {.text = "Review existing"}, .request_start = true}};
        const auto send = kit::NewCommandId();
        auto accepted = Take(co_await run.Collaboration().SendMessageAsync(send, request));
        auto again = Take(co_await run.Collaboration().SendMessageAsync(send, request));
        Check(accepted.stamp.commit == again.stamp.commit, "duplicate dispatch created a new intent");
        fixture->request = std::get<kit::MessageAccepted>(accepted.outcome).message;
        Take(co_await run.SaveCheckpointAsync({.stage = "review", .schema_version = 1,
                                               .state = kit::JsonValue{fixture->request.value}}));
        co_return co_await WaitForReview(std::move(run), fixture->request);
      },
      .restore = [](kit::RunContext run, kit::RunInput, kit::RunResume resume) -> kit::Async<kit::RunCompletion> {
        Check(resume.checkpoint && resume.checkpoint->stage == "review", "missing collaboration checkpoint");
        kit::CollaborationMessageId request{std::get<std::string>(resume.checkpoint->state.value)};
        co_return co_await WaitForReview(std::move(run), std::move(request));
      }}));
  return runtime;
}

huxerui::Task<void> DispatchReviews(huxerui::File file, std::shared_ptr<CollaborationFixture> fixture) {
  auto runtime = CollaborationRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto worker = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {.options = {.agent = {"reviewer"}}}));
  fixture->worker = std::get<kit::SessionCreated>(worker.outcome).session;
  auto parent = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {.options = {.agent = {"lead"}}}));
  fixture->parent = std::get<kit::SessionCreated>(parent.outcome);
  auto principal = Take(co_await runtime->Sessions().OpenAsync(fixture->parent.session));
  auto started = Take(co_await principal->SendAsync(kit::NewCommandId(), {.text = "Coordinate reviews"}));
  fixture->parent.initial_run = std::get<kit::RunAccepted>(started.outcome).run;
  for (int i = 0; i < 500 && principal->Snapshot()->summary.run_state == kit::RunState::Running; ++i) {
    co_await huxerui::Delay(2ms);
  }
  auto source = principal->Snapshot();
  Check(source->summary.run_state == kit::RunState::WaitingForReply && !fixture->request.value.empty(),
        "requesting run did not publish its reply wait");
  auto fork = Take(co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), {
      .from = {source->summary.id, source->messages.front().ref.message, source->through_sequence},
      .options = {.agent = {"reviewer"}}}));
  auto branch = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(fork.outcome).session));
  auto snapshot = branch->Snapshot();
  Check(snapshot->origin->fork_point && snapshot->messages.size() == 1 && snapshot->runs.empty() &&
        snapshot->interactions.empty() && snapshot->messages.front().ref == source->messages.front().ref,
        "fork did not preserve the sealed history prefix and independent run state");
  kit::SessionMessageRequest steer{.to = source->summary.id,
      .body = kit::SteerRequest{*source->summary.active_run, {.text = "Focus on the confirmed version"}}};
  auto sent = Take(co_await runtime->Collaboration().SendMessageAsync(kit::NewCommandId(), std::move(steer)));
  auto delivery = std::get<kit::MessageAccepted>(sent.outcome);
  Check(delivery.delivery == kit::DeliveryState::Queued && delivery.target_run == source->summary.active_run,
        "targeted input was applied before the run reached a boundary");
  fixture->steer = delivery.message;
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ResumeReviews(huxerui::File file, std::shared_ptr<CollaborationFixture> fixture) {
  auto runtime = CollaborationRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto parent = Take(co_await runtime->Sessions().OpenAsync(fixture->parent.session));
  auto queued = Take(co_await runtime->Collaboration().ReadMessageAsync(fixture->request));
  Check(queued.summary.delivery == kit::DeliveryState::Queued && !queued.summary.target_run, "held request started by itself");
  const auto command = kit::NewCommandId();
  auto accepted = Take(co_await runtime->Collaboration().StartRequestAsync(command, fixture->request));
  auto retry = Take(co_await runtime->Collaboration().StartRequestAsync(command, fixture->request));
  Check(accepted.stamp.commit == retry.stamp.commit && std::get<kit::MessageAccepted>(accepted.outcome).target_run,
        "request started more than once");
  for (int i = 0; i < 500 && parent->Snapshot()->summary.active_run; ++i) co_await huxerui::Delay(2ms);
  const auto output = parent->Snapshot();
  Check(output->runs.size() == 1 && output->runs.front().id == fixture->parent.initial_run &&
        output->runs.front().state == kit::RunState::Succeeded && fixture->replies == 2,
        "restored collaboration did not complete the original run");
  auto reply = Take(co_await runtime->Collaboration().WaitForReplyAsync(fixture->request));
  Check(reply.end == kit::ReplyEnd::Received && reply.reply->summary == "Review existing", "committed reply unavailable");
  auto steer = Take(co_await runtime->Collaboration().ReadMessageAsync(fixture->steer));
  Check(steer.summary.delivery == kit::DeliveryState::Delivered && steer.summary.target_run == fixture->parent.initial_run,
        "input application did not settle the original delivery");
  kit::SessionMessageRequest late{.to = fixture->parent.session,
      .body = kit::SteerRequest{*fixture->parent.initial_run, {.text = "Late message"}}};
  auto rejected = co_await runtime->Collaboration().SendMessageAsync(kit::NewCommandId(), std::move(late));
  Check(!rejected.Succeeded() && rejected.Error().code == kit::ErrorCode::AlreadyClosed,
        "late input was accepted into a different run");
  Take(co_await runtime->CloseAsync());
}

struct OperationFixture {
  kit::SessionId source;
  kit::CommandId command;
  kit::PendingApproval approval;
  bool pause_create = false;
  bool deny_send = false;
  bool confirm_start = false;
};

kit::Async<kit::RunCompletion> ApprovedCreate(kit::RunContext run, std::shared_ptr<OperationFixture> fixture) {
  const kit::CommandId command{"create-approved/" + run.Id().value};
  kit::CreateSessionRequest request{
      .options = {.agent = {"approval.worker"}},
      .initial = kit::WorkRequest{.input = {.text = "Check the report"}, .request_start = true}};
  auto created = Take(co_await run.Sessions().CreateAsync(command, request));
  Check(std::holds_alternative<kit::PendingApproval>(created.outcome), "creating an approved conversation did not replay its initial receipt");
  fixture->command = command;
  fixture->approval = std::get<kit::PendingApproval>(created.outcome);
  for (;;) {
    auto status = Take(co_await run.Commands().ReadAsync(command));
    if (status.state == kit::CommandState::Applied) {
      Check(status.result && std::get<kit::SessionCreated>(*status.result).initial_request, "missing approved conversation result");
      co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Created after approval"});
    }
    Check(status.state == kit::CommandState::AwaitingApproval || status.state == kit::CommandState::Scheduled,
          "approved creation failed");
    co_await huxerui::Delay(5ms);
  }
}

std::shared_ptr<kit::AgentRuntime> OperationRuntime(
    std::shared_ptr<kit::SessionStore> store, std::shared_ptr<OperationFixture> fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"approval.user"}); },
      .access = [fixture](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        auto access = kit::AccessDecision::Allow;
        if (request.capability == kit::Capability::CreateSession && request.actor.kind == kit::ActionParty::Kind::Agent) {
          if (fixture->pause_create) co_await huxerui::Delay(1h);
          access = kit::AccessDecision::RequireConfirmation;
        } else if (request.capability == kit::Capability::SendMessage && request.actor.kind == kit::ActionParty::Kind::User) {
          access = fixture->deny_send ? kit::AccessDecision::Deny : kit::AccessDecision::RequireConfirmation;
        } else if (request.capability == kit::Capability::StartSessionRun && fixture->confirm_start) {
          access = kit::AccessDecision::RequireConfirmation;
        }
        co_return kit::AgentResult<kit::PolicyDecision>::Success({access, "operation policy"});
      },
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"approval.local"}, 1}, {"test", "local", "script", {}}, std::make_shared<CountingModel>()}));
  Take(runtime->RegisterAgent({{.ref = {{"approval.worker"}, 1}, .display_name = "Worker", .model = {"approval.local"}}}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"approval.lead"}, 1}, .display_name = "Lead", .model = {"approval.local"},
                     .collaboration_targets = {{"approval.worker"}}, .capabilities = {kit::Capability::CreateSession}},
      .run = [fixture](kit::RunContext run, kit::RunInput) -> kit::Async<kit::RunCompletion> {
        Take(co_await run.SaveCheckpointAsync({.stage = "create", .schema_version = 1}));
        co_return co_await ApprovedCreate(std::move(run), fixture);
      },
      .restore = [fixture](kit::RunContext run, kit::RunInput, kit::RunResume) {
        return ApprovedCreate(std::move(run), fixture);
      }}));
  return runtime;
}

kit::Async<kit::CommandProgress> AwaitOperation(std::shared_ptr<kit::AgentRuntime> runtime, kit::CommandId id) {
  for (int i = 0; i < 300; ++i) {
    auto status = Take(co_await runtime->Commands().ReadAsync(id));
    if (status.state != kit::CommandState::Scheduled) {
      co_return kit::AgentResult<kit::CommandProgress>::Success(std::move(status));
    }
    co_await huxerui::Delay(5ms);
  }
  co_return kit::AgentResult<kit::CommandProgress>::Failure({.code = kit::ErrorCode::DeadlineExceeded, .message = "Operation did not settle"});
}

huxerui::Task<void> StageCreation(huxerui::File file, std::shared_ptr<OperationFixture> fixture) {
  auto runtime = OperationRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto source = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
      .options = {.agent = {"approval.lead"}}, .initial = kit::SendInput{.text = "Delegate a review"}}));
  fixture->source = std::get<kit::SessionCreated>(source.outcome).session;
  for (int i = 0; i < 300 && fixture->command.value.empty(); ++i) co_await huxerui::Delay(5ms);
  Check(!fixture->command.value.empty() && fixture->approval.session == fixture->source, "creation approval has no source");
  const auto status = Take(co_await runtime->Commands().ReadAsync(fixture->approval));
  Check(status.state == kit::CommandState::AwaitingApproval && !status.result, "staged creation published an object");
  auto page = Take(co_await runtime->Sessions().ListAsync({.agent = kit::AgentId{"approval.worker"}}, {}));
  Check(page.sessions.empty(), "pending creation made another conversation");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> CommitCreationDecision(huxerui::File file, std::shared_ptr<OperationFixture> fixture) {
  fixture->pause_create = true;
  auto runtime = OperationRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto source = Take(co_await runtime->Sessions().OpenAsync(fixture->source));
  auto again = Take(co_await runtime->Sessions().OpenAsync(fixture->source));
  const auto original = fixture->approval;
  const auto submit = kit::NewCommandId();
  const kit::InteractionReply choice{original.interaction, original.version, kit::ApprovalDecision::Approve};
  auto first = Take(co_await source->SubmitAsync(submit, choice));
  auto retry = Take(co_await again->SubmitAsync(submit, choice));
  Check(first.stamp.commit == retry.stamp.commit, "repeat approval submission was not idempotent");
  auto stale = co_await again->SubmitAsync(kit::NewCommandId(), choice);
  Check(!stale.Succeeded() && stale.Error().code == kit::ErrorCode::AlreadyClosed, "approval was accepted twice");
  auto status = Take(co_await runtime->Commands().ReadAsync(original));
  Check(status.state == kit::CommandState::Scheduled && !status.result, "approved command executed before the permission check");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> FinishApprovedOperations(huxerui::File file, std::shared_ptr<OperationFixture> fixture) {
  fixture->pause_create = false;
  auto store = std::make_shared<LostReceiptStore>(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), true);
  auto runtime = OperationRuntime(store, fixture);
  Take(co_await runtime->StartAsync());
  auto status = Take(co_await AwaitOperation(runtime, fixture->command));
  Check(status.state == kit::CommandState::Applied && status.result, "scheduled creation did not resume after restart");
  auto result = std::get<kit::SessionCreated>(*status.result);
  Check(result.initial_request && result.initial_run, "approved creation lost its initial request and run");
  auto children = Take(co_await runtime->Sessions().ListAsync({.agent = kit::AgentId{"approval.worker"}}, {}));
  Check(children.sessions.size() == 1 && children.sessions.front().id == result.session, "approved action produced duplicate conversations");
  Check(store->lookups == 1, "lost multi-session commit receipt was not recovered by command ID");
  auto target = Take(co_await runtime->Sessions().OpenAsync(result.session));
  auto source = Take(co_await runtime->Sessions().OpenAsync(fixture->source));
  bool source_committed = false;
  bool target_committed = false;
  const auto watches_commit = [&](bool& seen) {
    return [&, flag = &seen](kit::AgentResult<std::shared_ptr<const kit::SessionEventBatch>> update) {
      const auto batch = Take(std::move(update));
      *flag |= std::ranges::any_of(batch->events, [&](const auto& event) {
        return status.resolution && event.commit == status.resolution->commit && event.command == fixture->command;
      });
    };
  };
  auto source_events = Take(co_await source->ObserveEventsAsync(0, watches_commit(source_committed)));
  auto target_events = Take(co_await target->ObserveEventsAsync(0, watches_commit(target_committed)));
  co_await Eventually([&] { return source_committed && target_committed; }, "cross-session events did not share the resolved commit");
  source_events.Reset();
  target_events.Reset();
  auto initial_count = target->Snapshot()->inbox.size();
  for (const auto& [choice, revoke] : {
           std::pair{kit::ApprovalDecision::Deny, false}, std::pair{kit::ApprovalDecision::Approve, true},
           std::pair{kit::ApprovalDecision::Approve, false}}) {
    auto id = kit::NewCommandId();
    kit::SessionMessageRequest message{.to = result.session, .body = kit::Notice{.input = {.text = id.value}}};
    auto original = Take(co_await runtime->Collaboration().SendMessageAsync(id, message));
    auto approval = std::get<kit::PendingApproval>(original.outcome);
    Check(target->Snapshot()->inbox.size() == initial_count, "a notice arrived before approval");
    fixture->deny_send = revoke;
    auto decision = Take(co_await target->SubmitAsync(kit::NewCommandId(), {approval.interaction, approval.version, choice}));
    auto resolved = Take(co_await AwaitOperation(runtime, id));
    const auto expected = choice == kit::ApprovalDecision::Deny ? kit::CommandState::Rejected
        : revoke ? kit::CommandState::Failed : kit::CommandState::Applied;
    Check(resolved.state == expected, "collaboration approval resolved incorrectly");
    if (expected == kit::CommandState::Rejected) {
      Check(resolved.resolution && resolved.resolution->commit == decision.stamp.commit,
            "denial and terminal command state have different commit IDs");
    }
    auto repeated = Take(co_await runtime->Collaboration().SendMessageAsync(id, message));
    Check(repeated.stamp.commit == original.stamp.commit &&
          std::get<kit::PendingApproval>(repeated.outcome).interaction == approval.interaction,
          "command progress replaced the original receipt");
    if (expected == kit::CommandState::Applied) ++initial_count;
    Check(target->Snapshot()->inbox.size() == initial_count, "invalid approval changed the inbox");
    fixture->deny_send = false;
  }
  const auto dispatch = kit::NewCommandId();
  auto requested = Take(co_await runtime->Collaboration().SendMessageAsync(dispatch, {
      .to = result.session, .body = kit::WorkRequest{.input = {.text = "Second review"}}}));
  auto send_approval = std::get<kit::PendingApproval>(requested.outcome);
  Take(co_await target->SubmitAsync(kit::NewCommandId(),
      {send_approval.interaction, send_approval.version, kit::ApprovalDecision::Approve}));
  auto delivered = Take(co_await AwaitOperation(runtime, dispatch));
  const auto message = std::get<kit::MessageAccepted>(*delivered.result);
  Check(message.delivery == kit::DeliveryState::Queued && !message.target_run, "work began before receiving approval");
  for (int i = 0; i < 150 && target->Snapshot()->summary.active_run; ++i) co_await huxerui::Delay(5ms);
  fixture->confirm_start = true;
  const auto start = kit::NewCommandId();
  auto received = Take(co_await runtime->Collaboration().StartRequestAsync(start, message.message));
  auto start_approval = std::get<kit::PendingApproval>(received.outcome);
  auto duplicate = co_await runtime->Collaboration().StartRequestAsync(kit::NewCommandId(), message.message);
  Check(!duplicate.Succeeded() && duplicate.Error().code == kit::ErrorCode::Busy, "duplicate request start created another approval");
  Take(co_await target->SubmitAsync(kit::NewCommandId(),
      {start_approval.interaction, start_approval.version, kit::ApprovalDecision::Approve}));
  auto started = Take(co_await AwaitOperation(runtime, start));
  Check(started.state == kit::CommandState::Applied && started.result &&
        std::get<kit::MessageAccepted>(*started.result).target_run, "receiving approval did not start the queued request");
  Take(co_await runtime->CloseAsync());
}

struct ProjectFixture {
  kit::ProjectId project{"release-project"};
  kit::SessionId worker;
  kit::SessionId consumer;
  kit::SharedContextRef original;
  kit::SharedContextRef replacement;
  kit::CommandId published;
  kit::CommandId withdrawn;
  kit::CommitId publish_commit;
  kit::CommitId withdraw_commit;
  std::optional<kit::SharedContextRef> blocked_source;
  kit::PrincipalId principal{"project.user"};
  unsigned source_effects = 0;
  kit::TaskExecutionRef upstream;
  kit::TaskExecutionRef downstream;
  kit::ArtifactRef report;
  std::vector<std::string> model_context;
};

class ProjectModel final : public kit::ModelProvider {
public:
  explicit ProjectModel(std::shared_ptr<ProjectFixture> fixture) : fixture_(std::move(fixture)) {}

  kit::ModelCapabilities Capabilities() const override { return {.text_stream = true}; }

  kit::Async<std::unique_ptr<kit::ModelTurnStream>> StartTurnAsync(kit::ModelTurnRequest request) override {
    fixture_->model_context.clear();
    for (const auto& item : request.items) {
      const auto* content = std::get_if<kit::ModelContent>(&item);
      if (content && content->role == kit::ModelContent::Role::Context) fixture_->model_context.push_back(std::get<std::string>(content->body));
    }
    co_return kit::AgentResult<std::unique_ptr<kit::ModelTurnStream>>::Success(std::make_unique<OneTurn>());
  }
private:
  std::shared_ptr<ProjectFixture> fixture_;
};

std::shared_ptr<kit::AgentRuntime> ProjectRuntime(
    std::shared_ptr<kit::SessionStore> store, std::shared_ptr<ProjectFixture> fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [fixture] { return kit::AgentResult<kit::PrincipalId>::Success(fixture->principal); },
      .access = [fixture](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const bool blocked = fixture->blocked_source && request.capability == kit::Capability::ReadContext &&
            std::ranges::any_of(request.sources, [&](const auto& source) { return source.source == kit::SourceRef{*fixture->blocked_source}; });
        if (blocked) co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::Deny, {}});
        const bool confirm = request.capability == kit::Capability::ExecuteTool && request.tool &&
            request.tool->id.value == "project.use-baseline";
        co_return kit::AgentResult<kit::PolicyDecision>::Success({confirm ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow, {}});
      },
      .project = [fixture](kit::ProjectId id) -> kit::Async<kit::ProjectBinding> {
        if (id != fixture->project) co_return kit::AgentResult<kit::ProjectBinding>::Failure({.code = kit::ErrorCode::NotFound});
        co_return kit::AgentResult<kit::ProjectBinding>::Success({id});
      },
      .verify = [](kit::VerificationRequest request) -> kit::Async<kit::VerificationResult> {
        const bool accepted = request.evidence.values == kit::JsonValue{true};
        co_return kit::AgentResult<kit::VerificationResult>::Success({
            accepted ? kit::VerificationStatus::Passed : kit::VerificationStatus::MissingEvidence,
            accepted ? std::string{} : "The report is required", request.evidence.sources});
      },
      .artifact_store = {.preview = [](kit::ArtifactResolveRequest input) -> kit::Async<kit::ArtifactPreview> {
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = input.artifact,
            .structured = kit::JsonValue{std::string("Verified publication settings")}, .summary = "Verification report"});
      }},
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"project.local"}, 1}, {"test", "local", "script", {}}, std::make_shared<ProjectModel>(fixture)}));
  Take(runtime->RegisterTool({
      .id = {"project.report"}, .version = 1, .description = "Register a versioned verification report",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({});
      },
      .execute = [fixture](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        kit::ArtifactDraft report{.type = "verification-report", .name = "Report", .storage_handle = "report-v1",
            .dependencies = {{std::nullopt, {fixture->original}}}};
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
            .new_artifacts = {std::move(report)}});
      }}));
  Take(runtime->RegisterTool({
      .id = {"project.use-baseline"}, .version = 1, .description = "Apply a change based on the release notes",
      .input_schema = {kit::JsonValue{true}}, .effect = kit::ToolEffect::StageArtifact,
      .prepare = [fixture](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
        co_return kit::AgentResult<kit::PreparedChange>::Success({.preview = {.title = "Apply release notes"},
            .inputs = {{fixture->original}}});
      },
      .execute = [fixture](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
        ++fixture->source_effects;
        co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded});
      }}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"project.worker"}, 1}, .display_name = "Worker", .model = {"project.local"},
                     .tools = {{"project.report"}, {"project.use-baseline"}}, .capabilities = {kit::Capability::Reply}},
      .run = [](kit::RunContext run, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        if (input.input.text == "Use the release baseline") {
          kit::ToolRequest request{{"project.use-baseline"}, {}};
          auto tool = co_await run.CallToolAsync(std::move(request));
          if (!tool.Succeeded()) co_return kit::AgentResult<kit::RunCompletion>::Failure(tool.Error());
          co_return kit::AgentResult<kit::RunCompletion>::Success({});
        }
        Check(input.trigger == kit::RunTrigger::ProjectTask && input.task, "project run did not carry its attempt");
        if (input.input.text == "Produce report") {
          auto tool = Take(co_await run.CallToolAsync({{"project.report"}, {}}));
          co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Produced", .artifacts = tool.published_artifacts});
        }
        auto reply = Take(co_await run.CompleteAsync({.instruction = "Use the authorized project inputs"}));
        if (input.work_request) {
          Take(co_await run.Collaboration().ReplyAsync(kit::NewCommandId(), *input.work_request,
              {.status = kit::ReplyStatus::Succeeded, .summary = reply.text}));
        }
        co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = reply.text});
      }}));
  return runtime;
}

kit::Async<kit::ProjectTaskSnapshot> ReadTask(kit::ProjectService project, kit::ProjectId id, kit::ProjectTaskId task) {
  auto board = Take(co_await project.ReadAsync(std::move(id)));
  auto item = std::ranges::find(board.tasks, task, &kit::ProjectTaskSnapshot::id);
  Check(item != board.tasks.end(), "project read did not include the task");
  co_return kit::AgentResult<kit::ProjectTaskSnapshot>::Success(*item);
}

kit::Async<kit::TaskUpdated> UpdateTask(kit::ProjectService project, kit::ProjectId id, kit::TaskCommand command) {
  auto accepted = Take(co_await project.UpdateTaskAsync(std::move(id), kit::NewCommandId(), std::move(command)));
  co_return kit::AgentResult<kit::TaskUpdated>::Success(std::get<kit::TaskUpdated>(accepted.outcome));
}

kit::Async<kit::SharedContextRef> UpdateContext(kit::ProjectService project, kit::ProjectId id, kit::SharedContextCommand command) {
  auto accepted = Take(co_await project.UpdateContextAsync(std::move(id), kit::NewCommandId(), std::move(command)));
  co_return kit::AgentResult<kit::SharedContextRef>::Success(std::get<kit::SharedContextRef>(accepted.outcome));
}

kit::Async<kit::InteractionSnapshot> AwaitProjectSourceApproval(std::shared_ptr<kit::Conversation> consumer) {
  Take(co_await consumer->SendAsync(kit::NewCommandId(), {.text = "Use the release baseline"}));
  co_await Eventually([&] { return consumer->Snapshot()->summary.pending_interactions == 1; }, "shared source did not await approval");
  co_return kit::AgentResult<kit::InteractionSnapshot>::Success(consumer->Snapshot()->interactions.back());
}

huxerui::Task<void> CheckProjectSourceRetired(std::shared_ptr<kit::Conversation> consumer, kit::InteractionSnapshot approval,
                                              const ProjectFixture& fixture) {
  co_await Eventually([&] { return !consumer->Snapshot()->summary.active_run; }, "the stale shared source plan did not finish");
  const auto snapshot = consumer->Snapshot();
  Check(snapshot->interactions.back().state == kit::InteractionState::Invalidated && fixture.source_effects == 0 &&
        snapshot->tools.back().state == kit::ToolCallState::Conflict, "the stale shared source plan survived the change");
  auto decision = co_await consumer->SubmitAsync(kit::NewCommandId(), {approval.id, approval.version, kit::ApprovalDecision::Approve});
  Check(!decision.Succeeded(), "a stale shared source approval was accepted");
}

huxerui::Task<void> SetUpProject(huxerui::File file, std::shared_ptr<ProjectFixture> fixture) {
  auto runtime = ProjectRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto api = runtime->Projects();
  const auto id = fixture->project;
  auto session = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
      .options = {.agent = {"project.worker"}, .project = id}}));
  fixture->worker = std::get<kit::SessionCreated>(session.outcome).session;
  auto worker = Take(co_await runtime->Sessions().OpenAsync(fixture->worker));
  auto initial = Take(co_await api.ReadAsync(id));
  fixture->original = Take(co_await UpdateContext(api, id, kit::ProposeSharedContext{initial.revision,
      {.title = "Release notes", .content = kit::JsonValue{std::string("approved release baseline")}}}));
  Take(co_await UpdateContext(api, id, kit::ChangeSharedContext{fixture->original, kit::RequestContextReview{}}));
  const auto first_publish = kit::NewCommandId();
  kit::SharedContextCommand publish_original = kit::ChangeSharedContext{fixture->original, kit::PublishContext{}};
  Take(co_await api.UpdateContextAsync(id, first_publish, std::move(publish_original)));
  kit::ArtifactImpactQuery first_impact{.project = id, .change_command = first_publish};
  auto unknown = co_await runtime->Artifacts().InspectImpactAsync(std::move(first_impact));
  Check(!unknown.Succeeded() && unknown.Error().code == kit::ErrorCode::NotFound, "first publication had a source to replace");
  fixture->replacement = Take(co_await UpdateContext(api, id, kit::ChangeSharedContext{fixture->original,
      kit::EditSharedContext{{.title = "Release notes", .content = kit::JsonValue{std::string("later revision")}}}}));
  Check(fixture->original.item == fixture->replacement.item && fixture->replacement.version == 2, "edit did not create a new version");

  auto board = Take(co_await api.ReadAsync(id));
  auto first = Take(co_await UpdateTask(api, id, kit::CreateTask{board.revision,
      {.title = "Verify report", .input = {.text = "Produce report"}}}));
  board = Take(co_await api.ReadAsync(id));
  kit::CreateTask follow_up{board.revision,
      {.title = "Prepare release", .input = {.text = "Prepare release", .context = {{fixture->original}}},
       .dependencies = {{first.task, {"verification-report"}}, {first.task, {"verification-report"}}}}};
  auto next = Take(co_await UpdateTask(api, id, std::move(follow_up)));
  auto blocked = Take(co_await ReadTask(api, id, next.task));
  Check(blocked.state == kit::ProjectTaskState::Blocked && blocked.spec.dependencies.size() == 1, "dependency was not blocked or normalized");
  auto cycle = Take(co_await ReadTask(api, id, first.task)).spec;
  cycle.dependencies.push_back({next.task});
  board = Take(co_await api.ReadAsync(id));
  auto rejected = co_await api.UpdateTaskAsync(id, kit::NewCommandId(),
      kit::ChangeTask{first.task, first.revision, kit::EditTask{std::move(cycle), board.revision}});
  Check(!rejected.Succeeded() && rejected.Error().code == kit::ErrorCode::Conflict, "task cycle was accepted");

  auto assigned = Take(co_await UpdateTask(api, id, kit::ChangeTask{first.task, first.revision, kit::AssignTask{fixture->worker}}));
  auto claim = Take(co_await UpdateTask(api, id, kit::ChangeTask{first.task, assigned.revision, kit::ClaimTask{fixture->worker}}));
  auto extra_claim = co_await api.UpdateTaskAsync(id, kit::NewCommandId(),
      kit::ChangeTask{first.task, assigned.revision, kit::ClaimTask{fixture->worker}});
  Check(!extra_claim.Succeeded() && extra_claim.Error().code == kit::ErrorCode::Conflict, "two claims got the same task");
  fixture->upstream = {id, first.task, *claim.attempt};
  auto start_id = kit::NewCommandId();
  kit::TaskCommand start = kit::ChangeTask{first.task, claim.revision, kit::StartTask{*claim.attempt}};
  auto started = Take(co_await api.UpdateTaskAsync(id, start_id, start));
  auto retry = Take(co_await api.UpdateTaskAsync(id, start_id, start));
  Check(started.stamp.commit == retry.stamp.commit && Take(co_await runtime->Commands().ReadAsync(start_id)).result,
        "task command receipt was not idempotent or readable");
  for (int i = 0; i < 300 && worker->Snapshot()->summary.active_run; ++i) co_await huxerui::Delay(5ms);
  auto snapshot = worker->Snapshot();
  Check(snapshot->runs.size() == 1 && snapshot->runs.back().result && snapshot->artifacts.size() == 1, "task run did not publish its report");
  fixture->report = snapshot->artifacts.front();
  auto produced = Take(co_await ReadTask(api, id, first.task));
  Check(produced.state == kit::ProjectTaskState::Running, "run success bypassed required task verification");
  auto unverified = co_await api.UpdateTaskAsync(id, kit::NewCommandId(),
      kit::ChangeTask{first.task, produced.revision, kit::CompleteTask{*claim.attempt, {}, {fixture->report}}});
  Check(!unverified.Succeeded() && unverified.Error().code == kit::ErrorCode::InvalidState, "verification failure completed the task");
  Take(co_await UpdateTask(api, id, kit::ChangeTask{first.task, produced.revision,
      kit::CompleteTask{*claim.attempt, {kit::JsonValue{true}, {{fixture->report}}}, {fixture->report}}}));
  auto ready = Take(co_await ReadTask(api, id, next.task));
  Check(ready.state == kit::ProjectTaskState::Ready, "upstream artifact did not unblock the task");
  auto claimed = Take(co_await UpdateTask(api, id, kit::ChangeTask{next.task, ready.revision, kit::ClaimTask{fixture->worker}}));
  fixture->downstream = {id, next.task, *claimed.attempt};
  kit::CreateSessionRequest source_session{.options = {.agent = {"project.worker"}, .project = id}};
  auto source_created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(source_session)));
  fixture->consumer = std::get<kit::SessionCreated>(source_created.outcome).session;
  auto consumer = Take(co_await runtime->Sessions().OpenAsync(fixture->consumer));
  auto approval = Take(co_await AwaitProjectSourceApproval(consumer));
  Take(co_await UpdateContext(api, id, kit::ChangeSharedContext{fixture->replacement, kit::RequestContextReview{}}));
  Check(consumer->Snapshot()->interactions.back().state == kit::InteractionState::Pending,
        "a pending shared context revision invalidated an approval before publication");
  fixture->published = kit::NewCommandId();
  kit::SharedContextCommand publish = kit::ChangeSharedContext{fixture->replacement, kit::PublishContext{}};
  fixture->publish_commit = Take(co_await api.UpdateContextAsync(id, fixture->published, publish)).stamp.commit;
  Check(Take(co_await api.UpdateContextAsync(id, fixture->published, publish)).stamp.commit == fixture->publish_commit,
        "shared context publication was not idempotent");
  co_await CheckProjectSourceRetired(consumer, approval, *fixture);
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ReopenProject(huxerui::File file, std::shared_ptr<ProjectFixture> fixture) {
  auto runtime = ProjectRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto api = runtime->Projects();
  const auto id = fixture->project;
  kit::ArtifactImpactQuery publication{.project = id, .change_command = fixture->published};
  const auto impact = Take(co_await runtime->Artifacts().InspectImpactAsync(publication));
  Check(impact.recorded_change == kit::SourceChange{{fixture->original}, kit::SourceRef{fixture->replacement}, kit::SourceChangeKind::Modified} &&
        impact.affected.size() == 1 && impact.affected.front().affected == fixture->report,
        "the restored publication did not locate the report made from the original source");
  auto command = Take(co_await runtime->Commands().ReadAsync(fixture->published));
  Check(command.accepted.commit == fixture->publish_commit && command.result &&
        std::get<kit::SharedContextRef>(*command.result) == fixture->replacement,
        "impact publication did not preserve its original project receipt");
  fixture->blocked_source = fixture->replacement;
  auto hidden = co_await runtime->Artifacts().InspectImpactAsync(publication);
  Check(!hidden.Succeeded() && hidden.Error().code == kit::ErrorCode::PermissionDenied, "publication exposed a forbidden replacement");
  fixture->blocked_source.reset();
  auto outcome = Take(co_await api.WaitForTaskAsync(fixture->upstream));
  Check(outcome.state == kit::ProjectTaskState::Succeeded && outcome.artifacts == std::vector{fixture->report},
        "verified attempt or artifact reference was lost after restart");
  auto next = Take(co_await ReadTask(api, id, fixture->downstream.task));
  Check(next.attempt == fixture->downstream.attempt && next.resolved_inputs == std::vector<kit::SourceSlice>{
        {fixture->original}, {fixture->report}}, "claimed input versions changed after restart");
  auto original = Take(co_await api.ReadContextAsync(fixture->original));
  Check(original.data.content == kit::JsonValue{std::string("approved release baseline")}, "published data was replaced in place");
  auto dispatched = Take(co_await runtime->Collaboration().SendMessageAsync(kit::NewCommandId(), {
      .to = fixture->worker, .body = kit::WorkRequest{.input = next.spec.input, .task = fixture->downstream}}));
  const auto message = std::get<kit::MessageAccepted>(dispatched.outcome).message;
  auto started = Take(co_await runtime->Collaboration().StartRequestAsync(kit::NewCommandId(), message));
  Check(std::get<kit::MessageAccepted>(started.outcome).target_run.has_value(), "collaboration did not start the claimed task");
  auto worker = Take(co_await runtime->Sessions().OpenAsync(fixture->worker));
  for (int i = 0; i < 300 && worker->Snapshot()->summary.active_run; ++i) co_await huxerui::Delay(5ms);
  Check(fixture->model_context.size() == 2 && fixture->model_context.front().find("approved release baseline") != std::string::npos &&
        fixture->model_context.back().find(fixture->report.id.value) != std::string::npos &&
        fixture->model_context.back().find("Verified publication settings") != std::string::npos,
        "the model did not receive the content of the claimed input versions");
  auto response = Take(co_await runtime->Collaboration().WaitForReplyAsync(message));
  Check(response.end == kit::ReplyEnd::Received, "task-linked collaboration reply was lost");
  auto running = Take(co_await ReadTask(api, id, next.id));
  Check(running.attempt == next.attempt && running.run == std::get<kit::MessageAccepted>(started.outcome).target_run,
        "task attempt and collaboration used separate runs");
  Take(co_await UpdateTask(api, id, kit::ChangeTask{next.id, running.revision, kit::CompleteTask{*next.attempt, {kit::JsonValue{true}}, {}}}));

  auto board = Take(co_await api.ReadAsync(id));
  auto future = Take(co_await UpdateTask(api, id, kit::CreateTask{board.revision,
      {.title = "Revocation check", .input = {.text = "Use release notes", .context = {{fixture->original}}}}}));
  auto claimed = Take(co_await UpdateTask(api, id, kit::ChangeTask{future.task, future.revision, kit::ClaimTask{fixture->worker}}));
  auto consumer = Take(co_await runtime->Sessions().OpenAsync(fixture->consumer));
  auto approval = Take(co_await AwaitProjectSourceApproval(consumer));
  fixture->withdrawn = kit::NewCommandId();
  kit::SharedContextCommand withdraw = kit::ChangeSharedContext{fixture->original, kit::WithdrawContext{}};
  fixture->withdraw_commit = Take(co_await api.UpdateContextAsync(id, fixture->withdrawn, std::move(withdraw))).stamp.commit;
  co_await CheckProjectSourceRetired(consumer, approval, *fixture);
  auto stale = co_await api.UpdateTaskAsync(id, kit::NewCommandId(),
      kit::ChangeTask{future.task, claimed.revision, kit::StartTask{*claimed.attempt}});
  Check(!stale.Succeeded() && stale.Error().code == kit::ErrorCode::NotFound, "withdrawn input started a task");
  Check(worker->Snapshot()->runs.size() == 2, "withdrawn data reached an additional run");
  Check(Take(co_await api.ReadContextAsync(fixture->replacement)).state == kit::SharedContextState::Published,
        "withdrawal affected a different published version");
  auto missing = fixture->report;
  ++missing.version;
  auto denied = co_await runtime->Collaboration().SendMessageAsync(kit::NewCommandId(), {
      .to = fixture->worker, .body = kit::WorkRequest{.input = {.text = "Review this version", .context = {{missing}}}}});
  Check(!denied.Succeeded() && denied.Error().code == kit::ErrorCode::NotFound &&
        worker->Snapshot()->runs.size() == 2, "an unavailable artifact version was accepted as work");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> RecoverProjectWithdrawal(huxerui::File file, std::shared_ptr<ProjectFixture> fixture) {
  auto runtime = ProjectRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto api = runtime->Projects();
  const auto& id = fixture->project;
  kit::SharedContextCommand withdraw = kit::ChangeSharedContext{fixture->original, kit::WithdrawContext{}};
  const auto replay = Take(co_await api.UpdateContextAsync(id, fixture->withdrawn, withdraw));
  Check(replay.stamp.commit == fixture->withdraw_commit && std::get<kit::SharedContextRef>(replay.outcome) == fixture->original,
        "withdrawal did not replay its original receipt after restart");
  kit::ArtifactImpactQuery withdrawal{.project = id, .change_command = fixture->withdrawn};
  auto impact = Take(co_await runtime->Artifacts().InspectImpactAsync(withdrawal));
  Check(impact.recorded_change == kit::SourceChange{{fixture->original}, {}, kit::SourceChangeKind::Withdrawn} &&
        impact.affected.size() == 1 && impact.affected.front().affected == fixture->report,
        "the restored withdrawal did not locate the affected report");
  auto missing = co_await api.ReadContextAsync(fixture->original);
  Check(!missing.Succeeded() && missing.Error().code == kit::ErrorCode::NotFound, "the impact audit made the withdrawn content readable");
  kit::ArtifactImpactQuery ad_hoc{.project = id, .changes = {*impact.recorded_change}};
  auto unrecorded = co_await runtime->Artifacts().InspectImpactAsync(std::move(ad_hoc));
  Check(!unrecorded.Succeeded() && unrecorded.Error().code == kit::ErrorCode::NotFound, "an ad hoc query disclosed a withdrawn source");
  fixture->blocked_source = fixture->original;
  auto denied = co_await runtime->Artifacts().InspectImpactAsync(withdrawal);
  Check(!denied.Succeeded() && denied.Error().code == kit::ErrorCode::PermissionDenied, "withdrawal metadata bypassed the source ACL");
  fixture->blocked_source.reset();
  fixture->principal = {"project.other"};
  auto foreign = co_await runtime->Artifacts().InspectImpactAsync(withdrawal);
  Check(!foreign.Succeeded() && foreign.Error().code == kit::ErrorCode::NotFound, "another principal read a private withdrawal command");
  fixture->principal = {"project.user"};
  Take(co_await runtime->CloseAsync());
}

struct ForkReviewFixture {
  kit::ProjectId project{"alternatives"};
  kit::ForkReviewRequest request;
  kit::CommandId create;
  kit::CommandId close;
  kit::ForkReviewId review;
  kit::Revision close_revision{};
  std::map<kit::SessionId, kit::RunId> branches;
  kit::SessionId waiting;
  kit::InteractionSnapshot form;
  std::optional<kit::SessionId> hidden;
  unsigned starts = 0;
  unsigned restores = 0;
  unsigned staged = 0;
  unsigned writes = 0;
  std::optional<huxerui::File> adoption_target;
  kit::AdoptCandidateRequest adoption;
  kit::CommandId adoption_command;
  kit::InteractionSnapshot adoption_approval;
  kit::ToolCallId adoption_call;
  unsigned applied = 0;
  bool confirm_comparison = false;
  bool confirm_forks = false;
  bool deny_project_comparison = false;
  unsigned allocations = 0;
  kit::PendingApproval comparison_approval;
  kit::PendingApproval denied_comparison;
  kit::PendingApproval changed_comparison;
  kit::CommandId deny_command;
  kit::CommandId changed_command;
};

const kit::ResourceId adoption_resource{"comparison.main"};

std::string AdoptionContents(const ForkReviewFixture& fixture) {
  auto text = fixture.adoption_target->ReadString();
  Check(text.Succeeded(), "the adoption target was not readable");
  return std::move(text).Value();
}

kit::Async<kit::RunCompletion> RunForkReviewCandidate(kit::RunContext context, kit::RunInput input,
                                                     std::shared_ptr<ForkReviewFixture> fixture, bool resume) {
  using Result = kit::AgentResult<kit::RunCompletion>;
  if (input.trigger != kit::RunTrigger::ForkComparison) co_return Result::Success({.summary = "Sealed comparison source"});
  Check(input.input.context == fixture->request.inputs, "candidate did not receive the common source versions");
  if (resume) ++fixture->restores;
  else ++fixture->starts;
  if (input.input.text == "Try writing the shared workspace") {
    kit::ToolRequest write{{"comparison.write"}, {}};
    const auto requested = Take(co_await context.CallToolAsync(std::move(write)));
    Check(requested.outcome == kit::ToolOutcome::Failed && requested.reason && requested.reason->code == kit::ErrorCode::PermissionDenied,
          "the shared candidate was permitted to write");
    co_return Result::Failure(*requested.reason);
  }
  if (!resume) Take(co_await context.SaveCheckpointAsync({.stage = "candidate-review", .schema_version = 1}));
  const kit::CommandId review{"compare-form/" + context.Id().value};
  kit::FormRequest request{.title = "Confirm this candidate", .schema = {kit::JsonValue{true}}};
  const auto sent = Take(co_await context.RequestInputAsync(review, std::move(request)));
  const auto submitted = Take(co_await context.WaitForInputAsync(std::get<kit::InputRequested>(sent.outcome).id));
  Check(submitted.state == kit::InteractionState::Submitted, "comparison approval was not submitted");
  kit::ToolRequest stage{{"comparison.stage"}, {}};
  auto staged = Take(co_await context.CallToolAsync(stage));
  Check(staged.outcome == kit::ToolOutcome::Succeeded && staged.published_artifacts.size() == 1, "candidate did not publish its artifact");
  auto extra = co_await context.CallToolAsync(std::move(stage));
  Check(!extra.Succeeded() && extra.Error().code == kit::ErrorCode::BudgetExceeded, "candidate consumed the other branch's budget share");
  co_return Result::Success({.summary = "Candidate staged", .structured = kit::JsonValue{true}, .artifacts = staged.published_artifacts});
}

std::shared_ptr<kit::AgentRuntime> ForkReviewRuntime(std::shared_ptr<kit::SessionStore> store, std::shared_ptr<ForkReviewFixture> fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({.tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"comparison.user"}); },
      .access = [fixture](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const bool hidden = fixture->hidden && request.target.session == fixture->hidden &&
            (request.capability == kit::Capability::DiscoverSession || request.capability == kit::Capability::ReadSession);
        const bool review = (fixture->confirm_comparison && request.capability == kit::Capability::CreateComparison) ||
            (fixture->confirm_forks && request.capability == kit::Capability::ForkSession);
        const bool denied = fixture->deny_project_comparison && request.capability == kit::Capability::CreateComparison &&
            request.operation == kit::JsonValue{};
        co_return kit::AgentResult<kit::PolicyDecision>::Success({hidden || denied ? kit::AccessDecision::Deny :
            review ? kit::AccessDecision::RequireConfirmation : kit::AccessDecision::Allow, {}});
      },
      .tool_resources = [fixture](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
        using Result = kit::AgentResult<std::vector<kit::ResourceAccess>>;
        if (invocation.tool.id.value != "agentkit/adopt-candidate") co_return Result::Success({});
        Check(fixture->adoption_target && invocation.prepared_targets.size() == 1 &&
              invocation.prepared_targets.front().target == adoption_resource, "the host received an unbound adoption target");
        const auto mode = invocation.phase == kit::ToolAccessPhase::Prepare ? kit::ResourceAccessMode::Read : kit::ResourceAccessMode::Write;
        co_return Result::Success({{.resource = adoption_resource, .current = kit::ResourceRef{adoption_resource, AdoptionContents(*fixture)},
            .mode = mode}});
      },
      .workspace = [fixture](kit::WorkspaceRequest request) -> kit::Async<std::optional<kit::WorkspaceId>> {
        Check(request.comparison_command == fixture->create && request.choice && request.choice->copy_plan &&
              request.fork_from == fixture->request.from, "the copy resolver did not receive the approved comparison binding");
        ++fixture->allocations;
        co_return kit::AgentResult<std::optional<kit::WorkspaceId>>::Success(kit::WorkspaceId{request.choice->copy_plan->id.value});
      },
      .artifact_store = {.preview = [](kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact, .summary = "Candidate"});
      }},
      .changes = {
          .prepare_adoption = [](kit::ChangePrepareContext context, kit::AdoptCandidateRequest request) -> kit::Async<kit::PreparedChange> {
            Check(context.artifact && context.artifact->metadata.ref == request.candidate_artifact &&
                  context.artifact->metadata.name == "Selected design" && context.access.readable.size() == 1,
                  "the host did not receive the selected version and read grant");
            co_return kit::AgentResult<kit::PreparedChange>::Success({
                .preview = {.title = "Apply selected candidate", .summary = "Replace the comparison target with candidate-board"},
                .targets = std::move(request.targets), .execution_data = kit::JsonValue{std::string("candidate-board")}});
          },
          .execute_adoption = [fixture](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
            using Result = kit::AgentResult<kit::ToolExecutionResult>;
            const auto& target = call.change.targets.front();
            const auto& content = std::get<std::string>(call.change.execution_data.value);
            Check(context.Resources().size() == 1 && context.Resources().front().mode == kit::ResourceAccessMode::Write &&
                  target.expected && AdoptionContents(*fixture) == target.expected->version,
                  "adoption executed with a stale or missing write grant");
            Check(fixture->adoption_target->WriteString(content), "the approved candidate could not be written");
            ++fixture->applied;
            co_return Result::Success({.outcome = kit::ToolOutcome::Succeeded,
                .new_artifacts = {{.type = "adoption.result", .name = "Applied candidate", .storage_handle = content}},
                .effects = {{.resource = target.target, .state = kit::ResourceEffectState::Applied, .before = target.expected,
                    .after = kit::ResourceRef{target.target, content}}}});
          }},
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"comparison.local"}, 1}, {"test", "comparison", "script", {}}, std::make_shared<CountingModel>()}));
  for (const bool write : {false, true}) {
    Take(runtime->RegisterTool({.id = {write ? "comparison.write" : "comparison.stage"}, .version = 1,
        .description = "Comparison test tool", .input_schema = {kit::JsonValue{true}},
        .effect = write ? kit::ToolEffect::WriteResource : kit::ToolEffect::StageArtifact,
        .prepare = [](kit::ToolPrepareContext, kit::ToolRequest) -> kit::Async<kit::PreparedChange> {
          co_return kit::AgentResult<kit::PreparedChange>::Success({});
        },
        .execute = [fixture, write](kit::ToolContext, kit::PreparedToolCall) -> kit::Async<kit::ToolExecutionResult> {
          if (write) ++fixture->writes;
          else ++fixture->staged;
          std::vector<kit::ArtifactDraft> drafts{{.type = "candidate.design", .name = "Selected design", .storage_handle = "candidate-board"}};
          co_return kit::AgentResult<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
              .new_artifacts = std::move(drafts)});
        }}));
  }
  Take(runtime->RegisterAgent({.definition = {.ref = {{"comparison.worker"}, 1}, .display_name = "Comparison worker",
      .model = {"comparison.local"}, .tools = {{"comparison.stage"}, {"comparison.write"}}},
      .run = [fixture](kit::RunContext run, kit::RunInput input) {
        return RunForkReviewCandidate(std::move(run), std::move(input), fixture, false);
      },
      .restore = [fixture](kit::RunContext run, kit::RunInput input, kit::RunResume state) {
        Check(state.checkpoint && state.checkpoint->stage == "candidate-review", "restart changed the candidate checkpoint");
        return RunForkReviewCandidate(std::move(run), std::move(input), fixture, true);
      }}));
  return runtime;
}

bool SameApproval(const kit::PendingApproval& left, const kit::PendingApproval& right) {
  return left.session == right.session && left.interaction == right.interaction && left.version == right.version;
}

huxerui::Task<void> StageComparisonApprovals(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture) {
  auto runtime = ForkReviewRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  const auto source = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
      .options = {.agent = {"comparison.worker"}, .project = fixture->project, .workspace = kit::WorkspaceId{"main"}},
      .initial = kit::SendInput{.text = "Source conversation"}})).outcome);
  auto chat = Take(co_await runtime->Sessions().OpenAsync(source.session));
  co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "comparison source did not seal");
  fixture->request = {.from = {source.session, *source.initial_run, chat->Snapshot()->through_sequence}, .project = fixture->project,
      .candidates = {
          {{.agent = {"comparison.worker"}, .title = "Isolated design"},
              {kit::WorkspaceChoice::Kind::CopyFromSource, {"main"}, kit::WorkspaceCopyPlanRef{{"approved-copy"}, 1}},
              {.text = "Stage the candidate"}},
          {{.agent = {"comparison.worker"}, .title = "Shared design"}, {}, {.text = "Try writing the shared workspace"}}},
      .comparison_criteria = kit::JsonValue{std::string("Review both approaches")}, .shared_budget = {.tool_calls = 2}};
  fixture->deny_project_comparison = true;
  const auto denied = co_await runtime->ForkReviews().CreateComparisonAsync(kit::NewCommandId(), fixture->request);
  Check(!denied.Succeeded() && denied.Error().code == kit::ErrorCode::PermissionDenied && fixture->allocations == 0,
        "a scoped comparison bypassed the project policy");
  fixture->deny_project_comparison = false;
  fixture->confirm_comparison = true;
  fixture->create = kit::NewCommandId();
  const auto receipt = Take(co_await runtime->ForkReviews().CreateComparisonAsync(fixture->create, fixture->request));
  fixture->comparison_approval = std::get<kit::PendingApproval>(receipt.outcome);
  const auto retry = Take(co_await runtime->ForkReviews().CreateComparisonAsync(fixture->create, fixture->request));
  Check(retry.stamp.commit == receipt.stamp.commit &&
        SameApproval(std::get<kit::PendingApproval>(retry.outcome), fixture->comparison_approval),
        "comparison approval was duplicated");
  auto different = fixture->request;
  different.candidates.front().task.text = "Different request";
  auto blocked = co_await runtime->ForkReviews().CreateComparisonAsync(fixture->create, std::move(different));
  Check(!blocked.Succeeded() && blocked.Error().code == kit::ErrorCode::Conflict, "changed comparison reused a reviewed command");
  fixture->confirm_comparison = false;
  fixture->confirm_forks = true;
  fixture->deny_command = kit::NewCommandId();
  fixture->denied_comparison = std::get<kit::PendingApproval>(Take(co_await runtime->ForkReviews().CreateComparisonAsync(
      fixture->deny_command, fixture->request)).outcome);
  fixture->confirm_forks = false;
  fixture->confirm_comparison = true;
  fixture->changed_command = kit::NewCommandId();
  fixture->changed_comparison = std::get<kit::PendingApproval>(Take(co_await runtime->ForkReviews().CreateComparisonAsync(
      fixture->changed_command, fixture->request)).outcome);
  const auto progress = Take(co_await runtime->Commands().ReadAsync(fixture->comparison_approval));
  const auto comparisons = Take(co_await runtime->ForkReviews().ListComparisonsAsync(source.session));
  const auto listed = Take(co_await runtime->Sessions().ListAsync({.project = fixture->project}, {}));
  Check(progress.state == kit::CommandState::AwaitingApproval && !progress.result && fixture->allocations == 0 && fixture->starts == 0 &&
        comparisons.empty() && listed.sessions.size() == 1, "pending comparison allocated checkouts or published branches");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> DecideComparisonApprovals(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture) {
  auto store = std::make_shared<LostReceiptStore>(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), true, false);
  auto runtime = ForkReviewRuntime(store, fixture);
  Take(co_await runtime->StartAsync());
  const auto source = Take(co_await runtime->Sessions().OpenAsync(fixture->request.from.session));
  const auto original = Take(co_await runtime->Commands().ReadAsync(fixture->comparison_approval));
  Check(original.state == kit::CommandState::AwaitingApproval && original.approval &&
        SameApproval(*original.approval, fixture->comparison_approval) &&
        source->Snapshot()->summary.pending_interactions == 3, "comparison approvals changed after restart");
  const auto denied = fixture->denied_comparison;
  const kit::InteractionReply denial{denied.interaction, denied.version, kit::ApprovalDecision::Deny};
  Take(co_await source->SubmitAsync(kit::NewCommandId(), denial));
  Check(Take(co_await runtime->Commands().ReadAsync(fixture->deny_command)).state == kit::CommandState::Rejected,
        "denied comparison did not resolve its original command");
  fixture->confirm_forks = true;
  const auto changed = fixture->changed_comparison;
  const kit::InteractionReply outdated{changed.interaction, changed.version, kit::ApprovalDecision::Approve};
  Take(co_await source->SubmitAsync(kit::NewCommandId(), outdated));
  const auto conflict = Take(co_await AwaitOperation(runtime, fixture->changed_command));
  Check(conflict.state == kit::CommandState::Failed && conflict.failure && conflict.failure->code == kit::ErrorCode::Conflict &&
        fixture->allocations == 0 && fixture->starts == 0, "approval authorized a newly required candidate operation");
  fixture->confirm_forks = false;
  const auto approved = fixture->comparison_approval;
  const auto decision = kit::NewCommandId();
  const kit::InteractionReply answer{approved.interaction, approved.version, kit::ApprovalDecision::Approve};
  const auto once = Take(co_await source->SubmitAsync(decision, answer));
  const auto retry = Take(co_await source->SubmitAsync(decision, answer));
  Check(once.stamp.commit == retry.stamp.commit, "duplicate decision did not replay its receipt");
  co_await Eventually([&] { return store->lookups == 1; }, "approved comparison did not commit as a group");
  Check(fixture->allocations == 1 && fixture->starts == 0, "a comparison started before its commit was confirmed");
  const auto closed = co_await runtime->CloseAsync();
  Check(!closed.Succeeded() && closed.Error().code == kit::ErrorCode::CommitUncertain, "lost comparison commit was not reported");
}

huxerui::Task<void> RecoverComparisonApproval(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture) {
  auto runtime = ForkReviewRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  const auto progress = Take(co_await runtime->Commands().ReadAsync(fixture->create));
  Check(progress.state == kit::CommandState::Applied && progress.approval &&
        SameApproval(*progress.approval, fixture->comparison_approval) &&
        progress.resolution && progress.resolution->project == fixture->project && progress.result,
        "approved comparison did not recover its original command and result");
  const auto outcome = std::get<kit::ForkReviewCreated>(*progress.result);
  const auto reviews = Take(co_await runtime->ForkReviews().ListComparisonsAsync(fixture->request.from.session));
  Check(outcome.branches.size() == 2 && reviews.size() == 1 && reviews.front().id == outcome.review && fixture->allocations == 1,
        "recovering an approved comparison duplicated its worktree or review");
  co_await Eventually([&] { return fixture->starts == 2; }, "the committed branches did not resume");
  const auto replay = Take(co_await runtime->ForkReviews().CreateComparisonAsync(fixture->create, fixture->request));
  Check(SameApproval(std::get<kit::PendingApproval>(replay.outcome), fixture->comparison_approval) && fixture->allocations == 1,
        "comparison replay created another branch");
  fixture->hidden = outcome.branches.back();
  Take(co_await runtime->RefreshAccessAsync());
  const auto filtered = Take(co_await runtime->Commands().ReadAsync(fixture->comparison_approval));
  Check(std::get<kit::ForkReviewCreated>(*filtered.result).branches == std::vector{outcome.branches.front()},
        "the approved command exposed a revoked branch");
  fixture->confirm_comparison = false;
  auto direct = fixture->request;
  direct.candidates.front().workspace = {};
  const auto newer = std::get<kit::ForkReviewCreated>(
      Take(co_await runtime->ForkReviews().CreateComparisonAsync(kit::NewCommandId(), direct)).outcome);
  const auto ordered = Take(co_await runtime->ForkReviews().ListComparisonsAsync(direct.from.session));
  Check(ordered.size() == 2 && ordered.front().id == newer.review && ordered.back().id == outcome.review,
        "the comparison list did not order direct and approved receipts by commit");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> StoreForkReview(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture) {
  auto storage = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto lost = std::make_shared<LostReceiptStore>(storage, true, false);
  auto runtime = ForkReviewRuntime(lost, fixture);
  Take(co_await runtime->StartAsync());
  const auto project = fixture->project;
  auto board = Take(co_await runtime->Projects().ReadAsync(project));
  const auto source_ref = Take(co_await UpdateContext(runtime->Projects(), project,
      kit::ProposeSharedContext{board.revision, {.title = "Common brief", .content = kit::JsonValue{true}}}));
  Take(co_await UpdateContext(runtime->Projects(), project, kit::ChangeSharedContext{source_ref, kit::RequestContextReview{}}));
  Take(co_await UpdateContext(runtime->Projects(), project, kit::ChangeSharedContext{source_ref, kit::PublishContext{}}));
  kit::CreateSessionRequest source_request{.options = {.agent = {"comparison.worker"}, .project = project},
      .initial = kit::SendInput{.text = "Source conversation"}};
  const auto source = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(
      kit::NewCommandId(), std::move(source_request))).outcome);
  auto chat = Take(co_await runtime->Sessions().OpenAsync(source.session));
  co_await Eventually([&] { return !chat->Snapshot()->summary.active_run; }, "comparison source did not seal");
  fixture->request = {.from = {source.session, *source.initial_run, chat->Snapshot()->through_sequence}, .project = project,
      .inputs = {{source_ref}}, .candidates = {{{.agent = {"comparison.worker"}}, {}, {.text = "Stage the candidate"}},
          {{.agent = {"comparison.worker"}}, {}, {.text = "Try writing the shared workspace"}}},
      .comparison_criteria = kit::JsonValue{std::string("Verify the same requirements")}, .shared_budget = {.tool_calls = 2}};
  auto invalid = fixture->request;
  invalid.candidates.back().session.agent = {"missing.agent"};
  auto rejected = co_await runtime->ForkReviews().CreateComparisonAsync(kit::NewCommandId(), std::move(invalid));
  auto list = Take(co_await runtime->Sessions().ListAsync({.project = project}, {}));
  Check(!rejected.Succeeded() && rejected.Error().code == kit::ErrorCode::NotFound && list.sessions.size() == 1 && fixture->starts == 0,
        "failed candidate preparation left a visible branch");
  fixture->create = kit::NewCommandId();
  auto accepted = co_await runtime->ForkReviews().CreateComparisonAsync(fixture->create, fixture->request);
  Check(!accepted.Succeeded() && accepted.Error().code == kit::ErrorCode::CommitUncertain && lost->lookups == 1 && fixture->starts == 0,
        "candidate execution started before the grouped comparison receipt was confirmed");
  auto saved = Take(co_await storage->LoadAsync());
  const auto project_row = std::ranges::find(saved.records, project.value, &kit::StoreValue::key);
  Check(project_row != saved.records.end(), "the comparison was not committed");
  for (const auto& row : saved.records) {
    if (row.collection != "agentkit.session" || row.revision != project_row->revision || row.key == source.session.value) continue;
    const auto* data = reinterpret_cast<const std::uint8_t*>(row.data.data());
    const auto document = nlohmann::json::from_cbor(data, data + row.data.size());
    const auto& branch = document.at("session").at("data").at("fork_branch");
    const kit::ForkReviewId review{branch.at("review").get<std::string>()};
    if (fixture->review.value.empty()) fixture->review = review;
    Check(review == fixture->review, "the grouped receipt allocated multiple comparison IDs");
    fixture->branches.emplace(kit::SessionId{row.key}, kit::RunId{branch.at("run").get<std::string>()});
  }
  Check(fixture->branches.size() == 2, "the lost receipt omitted a committed branch");
  const auto stopped = co_await runtime->CloseAsync();
  Check(!stopped.Succeeded() && stopped.Error().code == kit::ErrorCode::CommitUncertain, "the failed store should retain the uncertain receipt");
}

huxerui::Task<void> ResumeForkReview(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture, bool finish) {
  auto runtime = ForkReviewRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  const auto command = Take(co_await runtime->Commands().ReadAsync(fixture->create));
  Check(command.result && std::get<kit::ForkReviewCreated>(*command.result).review == fixture->review,
        "the comparison command lost its receipt on restart");
  auto comparisons = runtime->ForkReviews();
  std::shared_ptr<const kit::ForkReviewSnapshot> view;
  auto observer = Take(co_await comparisons.ObserveComparisonAsync(fixture->review, [&](auto update) {
    if (update.Succeeded()) view = std::move(update).Value();
  }));
  co_await Eventually([&] { return view && view->candidates.size() == 2 && view->candidates.back().state == kit::CandidateState::Failed; },
      "the failed candidate did not reach the comparison observer");
  for (const auto& branch : view->candidates) Check(branch.run == fixture->branches.at(branch.session), "restart created another candidate attempt");
  Check(view->candidates.back().reason && view->candidates.back().reason->code == kit::ErrorCode::PermissionDenied && fixture->writes == 0,
        "the comparison did not retain the failed write diagnostic");
  fixture->waiting = view->candidates.front().session;
  auto candidate = Take(co_await runtime->Sessions().OpenAsync(fixture->waiting));
  co_await Eventually([&] { return candidate->Snapshot()->summary.pending_interactions == 1; }, "the first candidate did not wait for review");
  const auto approval = candidate->Snapshot()->interactions.back();
  if (!finish) {
    fixture->form = approval;
    auto stale = co_await comparisons.CloseComparisonAsync(kit::NewCommandId(), fixture->review, 1);
    Check(!stale.Succeeded() && stale.Error().code == kit::ErrorCode::Conflict, "an old comparison revision closed the review");
    fixture->close = kit::NewCommandId();
    fixture->close_revision = view->revision;
    Take(co_await comparisons.CloseComparisonAsync(fixture->close, fixture->review, fixture->close_revision));
    Check(candidate->Snapshot()->summary.pending_interactions == 1 && fixture->starts == 2, "closing the review stopped a branch");
  } else {
    Check(view->state == kit::ForkReviewState::Closed && approval.id == fixture->form.id && approval.version == fixture->form.version,
          "the closed review or branch approval changed on restart");
    Take(co_await comparisons.CloseComparisonAsync(fixture->close, fixture->review, fixture->close_revision));
    Take(co_await candidate->SubmitAsync(kit::NewCommandId(), {approval.id, approval.version, kit::JsonValue{true}}));
    co_await Eventually([&] { return view && view->candidates.front().state == kit::CandidateState::Ready; },
        "the restored candidate did not finish");
    const auto& result = view->candidates.front();
    Check(view->state == kit::ForkReviewState::Closed && result.artifacts.size() == 1 && result.usage.tool_calls == 1 &&
          result.validation && result.validation->evidence && result.validation->conditions == fixture->request.comparison_criteria &&
          fixture->starts == 2 && fixture->restores == 1 && fixture->staged == 1,
          "comparison lost the restored candidate's artifacts, evidence or usage");
    fixture->hidden = view->candidates.back().session;
    const auto hidden = Take(co_await comparisons.ReadComparisonAsync(fixture->review));
    const auto replay = Take(co_await comparisons.CreateComparisonAsync(fixture->create, fixture->request));
    const auto projected = Take(co_await runtime->Commands().ReadAsync(fixture->create));
    Check(hidden.candidates.size() == 1 && hidden.request.candidates.size() == 1 && replay.stamp.commit == command.accepted.commit &&
          std::get<kit::ForkReviewCreated>(replay.outcome).branches == std::vector{fixture->waiting} && projected.result &&
          std::get<kit::ForkReviewCreated>(*projected.result).branches == std::vector{fixture->waiting},
          "the comparison or command receipt exposed an unauthorized branch");
    const auto listed = Take(co_await comparisons.ListComparisonsAsync(fixture->request.from.session));
    Check(listed.size() == 1 && listed.front().id == fixture->review && listed.front().state == kit::ForkReviewState::Closed,
          "the comparison directory did not preserve the original review across restart");
    fixture->hidden = fixture->request.from.session;
    auto denied = co_await comparisons.ListComparisonsAsync(fixture->request.from.session);
    Check(!denied.Succeeded() && denied.Error().code == kit::ErrorCode::PermissionDenied,
          "the comparison directory disclosed a hidden source conversation");
    fixture->hidden.reset();
  }
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> StoreCandidateAdoption(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture) {
  auto runtime = ForkReviewRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto comparisons = runtime->ForkReviews();
  kit::CreateSessionRequest source{.options = {.agent = {"comparison.worker"}, .project = fixture->project},
      .initial = kit::SendInput{.text = "Main design"}};
  auto home = std::get<kit::SessionCreated>(Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), std::move(source))).outcome);
  auto target = Take(co_await runtime->Sessions().OpenAsync(home.session));
  co_await Eventually([&] { return !target->Snapshot()->summary.active_run; }, "the adoption target did not settle");
  fixture->request = {.from = {home.session, *home.initial_run, target->Snapshot()->through_sequence}, .project = fixture->project,
      .candidates = {{{.agent = {"comparison.worker"}}, {}, {.text = "Stage the candidate"}},
          {{.agent = {"comparison.worker"}}, {}, {.text = "Try writing the shared workspace"}}}, .shared_budget = {.tool_calls = 2}};
  const auto created = std::get<kit::ForkReviewCreated>(
      Take(co_await comparisons.CreateComparisonAsync(kit::NewCommandId(), fixture->request)).outcome);
  auto branch = Take(co_await runtime->Sessions().OpenAsync(created.branches.front()));
  co_await Eventually([&] { return branch->Snapshot()->summary.pending_interactions == 1; }, "candidate review did not arrive");
  const auto form = branch->Snapshot()->interactions.back();
  Take(co_await branch->SubmitAsync(kit::NewCommandId(), {form.id, form.version, kit::JsonValue{true}}));
  std::shared_ptr<const kit::ForkReviewSnapshot> view;
  auto observe = Take(co_await comparisons.ObserveComparisonAsync(created.review, [&](auto next) {
    if (next.Succeeded()) view = std::move(next).Value();
  }));
  co_await Eventually([&] { return view && view->state == kit::ForkReviewState::Ready; }, "the comparison did not finish");
  Check(view->candidates.front().artifacts.size() == 1, "candidate did not stage exactly one version");
  auto& request = fixture->adoption;
  request = {created.review, view->revision, view->candidates.front().id, view->candidates.front().artifacts.front(),
      home.session, {}, {{adoption_resource, kit::ResourceRef{adoption_resource, "main-v1"}}}};
  auto old = request;
  --old.expected_review;
  auto invalid = co_await comparisons.PrepareAdoptionAsync(kit::NewCommandId(), std::move(old));
  Check(!invalid.Succeeded() && invalid.Error().code == kit::ErrorCode::Conflict, "a stale comparison revision prepared an adoption");
  const auto first = kit::NewCommandId();
  Take(co_await comparisons.PrepareAdoptionAsync(first, request));
  co_await Eventually([&] { return view->adoption_state == kit::ToolCallState::AwaitingApproval &&
      target->Snapshot()->summary.pending_interactions == 1; }, "adoption did not require independent approval");
  Check(fixture->applied == 0 && AdoptionContents(*fixture) == "main-v1", "adoption wrote before approval");
  auto competing = request;
  competing.expected_review = view->revision;
  auto busy = co_await comparisons.PrepareAdoptionAsync(kit::NewCommandId(), std::move(competing));
  Check(!busy.Succeeded() && busy.Error().code == kit::ErrorCode::Busy, "a second adoption replaced a pending plan");
  Check(fixture->adoption_target->WriteString("user-edit"), "could not update the adoption baseline");
  const auto stale = target->Snapshot()->interactions.back();
  Take(co_await target->SubmitAsync(kit::NewCommandId(), {stale.id, stale.version, kit::ApprovalDecision::Approve}));
  co_await Eventually([&] { return view->adoption_state == kit::ToolCallState::Conflict && !target->Snapshot()->summary.active_run; },
      "the changed baseline did not settle the approval as a conflict");
  Check(AdoptionContents(*fixture) == "user-edit" && fixture->applied == 0, "an old approval overwrote the user's edit");
  request.expected_review = view->revision;
  request.targets.front().expected->version = "user-edit";
  fixture->adoption_command = kit::NewCommandId();
  Take(co_await comparisons.PrepareAdoptionAsync(fixture->adoption_command, request));
  co_await Eventually([&] { return view->adoption_state == kit::ToolCallState::AwaitingApproval &&
      target->Snapshot()->summary.pending_interactions == 1; }, "the updated adoption plan did not become reviewable");
  fixture->adoption_call = *view->adoption_call;
  fixture->adoption_approval = target->Snapshot()->interactions.back();
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> RecoverCandidateAdoption(huxerui::File file, std::shared_ptr<ForkReviewFixture> fixture, bool approve) {
  auto runtime = ForkReviewRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto comparisons = runtime->ForkReviews();
  const auto& request = fixture->adoption;
  const auto call = fixture->adoption_call;
  auto target = Take(co_await runtime->Sessions().OpenAsync(request.target_session));
  auto command = Take(co_await runtime->Commands().ReadAsync(fixture->adoption_command));
  Check(command.result && std::get<kit::ChangePlanAccepted>(*command.result).call == call, "the saved adoption call changed on restart");
  const auto replay = Take(co_await comparisons.PrepareAdoptionAsync(fixture->adoption_command, request));
  Check(replay.stamp.commit == command.accepted.commit, "adoption replay changed the command or allocated a new call");
  if (approve) {
    const auto interaction = target->Snapshot()->interactions.back();
    Check(interaction.id == fixture->adoption_approval.id && interaction.version == fixture->adoption_approval.version &&
          std::get<kit::ToolApproval>(interaction.body).call == call && fixture->applied == 0,
          "the original adoption approval was not restored");
    Take(co_await target->SubmitAsync(kit::NewCommandId(), {interaction.id, interaction.version, kit::ApprovalDecision::Approve}));
    co_await Eventually([&] { return !target->Snapshot()->summary.active_run && fixture->applied == 1; }, "the approved adoption did not finish");
  }
  const auto review = Take(co_await comparisons.ReadComparisonAsync(request.review));
  const auto selected = std::ranges::find(review.candidates, request.candidate, &kit::ForkCandidate::id);
  Check(review.selected == request.candidate && review.adoption_call == call && review.adoption_state == kit::ToolCallState::Succeeded &&
        selected != review.candidates.end() && selected->artifacts.front() == request.candidate_artifact &&
        fixture->applied == 1 && AdoptionContents(*fixture) == "candidate-board",
        "the comparison did not retain the applied candidate and original version");
  const auto snapshot = target->Snapshot();
  Check(snapshot->artifacts.size() == 1, "the adoption did not publish one output version");
  const auto artifact = Take(co_await runtime->Artifacts().ReadAsync(snapshot->artifacts.front()));
  Check(artifact.dependencies.size() == 1 && !artifact.dependencies.front().output_part &&
        artifact.dependencies.front().input == kit::SourceSlice{request.candidate_artifact},
        "the applied artifact lost its exact candidate provenance");
  Take(co_await runtime->CloseAsync());
}

struct GoalFixture {
  kit::SessionId session;
  kit::GoalRef original;
  kit::GoalRef revised;
  kit::RunId revised_run;
  kit::InteractionId form;
  kit::CommandId evidence_command;
  kit::GoalCommand evidence;
  kit::PendingApproval completion;
  unsigned verifications = 0;
  unsigned restores = 0;
};

kit::Async<kit::RunCompletion> WaitGoalForm(kit::RunContext run, kit::GoalRef goal, kit::CommandId command) {
  const auto definition = Take(co_await run.Goals().ReadDefinitionAsync(goal));
  auto reply = Take(co_await run.RequestInputAsync(command, {
      .title = definition.objective, .schema = {kit::JsonValue{true}}}));
  auto outcome = Take(co_await run.WaitForInputAsync(std::get<kit::InputRequested>(reply.outcome).id));
  Check(outcome.state == kit::InteractionState::Submitted, "the requested goal form has no answer");
  co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = definition.objective});
}

std::shared_ptr<kit::AgentRuntime> RegisterGoalRuntime(std::shared_ptr<kit::SessionStore> store,
                                                     std::shared_ptr<GoalFixture> fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [](kit::PolicyRequest) -> kit::Async<kit::PolicyDecision> {
        co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::Allow, {}});
      },
      .verify = [fixture](kit::VerificationRequest request) -> kit::Async<kit::VerificationResult> {
        ++fixture->verifications;
        const bool matches = std::get<kit::GoalRef>(request.subject) == fixture->revised && request.session == fixture->session &&
            request.conditions == kit::JsonValue{std::int64_t(5)} && request.evidence.values == request.conditions;
        co_return kit::AgentResult<kit::VerificationResult>::Success({
            .status = matches ? kit::VerificationStatus::Passed : kit::VerificationStatus::Failed,
            .reason = matches ? "Five verified pages" : "The evidence did not match the released deck"});
      },
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"test.goal-model"}, 1}, {"test", "test", "script", {}}, std::make_shared<CountingModel>()}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"test.goal"}, 1}, .display_name = "Release", .model = {"test.goal-model"},
                     .capabilities = {kit::Capability::ReadGoal}},
      .run = [](kit::RunContext run, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        if (!input.goal) co_return kit::AgentResult<kit::RunCompletion>::Success({.summary = "Ordinary input handled"});
        Check(input.trigger == kit::RunTrigger::Goal, "the workflow was given a goal by an ordinary run");
        const auto command = kit::NewCommandId();
        Take(co_await run.SaveCheckpointAsync({.stage = "release goal", .schema_version = 1, .state = kit::JsonValue{command.value}}));
        co_return co_await WaitGoalForm(std::move(run), *input.goal, command);
      },
      .restore = [fixture](kit::RunContext run, kit::RunInput input, kit::RunResume resume) -> kit::Async<kit::RunCompletion> {
        ++fixture->restores;
        Check(input.run == fixture->revised_run && input.goal == fixture->revised && resume.checkpoint &&
              std::get<kit::InteractionId>(resume.waits.at(0).subject) == fixture->form,
              "restoration did not bind the current goal version and original form");
        co_return co_await WaitGoalForm(std::move(run), *input.goal,
            {std::get<std::string>(resume.checkpoint->state.value)});
      }}));
  return runtime;
}

huxerui::Task<kit::GoalSnapshot> WaitGoalState(kit::SessionGoals goals, kit::GoalState expected) {
  for (int i = 0; i < 200; ++i) {
    const auto view = Take(co_await goals.ReadAsync());
    if (view && view->state == expected) co_return *view;
    co_await huxerui::Delay(5ms);
  }
  throw std::runtime_error("The goal did not reach the expected state");
}

huxerui::Task<void> ReviseReleaseGoal(huxerui::File file, std::shared_ptr<GoalFixture> fixture) {
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto runtime = RegisterGoalRuntime(store, fixture);
  Take(co_await runtime->StartAsync());
  auto created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {.options = {.agent = {"test.goal"}}}));
  fixture->session = std::get<kit::SessionCreated>(created.outcome).session;
  auto session = Take(co_await runtime->Sessions().OpenAsync(fixture->session));
  auto goals = session->Goals();
  kit::GoalSpec release{.definition = {.objective = "Export 10 slides", .completion_conditions = kit::JsonValue{std::int64_t(10)},
      .completion = kit::GoalCompletionPolicy::RequireConfirmation}, .budget = {.model_turns = 4}};
  auto proposed = Take(co_await goals.UpdateAsync(kit::NewCommandId(), kit::ProposeGoal{release}));
  const auto original = std::get<kit::GoalUpdated>(proposed.outcome);
  fixture->original = original.goal;
  Take(co_await goals.UpdateAsync(kit::NewCommandId(), kit::ChangeGoal{original.goal.id, original.revision, kit::EnableGoal{}}));
  co_await Eventually([&] { return session->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
      "enabled goal did not schedule");
  auto active = *Take(co_await goals.ReadAsync());
  auto before = session->Snapshot();
  Check(before->runs.front().goal == fixture->original && before->interactions.front().state == kit::InteractionState::Pending,
        "the initial goal version was not bound to its form");
  Take(co_await session->QueueAsync(kit::NewCommandId(), {.text = "Handle the ordinary request first"}));
  auto definition = release.definition;
  definition.objective = "Export 5 slides";
  definition.completion_conditions = kit::JsonValue{std::int64_t(5)};
  auto preview = Take(co_await goals.PreviewEditAsync(original.goal.id, active.revision, definition));
  Check(preview.impact.retiring_run == before->summary.active_run &&
        preview.impact.invalidated_interactions == std::vector{before->interactions.front().id},
        "edit preview did not name the retiring run and pending form");
  kit::GoalCommand edit = kit::ChangeGoal{original.goal.id, active.revision, kit::EditGoal{definition, preview.token}};
  const auto edit_id = kit::NewCommandId();
  auto receipt = Take(co_await goals.UpdateAsync(edit_id, edit));
  fixture->revised = std::get<kit::GoalUpdated>(receipt.outcome).goal;
  Check(fixture->revised.id == original.goal.id && fixture->revised.version == 2, "editing changed the goal identity");
  auto duplicate = Take(co_await goals.UpdateAsync(edit_id, edit));
  Check(duplicate.stamp.commit == receipt.stamp.commit, "a repeated edit produced a new commit");
  auto stale = co_await goals.UpdateAsync(kit::NewCommandId(), edit);
  Check(!stale.Succeeded() && stale.Error().code == kit::ErrorCode::Conflict, "a stale edit was accepted");
  Check(Take(co_await goals.ReadDefinitionAsync(fixture->original)) == release.definition, "editing lost the historical definition");
  co_await Eventually([&] {
    auto value = session->Snapshot();
    return value->runs.size() == 3 && value->summary.run_state == kit::RunState::WaitingForInput;
  }, "ordinary work and the revised goal did not run");
  auto after = session->Snapshot();
  Check(after->runs[0].state == kit::RunState::Cancelled && after->runs[0].end_reason == kit::RunEndReason::GoalRevised &&
        after->interactions.front().state == kit::InteractionState::Cancelled, "the old run or form was still usable");
  Check(!after->runs[1].goal && after->runs[1].state == kit::RunState::Succeeded && after->runs[2].goal == fixture->revised,
        "the revised goal ran ahead of queued ordinary input");
  fixture->revised_run = after->runs[2].id;
  fixture->form = after->interactions.back().id;
  auto revised = *Take(co_await goals.ReadAsync());
  auto wrong_version = co_await goals.UpdateAsync(kit::NewCommandId(),
      kit::ChangeGoal{revised.ref.id, revised.revision, kit::SubmitGoalEvidence{fixture->original, {kit::JsonValue{std::int64_t(10)}}}});
  Check(!wrong_version.Succeeded() && wrong_version.Error().code == kit::ErrorCode::Conflict, "old evidence could complete the revised goal");
  fixture->evidence_command = kit::NewCommandId();
  fixture->evidence = kit::ChangeGoal{revised.ref.id, revised.revision,
      kit::SubmitGoalEvidence{fixture->revised, {kit::JsonValue{std::int64_t(5)}}}};
  auto accepted = Take(co_await goals.UpdateAsync(fixture->evidence_command, fixture->evidence));
  Check(std::holds_alternative<kit::GoalUpdated>(accepted.outcome), "evidence registration did not return a typed receipt");
  auto review = co_await WaitGoalState(goals, kit::GoalState::AwaitingCompletionApproval);
  Check(review.completion_review && review.completion_review->definition == fixture->revised &&
        review.completion_review->verification_revision == review.revision, "the review is not pinned to verification");
  fixture->completion = review.completion_review->approval;
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ReviewRecoveredGoal(huxerui::File file, std::shared_ptr<GoalFixture> fixture) {
  auto runtime = RegisterGoalRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto session = Take(co_await runtime->Sessions().OpenAsync(fixture->session));
  auto goals = session->Goals();
  auto view = co_await WaitGoalState(goals, kit::GoalState::AwaitingCompletionApproval);
  Check(view.ref == fixture->revised && view.completion_review &&
        view.completion_review->approval.interaction == fixture->completion.interaction &&
        fixture->verifications == 1, "restart changed the approved payload or repeated verification");
  auto original = Take(co_await goals.UpdateAsync(fixture->evidence_command, fixture->evidence));
  Check(std::holds_alternative<kit::GoalUpdated>(original.outcome), "a repeated submit returned a later review receipt");
  auto pending = Take(co_await runtime->Commands().ReadAsync(fixture->completion));
  Check(pending.state == kit::CommandState::AwaitingApproval && pending.accepted.command == fixture->evidence_command,
        "the pending approval did not resolve to the original evidence command");
  auto decision = kit::InteractionReply{fixture->completion.interaction, fixture->completion.version, kit::ApprovalDecision::Approve};
  const auto decision_id = kit::NewCommandId();
  const auto accepted = Take(co_await session->SubmitAsync(decision_id, decision));
  auto closed = co_await WaitGoalState(goals, kit::GoalState::Completed);
  const auto evidence = Take(co_await runtime->Commands().ReadAsync(fixture->evidence_command));
  Check(closed.ref == fixture->revised && closed.verification && fixture->verifications == 1 &&
        evidence.state == kit::CommandState::Applied && evidence.resolution &&
        evidence.resolution->commit == accepted.stamp.commit, "the approval and evidence result did not commit together");
  Check(Take(co_await session->SubmitAsync(decision_id, decision)).stamp.commit == accepted.stamp.commit,
        "the approval command did not replay its original receipt");
  auto second = co_await session->SubmitAsync(kit::NewCommandId(), decision);
  Check(!second.Succeeded(), "a second approval was accepted");
  co_await Eventually([&] { return fixture->restores == 1; }, "the goal workflow did not restore");
  Take(co_await session->SubmitAsync(kit::NewCommandId(), {fixture->form, 1, kit::JsonValue{true}}));
  co_await Eventually([&] { return !session->Snapshot()->summary.active_run; }, "the restored form did not resume the original run");
  Check(session->Snapshot()->runs.back().id == fixture->revised_run &&
        session->Snapshot()->runs.back().state == kit::RunState::Succeeded, "completion restarted the current run");
  Take(co_await runtime->CloseAsync());
}

struct GoalDispatchFixture {
  kit::ProjectId project{"goal.dispatch.project"};
  kit::SessionId owner;
  std::vector<kit::SessionId> workers;
  std::vector<kit::TaskExecutionRef> claims;
  std::vector<kit::CollaborationMessageId> requests;
  std::vector<std::optional<kit::GoalRef>> receives;
  kit::GoalRef original;
  kit::GoalRef revised;
  kit::RunId continuing;
  kit::InteractionId waiting;
  kit::PendingApproval pending_start;
  kit::CommitId revised_commit;
  bool edited = false;
  bool hide_last_worker = false;
  bool stale_dispatch = false;
  unsigned worker_restores = 0;
};

struct PausedGoalDispatch {
  kit::SessionId recipient;
  kit::GoalRef goal;
  std::optional<kit::CollaborationMessageId> request;
};

std::shared_ptr<kit::AgentRuntime> PausedGoalRuntime(std::shared_ptr<kit::SessionStore> store,
    const std::shared_ptr<PausedGoalDispatch>& dispatch) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"goal.pause.user"}); },
      .access = [](kit::PolicyRequest) -> kit::Async<kit::PolicyDecision> {
        co_return kit::AgentResult<kit::PolicyDecision>::Success({kit::AccessDecision::Allow, {}});
      },
      .receive = [](kit::ReceiveRequest) -> kit::Async<kit::ReceiveDecision> {
        co_return kit::AgentResult<kit::ReceiveDecision>::Success({kit::ReceiveAction::Allow, true});
      },
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"goal.pause.model"}, 1}, {"test", "local", "script", {}},
      std::make_shared<CountingModel>()}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"goal.pause"}, 1}, .display_name = "Goal assistant", .model = {"goal.pause.model"},
                     .collaboration_targets = {{"goal.pause"}},
                     .capabilities = {kit::Capability::ReadGoal, kit::Capability::SendMessage}},
      .run = [dispatch](kit::RunContext run, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        if (input.trigger == kit::RunTrigger::Goal && input.goal == dispatch->goal && !dispatch->request) {
          auto receipt = Take(co_await run.Collaboration().SendMessageAsync(kit::NewCommandId(), {
              .to = dispatch->recipient,
              .body = kit::WorkRequest{.input = {.text = "Goal work"}, .request_start = true}}));
          dispatch->request = std::get<kit::MessageAccepted>(receipt.outcome).message;
          co_return co_await WaitGoalForm(std::move(run), *input.goal, kit::NewCommandId());
        }
        if (input.input.text == "Hold the recipient") {
          auto form = Take(co_await run.RequestInputAsync(kit::NewCommandId(), {
              .title = "Keep working", .schema = {kit::JsonValue{true}}}));
          Take(co_await run.WaitForInputAsync(std::get<kit::InputRequested>(form.outcome).id));
        }
        co_return kit::AgentResult<kit::RunCompletion>::Success({});
      }}));
  return runtime;
}

huxerui::Task<void> VerifyPausedGoalDispatch(huxerui::File file) {
  auto dispatch = std::make_shared<PausedGoalDispatch>();
  auto store = Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file));
  auto runtime = PausedGoalRuntime(std::move(store), dispatch);
  Take(co_await runtime->StartAsync());
  auto sessions = runtime->Sessions();
  const kit::CreateSessionRequest create{.options = {.agent = {"goal.pause"}}};
  const auto target = Take(co_await sessions.CreateAsync(kit::NewCommandId(), create));
  const auto source = Take(co_await sessions.CreateAsync(kit::NewCommandId(), create));
  dispatch->recipient = std::get<kit::SessionCreated>(target.outcome).session;
  auto recipient = Take(co_await sessions.OpenAsync(dispatch->recipient));
  auto owner = Take(co_await sessions.OpenAsync(std::get<kit::SessionCreated>(source.outcome).session));
  Take(co_await recipient->SendAsync(kit::NewCommandId(), {.text = "Hold the recipient"}));
  co_await Eventually([&] { return recipient->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
      "the receiving session did not reach its form");
  auto goals = owner->Goals();
  const auto proposed = Take(co_await goals.UpdateAsync(kit::NewCommandId(),
      kit::ProposeGoal{{.definition = {.objective = "Prepare a report"}}}));
  const auto initial = std::get<kit::GoalUpdated>(proposed.outcome);
  dispatch->goal = initial.goal;
  Take(co_await goals.UpdateAsync(kit::NewCommandId(), kit::ChangeGoal{initial.goal.id, initial.revision, kit::EnableGoal{}}));
  co_await Eventually([&] { return dispatch->request && owner->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
      "the active goal did not queue its request");
  const auto active = *Take(co_await goals.ReadAsync());
  Take(co_await goals.UpdateAsync(kit::NewCommandId(), kit::ChangeGoal{active.ref.id, active.revision, kit::PauseGoal{}}));
  auto collaboration = runtime->Collaboration();
  const auto independent = Take(co_await collaboration.SendMessageAsync(kit::NewCommandId(), {
      .to = dispatch->recipient,
      .body = kit::WorkRequest{.input = {.text = "Independent work"}, .request_start = true}}));
  const auto independent_id = std::get<kit::MessageAccepted>(independent.outcome).message;
  auto target_goals = recipient->Goals();
  const auto target_proposal = Take(co_await target_goals.UpdateAsync(kit::NewCommandId(),
      kit::ProposeGoal{{.definition = {.objective = "Check another report"}}}));
  const auto target_goal = std::get<kit::GoalUpdated>(target_proposal.outcome);
  Take(co_await target_goals.UpdateAsync(kit::NewCommandId(),
      kit::ChangeGoal{target_goal.goal.id, target_goal.revision, kit::EnableGoal{}}));
  const auto owner_form = owner->Snapshot()->interactions.front();
  Take(co_await owner->SubmitAsync(kit::NewCommandId(), {owner_form.id, owner_form.version, kit::JsonValue{true}}));
  const auto hold = recipient->Snapshot()->interactions.front();
  Take(co_await recipient->SubmitAsync(kit::NewCommandId(), {hold.id, hold.version, kit::JsonValue{true}}));
  co_await Eventually([&] {
    const auto snapshot = recipient->Snapshot();
    return snapshot->runs.size() == 3 && !snapshot->summary.active_run && !owner->Snapshot()->summary.active_run;
  }, "paused goal work prevented independent collaboration or the recipient's own goal from running");
  const auto held = Take(co_await collaboration.ReadMessageAsync(*dispatch->request));
  const auto delivered = Take(co_await collaboration.ReadMessageAsync(independent_id));
  auto before_resume = recipient->Snapshot();
  const auto paused = *Take(co_await goals.ReadAsync());
  Check(held.summary.delivery == kit::DeliveryState::Queued && !held.summary.target_run &&
        delivered.summary.target_run == before_resume->runs[1].id && !before_resume->runs[1].goal &&
        before_resume->runs[2].goal == target_goal.goal && paused.state == kit::GoalState::Paused &&
        owner->Snapshot()->runs.size() == 1, "the pause altered the queued request or ran a suspended goal");
  Take(co_await goals.UpdateAsync(kit::NewCommandId(), kit::ChangeGoal{paused.ref.id, paused.revision, kit::ResumeGoal{}}));
  co_await Eventually([&] {
    const auto snapshot = recipient->Snapshot();
    return snapshot->runs.size() == 4 && !snapshot->summary.active_run;
  }, "the original request did not run when its goal resumed");
  const auto resumed = Take(co_await collaboration.ReadMessageAsync(*dispatch->request));
  auto after_resume = recipient->Snapshot();
  Check(resumed.summary.target_run == after_resume->runs.back().id && after_resume->runs.back().goal == dispatch->goal &&
        after_resume->runs.back().state == kit::RunState::Succeeded && !after_resume->summary.queued_inputs,
        "resuming the goal did not consume the original request with its original goal");
  Take(co_await runtime->CloseAsync());
}

kit::Async<kit::RunCompletion> GoalDispatchWorkflow(kit::RunContext run, kit::RunInput input, kit::CommandId form,
                                                   const std::shared_ptr<GoalDispatchFixture>& fixture) {
  Check(input.goal.has_value(), "a goal run arrived without its definition version");
  const auto definition = Take(co_await run.Goals().ReadDefinitionAsync(*input.goal));
  if (input.task) {
    Check((input.goal == fixture->original && definition.objective == "Release ten pages") ||
          (input.goal == fixture->revised && definition.objective == "Release five pages"),
          "the receiving workflow loaded a different goal definition");
  }
  auto outcome = Take(co_await WaitGoalForm(run, *input.goal, form));
  if (input.work_request) {
    if (fixture->edited) {
      auto sent = co_await run.Collaboration().SendMessageAsync(kit::NewCommandId(), {
          .to = fixture->workers.back(), .body = kit::WorkRequest{.input = {.text = "Start more work"}}});
      fixture->stale_dispatch = !sent.Succeeded() && sent.Error().code == kit::ErrorCode::Conflict;
    }
    auto answer = Take(co_await run.CompleteAsync({.instruction = "Summarize the original goal"}));
    Check(answer.text == "Ready", "the independent workflow did not complete a model turn");
    Take(co_await run.Collaboration().ReplyAsync(kit::NewCommandId(), *input.work_request,
        {.status = kit::ReplyStatus::Succeeded, .summary = definition.objective}));
  }
  co_return kit::AgentResult<kit::RunCompletion>::Success(std::move(outcome));
}

std::shared_ptr<kit::AgentRuntime> GoalDispatchRuntime(std::shared_ptr<kit::SessionStore> store,
    const std::shared_ptr<GoalDispatchFixture>& fixture) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"goal.dispatch.user"}); },
      .access = [fixture](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        auto decision = kit::AccessDecision::Allow;
        if (fixture->workers.size() == 3) {
          if (request.capability == kit::Capability::StartSessionRun && request.target.message &&
              request.target.session == fixture->workers[1]) decision = kit::AccessDecision::RequireConfirmation;
          if (fixture->hide_last_worker && request.capability == kit::Capability::ReadSession &&
              request.target.session == fixture->workers.back()) decision = kit::AccessDecision::Deny;
        }
        co_return kit::AgentResult<kit::PolicyDecision>::Success({decision, {}});
      },
      .receive = [fixture](kit::ReceiveRequest request) -> kit::Async<kit::ReceiveDecision> {
        fixture->receives.push_back(request.goal);
        co_return kit::AgentResult<kit::ReceiveDecision>::Success({kit::ReceiveAction::Allow, false});
      },
      .project = [fixture](kit::ProjectId id) -> kit::Async<kit::ProjectBinding> {
        if (id != fixture->project) co_return kit::AgentResult<kit::ProjectBinding>::Failure({.code = kit::ErrorCode::NotFound});
        co_return kit::AgentResult<kit::ProjectBinding>::Success({id});
      },
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"goal.dispatch-model"}, 1}, {"test", "local", "script", {}},
      std::make_shared<CountingModel>()}));
  Take(runtime->RegisterAgent({
      .definition = {.ref = {{"goal.dispatch"}, 1}, .display_name = "Goal worker", .model = {"goal.dispatch-model"},
                     .collaboration_targets = {{"goal.dispatch"}},
                     .capabilities = {kit::Capability::ReadGoal, kit::Capability::Reply, kit::Capability::SendMessage}},
      .run = [fixture](kit::RunContext run, kit::RunInput input) -> kit::Async<kit::RunCompletion> {
        auto form = kit::NewCommandId();
        Take(co_await run.SaveCheckpointAsync({.stage = "dispatch-test", .schema_version = 1, .state = kit::JsonValue{form.value}}));
        co_return co_await GoalDispatchWorkflow(std::move(run), std::move(input), std::move(form), fixture);
      },
      .restore = [fixture](kit::RunContext run, kit::RunInput input, kit::RunResume resume) -> kit::Async<kit::RunCompletion> {
        if (input.work_request) {
          ++fixture->worker_restores;
          Check(input.goal == fixture->original && input.run == fixture->continuing &&
                std::get<kit::InteractionId>(resume.waits.at(0).subject) == fixture->waiting,
                "the independent run was replaced or restored against the new version");
        }
        co_return co_await GoalDispatchWorkflow(std::move(run), std::move(input),
            {std::get<std::string>(resume.checkpoint->state.value)}, fixture);
      }}));
  return runtime;
}

huxerui::Task<void> ReviseDispatchedGoal(huxerui::File file, std::shared_ptr<GoalDispatchFixture> fixture) {
  auto runtime = GoalDispatchRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  const kit::CreateSessionRequest create{.options = {.agent = {"goal.dispatch"}, .project = fixture->project}};
  auto main = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), create));
  fixture->owner = std::get<kit::SessionCreated>(main.outcome).session;
  for (int i = 0; i < 3; ++i) {
    auto worker = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), create));
    fixture->workers.push_back(std::get<kit::SessionCreated>(worker.outcome).session);
  }
  auto owner = Take(co_await runtime->Sessions().OpenAsync(fixture->owner));
  auto goals = owner->Goals();
  auto project = runtime->Projects();
  auto collaboration = runtime->Collaboration();
  auto receipt = Take(co_await goals.UpdateAsync(kit::NewCommandId(),
      kit::ProposeGoal{{.definition = {.objective = "Release ten pages"}, .budget = {.model_turns = 2}}}));
  auto goal = std::get<kit::GoalUpdated>(receipt.outcome);
  fixture->original = goal.goal;
  std::vector<kit::TaskUpdated> tasks;
  const kit::SendInput work{.text = "Check the publication"};
  for (int i = 0; i < 3; ++i) {
    auto board = Take(co_await project.ReadAsync(fixture->project));
    auto task = Take(co_await UpdateTask(project, fixture->project,
        kit::CreateTask{board.revision, {.title = "Check the publication", .input = work, .goal = goal.goal.id}}));
    tasks.push_back(task);
  }
  auto draft = *Take(co_await goals.ReadAsync());
  Check(draft.tasks.size() == 3, "task creation did not update goal membership");
  Take(co_await goals.UpdateAsync(kit::NewCommandId(), kit::ChangeGoal{goal.goal.id, draft.revision, kit::EnableGoal{}}));
  co_await Eventually([&] { return owner->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
      "the goal owner did not settle at its input boundary");
  for (std::size_t i = 0; i < fixture->workers.size(); ++i) {
    auto task = Take(co_await UpdateTask(project, fixture->project,
        kit::ChangeTask{tasks[i].task, tasks[i].revision, kit::ClaimTask{fixture->workers[i]}}));
    kit::TaskExecutionRef attempt{fixture->project, task.task, *task.attempt};
    fixture->claims.push_back(attempt);
    auto claimed = Take(co_await ReadTask(project, fixture->project, task.task));
    Check(claimed.attempt_goal == fixture->original, "claiming did not fix the goal version");
    auto dispatch = Take(co_await collaboration.SendMessageAsync(kit::NewCommandId(), {
        .to = fixture->workers[i], .body = kit::WorkRequest{.input = work, .task = attempt}}));
    fixture->requests.push_back(std::get<kit::MessageAccepted>(dispatch.outcome).message);
  }
  const kit::GoalDefinition next{.objective = "Release five pages"};
  auto active = *Take(co_await goals.ReadAsync());
  auto obsolete = Take(co_await goals.PreviewEditAsync(goal.goal.id, active.revision, next));
  auto started = Take(co_await collaboration.StartRequestAsync(kit::NewCommandId(), fixture->requests[0]));
  fixture->continuing = *std::get<kit::MessageAccepted>(started.outcome).target_run;
  auto running = Take(co_await runtime->Sessions().OpenAsync(fixture->workers[0]));
  co_await Eventually([&] { return running->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
      "independent work did not start");
  fixture->waiting = running->Snapshot()->interactions.front().id;
  auto pending = Take(co_await collaboration.StartRequestAsync(kit::NewCommandId(), fixture->requests[1]));
  fixture->pending_start = std::get<kit::PendingApproval>(pending.outcome);
  auto stale = co_await goals.UpdateAsync(kit::NewCommandId(),
      kit::ChangeGoal{goal.goal.id, active.revision, kit::EditGoal{next, obsolete.token}});
  Check(!stale.Succeeded() && stale.Error().code == kit::ErrorCode::Conflict,
        "a preview accepted a request that passed the start boundary");
  fixture->hide_last_worker = true;
  auto preview = Take(co_await goals.PreviewEditAsync(goal.goal.id, active.revision, next));
  Check(preview.impact.continuing_runs == std::vector{fixture->continuing} &&
        preview.impact.continuing_attempts == std::vector{fixture->claims.front()} &&
        preview.impact.released_attempts.size() == 2 &&
        preview.impact.withdrawn_requests == std::vector{fixture->requests[1]} &&
        std::ranges::find(preview.impact.invalidated_interactions, fixture->pending_start.interaction) !=
            preview.impact.invalidated_interactions.end(), "the edit preview did not project authorized impact");
  receipt = Take(co_await goals.UpdateAsync(kit::NewCommandId(),
      kit::ChangeGoal{goal.goal.id, active.revision, kit::EditGoal{next, preview.token}}));
  fixture->revised = std::get<kit::GoalUpdated>(receipt.outcome).goal;
  fixture->revised_commit = receipt.stamp.commit;
  fixture->edited = true;
  fixture->hide_last_worker = false;
  auto original = running->Snapshot();
  Check(original->summary.active_run == fixture->continuing && original->runs.back().goal == fixture->original,
        "revising the goal stopped already-started independent work");
  auto approval = Take(co_await runtime->Commands().ReadAsync(fixture->pending_start));
  Check(approval.state == kit::CommandState::Withdrawn && approval.resolution &&
        approval.resolution->commit == fixture->revised_commit, "dispatch approval was not invalidated with the goal edit");
  Check(!fixture->receives.empty() && std::ranges::all_of(fixture->receives, [&](auto ref) { return ref == fixture->original; }),
        "the receiving policy did not see the trusted bound goal");
  co_await Eventually([&] {
    const auto snapshot = owner->Snapshot();
    return snapshot->summary.run_state == kit::RunState::WaitingForInput && snapshot->runs.back().goal == fixture->revised;
  }, "the owner did not continue at the new version");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> RecoverDispatchedGoal(huxerui::File file, std::shared_ptr<GoalDispatchFixture> fixture) {
  auto runtime = GoalDispatchRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), fixture);
  Take(co_await runtime->StartAsync());
  auto owner = Take(co_await runtime->Sessions().OpenAsync(fixture->owner));
  auto running = Take(co_await runtime->Sessions().OpenAsync(fixture->workers[0]));
  auto goals = owner->Goals();
  auto project = runtime->Projects();
  for (std::size_t i = 1; i < fixture->workers.size(); ++i) {
    auto session = Take(co_await runtime->Sessions().OpenAsync(fixture->workers[i]));
    auto item = Take(co_await runtime->Collaboration().ReadMessageAsync(fixture->requests[i]));
    Check(item.summary.delivery == kit::DeliveryState::Withdrawn && item.outcome && item.outcome->end == kit::ReplyEnd::Withdrawn &&
          session->Snapshot()->runs.empty() && !session->Snapshot()->summary.queued_inputs,
          "an unstarted request survived the edit after restart");
    auto task = Take(co_await ReadTask(project, fixture->project, fixture->claims[i].task));
    auto retired = Take(co_await project.WaitForTaskAsync(fixture->claims[i]));
    Check(!task.attempt && task.state == kit::ProjectTaskState::Ready && retired.state == kit::ProjectTaskState::Cancelled &&
          retired.goal == fixture->original, "old claim was not released with historical outcome");
  }
  auto worker = Take(co_await runtime->Sessions().OpenAsync(fixture->workers[1]));
  auto outdated = co_await worker->SubmitAsync(kit::NewCommandId(),
      {fixture->pending_start.interaction, fixture->pending_start.version, kit::ApprovalDecision::Approve});
  Check(!outdated.Succeeded(), "an invalidated approval could start old work");
  co_await Eventually([&] { return fixture->worker_restores == 1; }, "already-started work did not recover");
  Take(co_await running->SubmitAsync(kit::NewCommandId(), {fixture->waiting, 1, kit::JsonValue{true}}));
  auto delivered = Take(co_await runtime->Collaboration().WaitForReplyAsync(fixture->requests.front()));
  Check(delivered.end == kit::ReplyEnd::Received && delivered.reply->summary == "Release ten pages" && fixture->stale_dispatch,
        "the old run could not reply on its existing request or bypassed the revision check for new work");
  co_await Eventually([&] { return !running->Snapshot()->summary.active_run; }, "the old work did not finish");
  auto view = *Take(co_await goals.ReadAsync());
  Check(view.ref == fixture->revised && view.usage.model_turns == 1 &&
        std::ranges::find(view.runs, fixture->continuing) != view.runs.end(), "independent usage was not charged to the original goal identity");
  auto task = Take(co_await ReadTask(project, fixture->project, fixture->claims[1].task));
  auto claim = Take(co_await UpdateTask(project, fixture->project, kit::ChangeTask{
      task.id, task.revision, kit::ClaimTask{fixture->workers[1]}}));
  auto start = Take(co_await UpdateTask(project, fixture->project, kit::ChangeTask{
      task.id, claim.revision, kit::StartTask{*claim.attempt}}));
  co_await Eventually([&] { return worker->Snapshot()->summary.run_state == kit::RunState::WaitingForInput; },
      "the newly claimed task did not start");
  auto rebound = Take(co_await ReadTask(project, fixture->project, task.id));
  Check(rebound.attempt == start.attempt && rebound.attempt != fixture->claims[1].attempt &&
        worker->Snapshot()->runs.back().goal == fixture->revised && rebound.attempt_goal == fixture->revised,
        "the new task attempt did not bind the new goal definition");
  Take(co_await runtime->CloseAsync());
}

struct WorkspaceForkProbe {
  kit::CommandId command = kit::NewCommandId();
  kit::ForkSessionRequest request;
  kit::PendingApproval approval;
  std::optional<kit::WorkspaceRequest> allocation;
  kit::WorkspaceId target{"forked-workspace"};
  bool revoke_target = false;
  bool ask = true;
};

std::shared_ptr<kit::AgentRuntime> ForkRuntime(std::shared_ptr<kit::SessionStore> store, WorkspaceForkProbe& probe) {
  auto runtime = Take(kit::AgentRuntime::Create({
      .tasks = huxerui::UseApplicationTaskScope(),
      .principal = [] { return kit::AgentResult<kit::PrincipalId>::Success({"test.user"}); },
      .access = [&probe](kit::PolicyRequest request) -> kit::Async<kit::PolicyDecision> {
        const auto fork = request.capability == kit::Capability::ForkSession;
        const auto denied = probe.revoke_target && request.capability == kit::Capability::CreateSession &&
            request.workspace == probe.target;
        co_return kit::AgentResult<kit::PolicyDecision>::Success({
            denied ? kit::AccessDecision::Deny : fork && probe.ask ? kit::AccessDecision::RequireConfirmation
                                                                 : kit::AccessDecision::Allow, {}});
      },
      .workspace = [&probe](kit::WorkspaceRequest workspace) -> kit::Async<std::optional<kit::WorkspaceId>> {
        Check(!probe.allocation, "resolver ran twice for one committed command");
        probe.allocation = std::move(workspace);
        co_return kit::AgentResult<std::optional<kit::WorkspaceId>>::Success(probe.target);
      },
      .store = std::move(store)}));
  Take(runtime->RegisterModel({{{"fork.model"}, 1}, {"test", "test", "script", {}}, std::make_shared<CountingModel>()}));
  Take(runtime->RegisterAgent({.definition = {.ref = {{"fork.agent"}, 1}, .display_name = "Fork test",
      .model = {"fork.model"}}}));
  return runtime;
}

huxerui::Task<void> StageWorkspaceFork(huxerui::File file, WorkspaceForkProbe& probe) {
  auto runtime = ForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  const kit::WorkspaceId workspace{"source-workspace"};
  const kit::ProjectId project{"shared-fork-project"};
  auto created = Take(co_await runtime->Sessions().CreateAsync(kit::NewCommandId(), {
      .options = {.agent = {"fork.agent"}, .project = project, .workspace = workspace},
      .initial = kit::SendInput{.text = "Committed source history"}}));
  auto source = Take(co_await runtime->Sessions().OpenAsync(std::get<kit::SessionCreated>(created.outcome).session));
  auto snapshot = source->Snapshot();
  probe.request = {.from = {snapshot->summary.id, snapshot->messages.front().ref.message, snapshot->messages.front().sequence},
      .options = {.agent = {"fork.agent"}, .project = project, .title = "Git branch"},
      .workspace = kit::WorkspaceChoice{kit::WorkspaceChoice::Kind::CopyFromSource, workspace,
          kit::WorkspaceCopyPlanRef{kit::WorkspaceCopyPlanId{kit::NewCommandId().value}, 1}}};
  auto mismatched = probe.request;
  mismatched.workspace->workspace = {"different-source"};
  auto mismatch = co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), std::move(mismatched));
  Check(!mismatch.Succeeded() && mismatch.Error().code == kit::ErrorCode::Conflict,
        "a workspace plan for another source was accepted");
  auto pending = Take(co_await runtime->Sessions().ForkAsync(probe.command, probe.request));
  probe.approval = std::get<kit::PendingApproval>(pending.outcome);
  Check(!probe.allocation, "worktree allocation started before the required approval");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ApproveWorkspaceFork(huxerui::File file, WorkspaceForkProbe& probe) {
  auto store = std::make_shared<LostReceiptStore>(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), true);
  auto runtime = ForkRuntime(store, probe);
  Take(co_await runtime->StartAsync());
  auto source = Take(co_await runtime->Sessions().OpenAsync(probe.request.from.session));
  Take(co_await source->SubmitAsync(kit::NewCommandId(),
      {probe.approval.interaction, probe.approval.version, kit::ApprovalDecision::Approve}));
  auto progress = Take(co_await AwaitOperation(runtime, probe.command));
  Check(progress.state == kit::CommandState::Applied && progress.result, "worktree approval did not settle");
  const auto& created = std::get<kit::SessionCreated>(*progress.result);
  auto replay = Take(co_await runtime->Sessions().ForkAsync(probe.command, probe.request));
  Check(std::get<kit::PendingApproval>(replay.outcome).interaction == probe.approval.interaction,
        "worktree replay replaced the original approval receipt");
  Check(probe.allocation && probe.allocation->command == probe.command &&
        probe.allocation->new_session == created.session && probe.allocation->choice == probe.request.workspace &&
        probe.allocation->fork_from == probe.request.from && probe.allocation->project == probe.request.options.project &&
        !created.initial_run && store->lookups == 1, "approved allocation or uncertain receipt was not reconciled");
  auto branch = Take(co_await runtime->Sessions().OpenAsync(created.session));
  auto view = branch->Snapshot();
  Check(view->summary.workspace == probe.target && view->summary.project == probe.request.options.project &&
        view->messages.size() == 1 && view->origin->fork_point == probe.request.from && view->interactions.empty(),
        "worktree fork did not bind the new workspace and sealed history");
  Take(co_await runtime->CloseAsync());
}

huxerui::Task<void> ReopenWorkspaceFork(huxerui::File file, WorkspaceForkProbe& probe) {
  auto runtime = ForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await runtime->StartAsync());
  auto progress = Take(co_await runtime->Commands().ReadAsync(probe.command));
  Check(progress.state == kit::CommandState::Applied && progress.result &&
        std::get<kit::SessionCreated>(*progress.result).session == probe.allocation->new_session,
        "a committed workspace fork was not found after restart");
  auto remove_committed = co_await runtime->Sessions().AbandonWorkspaceForkAsync(probe.command, probe.request, probe.target);
  Check(!remove_committed.Succeeded() && remove_committed.Error().code == kit::ErrorCode::Conflict,
        "the runtime allowed cleanup of a workspace used by a committed fork");
  auto reused = co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), probe.request);
  Check(!reused.Succeeded() && reused.Error().code == kit::ErrorCode::Conflict, "a workspace plan was consumed twice");
  auto changed = probe.request;
  changed.workspace->copy_plan->version = 2;
  auto mismatch = co_await runtime->Sessions().ForkAsync(probe.command, changed);
  Check(!mismatch.Succeeded() && mismatch.Error().code == kit::ErrorCode::Conflict,
        "a committed command was replayed with a different workspace plan");
  probe.ask = false;
  probe.revoke_target = true;
  probe.allocation.reset();
  auto rejected = co_await runtime->Sessions().ForkAsync(kit::NewCommandId(), std::move(changed));
  Check(!rejected.Succeeded() && rejected.Error().code == kit::ErrorCode::PermissionDenied,
        "the allocated workspace escaped target authorization");
  auto invisible = co_await runtime->Sessions().OpenAsync(probe.allocation.value().new_session);
  Check(!invisible.Succeeded(), "a denied workspace fork created a visible session");
  const kit::WorkspaceId retired{"uncommitted-workspace"};
  const auto abandoned = kit::NewCommandId();
  auto cleanup = probe.request;
  cleanup.workspace->copy_plan->version = 3;
  auto stale = ForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await stale->StartAsync());
  auto stale_cleanup = ForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await stale_cleanup->StartAsync());
  Take(co_await runtime->Sessions().AbandonWorkspaceForkAsync(abandoned, cleanup, retired));
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto lost = co_await stale_cleanup->Sessions().AbandonWorkspaceForkAsync(abandoned, cleanup, retired);
    Check(!lost.Succeeded() && lost.Error().code == kit::ErrorCode::Conflict,
          "an unconfirmed retirement was reported as durable");
  }
  auto cleanup_close = co_await stale_cleanup->CloseAsync();
  Check(!cleanup_close.Succeeded() && cleanup_close.Error().code == kit::ErrorCode::Conflict,
        "the stale cleanup runtime lost the store conflict");
  auto competing = co_await stale->Sessions().CreateAsync(
      kit::NewCommandId(), {.options = {.agent = {"fork.agent"}, .workspace = retired, .title = "Obsolete writer"}});
  Check(!competing.Succeeded() && competing.Error().code == kit::ErrorCode::Conflict,
        "a stale runtime committed a conversation after workspace retirement");
  auto stale_close = co_await stale->CloseAsync();
  Check(!stale_close.Succeeded() && stale_close.Error().code == kit::ErrorCode::Conflict,
        "the stale runtime lost the store conflict");
  Take(co_await runtime->CloseAsync());
  auto reopened = ForkRuntime(Take(co_await agentkit_host::OpenSqliteSessionStoreAsync(file)), probe);
  Take(co_await reopened->StartAsync());
  Take(co_await reopened->Sessions().AbandonWorkspaceForkAsync(abandoned, cleanup, retired));
  auto retry = co_await reopened->Sessions().ForkAsync(abandoned, cleanup);
  auto allocate = co_await reopened->Sessions().ForkAsync(kit::NewCommandId(), cleanup);
  auto use = co_await reopened->Sessions().CreateAsync(kit::NewCommandId(), {.options = {.agent = {"fork.agent"}, .workspace = retired}});
  Check(!retry.Succeeded() && retry.Error().code == kit::ErrorCode::AlreadyClosed &&
        !allocate.Succeeded() && allocate.Error().code == kit::ErrorCode::AlreadyClosed &&
        !use.Succeeded() && use.Error().code == kit::ErrorCode::AlreadyClosed,
        "a retired workspace or plan was reused after restart");
  Take(co_await reopened->CloseAsync());
}

void RunInApplication(std::function<huxerui::Task<void>()> scenario,
                      std::source_location location = std::source_location::current()) {
  using namespace huxerui;
  bool done = false;
  std::exception_ptr failure;
  Application app{
      []() -> View { return Text("AgentKit store test"); },
      {.application_hooks = {[&](ApplicationContext&) {
        UseApplicationTaskScope().Launch([&]() -> Task<void> {
          try { co_await scenario(); } catch (...) { failure = std::current_exception(); }
          done = true;
        });
      }}}};
  testing::UiTest ui(app);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!done && std::chrono::steady_clock::now() < deadline) {
    ui.Pump(2ms);
    std::this_thread::sleep_for(2ms);
  }
  if (!done) throw std::runtime_error("Timed out waiting for SQLite scenario at line " + std::to_string(location.line()));
  if (failure) std::rethrow_exception(failure);
}
} // namespace

int main() {
  using namespace huxerui;
  auto path = std::filesystem::temp_directory_path() / ("agentkit-" + kit::NewCommandId().value + ".sqlite3");
  auto adoption_path = path;
  adoption_path += ".adoption";
  auto comparison_approval_path = path;
  comparison_approval_path += ".comparison-approval";
  auto source_review_path = path;
  source_review_path += ".source-review";
  auto before_source_review_path = path;
  before_source_review_path += ".before-source-review";
  auto source_approval_path = path;
  source_approval_path += ".source-approval";
  auto adoption_target = path;
  adoption_target += ".target.txt";
  auto cleanup = [&] {
    std::error_code error;
    for (const auto* database : {&path, &adoption_path, &comparison_approval_path, &before_source_review_path,
                                &source_review_path, &source_approval_path}) {
      std::filesystem::remove(*database, error);
      for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        auto related = *database;
        related += suffix;
        std::filesystem::remove(related, error);
      }
    }
    std::filesystem::remove(adoption_target, error);
  };
  try {
    const auto file = File(path.u8string());
    RunInApplication([&]() -> Task<void> {
      co_await VerifyStore(file);
      co_await VerifyRuntimeReopen(file);
    });
    StreamingCheckpoint streaming;
    RunInApplication([&] { return SaveStreamingCheckpoint(file, streaming); });
    RunInApplication([&] { return RecoverStreamingCheckpoint(file, streaming); });
    ModelPublicationFixture publications;
    RunInApplication([&] { return VerifyModelPublications(file, publications, false); });
    RunInApplication([&] { return VerifyModelPublications(file, publications, true); });
    ResultAccessFixture result_access;
    RunInApplication([&] { return VerifyResultAccess(file, result_access, false); });
    RunInApplication([&] { return VerifyResultAccess(file, result_access, true); });
    for (const bool outcome_known : {false, true}) {
      auto probe = std::make_shared<EffectProbe>();
      probe->outcome_known = outcome_known;
      kit::SessionCreated created;
      RunInApplication([&] { return StartInterruptedEffect(file, probe, created); });
      RunInApplication([&] { return RecoverInterruptedEffect(file, probe, created); });
      if (!outcome_known) RunInApplication([&] { return VerifyReconciledEffect(file, probe, created); });
    }
    for (const auto scenario : {ApprovalRestart::Pending, ApprovalRestart::Approved, ApprovalRestart::Revoked,
                                ApprovalRestart::StoredContinuation, ApprovalRestart::VolatileContinuation}) {
      auto probe = std::make_shared<EffectProbe>();
      probe->ask = true;
      SavedApproval saved;
      RunInApplication([&] { return SaveApproval(file, probe, saved, scenario); });
      if (scenario == ApprovalRestart::Pending) {
        RunInApplication([&] { return UseLegacyToolPlanFields(file, saved.session.session); });
      }
      RunInApplication([&] { return ResumeApproval(file, probe, saved, scenario); });
    }
    for (kit::Revision restored_version : {1, 2}) {
      auto probe = std::make_shared<EffectProbe>();
      probe->ask = true;
      probe->hold_effect = false;
      SavedApproval saved;
      RunInApplication([&] { return VerifyCatalogApproval(file, saved, probe, false, 1); });
      RunInApplication([&] { return VerifyCatalogApproval(file, saved, probe, true, restored_version); });
    }
    {
      SavedToolTurn saved;
      RunInApplication([&] { return VerifyStoredToolTurn(file, saved, false); });
      RunInApplication([&] { return VerifyStoredToolTurn(file, saved, true); });
    }
    {
      SavedRework saved;
      RunInApplication([&] { return VerifyRework(file, saved, false); });
      RunInApplication([&] { return UseLegacyReworkPlanField(file, saved.session); });
      RunInApplication([&] { return VerifyRework(file, saved, true); });
    }
    for (const auto scenario : {SourceApprovalCase::Reviewing, SourceApprovalCase::Approved}) {
      SavedSourceApproval saved;
      const auto& database = scenario == SourceApprovalCase::Reviewing ? source_review_path : source_approval_path;
      const File source_file(database.u8string());
      RunInApplication([&] { return VerifySourceApproval(source_file, saved, scenario, false); });
      RunInApplication([&] { return VerifySourceApproval(source_file, saved, scenario, true); });
    }
    {
      SavedSourceApproval saved;
      const File source_file(before_source_review_path.u8string());
      RunInApplication([&] { return VerifySourceApproval(source_file, saved, SourceApprovalCase::BeforeReview, false); });
    }
    for (const auto scenario : {ReviewRestart::Pending, ReviewRestart::Approved, ReviewRestart::ReviewerChanged}) {
      auto probe = std::make_shared<EffectProbe>();
      probe->review = true;
      probe->hold_effect = false;
      SavedReview saved;
      RunInApplication([&] { return SaveToolReview(file, probe, saved, scenario); });
      RunInApplication([&] { return ReopenToolReview(file, probe, saved, scenario); });
    }
    for (const auto trigger : {kit::SkillActivationTrigger::User, kit::SkillActivationTrigger::Model}) {
      SavedSkill saved;
      RunInApplication([&] { return VerifySkillApproval(file, saved, false, trigger); });
      RunInApplication([&] { return VerifySkillApproval(file, saved, true, trigger); });
    }
    {
      SavedSkill missing;
      RunInApplication([&] { return VerifySkillApproval(file, missing, false, kit::SkillActivationTrigger::User); });
      RunInApplication([&] { return VerifySkillApproval(file, missing, true, kit::SkillActivationTrigger::User, false); });
    }
    {
      SavedSkill revoked;
      RunInApplication([&] { return VerifySkillApproval(file, revoked, false, kit::SkillActivationTrigger::User); });
      RunInApplication([&] { return VerifySkillApproval(file, revoked, true, kit::SkillActivationTrigger::User, true, true); });
    }
    for (const bool answer_before_restart : {false, true}) {
      auto probe = std::make_shared<FormProbe>();
      kit::SessionCreated created;
      probe->hold_before_wait = answer_before_restart;
      RunInApplication([&] { return FormRun(file, probe, created, false, answer_before_restart); });
      probe->hold_before_wait = false;
      RunInApplication([&] { return FormRun(file, probe, created, true, answer_before_restart); });
    }
    auto side_probe = std::make_shared<FormProbe>();
    side_probe->receive_conclusion = true;
    StoredSideChat side;
    RunInApplication([&] { return SaveSideChat(file, side_probe, side); });
    RunInApplication([&] { return ReopenSideChat(file, side_probe, side); });
    auto recovery_probe = std::make_shared<FormProbe>();
    recovery_probe->receive_conclusion = true;
    StoredSideChat recovery;
    RunInApplication([&] { return SaveSideChat(file, recovery_probe, recovery, SideChatDelivery::RejectedQueue); });
    RunInApplication([&] { return RecoverRejectedReferences(file, recovery_probe, recovery); });
    auto steer_probe = std::make_shared<FormProbe>();
    steer_probe->receive_conclusion = true;
    steer_probe->skills = std::make_shared<ReleaseSkillSource>(kit::AgentId{"test.form"});
    steer_probe->skills->current = recovered_skill.version;
    StoredSideChat steer;
    RunInApplication([&] { return SaveSideChat(file, steer_probe, steer, SideChatDelivery::Steer); });
    RunInApplication([&] { return FinishUnappliedSteer(file, steer_probe, steer); });
    RunInApplication([&] { return RecoverUnappliedSteer(file, steer_probe, steer); });
    auto collaboration = std::make_shared<CollaborationFixture>();
    RunInApplication([&] { return DispatchReviews(file, collaboration); });
    RunInApplication([&] { return ResumeReviews(file, collaboration); });
    auto approvals = std::make_shared<OperationFixture>();
    RunInApplication([&] { return StageCreation(file, approvals); });
    RunInApplication([&] { return CommitCreationDecision(file, approvals); });
    RunInApplication([&] { return FinishApprovedOperations(file, approvals); });
    WorkspaceForkProbe workspace_fork;
    RunInApplication([&] { return StageWorkspaceFork(file, workspace_fork); });
    RunInApplication([&] { return ApproveWorkspaceFork(file, workspace_fork); });
    RunInApplication([&] { return ReopenWorkspaceFork(file, workspace_fork); });
    RunInApplication([&] { return VerifyArtifactFork(file); });
    auto project = std::make_shared<ProjectFixture>();
    RunInApplication([&] { return SetUpProject(file, project); });
    RunInApplication([&] { return ReopenProject(file, project); });
    RunInApplication([&] { return RecoverProjectWithdrawal(file, project); });
    auto comparison = std::make_shared<ForkReviewFixture>();
    RunInApplication([&] { return StoreForkReview(file, comparison); });
    RunInApplication([&] { return ResumeForkReview(file, comparison, false); });
    RunInApplication([&] { return ResumeForkReview(file, comparison, true); });
    auto comparison_approval = std::make_shared<ForkReviewFixture>();
    const File comparison_approval_store(comparison_approval_path.u8string());
    RunInApplication([&] { return StageComparisonApprovals(comparison_approval_store, comparison_approval); });
    RunInApplication([&] { return DecideComparisonApprovals(comparison_approval_store, comparison_approval); });
    RunInApplication([&] { return RecoverComparisonApproval(comparison_approval_store, comparison_approval); });
    auto adoption = std::make_shared<ForkReviewFixture>();
    const File adoption_store(adoption_path.u8string());
    adoption->adoption_target.emplace(adoption_target.u8string());
    Check(adoption->adoption_target->WriteString("main-v1"), "could not seed the adoption target");
    RunInApplication([&] { return StoreCandidateAdoption(adoption_store, adoption); });
    RunInApplication([&] { return RecoverCandidateAdoption(adoption_store, adoption, true); });
    RunInApplication([&] { return RecoverCandidateAdoption(adoption_store, adoption, false); });
    auto goal = std::make_shared<GoalFixture>();
    RunInApplication([&] { return ReviseReleaseGoal(file, goal); });
    RunInApplication([&] { return ReviewRecoveredGoal(file, goal); });
    RunInApplication([&] { return VerifyPausedGoalDispatch(file); });
    auto dispatch = std::make_shared<GoalDispatchFixture>();
    RunInApplication([&] { return ReviseDispatchedGoal(file, dispatch); });
    RunInApplication([&] { return RecoverDispatchedGoal(file, dispatch); });
    cleanup();
    std::cout << "AgentKit SQLite transactions, effects, approvals, collaboration, projects, and goals passed\n";
    return 0;
  } catch (const std::exception& error) {
    cleanup();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
