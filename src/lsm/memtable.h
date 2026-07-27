#pragma once

#include <cstddef>
#include <string>

#include "common/config.h"
#include "lsm/lsm_value.h"
#include "lsm/skip_list.h"

namespace dbengine {

// The LSM-tree's in-memory write buffer: a skip list ordered by key, holding
// either values or tombstones. Not thread-safe on its own — the engine that
// owns one serializes access via its own lock (see LSMTreeEngine).
class MemTable {
 public:
  void Put(KeyType key, const std::string& value) {
    list_.Insert(key, LSMValue::Value(value));
    approximate_size_bytes_ += sizeof(KeyType) + value.size();
  }

  void Delete(KeyType key) {
    list_.Insert(key, LSMValue::Tombstone());
    approximate_size_bytes_ += sizeof(KeyType);
  }

  // Returns true if `key` has an entry (value or tombstone) in this
  // memtable; the caller distinguishes the two via `out->deleted`.
  bool Get(KeyType key, LSMValue* out) const { return list_.Find(key, out); }

  size_t ApproximateSizeBytes() const { return approximate_size_bytes_; }
  size_t EntryCount() const { return list_.Size(); }

  using Iterator = SkipList<KeyType, LSMValue>::Iterator;
  Iterator Begin() const { return list_.Begin(); }
  Iterator LowerBound(KeyType key) const { return list_.LowerBound(key); }

 private:
  SkipList<KeyType, LSMValue> list_;
  size_t approximate_size_bytes_ = 0;
};

}  // namespace dbengine
