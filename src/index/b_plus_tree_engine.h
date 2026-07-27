#pragma once

#include <memory>
#include <string>

#include "buffer/buffer_pool_manager.h"
#include "engine/storage_engine.h"
#include "index/b_plus_tree.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {

// StorageEngine backed by a disk-based B+-tree. Owns its own DiskManager and
// BufferPoolManager (constructed from a db file path), so a caller just
// needs a path to get a fully working, swappable-behind-StorageEngine
// instance. Page 0 is reserved as a metadata page holding the tree's root
// page id, so the tree survives being closed and reopened.
class BPlusTreeEngine : public StorageEngine {
 public:
  BPlusTreeEngine(const std::string& db_file, size_t buffer_pool_size);
  ~BPlusTreeEngine() override = default;

  bool Get(KeyType key, std::string* value) override;
  void Put(KeyType key, const std::string& value) override;
  bool Delete(KeyType key) override;
  std::unique_ptr<StorageIterator> Scan(KeyType start_key) override;

  // Introspection used by the head-to-head benchmark (read/write/space
  // amplification) — not part of StorageEngine.
  size_t GetDiskReadCount() const { return disk_manager_->GetNumReads(); }
  size_t GetDiskWriteCount() const { return disk_manager_->GetNumWrites(); }
  size_t OnDiskBytes() const { return disk_manager_->GetNumPages() * PAGE_SIZE; }

 private:
  static constexpr page_id_t kMetaPageId = 0;

  struct MetaPage {
    page_id_t root_page_id;
  };

  // Writes the tree's current root page id to the metadata page if it
  // changed (an insert/delete that grows/shrinks the tree height moves the
  // root to a new page).
  void PersistRootPageIdIfChanged();

  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<BPlusTree> tree_;
};

}  // namespace dbengine
