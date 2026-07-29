#include "wal/wal_b_plus_tree_engine.h"

#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "buffer/lru_k_replacer.h"
#include "index/internal_page.h"
#include "index/leaf_page.h"

namespace dbengine {

namespace {

class WALBPlusTreeStorageIterator : public StorageIterator {
 public:
  explicit WALBPlusTreeStorageIterator(BPlusTree::Iterator it) : it_(std::move(it)) {}
  bool IsValid() const override { return !it_.IsEnd(); }
  void Next() override { ++it_; }
  KeyType GetKey() const override { return it_.GetKey(); }
  std::string GetValue() const override { return it_.GetValue(); }

 private:
  BPlusTree::Iterator it_;
};

// Given a logged operation (op, whether/what old value it had), returns the
// compensating (undo) action: what to apply to reverse it, expressed the
// same way a forward operation is (an op + optional value) so both live
// Abort() and recovery's REDO phase can treat a CLR exactly like an UPDATE.
void ComputeCompensation(LogOpType original_op, bool has_old_value, const std::string& old_value,
                         LogOpType* comp_op, std::string* comp_value, bool* comp_has_value) {
  if (original_op == LogOpType::PUT) {
    if (has_old_value) {
      *comp_op = LogOpType::PUT;
      *comp_value = old_value;
      *comp_has_value = true;
    } else {
      *comp_op = LogOpType::DELETE;
      *comp_has_value = false;
    }
  } else {
    // A logged DELETE always had an old value (Delete() on an absent key
    // logs nothing — see Delete(txn_id, ...) below), so undoing it always
    // restores that value.
    *comp_op = LogOpType::PUT;
    *comp_value = old_value;
    *comp_has_value = true;
  }
}

}  // namespace

WALBPlusTreeEngine::WALBPlusTreeEngine(const std::string& db_path, size_t buffer_pool_size,
                                       bool enable_wal, int checkpoint_interval_txns)
    : wal_enabled_(enable_wal), checkpoint_interval_txns_(checkpoint_interval_txns) {
  disk_manager_ = std::make_unique<DiskManager>(db_path);
  bpm_ = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager_.get(),
                                              std::make_unique<LRUKReplacer>(buffer_pool_size, 2));

  page_id_t root_page_id = INVALID_PAGE_ID;
  if (disk_manager_->GetNumPages() == 0) {
    page_id_t meta_id;
    Page* meta = bpm_->NewPage(&meta_id);
    // The page arrives zero-filled, not default-constructed — MetaPage's
    // default member initializers never run, so every field must be set
    // explicitly here rather than relying on them.
    auto* mp = reinterpret_cast<MetaPage*>(meta->GetData());
    mp->root_page_id = INVALID_PAGE_ID;
    mp->last_checkpoint_begin_lsn = -1;
    bpm_->UnpinPage(meta_id, true);
  } else {
    Page* meta = bpm_->FetchPage(kMetaPageId);
    root_page_id = reinterpret_cast<MetaPage*>(meta->GetData())->root_page_id;
    bpm_->UnpinPage(kMetaPageId, false);
  }
  tree_ = std::make_unique<BPlusTree>(bpm_.get(), root_page_id, LeafPage::Capacity() - 1,
                                      InternalPage::Capacity() - 1);

  if (wal_enabled_) {
    log_manager_ = std::make_unique<LogManager>(db_path + ".wal");
    RecoverOnStartup();
  }
}

void WALBPlusTreeEngine::MaybeUpdateRootPageId() {
  Page* meta = bpm_->FetchPage(kMetaPageId);
  auto* mp = reinterpret_cast<MetaPage*>(meta->GetData());
  bool changed = mp->root_page_id != tree_->GetRootPageId();
  if (changed) {
    mp->root_page_id = tree_->GetRootPageId();
  }
  bpm_->UnpinPage(kMetaPageId, changed);
}

// ---------------------------------------------------------------------------
// StorageEngine (implicit single-op transactions)
// ---------------------------------------------------------------------------

bool WALBPlusTreeEngine::Get(KeyType key, std::string* value) { return tree_->GetValue(key, value); }

void WALBPlusTreeEngine::Put(KeyType key, const std::string& value) {
  int64_t txn_id = Begin();
  Put(txn_id, key, value);
  Commit(txn_id);
}

bool WALBPlusTreeEngine::Delete(KeyType key) {
  int64_t txn_id = Begin();
  bool existed = Delete(txn_id, key);
  Commit(txn_id);
  return existed;
}

std::unique_ptr<StorageIterator> WALBPlusTreeEngine::Scan(KeyType start_key) {
  BPlusTree::Iterator it = (start_key == INVALID_KEY) ? tree_->Begin() : tree_->Begin(start_key);
  return std::make_unique<WALBPlusTreeStorageIterator>(std::move(it));
}

// ---------------------------------------------------------------------------
// Explicit transactions
// ---------------------------------------------------------------------------

int64_t WALBPlusTreeEngine::Begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t txn_id = next_txn_id_++;
  TxnState state;
  if (wal_enabled_) {
    LogRecord rec;
    rec.txn_id = txn_id;
    rec.type = LogRecordType::BEGIN;
    rec.prev_lsn = -1;
    state.last_lsn = log_manager_->Append(rec);
  }
  active_txns_.emplace(txn_id, std::move(state));
  return txn_id;
}

void WALBPlusTreeEngine::Put(int64_t txn_id, KeyType key, const std::string& value) {
  if (value.size() > MAX_VALUE_SIZE) {
    throw std::invalid_argument("WALBPlusTreeEngine::Put: value exceeds MAX_VALUE_SIZE");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_txns_.find(txn_id);
  if (it == active_txns_.end()) {
    throw std::runtime_error("WALBPlusTreeEngine::Put: unknown or already-finished transaction");
  }
  TxnState& state = it->second;

  std::string old_value;
  bool had_old = tree_->GetValue(key, &old_value);

  UndoEntry entry;
  entry.op = LogOpType::PUT;
  entry.key = key;
  entry.has_old_value = had_old;
  entry.old_value = had_old ? old_value : std::string();
  entry.undo_next_lsn = state.last_lsn;

  if (wal_enabled_) {
    LogRecord rec;
    rec.txn_id = txn_id;
    rec.type = LogRecordType::UPDATE;
    rec.op = LogOpType::PUT;
    rec.key = key;
    rec.prev_lsn = state.last_lsn;
    if (had_old) {
      rec.SetOldValue(old_value);
    }
    rec.SetNewValue(value);
    state.last_lsn = log_manager_->Append(rec);
  }
  state.undo_log.push_back(std::move(entry));

  tree_->Insert(key, value);
  MaybeUpdateRootPageId();
}

bool WALBPlusTreeEngine::Delete(int64_t txn_id, KeyType key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_txns_.find(txn_id);
  if (it == active_txns_.end()) {
    throw std::runtime_error("WALBPlusTreeEngine::Delete: unknown or already-finished transaction");
  }
  TxnState& state = it->second;

  std::string old_value;
  bool had_old = tree_->GetValue(key, &old_value);
  if (!had_old) {
    return false;
  }

  UndoEntry entry;
  entry.op = LogOpType::DELETE;
  entry.key = key;
  entry.has_old_value = true;
  entry.old_value = old_value;
  entry.undo_next_lsn = state.last_lsn;

  if (wal_enabled_) {
    LogRecord rec;
    rec.txn_id = txn_id;
    rec.type = LogRecordType::UPDATE;
    rec.op = LogOpType::DELETE;
    rec.key = key;
    rec.prev_lsn = state.last_lsn;
    rec.SetOldValue(old_value);
    state.last_lsn = log_manager_->Append(rec);
  }
  state.undo_log.push_back(std::move(entry));

  tree_->Remove(key);
  MaybeUpdateRootPageId();
  return true;
}

bool WALBPlusTreeEngine::Get(int64_t txn_id, KeyType key, std::string* value) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_txns_.find(txn_id) == active_txns_.end()) {
      throw std::runtime_error("WALBPlusTreeEngine::Get: unknown or already-finished transaction");
    }
  }
  return tree_->GetValue(key, value);
}

void WALBPlusTreeEngine::Commit(int64_t txn_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = active_txns_.find(txn_id);
  if (it == active_txns_.end()) {
    throw std::runtime_error("WALBPlusTreeEngine::Commit: unknown or already-finished transaction");
  }

  if (wal_enabled_) {
    LogRecord rec;
    rec.txn_id = txn_id;
    rec.type = LogRecordType::COMMIT;
    rec.prev_lsn = it->second.last_lsn;
    log_manager_->Append(rec);
    // COMMIT must be fsynced before Commit() returns to the caller.
    log_manager_->Flush();
  }
  active_txns_.erase(it);

  ++txns_since_checkpoint_;
  if (wal_enabled_ && txns_since_checkpoint_ >= checkpoint_interval_txns_) {
    DoCheckpoint();
    txns_since_checkpoint_ = 0;
  }
}

void WALBPlusTreeEngine::Abort(int64_t txn_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = active_txns_.find(txn_id);
  if (it == active_txns_.end()) {
    throw std::runtime_error("WALBPlusTreeEngine::Abort: unknown or already-finished transaction");
  }
  std::vector<UndoEntry> entries = std::move(it->second.undo_log);
  int64_t last_lsn = it->second.last_lsn;
  active_txns_.erase(it);

  if (wal_enabled_) {
    LogRecord abort_rec;
    abort_rec.txn_id = txn_id;
    abort_rec.type = LogRecordType::ABORT;
    abort_rec.prev_lsn = last_lsn;
    last_lsn = log_manager_->Append(abort_rec);
  }

  for (auto rit = entries.rbegin(); rit != entries.rend(); ++rit) {
    const UndoEntry& entry = *rit;
    LogOpType comp_op;
    std::string comp_value;
    bool comp_has_value;
    ComputeCompensation(entry.op, entry.has_old_value, entry.old_value, &comp_op, &comp_value,
                        &comp_has_value);

    if (comp_op == LogOpType::PUT) {
      tree_->Insert(entry.key, comp_value);
    } else {
      tree_->Remove(entry.key);
    }
    MaybeUpdateRootPageId();

    if (wal_enabled_) {
      LogRecord clr;
      clr.txn_id = txn_id;
      clr.type = LogRecordType::CLR;
      clr.op = comp_op;
      clr.key = entry.key;
      clr.prev_lsn = last_lsn;
      clr.undo_next_lsn = entry.undo_next_lsn;
      if (comp_has_value) {
        clr.SetNewValue(comp_value);
      }
      last_lsn = log_manager_->Append(clr);
    }
  }
  if (wal_enabled_) {
    log_manager_->Flush();
  }

  ++txns_since_checkpoint_;
  if (wal_enabled_ && txns_since_checkpoint_ >= checkpoint_interval_txns_) {
    DoCheckpoint();
    txns_since_checkpoint_ = 0;
  }
}

// ---------------------------------------------------------------------------
// Checkpointing
// ---------------------------------------------------------------------------

void WALBPlusTreeEngine::DoCheckpoint() {
  // Sharp checkpoint: flush every dirty page first, so the only updates not
  // yet on disk belong to transactions still active right now (recorded
  // below) — avoids needing a per-page dirty-page table during recovery.
  bpm_->FlushAllPages();

  LogRecord begin_rec;
  begin_rec.type = LogRecordType::CHECKPOINT_BEGIN;
  int64_t begin_lsn = log_manager_->Append(begin_rec);

  for (const auto& [txn_id, state] : active_txns_) {
    LogRecord active_rec;
    active_rec.type = LogRecordType::ACTIVE_TXN;
    active_rec.active_txn_id = txn_id;
    active_rec.active_txn_last_lsn = state.last_lsn;
    log_manager_->Append(active_rec);
  }

  LogRecord end_rec;
  end_rec.type = LogRecordType::CHECKPOINT_END;
  log_manager_->Append(end_rec);

  log_manager_->Flush();

  // Record where this checkpoint starts so recovery can jump straight to it
  // instead of scanning the whole log.
  Page* meta = bpm_->FetchPage(kMetaPageId);
  reinterpret_cast<MetaPage*>(meta->GetData())->last_checkpoint_begin_lsn = begin_lsn;
  bpm_->UnpinPage(kMetaPageId, true);
  bpm_->FlushPage(kMetaPageId);

  ++checkpoint_count_;
}

// ---------------------------------------------------------------------------
// Recovery: Analysis, Redo, Undo
// ---------------------------------------------------------------------------

void WALBPlusTreeEngine::RecoverOnStartup() {
  int64_t next_lsn = log_manager_->GetNextLsn();
  if (next_lsn == 0) {
    return;  // empty log.
  }

  // Jump straight to the last checkpoint via the meta page's pointer; its
  // bracket (CHECKPOINT_BEGIN, ACTIVE_TXN records, CHECKPOINT_END) is read
  // directly by LSN, and only records from the resulting redo_start_lsn
  // onward are read via ReadFrom() — recovery time tracks log-since-
  // checkpoint, not total log size.
  int64_t redo_start_lsn = 0;
  std::vector<std::pair<int64_t, int64_t>> checkpoint_active_txns;
  {
    Page* meta = bpm_->FetchPage(kMetaPageId);
    int64_t checkpoint_begin_lsn = reinterpret_cast<MetaPage*>(meta->GetData())->last_checkpoint_begin_lsn;
    bpm_->UnpinPage(kMetaPageId, false);

    if (checkpoint_begin_lsn >= 0 && checkpoint_begin_lsn < next_lsn &&
        log_manager_->ReadRecord(checkpoint_begin_lsn).type == LogRecordType::CHECKPOINT_BEGIN) {
      std::vector<std::pair<int64_t, int64_t>> candidate_active;
      int64_t lsn = checkpoint_begin_lsn + 1;
      bool found_end = false;
      while (lsn < next_lsn) {
        LogRecord rec = log_manager_->ReadRecord(lsn);
        if (rec.type == LogRecordType::ACTIVE_TXN) {
          candidate_active.emplace_back(rec.active_txn_id, rec.active_txn_last_lsn);
        } else if (rec.type == LogRecordType::CHECKPOINT_END) {
          found_end = true;
          break;
        } else {
          break;  // malformed bracket; fall back to scanning from the start.
        }
        ++lsn;
      }
      if (found_end) {
        int64_t min_lsn = checkpoint_begin_lsn;
        for (const auto& [txn_id, last_lsn] : candidate_active) {
          (void)txn_id;
          if (last_lsn >= 0 && last_lsn < min_lsn) {
            min_lsn = last_lsn;
          }
        }
        redo_start_lsn = min_lsn;
        checkpoint_active_txns = std::move(candidate_active);
      }
    }
  }

  auto records = log_manager_->ReadFrom(redo_start_lsn);
  if (records.empty()) {
    return;
  }

  // ANALYSIS: seed the active-transaction table from the checkpoint, then
  // scan forward to find every transaction that never committed — the
  // "losers" that Undo must roll back.
  std::unordered_map<int64_t, int64_t> active;
  for (const auto& [txn_id, last_lsn] : checkpoint_active_txns) {
    active[txn_id] = last_lsn;
  }
  for (const auto& rec : records) {
    switch (rec.type) {
      case LogRecordType::BEGIN:
      case LogRecordType::UPDATE:
      case LogRecordType::CLR:
        active[rec.txn_id] = rec.lsn;
        break;
      case LogRecordType::COMMIT:
        active.erase(rec.txn_id);
        break;
      default:
        break;
    }
  }

  // REDO: replay every UPDATE/CLR from the redo start point forward, in
  // order, regardless of which transaction they belong to — including
  // eventual losers, exactly reconstructing the state at crash time before
  // Undo rolls the losers back.
  for (const auto& rec : records) {
    if (rec.type == LogRecordType::UPDATE || rec.type == LogRecordType::CLR) {
      if (rec.op == LogOpType::PUT) {
        tree_->Insert(rec.key, rec.NewValue());
      } else if (rec.op == LogOpType::DELETE) {
        tree_->Remove(rec.key);
      }
      MaybeUpdateRootPageId();
    }
  }

  // UNDO: roll back every loser transaction. Each transaction's undo chain
  // is independent (logical, key-level undo, not physical page-level undo),
  // so processing losers one at a time is correct here, unlike ARIES's
  // globally LSN-interleaved order which matters for page latching.
  for (const auto& [txn_id, last_lsn] : active) {
    UndoTransactionFromLog(txn_id, last_lsn);
  }
}

void WALBPlusTreeEngine::UndoTransactionFromLog(int64_t txn_id, int64_t start_lsn) {
  int64_t lsn = start_lsn;
  while (lsn != -1) {
    LogRecord rec = log_manager_->ReadRecord(lsn);

    if (rec.type == LogRecordType::BEGIN) {
      break;
    }
    if (rec.type == LogRecordType::CLR) {
      // Already compensated in a previous, interrupted undo attempt; skip
      // straight to its resume point without redoing the compensation.
      lsn = rec.undo_next_lsn;
      continue;
    }
    if (rec.type == LogRecordType::UPDATE) {
      LogOpType comp_op;
      std::string comp_value;
      bool comp_has_value;
      ComputeCompensation(rec.op, rec.has_old_value != 0, rec.OldValue(), &comp_op, &comp_value,
                          &comp_has_value);

      if (comp_op == LogOpType::PUT) {
        tree_->Insert(rec.key, comp_value);
      } else {
        tree_->Remove(rec.key);
      }
      MaybeUpdateRootPageId();

      LogRecord clr;
      clr.txn_id = txn_id;
      clr.type = LogRecordType::CLR;
      clr.op = comp_op;
      clr.key = rec.key;
      clr.undo_next_lsn = rec.prev_lsn;
      if (comp_has_value) {
        clr.SetNewValue(comp_value);
      }
      log_manager_->Append(clr);
    }
    lsn = rec.prev_lsn;
  }
  log_manager_->Flush();
}

size_t WALBPlusTreeEngine::GetLogSizeBytes() const {
  return wal_enabled_ ? log_manager_->GetLogSizeBytes() : 0;
}

}  // namespace dbengine
