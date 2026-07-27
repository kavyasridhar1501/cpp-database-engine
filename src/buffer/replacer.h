#pragma once

#include <cstddef>

#include "common/config.h"

namespace dbengine {

// A Replacer tracks which buffer pool frames are candidates for eviction and
// picks a victim among them. It knows nothing about pages, disk I/O, or pin
// counts as concepts in their own right — the BufferPoolManager is
// responsible for translating "pin count reached zero" into
// SetEvictable(frame_id, true) and vice versa. This separation lets the
// eviction policy (LRU-K here, or a CLOCK/plain-LRU alternative for
// comparison) be swapped without touching BufferPoolManager.
class Replacer {
 public:
  virtual ~Replacer() = default;

  // Picks a victim frame among evictable frames and removes it from further
  // consideration (as if SetEvictable(*frame_id, false) had been called).
  // Returns false if no frame is evictable.
  virtual bool Evict(frame_id_t* frame_id) = 0;

  // Records that frame_id was just accessed (on every buffer pool hit, and
  // once when a page is first loaded into the frame).
  virtual void RecordAccess(frame_id_t frame_id) = 0;

  // Marks frame_id as evictable or not. The buffer pool manager calls this
  // with `false` while a frame is pinned and `true` once its pin count drops
  // to zero.
  virtual void SetEvictable(frame_id_t frame_id, bool evictable) = 0;

  // Stops tracking frame_id entirely (e.g. its page was deleted). No-op if
  // the frame isn't tracked. Throws if the frame is currently non-evictable.
  virtual void Remove(frame_id_t frame_id) = 0;

  // Number of frames currently evictable.
  virtual size_t Size() const = 0;
};

}  // namespace dbengine
