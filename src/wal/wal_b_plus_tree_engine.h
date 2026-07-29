#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "engine/storage_engine.h"
#include "index/b_plus_tree.h"
#include "storage/disk/disk_manager.h"
#include "wal/log_manager.h"
#include "wal/log_record.h"

namespace dbengine {

// A B+-tree StorageEngine with ARIES-style write-ahead logging and crash
// recovery. StorageEngine's Get/Put/Delete/Scan are each an implicit
// single-operation transaction; Begin/Put/Delete/Get/Commit/Abort below
// expose multi-operation transactions explicitly.
//
// Logging is *logical* (an UPDATE record is "Put(key, new_value)" or
// "Delete(key)" plus the prior value, not a page byte-image). Redo replays
// logical ops and is idempotent; undo applies the inverse op, writing a
// Compensation Log Record (CLR) per step so an undo interrupted by another
// crash can resume without redoing or re-undoing work.
//
// Checkpoints are "sharp": DoCheckpoint() flushes every dirty page first,
// trading checkpoint cost for not needing a per-page dirty-page table
// during recovery.
class WALBPlusTreeEngine : public StorageEngine {
 public:
  explicit WALBPlusTreeEngine(const std::string& db_path, size_t buffer_pool_size,
                              bool enable_wal = true, int checkpoint_interval_txns = 100);
  ~WALBPlusTreeEngine() override = default;

  WALBPlusTreeEngine(const WALBPlusTreeEngine&) = delete;
  WALBPlusTreeEngine& operator=(const WALBPlusTreeEngine&) = delete;

  // StorageEngine: each call is its own implicit single-operation
  // transaction (Begin, the operation, Commit).
  bool Get(KeyType key, std::string* value) override;
  void Put(KeyType key, const std::string& value) override;
  bool Delete(KeyType key) override;
  std::unique_ptr<StorageIterator> Scan(KeyType start_key) override;

  // Explicit multi-operation transactions. Reads see whatever is currently
  // in the tree (including any transaction's uncommitted writes) — there's
  // no isolation. What these provide is atomicity and durability across a
  // crash: either every operation under a committed txn_id survives
  // recovery, or none of them do.
  int64_t Begin();
  void Put(int64_t txn_id, KeyType key, const std::string& value);
  bool Delete(int64_t txn_id, KeyType key);
  bool Get(int64_t txn_id, KeyType key, std::string* value) const;
  void Commit(int64_t txn_id);
  void Abort(int64_t txn_id);

  bool IsWALEnabled() const { return wal_enabled_; }
  size_t GetLogSizeBytes() const;
  size_t GetCheckpointCount() const { return checkpoint_count_; }

 private:
  static constexpr page_id_t kMetaPageId = 0;
  struct MetaPage {
    page_id_t root_page_id;
    // LSN of the most recent *complete* checkpoint's CHECKPOINT_BEGIN
    // record, or -1 if none yet. Lets RecoverOnStartup() jump straight to
    // the checkpoint instead of scanning the whole log to find it; without
    // this, recovery time would be bounded by total log size regardless of
    // checkpoint frequency, defeating the point.
    int64_t last_checkpoint_begin_lsn = -1;
  };

  // What's needed to undo one logical operation: the inverse action
  // (restore old_value, or delete if there wasn't one) plus the LSN a CLR
  // compensating it should record as its resume point.
  struct UndoEntry {
    LogOpType op;
    KeyType key;
    bool has_old_value;
    std::string old_value;
    int64_t undo_next_lsn;
  };
  struct TxnState {
    int64_t last_lsn = -1;
    std::vector<UndoEntry> undo_log;
  };

  void MaybeUpdateRootPageId();
  void DoCheckpoint();  // caller must hold mutex_.
  void RecoverOnStartup();
  void UndoTransactionFromLog(int64_t txn_id, int64_t start_lsn);

  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<BPlusTree> tree_;
  std::unique_ptr<LogManager> log_manager_;  // null if !wal_enabled_.
  bool wal_enabled_;
  int checkpoint_interval_txns_;
  int txns_since_checkpoint_ = 0;
  size_t checkpoint_count_ = 0;

  mutable std::mutex mutex_;
  int64_t next_txn_id_ = 0;
  std::unordered_map<int64_t, TxnState> active_txns_;
};

}  // namespace dbengine
