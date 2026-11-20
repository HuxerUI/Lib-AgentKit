#pragma once

/// @file
/// @brief Transactional persistence for AgentRuntime.
/// Include <huxerui/agentkit/store.h> and link HuxerUI::AgentKit. Implement SessionStore to reopen sessions,
/// pending work and project state. The runtime owns record encoding. The store owns atomicity and commit receipts.

#include <huxerui/agentkit/types.h>

namespace huxerui::agentkit {

/// @brief One conditional write or delete in a StoreTransaction.
struct StoreWrite {
  /// Runtime-owned collection name.
  std::string collection;
  /// Record key within the collection.
  std::string key;
  /// Expected record revision. Empty requires the key to be absent.
  std::optional<Revision> expected;
  /// Encoded replacement. Empty deletes the record.
  std::optional<std::vector<std::byte>> data;
};

/// @brief One live record returned by SessionStore::LoadAsync().
struct StoreValue {
  /// Collection containing the record.
  std::string collection;
  /// Key within the collection.
  std::string key;
  /// Revision to match in a subsequent StoreWrite.
  Revision revision;
  /// Opaque bytes owned by the runtime's storage schema.
  std::vector<std::byte> data;
};

/// @brief A consistent snapshot of committed runtime records.
struct StoreImage {
  /// Global store revision. Zero represents a new store.
  Revision revision = 0;
  /// Live records across all collections at revision.
  std::vector<StoreValue> records;
};

/// @brief An atomic, replayable batch formed by the runtime.
struct StoreTransaction {
  /// Stable commit ID. Replaying the same batch returns the original revision.
  CommitId commit;
  /// Global revision that must match before a new commit.
  Revision expected_store_revision;
  /// Record mutations committed together with the receipt.
  std::vector<StoreWrite> writes;
};

/// @brief Host storage interface used by RuntimeOptions::store.
/// The runtime checks FindCommitAsync() after an uncertain submission. Store implementations compare the original
/// transaction on replay and reject different data submitted under the same CommitId.
class SessionStore {
public:
  /// @brief Releases the host storage implementation.
  virtual ~SessionStore() = default;

  /// @brief Loads one consistent StoreImage, including all live record revisions.
  /// @return The store image at one committed revision, including live records needed for recovery.
  virtual Async<StoreImage> LoadAsync() = 0;

  /// @brief Atomically checks the store and record revisions, applies writes, and records the commit ID.
  /// @param transaction Owned batch to commit or replay.
  /// @return The committed store revision. A precondition mismatch returns ErrorCode::Conflict.
  virtual Async<Revision> CommitAsync(StoreTransaction transaction) = 0;

  /// @brief Looks up an existing commit receipt without applying writes.
  /// @return The committed revision, or an empty value if no receipt exists.
  /// @param commit Stable ID from the original StoreTransaction.
  virtual Async<std::optional<Revision>> FindCommitAsync(CommitId commit) = 0;
};

} // namespace huxerui::agentkit
