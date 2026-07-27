#pragma once

#include <cstdint>
#include <string>

#include "common/config.h"
#include "index/b_plus_tree_page.h"

namespace dbengine {

// A leaf B+-tree node: `size_` (key, value) pairs in ascending key order,
// plus a `next_page_id_` link to the next leaf, so a range scan can walk
// leaves left-to-right without ever revisiting internal nodes. Values are
// stored inline as fixed-size slots (see MAX_VALUE_SIZE in common/config.h)
// so entries — and therefore leaf capacity — are a compile-time constant;
// no slotted-page / free-space management is needed. Like BPlusTreePage,
// this is overlaid directly on a Page's byte buffer (no vtable).
class LeafPage : public BPlusTreePage {
 public:
  void Init(page_id_t parent_id, int max_size);

  KeyType KeyAt(int index) const;
  std::string ValueAt(int index) const;

  page_id_t GetNextPageId() const;
  void SetNextPageId(page_id_t next_page_id);

  // First index with KeyAt(index) >= key (a.k.a. lower_bound). May equal
  // GetSize() if every key is smaller.
  int KeyIndex(KeyType key) const;

  // Exact-match lookup. Returns true and fills `*value` if `key` is present.
  bool Lookup(KeyType key, std::string* value) const;

  // Upsert: inserts (key, value) in sorted position, or overwrites the
  // existing entry's value if `key` is already present. Returns the size
  // after the operation — callers compare against the pre-call size to
  // tell an insert (size grew) from an update (size unchanged). Throws
  // std::invalid_argument if value.size() > MAX_VALUE_SIZE.
  int Insert(KeyType key, const std::string& value);

  // Removes `key` if present, shifting subsequent entries left. Returns
  // true if it was found and removed.
  bool Remove(KeyType key);

  // Split: moves the upper half of this node's entries to `recipient`
  // (freshly allocated, empty) and splices it into the leaf chain right
  // after this node. `recipient_page_id` is `recipient`'s own page id
  // (needed for the chain link; a LeafPage doesn't know its own id).
  void MoveHalfTo(LeafPage* recipient, page_id_t recipient_page_id);

  // Merge: appends all of this node's entries onto the end of `recipient`
  // (its left sibling) and removes this node from the leaf chain.
  void MoveAllTo(LeafPage* recipient);

  // Redistribute: moves this node's first entry to the end of `recipient`
  // (its left sibling), which is under-full.
  void MoveFirstToEndOf(LeafPage* recipient);

  // Redistribute: moves this node's last entry to the front of `recipient`
  // (its right sibling), which is under-full.
  void MoveLastToFrontOf(LeafPage* recipient);

 private:
  struct Entry {
    KeyType key;
    uint16_t value_len;
    char value[MAX_VALUE_SIZE];
  };

  static constexpr int kMaxSize =
      static_cast<int>((PAGE_SIZE - sizeof(BPlusTreePage) - sizeof(page_id_t)) / sizeof(Entry));

  page_id_t next_page_id_ = INVALID_PAGE_ID;
  Entry array_[kMaxSize];

 public:
  static constexpr int Capacity() { return kMaxSize; }
};

}  // namespace dbengine
