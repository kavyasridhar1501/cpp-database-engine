#include "buffer/lru_k_replacer.h"

#include <limits>

#include "common/exception.h"

namespace dbengine {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : num_frames_(num_frames), k_(k) {
  if (k_ == 0) {
    throw IOException("LRUKReplacer: k must be >= 1");
  }
}

void LRUKReplacer::RecordAccess(frame_id_t frame_id) {
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) {
    throw IOException("LRUKReplacer: frame_id out of range");
  }
  FrameState& state = frames_[frame_id];
  state.history.push_back(current_timestamp_);
  if (state.history.size() > k_) {
    state.history.pop_front();
  }
  ++current_timestamp_;
}

void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool evictable) {
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    // A frame can be marked evictable before its first RecordAccess in
    // principle; track it with empty history so Evict() still sees it.
    it = frames_.emplace(frame_id, FrameState{}).first;
  }
  if (it->second.evictable != evictable) {
    evictable_count_ += evictable ? 1 : -1;
  }
  it->second.evictable = evictable;
}

bool LRUKReplacer::Evict(frame_id_t* frame_id) {
  bool found = false;
  frame_id_t best_frame = INVALID_FRAME_ID;
  bool best_is_inf = false;
  size_t best_metric = 0;

  for (const auto& [id, state] : frames_) {
    if (!state.evictable) {
      continue;
    }
    bool is_inf = state.history.size() < k_;
    // Infinite-backward-distance frames tie-break on the oldest recorded
    // access (smaller timestamp = longer untouched = evicted first).
    // Finite-distance frames tie-break on the largest backward k-distance.
    size_t metric = is_inf ? state.history.front() : (current_timestamp_ - state.history.front());

    bool adopt;
    if (!found) {
      adopt = true;
    } else if (is_inf != best_is_inf) {
      adopt = is_inf;  // any +inf frame outranks any finite frame.
    } else if (is_inf) {
      adopt = metric < best_metric;  // older first-touch wins among +inf.
    } else {
      adopt = metric > best_metric;  // larger backward distance wins.
    }

    if (adopt) {
      found = true;
      best_frame = id;
      best_is_inf = is_inf;
      best_metric = metric;
    }
  }

  if (!found) {
    return false;
  }

  frames_.erase(best_frame);
  --evictable_count_;
  *frame_id = best_frame;
  return true;
}

void LRUKReplacer::Remove(frame_id_t frame_id) {
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    return;
  }
  if (!it->second.evictable) {
    throw IOException("LRUKReplacer: cannot remove a non-evictable (pinned) frame");
  }
  --evictable_count_;
  frames_.erase(it);
}

size_t LRUKReplacer::Size() const { return evictable_count_; }

}  // namespace dbengine
