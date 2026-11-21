#pragma once

#include "session_store_validation.h"

#include <huxerui/file.h>
#include <huxerui/sqlite.h>

namespace agentkit_host {

// Install session tables in the caller's transaction so a host can initialize its schema together with them.
huxerui::sqlite::Result<void> InstallSqliteSessionSchema(huxerui::sqlite::Transaction& transaction);

// Use an open database whose session schema has already been installed.
std::shared_ptr<huxerui::agentkit::SessionStore> MakeSqliteSessionStore(huxerui::sqlite::Database database);

// Open a database and install only the session schema.
huxerui::agentkit::Async<std::shared_ptr<huxerui::agentkit::SessionStore>> OpenSqliteSessionStoreAsync(huxerui::File file);

namespace storage {

template<class T>
huxerui::agentkit::AgentResult<T> StoreError(const huxerui::sqlite::Error& error) {
  return Fail<T>(error.Operation() == "rollback transaction" ? kit::ErrorCode::CommitUncertain
                                                            : kit::ErrorCode::PersistenceFailure,
                 error.Message());
}

// Decode SQLite's signed revision without admitting negative values.
huxerui::sqlite::Result<huxerui::agentkit::Revision> RevisionColumn(const huxerui::sqlite::RowView& row, std::size_t column);

} // namespace storage

} // namespace agentkit_host
