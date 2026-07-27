#pragma once

#include "common/config.h"
#include "index/b_plus_tree_page.h"

namespace dbengine {

class BufferPoolManager;

// An internal (non-leaf) B+-tree node: `size_` (key, child page id) pairs,
// where entry 0's key is an unused placeholder — value_[0] is "children
// reachable through this pointer have keys < key_[1]", value_[i] (i>=1) is
// "keys in [key_[i], key_[i+1])". This is the standard internal-node
// convention (e.g. CLRS chapter on B-trees, adapted for B+-trees where all
// data lives in leaves).
//
// Like BPlusTreePage, this is overlaid directly on a Page's byte buffer, so
// it carries no vtable and `array_` is a fixed-size C array sized to fill
// exactly one page.
class InternalPage : public BPlusTreePage {
 public:
  void Init(page_id_t parent_id, int max_size);

  KeyType KeyAt(int index) const;
  void SetKeyAt(int index, KeyType key);
  page_id_t ValueAt(int index) const;
  void SetValueAt(int index, page_id_t value);

  // Index of the entry whose value is `value`, or -1 if not present.
  int ValueIndex(page_id_t value) const;

  // Child pointer to follow when searching for `key`.
  page_id_t Lookup(KeyType key) const;

  // Turns this (empty) node into a fresh root pointing at `old_value` and
  // `new_value`, split by `new_key`. Used when the tree grows a level.
  void PopulateNewRoot(page_id_t old_value, KeyType new_key, page_id_t new_value);

  // Inserts (new_key, new_value) immediately after the entry for
  // `old_value`. Used to propagate a split up to the parent. Returns the
  // new size. Caller must ensure capacity (<= array capacity) before
  // calling; this class allows one entry of transient overflow beyond
  // max_size_ to make splitting straightforward.
  int InsertNodeAfter(page_id_t old_value, KeyType new_key, page_id_t new_value);

  void Remove(int index);

  // Empties this node (which must have exactly one child left after a
  // merge) and returns that child, for the caller to install as the new
  // root.
  page_id_t RemoveAndReturnOnlyChild();

  // Split: moves the upper half of this node's entries to `recipient`
  // (freshly allocated, empty, whose own page id is `recipient_page_id` —
  // an InternalPage has no notion of its own page id, only its parent's, so
  // callers must pass it in), fixing up the moved children's parent
  // pointers via `bpm`.
  void MoveHalfTo(InternalPage* recipient, page_id_t recipient_page_id, BufferPoolManager* bpm);

  // Merge: appends all of this node's entries onto the end of `recipient`
  // (its left sibling, page id `recipient_page_id`), using `middle_key` —
  // pulled down from the parent — as the separator for this node's first
  // (previously dummy-keyed) entry.
  void MoveAllTo(InternalPage* recipient, page_id_t recipient_page_id, KeyType middle_key,
                 BufferPoolManager* bpm);

  // Redistribute: moves this node's first entry to the end of `recipient`
  // (its left sibling, page id `recipient_page_id`), which is under-full.
  void MoveFirstToEndOf(InternalPage* recipient, page_id_t recipient_page_id, KeyType middle_key,
                        BufferPoolManager* bpm);

  // Redistribute: moves this node's last entry to the front of `recipient`
  // (its right sibling, page id `recipient_page_id`), which is under-full.
  void MoveLastToFrontOf(InternalPage* recipient, page_id_t recipient_page_id, KeyType middle_key,
                         BufferPoolManager* bpm);

 private:
  struct MappingType {
    KeyType key;
    page_id_t value;
  };

  static void UpdateChildParent(page_id_t child_page_id, page_id_t new_parent_id,
                                 BufferPoolManager* bpm);

  static constexpr int kMaxSize =
      static_cast<int>((PAGE_SIZE - sizeof(BPlusTreePage)) / sizeof(MappingType));

  MappingType array_[kMaxSize];

 public:
  static constexpr int Capacity() { return kMaxSize; }
};

}  // namespace dbengine
