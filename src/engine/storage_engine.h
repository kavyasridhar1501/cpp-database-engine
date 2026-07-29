#pragma once

#include <memory>
#include <string>

#include "common/config.h"

namespace dbengine {

// A read-only cursor over an ordered range of a StorageEngine. Implementors
// (B+-tree, later LSM-tree) return one from Scan(); callers drive it with
// Next()/IsValid() and never construct one directly.
class StorageIterator {
 public:
  virtual ~StorageIterator() = default;

  virtual bool IsValid() const = 0;
  virtual void Next() = 0;
  virtual KeyType GetKey() const = 0;
  virtual std::string GetValue() const = 0;
};

// The interface every storage engine (disk-backed B+-tree, LSM-tree)
// implements identically, so callers never need to know which one they're
// talking to.
//
// Keys are fixed-size 64-bit integers (KeyType) and unique per engine; Put
// on an existing key overwrites its value (upsert, not insert-or-fail).
// Values are byte strings capped at MAX_VALUE_SIZE.
class StorageEngine {
 public:
  virtual ~StorageEngine() = default;

  // Looks up `key`. Returns true and sets `*value` if found, false
  // (leaving `*value` untouched) otherwise.
  virtual bool Get(KeyType key, std::string* value) = 0;

  // Inserts `key` with `value`, or overwrites its value if `key` already
  // exists. Throws std::invalid_argument if value.size() > MAX_VALUE_SIZE.
  virtual void Put(KeyType key, const std::string& value) = 0;

  // Removes `key`. Returns true if it existed, false if it didn't.
  virtual bool Delete(KeyType key) = 0;

  // Returns an iterator positioned at the first key >= `start_key` (pass
  // INVALID_KEY, the type's minimum, to scan from the beginning). Iterates
  // in ascending key order until exhausted.
  virtual std::unique_ptr<StorageIterator> Scan(KeyType start_key) = 0;
};

}  // namespace dbengine
