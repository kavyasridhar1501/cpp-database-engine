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

namespace dbengine {

// StorageEngine backed by an LSM-tree: an in-memory skip-list memtable
// flushed to immutable, page-based, Bloom-filtered SSTables once it crosses
// a size threshold, with a background thread that merges a "tier" of
// SSTables into one larger SSTable (promoted to the next tier) once that
// tier accumulates `tier_compaction_threshold` tables — classic size-tiered
// compaction. See DESIGN.md for the concurrency model (snapshot reads via
// shared_ptr, synchronous flush, asynchronous compaction) and the
// simplifications made relative to a production LSM engine.
//
// Each SSTable is its own file (db_path + ".sst" + id); a small manifest
// page (page 0 of `db_path` itself) tracks which SSTable ids belong to
// which tier, so a closed engine can be reopened.
class LSMTreeEngine : public StorageEngine {
 public:
  explicit LSMTreeEngine(const std::string& db_path, size_t memtable_flush_threshold_bytes = 1 << 20,
                         int tier_compaction_threshold = 4);
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

  // Manifest capacity (public so the on-disk ManifestPage struct, defined
  // in the .cpp, can size its arrays against them).
  static constexpr int kMaxManifestTiers = 8;
  static constexpr int kMaxSSTablesPerManifestTier = 16;

  // Backpressure threshold, comfortably below kMaxSSTablesPerManifestTier:
  // if a tier reaches this size, a writer whose flush just landed there
  // helps compact it inline (see FlushActiveMemtable) instead of trusting
  // the background thread's scheduling alone. Without this, a foreground
  // loop issuing writes fast enough can starve the background compaction
  // thread of lock time and grow a tier past its hard manifest capacity —
  // observed in practice under CI-style stress (see DESIGN.md).
  static constexpr int kWriteStallTierSize = 8;

 private:
  std::string SSTablePath(int64_t id) const;
  void LoadManifestOrInitialize();
  void PersistManifestLocked();
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

  mutable std::mutex mutex_;
  std::shared_ptr<MemTable> active_memtable_;
  std::vector<std::vector<std::shared_ptr<SSTable>>> tiers_;
  bool stop_ = false;

  std::condition_variable compaction_cv_;
  std::thread compaction_thread_;
  std::atomic<bool> compaction_running_{false};
};

}  // namespace dbengine
