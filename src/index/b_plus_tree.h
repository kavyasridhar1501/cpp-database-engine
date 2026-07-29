#pragma once

#include <mutex>
#include <string>

#include "buffer/buffer_pool_manager.h"
#include "common/config.h"
#include "index/internal_page.h"
#include "index/leaf_page.h"

namespace dbengine {

// A disk-backed B+-tree, indexing fixed-size KeyType keys to byte-string
// values (capped at MAX_VALUE_SIZE) via a BufferPoolManager. Every node is
// exactly one page; leaves are linked left-to-right for range scans.
//
// Concurrency: a single mutex serializes all mutation and lookup. An
// Iterator returned by Begin() holds a pin on its current leaf but does
// *not* hold the tree mutex between calls, so concurrent mutation during a
// scan is unsupported.
class BPlusTree {
 public:
  // `root_page_id` is INVALID_PAGE_ID for a brand-new tree, or a
  // previously-persisted root for one being reopened (see
  // BPlusTreeEngine, which owns that persistence). `leaf_max_size` /
  // `internal_max_size` cap logical node occupancy; they must leave at
  // least one slot of headroom below the physical per-page capacity
  // (LeafPage::Capacity() / InternalPage::Capacity()) so a node can
  // transiently hold one extra entry between "insert" and "split".
  BPlusTree(BufferPoolManager* bpm, page_id_t root_page_id, int leaf_max_size,
            int internal_max_size);

  bool IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }
  page_id_t GetRootPageId() const { return root_page_id_; }

  bool GetValue(KeyType key, std::string* value);

  // Upsert. Returns true if `key` was newly inserted, false if it updated
  // an existing key's value.
  bool Insert(KeyType key, const std::string& value);

  // Returns true if `key` was found and removed, false if it wasn't
  // present.
  bool Remove(KeyType key);

  // A forward-only cursor over leaf entries in ascending key order. Holds a
  // pin on its current leaf page while alive; move-only (copying would mean
  // two owners of the same pin).
  class Iterator {
   public:
    Iterator() = default;
    ~Iterator();
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    Iterator(Iterator&& other) noexcept;
    Iterator& operator=(Iterator&& other) noexcept;

    bool IsEnd() const { return page_ == nullptr; }
    KeyType GetKey() const;
    std::string GetValue() const;
    Iterator& operator++();

   private:
    friend class BPlusTree;
    Iterator(BufferPoolManager* bpm, page_id_t page_id, Page* page, int index)
        : bpm_(bpm), page_id_(page_id), page_(page), index_(index) {}
    void Release();
    void MoveFrom(Iterator& other);

    BufferPoolManager* bpm_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
    Page* page_ = nullptr;
    int index_ = 0;
  };

  // Full scan from the smallest key.
  Iterator Begin();
  // Scan starting at the first key >= `key`.
  Iterator Begin(KeyType key);

 private:
  // RAII wrapper around a pinned Page: unpins (with the accumulated dirty
  // flag) on destruction unless Release()d or Reset() early. Used
  // throughout Insert/Remove so early-return and recursive code paths can't
  // leak a pin.
  class PageGuard {
   public:
    PageGuard() = default;
    PageGuard(BufferPoolManager* bpm, page_id_t page_id, Page* page)
        : bpm_(bpm), page_id_(page_id), page_(page) {}
    ~PageGuard() { Reset(); }
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& other) noexcept { MoveFrom(other); }
    PageGuard& operator=(PageGuard&& other) noexcept {
      if (this != &other) {
        Reset();
        MoveFrom(other);
      }
      return *this;
    }

    Page* page() const { return page_; }
    page_id_t page_id() const { return page_id_; }
    void SetDirty() { dirty_ = true; }

    void Reset() {
      if (page_ != nullptr) {
        bpm_->UnpinPage(page_id_, dirty_);
        page_ = nullptr;
      }
    }

    // Releases ownership of the (still-pinned) page to the caller, who
    // becomes responsible for eventually unpinning it.
    Page* Release() {
      Page* p = page_;
      page_ = nullptr;
      return p;
    }

   private:
    void MoveFrom(PageGuard& other) {
      bpm_ = other.bpm_;
      page_id_ = other.page_id_;
      page_ = other.page_;
      dirty_ = other.dirty_;
      other.page_ = nullptr;
    }

    BufferPoolManager* bpm_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
    Page* page_ = nullptr;
    bool dirty_ = false;
  };

  static BPlusTreePage* AsTreePage(Page* page) {
    return reinterpret_cast<BPlusTreePage*>(page->GetData());
  }
  static LeafPage* AsLeaf(Page* page) { return reinterpret_cast<LeafPage*>(page->GetData()); }
  static InternalPage* AsInternal(Page* page) {
    return reinterpret_cast<InternalPage*>(page->GetData());
  }

  // Pins and returns the leaf page that contains (or would contain) `key`,
  // descending from the root. If `leftmost` is set, `key` is ignored and
  // the descent always follows each internal node's value_[0] (used for
  // Begin(), a full left-to-right scan).
  PageGuard FindLeafPageGuard(KeyType key, bool leftmost = false) const;

  // Propagates a split up the tree: inserts (middle_key -> right_id) into
  // left_id's parent, splitting that parent (recursively) if it overflows,
  // or creating a new root if left_id had none.
  void InsertIntoParent(page_id_t left_id, KeyType middle_key, page_id_t right_id);

  // Called after a deletion drops `node_id` below its minimum occupancy (or
  // whenever a merge upstream needs to re-check its parent). Takes
  // ownership of `node_guard`. Redistributes from a sibling if one can
  // spare an entry, otherwise merges with a sibling and recurses on the
  // parent; handles the root specially (shrinking tree height / emptying
  // the tree).
  void HandleUnderflow(page_id_t node_id, PageGuard node_guard);
  void HandleRootUnderflow(page_id_t node_id, PageGuard node_guard);

  BufferPoolManager* bpm_;
  page_id_t root_page_id_;
  int leaf_max_size_;
  int internal_max_size_;
  mutable std::mutex latch_;
};

}  // namespace dbengine
