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

// Holds a shared_ptr to the SSTable so it (and the pin its iterator holds)
// stays valid even if compaction concurrently retires it. `sst_` must be
// declared before `it_` so it outlives it (members destroy in reverse
// declaration order).
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
