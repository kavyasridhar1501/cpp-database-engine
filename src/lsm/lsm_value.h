#pragma once

#include <string>

namespace dbengine {

// The unit of storage for both the memtable and SSTables: either a value or
// a tombstone. Tombstones exist because an LSM-tree never modifies data in
// place — a delete of a key in an older, immutable SSTable can only be
// recorded as a newer shadowing entry, not applied directly.
struct LSMValue {
  std::string value;
  bool deleted = false;

  static LSMValue Value(std::string v) { return LSMValue{std::move(v), false}; }
  static LSMValue Tombstone() { return LSMValue{std::string(), true}; }
};

}  // namespace dbengine
