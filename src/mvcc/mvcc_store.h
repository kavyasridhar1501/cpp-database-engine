#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/config.h"

namespace dbengine {

// Snapshot-isolation-style multi-version concurrency control over an
// in-memory key/value table. Deliberately not a StorageEngine, since that
// interface has no room for transaction boundaries or isolation levels.
// Plain SNAPSHOT isolation still allows write skew; only
// SERIALIZABLE_SNAPSHOT closes that hole.
//
// Concurrency-safety contract: exactly one thread drives a given txn_id at a
// time (one-thread-per-session). Different txn_ids may run fully
// concurrently on different threads.
enum class IsolationLevel {
  READ_UNCOMMITTED,
  READ_COMMITTED,
  SNAPSHOT,
  SERIALIZABLE_SNAPSHOT,
};

class MVCCStore {
 public:
  MVCCStore() = default;
  MVCCStore(const MVCCStore&) = delete;
  MVCCStore& operator=(const MVCCStore&) = delete;

  int64_t Begin(IsolationLevel level = IsolationLevel::SNAPSHOT);

  // Returns false if the key is absent or deleted as of what txn_id can see.
  bool Read(int64_t txn_id, KeyType key, std::string* value);
  void Write(int64_t txn_id, KeyType key, const std::string& value);
  // Returns whether the key existed (visible to txn_id) before the delete.
  bool Delete(int64_t txn_id, KeyType key);

  // Returns false if the transaction was aborted due to a write-write (or,
  // under SERIALIZABLE_SNAPSHOT, read-write) conflict.
  bool Commit(int64_t txn_id);
  void Abort(int64_t txn_id);

  // Number of versions currently stored for key (committed + pending).
  // Test/diagnostic use.
  size_t GetVersionCount(KeyType key) const;

  // Removes committed versions no longer visible to any active transaction's
  // snapshot. Returns the number of versions removed. Safe to call
  // concurrently with Begin/Read/Write/Commit/Abort on other keys/txns.
  size_t RunGarbageCollection();

 private:
  struct Version {
    int64_t txn_id;
    // Assigned at write time from global_ts_; gives a total order over
    // writes regardless of commit status, which is all READ_UNCOMMITTED
    // needs ("see the latest write, committed or not").
    int64_t seq;
    // Assigned at commit time from global_ts_; -1 until then.
    // READ_COMMITTED/SNAPSHOT/SERIALIZABLE_SNAPSHOT order by this instead.
    int64_t commit_ts = -1;
    bool committed = false;
    bool deleted = false;
    std::string value;
  };

  // One per key. std::list (not vector) specifically so a WriteRecord can
  // hold a stable iterator into it, giving O(1) commit/abort instead of a
  // re-search through the chain.
  struct VersionChain {
    std::mutex mutex;
    std::list<Version> versions;
  };

  struct WriteRecord {
    KeyType key;
    VersionChain* chain;
    std::list<Version>::iterator it;
  };

  struct Transaction {
    IsolationLevel isolation;
    int64_t start_ts;
    std::vector<WriteRecord> write_records;
    std::unordered_set<KeyType> write_set;
    std::unordered_set<KeyType> read_set;
  };

  // Sharded by txn_id so concurrent transactions on different threads
  // mostly avoid contending on the same mutex: every Begin/Commit touches
  // the transaction table, so a single global mutex here would bottleneck
  // multi-threaded throughput regardless of the per-key VersionChain
  // locking's granularity.
  struct TxnShard {
    std::mutex mutex;
    std::unordered_map<int64_t, std::unique_ptr<Transaction>> txns;
  };
  static constexpr size_t kTxnShardCount = 16;
  TxnShard& ShardFor(int64_t txn_id) { return txn_shards_[static_cast<uint64_t>(txn_id) % kTxnShardCount]; }

  VersionChain* GetOrCreateChain(KeyType key, bool create);
  // Looks up an active transaction by id. Relies on the one-thread-per-txn
  // contract: the returned pointer stays valid because no other thread can
  // be concurrently committing/aborting (and thus erasing) this txn_id.
  Transaction* GetTransaction(int64_t txn_id);
  void WriteInternal(Transaction* txn, int64_t txn_id, KeyType key, bool deleted,
                      const std::string& value);
  void AbortInternal(Transaction* txn);
  // True if `v` (already known committed) is visible to a transaction with
  // the given isolation/start_ts.
  static bool IsVisible(const Version& v, IsolationLevel isolation, int64_t start_ts);

  std::atomic<int64_t> global_ts_{0};
  std::atomic<int64_t> next_txn_id_{0};

  mutable std::shared_mutex table_mutex_;
  std::unordered_map<KeyType, std::unique_ptr<VersionChain>> table_;

  std::array<TxnShard, kTxnShardCount> txn_shards_;
};

}  // namespace dbengine
