#include "mvcc/mvcc_store.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace dbengine {

int64_t MVCCStore::Begin(IsolationLevel level) {
  int64_t txn_id = next_txn_id_.fetch_add(1);
  int64_t start_ts = global_ts_.fetch_add(1);

  auto txn = std::make_unique<Transaction>();
  txn->isolation = level;
  txn->start_ts = start_ts;

  TxnShard& shard = ShardFor(txn_id);
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.txns.emplace(txn_id, std::move(txn));
  return txn_id;
}

MVCCStore::Transaction* MVCCStore::GetTransaction(int64_t txn_id) {
  TxnShard& shard = ShardFor(txn_id);
  std::lock_guard<std::mutex> lock(shard.mutex);
  return shard.txns.at(txn_id).get();
}

MVCCStore::VersionChain* MVCCStore::GetOrCreateChain(KeyType key, bool create) {
  {
    std::shared_lock lock(table_mutex_);
    auto it = table_.find(key);
    if (it != table_.end()) return it->second.get();
  }
  if (!create) return nullptr;

  std::unique_lock lock(table_mutex_);
  auto it = table_.find(key);
  if (it != table_.end()) return it->second.get();
  auto chain = std::make_unique<VersionChain>();
  VersionChain* ptr = chain.get();
  table_.emplace(key, std::move(chain));
  return ptr;
}

bool MVCCStore::IsVisible(const Version& v, IsolationLevel isolation, int64_t start_ts) {
  switch (isolation) {
    case IsolationLevel::READ_COMMITTED:
      return true;
    case IsolationLevel::SNAPSHOT:
    case IsolationLevel::SERIALIZABLE_SNAPSHOT:
      return v.commit_ts <= start_ts;
    case IsolationLevel::READ_UNCOMMITTED:
    default:
      return true;
  }
}

bool MVCCStore::Read(int64_t txn_id, KeyType key, std::string* value) {
  Transaction* txn = GetTransaction(txn_id);
  txn->read_set.insert(key);

  VersionChain* chain = GetOrCreateChain(key, /*create=*/false);
  if (chain == nullptr) return false;

  std::lock_guard<std::mutex> lock(chain->mutex);

  // Read-your-own-writes: this txn's own pending (uncommitted) version wins
  // regardless of isolation level.
  for (const Version& v : chain->versions) {
    if (v.txn_id == txn_id && !v.committed) {
      if (v.deleted) return false;
      *value = v.value;
      return true;
    }
  }

  if (txn->isolation == IsolationLevel::READ_UNCOMMITTED) {
    const Version* best = nullptr;
    for (const Version& v : chain->versions) {
      if (best == nullptr || v.seq > best->seq) best = &v;
    }
    if (best == nullptr || best->deleted) return false;
    *value = best->value;
    return true;
  }

  const Version* best = nullptr;
  for (const Version& v : chain->versions) {
    if (!v.committed) continue;
    if (!IsVisible(v, txn->isolation, txn->start_ts)) continue;
    if (best == nullptr || v.commit_ts > best->commit_ts) best = &v;
  }
  if (best == nullptr || best->deleted) return false;
  *value = best->value;
  return true;
}

void MVCCStore::WriteInternal(Transaction* txn, int64_t txn_id, KeyType key, bool deleted,
                               const std::string& value) {
  VersionChain* chain = GetOrCreateChain(key, /*create=*/true);
  std::lock_guard<std::mutex> lock(chain->mutex);

  // A repeated write to the same key within one txn updates its existing
  // pending version in place instead of piling up duplicate pending
  // versions.
  for (auto it = chain->versions.begin(); it != chain->versions.end(); ++it) {
    if (it->txn_id == txn_id && !it->committed) {
      it->deleted = deleted;
      it->value = deleted ? std::string() : value;
      it->seq = global_ts_.fetch_add(1);
      return;
    }
  }

  Version v;
  v.txn_id = txn_id;
  v.seq = global_ts_.fetch_add(1);
  v.committed = false;
  v.deleted = deleted;
  v.value = deleted ? std::string() : value;
  chain->versions.push_back(std::move(v));
  auto it = std::prev(chain->versions.end());

  txn->write_records.push_back(WriteRecord{key, chain, it});
  txn->write_set.insert(key);
}

void MVCCStore::Write(int64_t txn_id, KeyType key, const std::string& value) {
  Transaction* txn = GetTransaction(txn_id);
  WriteInternal(txn, txn_id, key, /*deleted=*/false, value);
}

bool MVCCStore::Delete(int64_t txn_id, KeyType key) {
  std::string discard;
  bool existed = Read(txn_id, key, &discard);

  Transaction* txn = GetTransaction(txn_id);
  WriteInternal(txn, txn_id, key, /*deleted=*/true, "");
  return existed;
}

void MVCCStore::AbortInternal(Transaction* txn) {
  for (WriteRecord& wr : txn->write_records) {
    std::lock_guard<std::mutex> lock(wr.chain->mutex);
    wr.chain->versions.erase(wr.it);
  }
  txn->write_records.clear();
}

void MVCCStore::Abort(int64_t txn_id) {
  std::unique_ptr<Transaction> txn;
  TxnShard& shard = ShardFor(txn_id);
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.txns.find(txn_id);
    if (it == shard.txns.end()) return;
    txn = std::move(it->second);
    shard.txns.erase(it);
  }
  AbortInternal(txn.get());
}

bool MVCCStore::Commit(int64_t txn_id) {
  Transaction* txn = GetTransaction(txn_id);

  // Keys to conflict-check: the write set always; under
  // SERIALIZABLE_SNAPSHOT also the read set, so a read-write
  // anti-dependency (the write-skew pattern) is caught, not just
  // write-write conflicts.
  std::vector<KeyType> check_keys(txn->write_set.begin(), txn->write_set.end());
  if (txn->isolation == IsolationLevel::SERIALIZABLE_SNAPSHOT) {
    check_keys.insert(check_keys.end(), txn->read_set.begin(), txn->read_set.end());
  }
  std::sort(check_keys.begin(), check_keys.end());
  check_keys.erase(std::unique(check_keys.begin(), check_keys.end()), check_keys.end());

  // Lock every touched chain in a fixed (sorted-key) order so two
  // concurrently-committing transactions can never deadlock against each
  // other over the same pair of keys.
  std::vector<VersionChain*> chains;
  chains.reserve(check_keys.size());
  for (KeyType k : check_keys) {
    VersionChain* c = GetOrCreateChain(k, /*create=*/false);
    if (c != nullptr) chains.push_back(c);
  }

  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(chains.size());
  for (VersionChain* c : chains) locks.emplace_back(c->mutex);

  bool conflict = false;
  for (VersionChain* c : chains) {
    for (const Version& v : c->versions) {
      if (v.committed && v.txn_id != txn_id && v.commit_ts > txn->start_ts) {
        conflict = true;
        break;
      }
    }
    if (conflict) break;
  }

  if (conflict) {
    locks.clear();
    AbortInternal(txn);
    TxnShard& shard = ShardFor(txn_id);
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.txns.erase(txn_id);
    return false;
  }

  int64_t commit_ts = global_ts_.fetch_add(1);
  for (WriteRecord& wr : txn->write_records) {
    wr.it->committed = true;
    wr.it->commit_ts = commit_ts;
  }
  locks.clear();

  TxnShard& shard = ShardFor(txn_id);
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.txns.erase(txn_id);
  return true;
}

size_t MVCCStore::GetVersionCount(KeyType key) const {
  std::shared_lock lock(table_mutex_);
  auto it = table_.find(key);
  if (it == table_.end()) return 0;
  std::lock_guard<std::mutex> chain_lock(it->second->mutex);
  return it->second->versions.size();
}

size_t MVCCStore::RunGarbageCollection() {
  // Every active transaction's start_ts, since each one could in principle
  // be the txn whose snapshot still resolves to some middle version in a
  // chain — not just the oldest.
  std::vector<int64_t> active_starts;
  for (TxnShard& shard : txn_shards_) {
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& [id, txn] : shard.txns) active_starts.push_back(txn->start_ts);
  }
  std::sort(active_starts.begin(), active_starts.end());

  size_t removed = 0;
  std::shared_lock table_lock(table_mutex_);
  for (const auto& [key, chain_ptr] : table_) {
    VersionChain* chain = chain_ptr.get();
    std::lock_guard<std::mutex> lock(chain->mutex);
    if (chain->versions.size() <= 1) continue;

    std::vector<std::list<Version>::iterator> committed;
    for (auto it = chain->versions.begin(); it != chain->versions.end(); ++it) {
      if (it->committed) committed.push_back(it);
    }
    if (committed.size() <= 1) continue;
    std::sort(committed.begin(), committed.end(),
              [](const std::list<Version>::iterator& a, const std::list<Version>::iterator& b) {
                return a->commit_ts < b->commit_ts;
              });

    // The newest committed version is always kept (nothing to supersede
    // it). Every other committed version committed[i] is the visible
    // version for a snapshot with start_ts s exactly when
    // committed[i].commit_ts <= s < committed[i+1].commit_ts; it's safe to
    // reclaim iff no active transaction's start_ts falls in that range.
    for (size_t i = 0; i + 1 < committed.size(); ++i) {
      int64_t lo = committed[i]->commit_ts;
      int64_t hi = committed[i + 1]->commit_ts;
      bool needed = std::any_of(active_starts.begin(), active_starts.end(),
                                 [&](int64_t s) { return s >= lo && s < hi; });
      if (!needed) {
        chain->versions.erase(committed[i]);
        ++removed;
      }
    }
  }
  return removed;
}

}  // namespace dbengine
