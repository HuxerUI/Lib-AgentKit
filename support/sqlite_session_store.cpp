#include "sqlite_session_store.h"

namespace agentkit_host {
namespace db = huxerui::sqlite;
namespace kit = huxerui::agentkit;

db::Result<kit::Revision> storage::RevisionColumn(const db::RowView& row, std::size_t column) {
  auto value = row.Get<std::int64_t>(column);
  if (!value) return value.Error();
  if (*value < 0) return db::Error(db::ErrorCode::Decode, "Negative store revision");
  return static_cast<kit::Revision>(*value);
}

namespace {
using huxerui::Bytes;
using storage::Fail;
using storage::max_revision;
using storage::RevisionColumn;
using storage::StoreError;

db::Result<kit::Revision> CurrentRevision(db::Transaction& txn) {
  auto current = txn.Query<kit::Revision>(
      "SELECT revision FROM agentkit_store_meta WHERE singleton = 1 AND schema_version = 1",
      [](const db::RowView& row) { return RevisionColumn(row, 0); });
  if (!current) return current.Error();
  if (current->size() != 1) return db::Error(db::ErrorCode::SchemaMismatch, "Unsupported AgentKit store schema");
  return current->front();
}

void AppendInteger(Bytes& output, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xff));
  }
}

void AppendText(Bytes& output, const std::string& text) {
  AppendInteger(output, text.size());
  for (unsigned char ch : text) output.push_back(static_cast<std::byte>(ch));
}

Bytes CommitPayload(const kit::StoreTransaction& request) {
  Bytes bytes;
  AppendInteger(bytes, request.expected_store_revision);
  AppendInteger(bytes, request.writes.size());
  for (const auto& write : request.writes) {
    AppendText(bytes, write.collection);
    AppendText(bytes, write.key);
    AppendInteger(bytes, write.expected.value_or(0));
    bytes.push_back(write.data ? std::byte{1} : std::byte{0});
    if (write.data) {
      AppendInteger(bytes, write.data->size());
      bytes.insert(bytes.end(), write.data->begin(), write.data->end());
    }
  }
  return bytes;
}

db::Result<kit::StoreValue> ReadValue(const db::RowView& row) {
  auto collection = row.Get<std::string>(0);
  if (!collection) return collection.Error();
  auto key = row.Get<std::string>(1);
  if (!key) return key.Error();
  auto revision = RevisionColumn(row, 2);
  if (!revision) return revision.Error();
  auto data = row.Get<Bytes>(3);
  if (!data) return data.Error();
  return kit::StoreValue{std::move(collection).Value(), std::move(key).Value(), *revision, std::move(data).Value()};
}

db::Result<kit::StoreImage> Load(db::Transaction& txn) {
  auto revision = CurrentRevision(txn);
  if (!revision) return revision.Error();
  auto rows = txn.Query<kit::StoreValue>(
      "SELECT collection, record_key, revision, data FROM agentkit_store_records ORDER BY collection, record_key",
      ReadValue);
  if (!rows) return rows.Error();
  return kit::StoreImage{*revision, std::move(rows).Value()};
}

using CommitRow = std::pair<kit::Revision, Bytes>;

db::Result<CommitRow> DecodeCommit(const db::RowView& row) {
  auto revision = RevisionColumn(row, 0);
  if (!revision) return revision.Error();
  auto payload = row.Get<Bytes>(1);
  if (!payload) return payload.Error();
  return CommitRow{*revision, std::move(payload).Value()};
}

db::Result<kit::AgentResult<kit::Revision>> Commit(db::Transaction& txn, kit::StoreTransaction request) {
  auto current = CurrentRevision(txn);
  if (!current) return current.Error();
  auto existing = txn.Query<CommitRow>(
      "SELECT revision, payload FROM agentkit_store_commits WHERE commit_id = ?", DecodeCommit, request.commit.value);
  if (!existing) return existing.Error();
  const auto payload = CommitPayload(request);
  if (!existing->empty()) {
    const auto& [revision, recorded] = existing->front();
    return recorded == payload ? kit::AgentResult<kit::Revision>::Success(revision)
        : Fail<kit::Revision>(kit::ErrorCode::Conflict, "Commit ID already has a different payload", *current);
  }
  if (*current != request.expected_store_revision) {
    return Fail<kit::Revision>(kit::ErrorCode::Conflict, "Store revision changed", *current);
  }
  if (*current >= max_revision) {
    return Fail<kit::Revision>(kit::ErrorCode::PersistenceFailure, "Store revision exhausted", *current);
  }
  for (const auto& write : request.writes) {
    auto target = txn.Query<kit::Revision>(
        "SELECT revision FROM agentkit_store_records WHERE collection = ? AND record_key = ?",
        [](const db::RowView& row) { return RevisionColumn(row, 0); }, write.collection, write.key);
    if (!target) return target.Error();
    const auto found = target->empty() ? std::nullopt : std::optional<kit::Revision>(target->front());
    if (found != write.expected) {
      return Fail<kit::Revision>(kit::ErrorCode::Conflict, "Store record revision changed", found);
    }
  }

  const auto revision = static_cast<std::int64_t>(*current + 1);
  for (const auto& write : request.writes) {
    auto applied = write.data ? txn.Execute(
        "INSERT INTO agentkit_store_records(collection, record_key, revision, data) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(collection, record_key) DO UPDATE SET revision = excluded.revision, data = excluded.data",
        write.collection, write.key, revision, *write.data)
        : txn.Execute("DELETE FROM agentkit_store_records WHERE collection = ? AND record_key = ?",
                      write.collection, write.key);
    if (!applied) return applied.Error();
  }
  auto recorded = txn.Execute(
      "INSERT INTO agentkit_store_commits(commit_id, revision, payload) VALUES (?, ?, ?)",
      request.commit.value, revision, payload);
  if (!recorded) return recorded.Error();
  auto published = txn.Execute("UPDATE agentkit_store_meta SET revision = ? WHERE singleton = 1", revision);
  if (!published) return published.Error();
  return kit::AgentResult<kit::Revision>::Success(static_cast<kit::Revision>(revision));
}

kit::Async<kit::StoreImage> LoadImage(db::Database database) {
  auto loaded = co_await database.TransactionAsync(Load);
  if (!loaded) co_return StoreError<kit::StoreImage>(loaded.Error());
  co_return kit::AgentResult<kit::StoreImage>::Success(std::move(loaded).Value());
}

kit::Async<kit::Revision> WriteTransaction(db::Database database, kit::StoreTransaction request) {
  auto validated = storage::NormalizeCommit(request);
  if (!validated.Succeeded()) co_return kit::AgentResult<kit::Revision>::Failure(validated.Error());
  auto transaction = [request = std::move(request)](db::Transaction& txn) mutable {
    return Commit(txn, std::move(request));
  };
  auto committed = co_await database.TransactionAsync(std::move(transaction));
  if (!committed) co_return StoreError<kit::Revision>(committed.Error());
  co_return std::move(committed).Value();
}

kit::Async<std::optional<kit::Revision>> FindCommit(db::Database database, kit::CommitId commit) {
  if (commit.value.empty()) co_return Fail<std::optional<kit::Revision>>(kit::ErrorCode::InvalidArgument, "Commit ID is required");
  auto result = co_await database.QueryAsync<kit::Revision>(
      "SELECT revision FROM agentkit_store_commits WHERE commit_id = ?",
      [](const db::RowView& row) { return RevisionColumn(row, 0); }, commit.value);
  if (!result) co_return StoreError<std::optional<kit::Revision>>(result.Error());
  co_return kit::AgentResult<std::optional<kit::Revision>>::Success(
      result->empty() ? std::nullopt : std::optional<kit::Revision>(result->front()));
}

class SqliteSessionStore final : public kit::SessionStore {
public:
  explicit SqliteSessionStore(db::Database database) : database_(std::move(database)) {}

  kit::Async<kit::StoreImage> LoadAsync() override { return LoadImage(database_); }

  kit::Async<kit::Revision> CommitAsync(kit::StoreTransaction request) override {
    return WriteTransaction(database_, std::move(request));
  }

  kit::Async<std::optional<kit::Revision>> FindCommitAsync(kit::CommitId commit) override {
    return FindCommit(database_, std::move(commit));
  }

private:
  db::Database database_;
};

} // namespace

db::Result<void> InstallSqliteSessionSchema(db::Transaction& txn) {
  for (const auto* statement : {
           "CREATE TABLE IF NOT EXISTS agentkit_store_meta("
           "singleton INTEGER PRIMARY KEY CHECK(singleton = 1), schema_version INTEGER NOT NULL, "
           "revision INTEGER NOT NULL CHECK(revision >= 0))",
           "CREATE TABLE IF NOT EXISTS agentkit_store_records("
           "collection TEXT NOT NULL, record_key TEXT NOT NULL, revision INTEGER NOT NULL CHECK(revision > 0), "
           "data BLOB NOT NULL, PRIMARY KEY(collection, record_key)) WITHOUT ROWID",
           "CREATE TABLE IF NOT EXISTS agentkit_store_commits("
           "commit_id TEXT PRIMARY KEY NOT NULL, revision INTEGER UNIQUE NOT NULL CHECK(revision > 0), "
           "payload BLOB NOT NULL) WITHOUT ROWID",
           "INSERT OR IGNORE INTO agentkit_store_meta(singleton, schema_version, revision) VALUES(1, 1, 0)"}) {
    auto result = txn.Execute(statement);
    if (!result) return result.Error();
  }
  auto revision = CurrentRevision(txn);
  if (!revision) return revision.Error();
  return {};
}

std::shared_ptr<kit::SessionStore> MakeSqliteSessionStore(db::Database database) {
  return std::make_shared<SqliteSessionStore>(std::move(database));
}

kit::Async<std::shared_ptr<kit::SessionStore>> OpenSqliteSessionStoreAsync(huxerui::File file) {
  using Store = std::shared_ptr<kit::SessionStore>;
  auto database = co_await db::Database::OpenAsync(std::move(file), {.create_parent_directories = true});
  if (!database) co_return StoreError<Store>(database.Error());
  auto schema = co_await database->TransactionAsync(InstallSqliteSessionSchema);
  if (!schema) co_return StoreError<Store>(schema.Error());
  co_return kit::AgentResult<Store>::Success(MakeSqliteSessionStore(std::move(database).Value()));
}

} // namespace agentkit_host
