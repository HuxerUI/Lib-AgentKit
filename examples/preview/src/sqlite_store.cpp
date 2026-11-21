#include "preview_store_codec.h"
#include "sqlite_session_store.h"

#include <nlohmann/json.hpp>

namespace agentkit_preview {
namespace {
namespace db = huxerui::sqlite;
namespace kit = huxerui::agentkit;

using agentkit_host::storage::RevisionColumn;
using agentkit_host::storage::StoreError;
using storage::Fail;

db::Result<void> InstallPreviewSchema(db::Transaction& txn) {
  auto sessions = agentkit_host::InstallSqliteSessionSchema(txn);
  if (!sessions) return sessions.Error();
  for (const auto* statement : {
           "CREATE TABLE IF NOT EXISTS agentkit_preview_preferences("
           "principal TEXT NOT NULL, session_id TEXT NOT NULL, "
           "model_id TEXT NOT NULL, model_version INTEGER NOT NULL CHECK(model_version >= 0), "
           "profile_id TEXT NOT NULL, profile_version INTEGER NOT NULL CHECK(profile_version >= 0), "
           "PRIMARY KEY(principal, session_id)) WITHOUT ROWID",
           "CREATE TABLE IF NOT EXISTS agentkit_preview_navigation("
           "principal TEXT PRIMARY KEY NOT NULL, payload TEXT NOT NULL) WITHOUT ROWID"}) {
    auto result = txn.Execute(statement);
    if (!result) return result.Error();
  }
  return {};
}

constexpr auto navigation_query = "SELECT payload FROM agentkit_preview_navigation WHERE principal = ?";

db::Result<std::string> NavigationPayload(const db::RowView& row) { return row.Get<std::string>(0); }

db::Result<NavigationPreferences> DecodeNavigationRows(db::Result<std::vector<std::string>> rows) {
  if (!rows) return rows.Error();
  if (rows->empty()) return NavigationPreferences{};
  auto navigation = storage::DecodeNavigation(nlohmann::json::parse(rows->front(), nullptr, false));
  if (!navigation.Succeeded()) return db::Error(db::ErrorCode::Decode, navigation.Error().message);
  return std::move(navigation).Value();
}

class SqliteComposerPreferences final : public ComposerPreferences {
public:
  explicit SqliteComposerPreferences(db::Database database) : database_(std::move(database)) {}

  kit::Async<Entries> LoadAsync(kit::PrincipalId principal) override;
  kit::Async<void> SaveAsync(kit::PrincipalId principal, kit::SessionId session, kit::UserRunOptions options) override;

private:
  db::Database database_;
};

class SqliteNavigationStore final : public ConversationNavigationStore {
public:
  explicit SqliteNavigationStore(db::Database database) : database_(std::move(database)) {}

  kit::Async<NavigationPreferences> LoadAsync(kit::PrincipalId principal) override;
  kit::Async<NavigationPreferences> EditAsync(kit::PrincipalId principal, NavigationEdit edit) override;

private:
  db::Database database_;
};

kit::Async<ComposerPreferences::Entries> SqliteComposerPreferences::LoadAsync(kit::PrincipalId principal) {
  using Entry = std::pair<kit::SessionId, kit::UserRunOptions>;
  auto rows = co_await database_.QueryAsync<Entry>(
      "SELECT session_id, model_id, model_version, profile_id, profile_version "
      "FROM agentkit_preview_preferences WHERE principal = ?",
      [](const db::RowView& row) -> db::Result<Entry> {
        auto session = row.Get<std::string>(0);
        auto model = row.Get<std::string>(1);
        auto model_version = RevisionColumn(row, 2);
        auto profile = row.Get<std::string>(3);
        auto profile_version = RevisionColumn(row, 4);
        if (!session || !model || !model_version || !profile || !profile_version || session->empty() ||
            model->empty() != (*model_version == 0) || profile->empty() != (*profile_version == 0)) {
          return db::Error(db::ErrorCode::Decode, "Invalid saved run configuration");
        }
        kit::UserRunOptions options;
        if (*model_version) options.model = kit::ModelProviderRef{{std::move(model).Value()}, *model_version};
        if (*profile_version) options.execution_profile =
            kit::ExecutionProfileRef{{std::move(profile).Value()}, *profile_version};
        return Entry{{std::move(session).Value()}, std::move(options)};
      }, std::move(principal.value));
  if (!rows) co_return StoreError<Entries>(rows.Error());
  Entries entries;
  for (auto& [id, options] : rows.Value()) entries.emplace(std::move(id), std::move(options));
  co_return kit::AgentResult<Entries>::Success(std::move(entries));
}

kit::Async<void> SqliteComposerPreferences::SaveAsync(kit::PrincipalId principal, kit::SessionId session, kit::UserRunOptions options) {
  if (!storage::ValidPreference(principal, session, options)) {
    co_return Fail<void>(kit::ErrorCode::InvalidArgument, "A valid run configuration is required");
  }
  auto saved = co_await database_.ExecuteAsync(
      "INSERT INTO agentkit_preview_preferences(principal, session_id, model_id, model_version, profile_id, profile_version) "
      "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(principal, session_id) DO UPDATE SET model_id = excluded.model_id, "
      "model_version = excluded.model_version, profile_id = excluded.profile_id, profile_version = excluded.profile_version",
      std::move(principal.value), std::move(session.value), options.model ? options.model->id.value : std::string{},
      static_cast<std::int64_t>(options.model ? options.model->version : 0),
      options.execution_profile ? options.execution_profile->id.value : std::string{},
      static_cast<std::int64_t>(options.execution_profile ? options.execution_profile->version : 0));
  if (!saved) co_return StoreError<void>(saved.Error());
  co_return kit::AgentResult<void>::Success();
}

kit::Async<NavigationPreferences> SqliteNavigationStore::LoadAsync(kit::PrincipalId principal) {
  auto rows = co_await database_.QueryAsync<std::string>(navigation_query, NavigationPayload, std::move(principal.value));
  auto result = DecodeNavigationRows(std::move(rows));
  if (!result) co_return StoreError<NavigationPreferences>(result.Error());
  co_return kit::AgentResult<NavigationPreferences>::Success(std::move(result).Value());
}

kit::Async<NavigationPreferences> SqliteNavigationStore::EditAsync(kit::PrincipalId principal, NavigationEdit edit) {
  using Result = kit::AgentResult<NavigationPreferences>;
  auto apply = [principal = std::move(principal.value), edit = std::move(edit)](db::Transaction& txn) -> db::Result<Result> {
    auto loaded = DecodeNavigationRows(txn.Query<std::string>(navigation_query, NavigationPayload, principal));
    if (!loaded) return loaded.Error();
    auto changed = edit(loaded.Value());
    if (!changed.Succeeded()) return Result::Failure(changed.Error());
    auto written = txn.Execute("INSERT INTO agentkit_preview_navigation(principal, payload) VALUES (?, ?) "
        "ON CONFLICT(principal) DO UPDATE SET payload = excluded.payload", principal, storage::EncodeNavigation(loaded.Value()).dump());
    if (!written) return written.Error();
    return Result::Success(std::move(loaded).Value());
  };
  auto saved = co_await database_.TransactionAsync(std::move(apply));
  if (!saved) co_return StoreError<NavigationPreferences>(saved.Error());
  co_return std::move(saved).Value();
}

} // namespace

kit::Async<PreviewStores> OpenPreviewStoresAsync(huxerui::File file) {
  auto database = co_await db::Database::OpenAsync(std::move(file), {.create_parent_directories = true});
  if (!database) co_return StoreError<PreviewStores>(database.Error());
  auto schema = co_await database->TransactionAsync(InstallPreviewSchema);
  if (!schema) co_return StoreError<PreviewStores>(schema.Error());
  co_return kit::AgentResult<PreviewStores>::Success({agentkit_host::MakeSqliteSessionStore(database.Value()),
      std::make_shared<SqliteComposerPreferences>(database.Value()),
      std::make_shared<SqliteNavigationStore>(std::move(database).Value())});
}

} // namespace agentkit_preview
