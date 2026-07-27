#pragma once

#include <string>

namespace dbengine {

// The unit of storage for both the memtable and SSTables: either a value,
// or a tombstone recording that a key was deleted. Tombstones are
// necessary because an LSM-tree never modifies data in place — a delete on
// a key whose value lives in an older, immutable SSTable can only be
// recorded as a newer entry that shadows it, not applied directly.
struct LSMValue {
  std::string value;
  bool deleted = false;

  static LSMValue Value(std::string v) { return LSMValue{std::move(v), false}; }
  static LSMValue Tombstone() { return LSMValue{std::string(), true}; }
};

}  // namespace dbengine
