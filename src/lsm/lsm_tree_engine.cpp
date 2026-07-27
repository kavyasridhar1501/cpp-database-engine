#include "lsm/lsm_tree_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "lsm/lsm_cursor.h"
#include "lsm/merge_iterator.h"
#include "wal/log_record.h"

namespace dbengine {

namespace {

struct ManifestPage {
  int64_t next_sstable_id;
  int32_t tier_count[LSMTreeEngine::kMaxManifestTiers];
  int64_t tier_ids[LSMTreeEngine::kMaxManifestTiers][LSMTreeEngine::kMaxSSTablesPerManifestTier];
};

// Wraps the merge iterator with a filter that hides tombstones (a Scan()
// caller never sees deleted keys) and keeps the snapshotted memtable alive
// for as long as the returned iterator is, since MemTableCursor itself only
// holds a raw skip-list iterator, not a reference-counted handle.
class LSMScanIterator : public StorageIterator {
 public:
  LSMScanIterator(std::shared_ptr<MemTable> mt, std::unique_ptr<LSMMergeIterator> merge)
      : mt_(std::move(mt)), merge_(std::move(merge)) {
    SkipTombstones();
  }

  bool IsValid() const override { return merge_->Valid(); }
  void Next() override {
    merge_->Next();
    SkipTombstones();
  }
  KeyType GetKey() const override { return merge_->Key(); }
  std::string GetValue() const override { return merge_->Value().value; }

 private:
  void SkipTombstones() {
    while (merge_->Valid() && merge_->Value().deleted) {
      merge_->Next();
    }
  }

  std::shared_ptr<MemTable> mt_;
  std::unique_ptr<LSMMergeIterator> merge_;
};

}  // namespace

LSMTreeEngine::LSMTreeEngine(const std::string& db_path, size_t memtable_flush_threshold_bytes,
                             int tier_compaction_threshold, bool enable_wal)
    : db_path_(db_path),
      flush_threshold_bytes_(memtable_flush_threshold_bytes),
      tier_threshold_(tier_compaction_threshold),
      wal_enabled_(enable_wal) {
  LoadManifestOrInitialize();
  active_memtable_ = std::make_shared<MemTable>();
  if (wal_enabled_) {
    RecoverMemWALOnStartup();
  }
  compaction_thread_ = std::thread(&LSMTreeEngine::CompactionLoop, this);
}

LSMTreeEngine::~LSMTreeEngine() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  compaction_cv_.notify_all();
  compaction_thread_.join();

  if (active_memtable_ && active_memtable_->EntryCount() > 0) {
    FlushActiveMemtable();
  }
}

std::string LSMTreeEngine::SSTablePath(int64_t id) const {
  return db_path_ + ".sst" + std::to_string(id);
}

std::string LSMTreeEngine::MemWALPath(int64_t generation) const {
  return db_path_ + ".memwal" + std::to_string(generation);
}

void LSMTreeEngine::RecoverMemWALOnStartup() {
  // Discover any generation files a crash left behind. Under normal
  // operation there's at most one (the currently-active generation); a
  // crash mid-flush can leave two — the generation being flushed (not yet
  // deleted, since deletion only happens after the SSTable build succeeds)
  // and the generation swapped in to replace it. Replaying all of them, in
  // ascending generation order, reconstructs the exact memtable state at
  // crash time regardless of which case applies.
  std::vector<int64_t> found_generations;
  std::filesystem::path db_path(db_path_);
  std::filesystem::path dir = db_path.parent_path();
  if (dir.empty()) {
    dir = ".";
  }
  std::string prefix = db_path.filename().string() + ".memwal";
  if (std::filesystem::exists(dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      std::string fname = entry.path().filename().string();
      if (fname.rfind(prefix, 0) != 0) continue;
      std::string suffix = fname.substr(prefix.size());
      if (!suffix.empty() &&
          std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); })) {
        found_generations.push_back(std::stoll(suffix));
      }
    }
  }
  std::sort(found_generations.begin(), found_generations.end());

  int64_t new_generation = found_generations.empty() ? 0 : found_generations.back() + 1;
  memtable_wal_ = std::make_unique<LogManager>(MemWALPath(new_generation));

  for (int64_t gen : found_generations) {
    LogManager old_wal(MemWALPath(gen));
    for (const auto& rec : old_wal.ReadAll()) {
      if (rec.op == LogOpType::PUT) {
        active_memtable_->Put(rec.key, rec.NewValue());
      } else if (rec.op == LogOpType::DELETE) {
        active_memtable_->Delete(rec.key);
      }
      // Re-log into the fresh generation so this data stays crash-protected
      // until the next flush, even though we just "replayed" rather than
      // freshly wrote it.
      memtable_wal_->Append(rec);
    }
  }
  memtable_wal_->Flush();
  for (int64_t gen : found_generations) {
    std::filesystem::remove(MemWALPath(gen));
  }
  next_wal_generation_ = new_generation + 1;
}

size_t LSMTreeEngine::GetWALSizeBytes() const {
  return wal_enabled_ ? memtable_wal_->GetLogSizeBytes() : 0;
}

void LSMTreeEngine::LoadManifestOrInitialize() {
  manifest_disk_manager_ = std::make_unique<DiskManager>(db_path_);

  if (manifest_disk_manager_->GetNumPages() == 0) {
    ManifestPage mp{};
    mp.next_sstable_id = 0;
    page_id_t page_id = manifest_disk_manager_->AllocatePage();
    manifest_disk_manager_->WritePage(page_id, reinterpret_cast<const char*>(&mp));
    next_sstable_id_ = 0;
    return;
  }

  char buf[PAGE_SIZE];
  manifest_disk_manager_->ReadPage(0, buf);
  const auto* mp = reinterpret_cast<const ManifestPage*>(buf);
  next_sstable_id_ = mp->next_sstable_id;

  int highest = -1;
  for (int t = 0; t < kMaxManifestTiers; ++t) {
    if (mp->tier_count[t] > 0) highest = t;
  }
  tiers_.assign(static_cast<size_t>(highest + 1), {});
  for (int t = 0; t <= highest; ++t) {
    for (int j = 0; j < mp->tier_count[t]; ++j) {
      int64_t id = mp->tier_ids[t][j];
      auto sst = SSTable::Open(SSTablePath(id));
      sst->SetId(id);
      tiers_[static_cast<size_t>(t)].push_back(sst);
    }
  }
}

void LSMTreeEngine::PersistManifestLocked() {
  if (tiers_.size() > static_cast<size_t>(kMaxManifestTiers)) {
    throw std::runtime_error("LSMTreeEngine: exceeded manifest tier capacity");
  }
  ManifestPage mp{};
  mp.next_sstable_id = next_sstable_id_.load();
  for (size_t t = 0; t < tiers_.size(); ++t) {
    if (tiers_[t].size() > static_cast<size_t>(kMaxSSTablesPerManifestTier)) {
      throw std::runtime_error("LSMTreeEngine: exceeded manifest per-tier SSTable capacity");
    }
    mp.tier_count[t] = static_cast<int32_t>(tiers_[t].size());
    for (size_t j = 0; j < tiers_[t].size(); ++j) {
      mp.tier_ids[t][j] = tiers_[t][j]->Id();
    }
  }
  manifest_disk_manager_->WritePage(0, reinterpret_cast<const char*>(&mp));
}

void LSMTreeEngine::FlushActiveMemtable() {
  std::shared_ptr<MemTable> old;
  std::string old_wal_path;
  bool had_wal = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old = active_memtable_;
    active_memtable_ = std::make_shared<MemTable>();
    if (wal_enabled_) {
      // Rotate to a fresh generation file *before* releasing the lock, so
      // any write that lands in the new memtable concurrently with this
      // flush is logged to the new generation, never the one about to be
      // retired — see DESIGN.md for the data-loss race this avoids.
      had_wal = true;
      old_wal_path = MemWALPath(next_wal_generation_ - 1);  // set by the previous rotation/startup
      int64_t new_generation = next_wal_generation_.fetch_add(1);
      memtable_wal_ = std::make_unique<LogManager>(MemWALPath(new_generation));
    }
  }
  if (old->EntryCount() == 0) {
    if (had_wal) {
      std::filesystem::remove(old_wal_path);
    }
    return;
  }

  std::vector<std::pair<KeyType, LSMValue>> entries;
  entries.reserve(old->EntryCount());
  for (auto it = old->Begin(); it.Valid(); it.Next()) {
    entries.emplace_back(it.key(), it.value());
  }

  int64_t id = next_sstable_id_.fetch_add(1);
  std::string path = SSTablePath(id);
  SSTable::Build(path, entries);
  auto sst = SSTable::Open(path);
  sst->SetId(id);

  // The memtable's data is now durably in the SSTable; the old generation's
  // WAL is no longer needed to protect it.
  if (had_wal) {
    std::filesystem::remove(old_wal_path);
  }

  bool backlogged;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tiers_.empty()) {
      tiers_.resize(1);
    }
    tiers_[0].insert(tiers_[0].begin(), sst);
    PersistManifestLocked();
    backlogged = tiers_[0].size() >= static_cast<size_t>(kWriteStallTierSize);
  }
  compaction_cv_.notify_one();

  if (backlogged) {
    // The background thread may be starved of lock time by a busy
    // foreground workload (observed under stress — see DESIGN.md); help it
    // out inline rather than risking an unbounded tier.
    CompactOneTierIfNeeded();
  }
}

int LSMTreeEngine::FindTierNeedingCompactionLocked() const {
  for (size_t t = 0; t < tiers_.size(); ++t) {
    if (static_cast<int>(tiers_[t].size()) >= tier_threshold_) {
      return static_cast<int>(t);
    }
  }
  return -1;
}

bool LSMTreeEngine::CompactOneTierIfNeeded() {
  bool expected = false;
  if (!compaction_running_.compare_exchange_strong(expected, true)) {
    // Someone else (the background thread, or another writer helping out)
    // is already mid-compaction; trust it rather than duplicating work.
    return false;
  }
  struct Guard {
    std::atomic<bool>& flag;
    ~Guard() { flag.store(false); }
  } guard{compaction_running_};

  std::unique_lock<std::mutex> lock(mutex_);
  int tier = FindTierNeedingCompactionLocked();
  if (tier < 0) {
    return false;
  }

  std::vector<std::shared_ptr<SSTable>> to_compact = tiers_[static_cast<size_t>(tier)];
  bool bottommost = true;
  for (size_t t = static_cast<size_t>(tier) + 1; t < tiers_.size(); ++t) {
    if (!tiers_[t].empty()) {
      bottommost = false;
      break;
    }
  }
  lock.unlock();

  std::vector<std::unique_ptr<LSMCursor>> cursors;
  cursors.reserve(to_compact.size());
  for (auto& sst : to_compact) {
    cursors.push_back(std::make_unique<SSTableCursor>(sst, sst->NewIterator(INVALID_KEY)));
  }
  LSMMergeIterator merge(std::move(cursors));
  std::vector<std::pair<KeyType, LSMValue>> merged;
  for (; merge.Valid(); merge.Next()) {
    if (merge.Value().deleted && bottommost) {
      continue;
    }
    merged.emplace_back(merge.Key(), merge.Value());
  }

  std::shared_ptr<SSTable> new_sst;
  if (!merged.empty()) {
    int64_t new_id = next_sstable_id_.fetch_add(1);
    std::string path = SSTablePath(new_id);
    SSTable::Build(path, merged);
    new_sst = SSTable::Open(path);
    new_sst->SetId(new_id);
  }

  lock.lock();
  size_t next_tier = static_cast<size_t>(tier) + 1;
  if (tiers_.size() <= next_tier) {
    tiers_.resize(next_tier + 1);
  }
  if (new_sst) {
    tiers_[next_tier].insert(tiers_[next_tier].begin(), new_sst);
  }

  std::vector<std::shared_ptr<SSTable>> remaining;
  for (auto& sst : tiers_[static_cast<size_t>(tier)]) {
    bool was_compacted = false;
    for (auto& c : to_compact) {
      if (c.get() == sst.get()) {
        was_compacted = true;
        break;
      }
    }
    if (!was_compacted) {
      remaining.push_back(sst);
    }
  }
  tiers_[static_cast<size_t>(tier)] = std::move(remaining);

  for (auto& sst : to_compact) {
    sst->MarkObsolete();
  }
  PersistManifestLocked();
  to_compact.clear();
  return true;
}

void LSMTreeEngine::CompactionLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    compaction_cv_.wait_for(lock, std::chrono::milliseconds(100),
                            [&] { return stop_ || FindTierNeedingCompactionLocked() >= 0; });
    if (stop_) {
      return;
    }
    lock.unlock();
    CompactOneTierIfNeeded();
    lock.lock();
  }
}

bool LSMTreeEngine::Get(KeyType key, std::string* value) {
  std::shared_ptr<MemTable> mt;
  std::vector<std::vector<std::shared_ptr<SSTable>>> tiers_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mt = active_memtable_;
    tiers_snapshot = tiers_;
  }

  LSMValue v;
  if (mt->Get(key, &v)) {
    if (v.deleted) return false;
    *value = v.value;
    return true;
  }
  for (auto& tier : tiers_snapshot) {
    for (auto& sst : tier) {
      if (!sst->MightContain(key)) continue;
      if (sst->Get(key, &v)) {
        if (v.deleted) return false;
        *value = v.value;
        return true;
      }
    }
  }
  return false;
}

void LSMTreeEngine::Put(KeyType key, const std::string& value) {
  if (value.size() > MAX_VALUE_SIZE) {
    throw std::invalid_argument("LSMTreeEngine::Put: value exceeds MAX_VALUE_SIZE");
  }
  bool need_flush;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_enabled_) {
      LogRecord rec;
      rec.type = LogRecordType::UPDATE;
      rec.op = LogOpType::PUT;
      rec.key = key;
      rec.SetNewValue(value);
      memtable_wal_->Append(rec);
      memtable_wal_->Flush();
    }
    active_memtable_->Put(key, value);
    need_flush = active_memtable_->ApproximateSizeBytes() >= flush_threshold_bytes_;
  }
  if (need_flush) {
    FlushActiveMemtable();
  }
}

bool LSMTreeEngine::Delete(KeyType key) {
  std::string tmp;
  bool existed = Get(key, &tmp);

  bool need_flush;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wal_enabled_) {
      LogRecord rec;
      rec.type = LogRecordType::UPDATE;
      rec.op = LogOpType::DELETE;
      rec.key = key;
      memtable_wal_->Append(rec);
      memtable_wal_->Flush();
    }
    active_memtable_->Delete(key);
    need_flush = active_memtable_->ApproximateSizeBytes() >= flush_threshold_bytes_;
  }
  if (need_flush) {
    FlushActiveMemtable();
  }
  return existed;
}

std::unique_ptr<StorageIterator> LSMTreeEngine::Scan(KeyType start_key) {
  std::shared_ptr<MemTable> mt;
  std::vector<std::vector<std::shared_ptr<SSTable>>> tiers_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mt = active_memtable_;
    tiers_snapshot = tiers_;
  }

  std::vector<std::unique_ptr<LSMCursor>> cursors;
  cursors.push_back(std::make_unique<MemTableCursor>(mt->LowerBound(start_key)));
  for (auto& tier : tiers_snapshot) {
    for (auto& sst : tier) {
      if (sst->MaxKey() < start_key) continue;
      cursors.push_back(std::make_unique<SSTableCursor>(sst, sst->NewIterator(start_key)));
    }
  }

  auto merge = std::make_unique<LSMMergeIterator>(std::move(cursors));
  return std::make_unique<LSMScanIterator>(std::move(mt), std::move(merge));
}

size_t LSMTreeEngine::NumSSTables() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (auto& tier : tiers_) total += tier.size();
  return total;
}

size_t LSMTreeEngine::NumTiers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tiers_.size();
}

size_t LSMTreeEngine::GetDiskReadCount() const {
  std::vector<std::vector<std::shared_ptr<SSTable>>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = tiers_;
  }
  size_t total = manifest_disk_manager_->GetNumReads();
  for (auto& tier : snapshot) {
    for (auto& sst : tier) total += sst->DiskReadCount();
  }
  return total;
}

size_t LSMTreeEngine::GetDiskWriteCount() const {
  std::vector<std::vector<std::shared_ptr<SSTable>>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = tiers_;
  }
  size_t total = manifest_disk_manager_->GetNumWrites();
  for (auto& tier : snapshot) {
    for (auto& sst : tier) total += sst->DiskWriteCount();
  }
  return total;
}

size_t LSMTreeEngine::TotalSSTableBytesOnDisk() const {
  std::vector<std::vector<std::shared_ptr<SSTable>>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = tiers_;
  }
  size_t total = 0;
  for (auto& tier : snapshot) {
    for (auto& sst : tier) {
      std::error_code ec;
      auto sz = std::filesystem::file_size(sst->Path(), ec);
      if (!ec) total += sz;
    }
  }
  return total;
}

}  // namespace dbengine
