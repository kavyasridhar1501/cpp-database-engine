#pragma once

#include <cstddef>
#include <deque>
#include <unordered_map>

#include "buffer/replacer.h"
#include "common/config.h"

namespace dbengine {

// LRU-K eviction: a frame's priority is its "backward k-distance" — how long
// ago its k-th most recent access happened. Frames with fewer than k
// accesses get +inf backward distance (evicted first), so a page touched
// once by a sequential scan doesn't look "hot" the way it would under plain
// LRU.
class LRUKReplacer : public Replacer {
 public:
  // k=2 is the classic choice from the paper.
  LRUKReplacer(size_t num_frames, size_t k);

  bool Evict(frame_id_t* frame_id) override;
  void RecordAccess(frame_id_t frame_id) override;
  void SetEvictable(frame_id_t frame_id, bool evictable) override;
  void Remove(frame_id_t frame_id) override;
  size_t Size() const override;

 private:
  struct FrameState {
    std::deque<size_t> history;  // oldest access first; capped at k entries.
    bool evictable = false;
  };

  const size_t num_frames_;
  const size_t k_;
  size_t current_timestamp_ = 0;
  size_t evictable_count_ = 0;
  std::unordered_map<frame_id_t, FrameState> frames_;
};

}  // namespace dbengine
