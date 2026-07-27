#pragma once

#include <cstdint>

#include "common/config.h"

namespace dbengine {

// Common header shared by LeafPage and InternalPage. This is overlaid
// directly on a Page's raw byte buffer via reinterpret_cast (see
// b_plus_tree.cpp), so it must stay a plain-old-data layout: no virtual
// functions (that would inject a vtable pointer into the byte layout) and
// no members beyond simple fixed-size fields.
class BPlusTreePage {
 public:
  enum class PageType : int32_t { INVALID = 0, LEAF = 1, INTERNAL = 2 };

  bool IsLeafPage() const { return page_type_ == PageType::LEAF; }
  PageType GetPageType() const { return page_type_; }
  void SetPageType(PageType type) { page_type_ = type; }

  int GetSize() const { return size_; }
  void SetSize(int size) { size_ = size; }
  void IncreaseSize(int delta) { size_ += delta; }

  int GetMaxSize() const { return max_size_; }
  void SetMaxSize(int max_size) { max_size_ = max_size; }

  // Minimum occupancy before a non-root node must redistribute or merge.
  // Matches the standard B+-tree rule: at least half full (ceiling), except
  // internal nodes may transiently drop to 1 child right after the root
  // shrinks — BPlusTree::Remove handles that case explicitly rather than
  // baking it into this threshold.
  int GetMinSize() const { return (max_size_ + 1) / 2; }

  bool IsRootPage() const { return parent_page_id_ == INVALID_PAGE_ID; }
  page_id_t GetParentPageId() const { return parent_page_id_; }
  void SetParentPageId(page_id_t parent_page_id) { parent_page_id_ = parent_page_id; }

 protected:
  PageType page_type_ = PageType::INVALID;
  int32_t size_ = 0;
  int32_t max_size_ = 0;
  page_id_t parent_page_id_ = INVALID_PAGE_ID;
};

}  // namespace dbengine
