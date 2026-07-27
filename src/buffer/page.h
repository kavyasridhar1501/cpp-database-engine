#pragma once

#include <cstring>

#include "common/config.h"

namespace dbengine {

// A fixed-size in-memory frame holding one page's worth of data plus the
// bookkeeping the buffer pool needs (identity, pin count, dirty flag).
// Callers get a `Page*` back from BufferPoolManager and must not hold onto
// it past the matching UnpinPage call — the frame's contents (and page_id_)
// can change once unpinned and evicted.
class Page {
 public:
  friend class BufferPoolManager;

  Page() { std::memset(data_, 0, PAGE_SIZE); }

  char* GetData() { return data_; }
  const char* GetData() const { return data_; }
  page_id_t GetPageId() const { return page_id_; }
  int GetPinCount() const { return pin_count_; }
  bool IsDirty() const { return is_dirty_; }

 private:
  void ResetMemory() {
    std::memset(data_, 0, PAGE_SIZE);
    page_id_ = INVALID_PAGE_ID;
    pin_count_ = 0;
    is_dirty_ = false;
  }

  char data_[PAGE_SIZE];
  page_id_t page_id_ = INVALID_PAGE_ID;
  int pin_count_ = 0;
  bool is_dirty_ = false;
};

}  // namespace dbengine
