#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "common/config.h"
#include "lsm/bloom_filter.h"
#include "lsm/lsm_value.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {

// An immutable, sorted, page-based run of (key, LSMValue) entries, written
// once (by a memtable flush or a compaction merge) and never modified. Each
// SSTable owns its own file and a small BufferPoolManager for caching
// data-page reads; its sparse index and Bloom filter are loaded fully into
// memory on Open().
//
// Lifetime is managed via shared_ptr: a compaction that supersedes an
// SSTable calls MarkObsolete() and drops the engine's reference, but any
// reader holding a shared_ptr keeps the object (and its file) alive until
// done — the destructor deletes the file only if MarkObsolete() was called.
class SSTable {
 public:
  ~SSTable();

  SSTable(const SSTable&) = delete;
  SSTable& operator=(const SSTable&) = delete;

  // Writes a new SSTable file at `path` from `entries`, which must already
  // be sorted ascending by key with no duplicate keys — merging multiple
  // sources, resolving which value wins on a duplicate key, and deciding
  // whether a tombstone can be dropped are all the caller's job (see
  // LSMTreeEngine); this just packs whatever stream it's given.
  static void Build(const std::string& path, const std::vector<std::pair<KeyType, LSMValue>>& entries,
                     double bloom_false_positive_rate = 0.01);

  static std::shared_ptr<SSTable> Open(const std::string& path);

  void MarkObsolete() { obsolete_ = true; }

  // Purely engine-level bookkeeping (manifest persistence); SSTable itself
  // never reads this value.
  void SetId(int64_t id) { id_ = id; }
  int64_t Id() const { return id_; }

  KeyType MinKey() const { return min_key_; }
  KeyType MaxKey() const { return max_key_; }
  int64_t NumEntries() const { return num_entries_; }
  const std::string& Path() const { return path_; }

  // Introspection used by the head-to-head benchmark.
  size_t DiskReadCount() const { return disk_manager_->GetNumReads(); }
  size_t DiskWriteCount() const { return disk_manager_->GetNumWrites(); }

  // Cheap pre-filter: false means `key` is definitely not in this table.
  bool MightContain(KeyType key) const;

  // Full lookup. Returns false if `key` isn't in this table at all;
  // otherwise fills `*out` (a value, or a tombstone if the key was
  // deleted in this run).
  bool Get(KeyType key, LSMValue* out);

  // A forward cursor over entries from the first key >= `start_key`
  // (pass INVALID_KEY for "from the beginning"). Holds a pin on its
  // current data page while alive; the SSTable itself must be kept alive
  // (via its shared_ptr) for at least as long as any iterator over it.
  class Iterator {
   public:
    Iterator() = default;
    ~Iterator();
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    Iterator(Iterator&& other) noexcept;
    Iterator& operator=(Iterator&& other) noexcept;

    bool Valid() const { return page_ != nullptr; }
    KeyType Key() const { return cached_key_; }
    const LSMValue& Value() const { return cached_value_; }
    void Next();

   private:
    friend class SSTable;
    void Release();
    void MoveFrom(Iterator& other);
    void LoadCurrent();

    SSTable* owner_ = nullptr;
    Page* page_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
    int index_ = 0;
    int32_t page_pos_ = 0;
    KeyType cached_key_ = 0;
    LSMValue cached_value_;
  };

  Iterator NewIterator(KeyType start_key);

 private:
  friend class Iterator;

  SSTable() = default;

  // Finds the data-page index (into [0, num_data_pages_)) that would
  // contain `key`, via binary search over the in-memory sparse index.
  int32_t FindPagePos(KeyType key) const;

  std::string path_;
  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> bpm_;

  int64_t num_entries_ = 0;
  KeyType min_key_ = 0;
  KeyType max_key_ = 0;
  int32_t num_data_pages_ = 0;
  page_id_t first_data_page_id_ = INVALID_PAGE_ID;
  std::vector<std::pair<KeyType, page_id_t>> sparse_index_;
  std::unique_ptr<BloomFilter> bloom_;
  int64_t id_ = -1;

  std::atomic<bool> obsolete_{false};
};

}  // namespace dbengine
