#pragma once

#include <memory>

#include "common/config.h"
#include "lsm/lsm_value.h"
#include "lsm/memtable.h"
#include "lsm/sstable.h"

namespace dbengine {

// Common interface over a memtable's or an SSTable's forward iterator, so
// the merge iterator (see merge_iterator.h) can k-way-merge sources of
// either kind without caring which.
class LSMCursor {
 public:
  virtual ~LSMCursor() = default;
  virtual bool Valid() const = 0;
  virtual KeyType Key() const = 0;
  virtual const LSMValue& Value() const = 0;
  virtual void Next() = 0;
};

class MemTableCursor : public LSMCursor {
 public:
  explicit MemTableCursor(MemTable::Iterator it) : it_(std::move(it)) {}
  bool Valid() const override { return it_.Valid(); }
  KeyType Key() const override { return it_.key(); }
  const LSMValue& Value() const override { return it_.value(); }
  void Next() override { it_.Next(); }

 private:
  MemTable::Iterator it_;
};

// Holds a shared_ptr to the SSTable alongside its iterator so the table
// (and the pin its iterator holds) stays valid for the cursor's lifetime,
// even if the engine's compaction thread concurrently retires the table
// from its tier list. Declaration order matters here: `sst_` must outlive
// `it_`, and C++ destroys members in reverse declaration order, so `sst_`
// is declared first (destroyed last).
class SSTableCursor : public LSMCursor {
 public:
  SSTableCursor(std::shared_ptr<SSTable> sst, SSTable::Iterator it)
      : sst_(std::move(sst)), it_(std::move(it)) {}
  bool Valid() const override { return it_.Valid(); }
  KeyType Key() const override { return it_.Key(); }
  const LSMValue& Value() const override { return it_.Value(); }
  void Next() override { it_.Next(); }

 private:
  std::shared_ptr<SSTable> sst_;
  SSTable::Iterator it_;
};

}  // namespace dbengine
