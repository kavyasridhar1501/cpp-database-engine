#pragma once

#include <cstddef>
#include <deque>
#include <unordered_map>

#include "buffer/replacer.h"
#include "common/config.h"

namespace dbengine {

// LRU-K eviction: a frame's priority is its "backward k-distance" — how long
// ago its k-th most recent access happened. Frames with fewer than k
// recorded accesses have infinite backward distance (evicted first, tie
// broken by plain recency) since a single access is weak evidence they'll be
// used again. Frames with at least k accesses are evicted in order of
// largest backward k-distance (least likely to be reused soon).
//
// This is what distinguishes LRU-K from plain LRU: a page touched once by a
// long sequential scan looks identical to a hot page under LRU (both are
// "most recently used"), but under LRU-K the scanned page has no second
// access to fall back on, so it's evicted ahead of anything with a real
// access history. See O'Neil, O'Neil & Weikum, "The LRU-K Page Replacement
// Algorithm For Database Disk Buffering" (SIGMOD 1993).
class LRUKReplacer : public Replacer {
 public:
  // `num_frames` is the total number of frames the replacer will ever be
  // asked to track (== buffer pool size). `k` is the number of historical
  // accesses used to compute backward distance; k=2 is the classic choice
  // from the paper and BusTub's default.
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
