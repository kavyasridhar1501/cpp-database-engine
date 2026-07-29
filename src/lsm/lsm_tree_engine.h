#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/storage_engine.h"
#include "lsm/memtable.h"
#include "lsm/sstable.h"
#include "storage/disk/disk_manager.h"
#include "wal/log_manager.h"

namespace dbengine {

// StorageEngine backed by an LSM-tree: an in-memory skip-list memtable
// flushed to immutable, page-based, Bloom-filtered SSTables once it crosses
// a size threshold, with a background thread doing size-tiered compaction —
// merging a tier's SSTables into one larger table promoted to the next tier
// once the tier hits `tier_compaction_threshold` tables.
//
// Each SSTable is its own file (db_path + ".sst" + id); a small manifest
// page (page 0 of `db_path` itself) tracks which SSTable ids belong to
// which tier, so a closed engine can be reopened.
class LSMTreeEngine : public StorageEngine {
 public:
  // `enable_wal` adds durability for the active memtable: every Put/Delete
  // is logged (and fsynced) before being applied, so a crash before the
  // next flush doesn't lose recent writes. Off by default.
  explicit LSMTreeEngine(const std::string& db_path, size_t memtable_flush_threshold_bytes = 1 << 20,
                         int tier_compaction_threshold = 4, bool enable_wal = false);
  ~LSMTreeEngine() override;

  LSMTreeEngine(const LSMTreeEngine&) = delete;
  LSMTreeEngine& operator=(const LSMTreeEngine&) = delete;

  bool Get(KeyType key, std::string* value) override;
  void Put(KeyType key, const std::string& value) override;
  bool Delete(KeyType key) override;
  std::unique_ptr<StorageIterator> Scan(KeyType start_key) override;

  // Introspection used by the compaction demo and the space-amplification
  // benchmark — not part of StorageEngine, specific to this engine.
  size_t NumSSTables() const;
  size_t NumTiers() const;
  size_t TotalSSTableBytesOnDisk() const;
  size_t GetDiskReadCount() const;
  size_t GetDiskWriteCount() const;
  bool IsWALEnabled() const { return wal_enabled_; }
  size_t GetWALSizeBytes() const;

  // Manifest capacity (public so the on-disk ManifestPage struct, defined
  // in the .cpp, can size its arrays against them).
  static constexpr int kMaxManifestTiers = 8;
  static constexpr int kMaxSSTablesPerManifestTier = 16;

  // Backpressure threshold below kMaxSSTablesPerManifestTier: a writer whose
  // flush lands in a tier this large helps compact it inline (see
  // FlushActiveMemtable) rather than trusting the background thread alone,
  // which a fast foreground write loop can starve of lock time, growing a
  // tier past its hard manifest capacity.
  static constexpr int kWriteStallTierSize = 8;

 private:
  std::string SSTablePath(int64_t id) const;
  std::string MemWALPath(int64_t generation) const;
  void LoadManifestOrInitialize();
  void PersistManifestLocked();
  // Replays any memtable-WAL generation files left over from a crash into
  // active_memtable_ in order, re-logs them into a fresh generation, and
  // removes the old files. No-op if !wal_enabled_.
  void RecoverMemWALOnStartup();
  void FlushActiveMemtable();
  void CompactionLoop();
  // Performs at most one compaction pass (whichever tier needs it most).
  // Safe to call from any thread; if another thread is already compacting,
  // returns false immediately rather than duplicating the work. Returns
  // true if a compaction pass actually ran (whether or not anything was
  // found to do).
  bool CompactOneTierIfNeeded();
  int FindTierNeedingCompactionLocked() const;

  std::string db_path_;
  size_t flush_threshold_bytes_;
  int tier_threshold_;

  std::unique_ptr<DiskManager> manifest_disk_manager_;
  std::atomic<int64_t> next_sstable_id_{0};

  bool wal_enabled_;
  std::atomic<int64_t> next_wal_generation_{0};
  std::unique_ptr<LogManager> memtable_wal_;  // null if !wal_enabled_.

  mutable std::mutex mutex_;
  std::shared_ptr<MemTable> active_memtable_;
  std::vector<std::vector<std::shared_ptr<SSTable>>> tiers_;
  bool stop_ = false;

  std::condition_variable compaction_cv_;
  std::thread compaction_thread_;
  std::atomic<bool> compaction_running_{false};
};

}  // namespace dbengine
