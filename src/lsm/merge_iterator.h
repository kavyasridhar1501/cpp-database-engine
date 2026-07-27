#pragma once

#include <memory>
#include <queue>
#include <vector>

#include "common/config.h"
#include "lsm/lsm_cursor.h"
#include "lsm/lsm_value.h"

namespace dbengine {

// K-way merge over a set of sorted cursors (a memtable and/or SSTables),
// producing one entry per distinct key in ascending order — including
// tombstones, since what to do with a tombstone (drop it, or keep it to
// keep shadowing older data) is a decision only the caller can make (see
// LSMTreeEngine::Scan, which filters them out, versus compaction, which
// keeps them unless it's compacting the bottommost tier).
//
// `cursors` must be given in priority order, highest first: index 0 wins
// any tie. LSMTreeEngine builds this list as [memtable, tier0 newest..
// oldest, tier1 newest..oldest, ...] for Scan, matching the same
// precedence Get() uses, and as [tier newest..oldest] for a single tier's
// compaction merge.
class LSMMergeIterator {
 public:
  explicit LSMMergeIterator(std::vector<std::unique_ptr<LSMCursor>> cursors);

  bool Valid() const { return valid_; }
  KeyType Key() const { return current_key_; }
  const LSMValue& Value() const { return current_value_; }
  void Next() { Advance(); }

 private:
  struct HeapItem {
    KeyType key;
    int priority;
  };
  // std::priority_queue is a max-heap; this comparator inverts it into a
  // min-heap ordered by (key ascending, then priority ascending) so top()
  // is always the smallest key, and ties go to the highest-priority (most
  // recent) source.
  struct Compare {
    bool operator()(const HeapItem& a, const HeapItem& b) const {
      if (a.key != b.key) return a.key > b.key;
      return a.priority > b.priority;
    }
  };

  void Advance();

  std::vector<std::unique_ptr<LSMCursor>> cursors_;
  std::priority_queue<HeapItem, std::vector<HeapItem>, Compare> heap_;
  KeyType current_key_ = 0;
  LSMValue current_value_;
  bool valid_ = false;
};

}  // namespace dbengine
