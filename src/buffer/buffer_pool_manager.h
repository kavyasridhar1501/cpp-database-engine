#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "buffer/page.h"
#include "buffer/replacer.h"
#include "common/config.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {

// Fixed-frame buffer pool sitting between callers and DiskManager. Pages are
// pinned while in use (FetchPage/NewPage bump the pin count; UnpinPage drops
// it) and only become eviction candidates once their pin count reaches zero.
// Eviction policy is pluggable via Replacer (lru_k_replacer.h is the
// production policy; lru_replacer.h is a plain-LRU comparison baseline).
//
// All public methods take a single mutex rather than per-page latching.
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                     std::unique_ptr<Replacer> replacer);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  // Allocates a new page (via DiskManager::AllocatePage) and pins it in a
  // frame, zero-initialized. Returns nullptr if the pool is full and every
  // frame is pinned.
  Page* NewPage(page_id_t* page_id);

  // Fetches `page_id`, reading it from disk if not already cached, and pins
  // it. Returns nullptr if the pool is full and every frame is pinned.
  Page* FetchPage(page_id_t page_id);

  // Unpins `page_id`. `is_dirty` is OR'd into the page's dirty flag (never
  // clears it — only a flush does that). Returns false if the page isn't
  // currently in the pool or its pin count is already zero.
  bool UnpinPage(page_id_t page_id, bool is_dirty);

  // Writes `page_id` back to disk if present in the pool, regardless of pin
  // count, and clears its dirty flag. Returns false if not present.
  bool FlushPage(page_id_t page_id);

  void FlushAllPages();

  // Removes `page_id` from the pool (no-op if absent) and returns its frame
  // to the free list. Returns false if the page is currently pinned.
  bool DeletePage(page_id_t page_id);

  size_t GetPoolSize() const { return pool_size_; }
  size_t GetNumHits() const { return num_hits_; }
  size_t GetNumMisses() const { return num_misses_; }

 private:
  // Obtains a frame ready to hold a page: from the free list if one exists,
  // otherwise by evicting a victim (writing it back first if dirty). Returns
  // false if no frame is available (pool full, everything pinned).
  bool AcquireFrame(frame_id_t* frame_id);

  const size_t pool_size_;
  DiskManager* disk_manager_;
  std::unique_ptr<Replacer> replacer_;

  std::vector<Page> pages_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> free_list_;

  size_t num_hits_ = 0;
  size_t num_misses_ = 0;

  mutable std::mutex latch_;
};

}  // namespace dbengine
