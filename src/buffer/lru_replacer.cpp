#include "buffer/lru_replacer.h"

#include "common/exception.h"

namespace dbengine {

LRUReplacer::LRUReplacer(size_t num_frames) : num_frames_(num_frames) {}

void LRUReplacer::RecordAccess(frame_id_t frame_id) {
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= num_frames_) {
    throw IOException("LRUReplacer: frame_id out of range");
  }
  auto it = frames_.find(frame_id);
  if (it != frames_.end()) {
    lru_list_.erase(it->second.pos);
  }
  lru_list_.push_back(frame_id);
  frames_[frame_id].pos = std::prev(lru_list_.end());
}

void LRUReplacer::SetEvictable(frame_id_t frame_id, bool evictable) {
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    lru_list_.push_back(frame_id);
    it = frames_.emplace(frame_id, FrameState{std::prev(lru_list_.end()), false}).first;
  }
  if (it->second.evictable != evictable) {
    evictable_count_ += evictable ? 1 : -1;
  }
  it->second.evictable = evictable;
}

bool LRUReplacer::Evict(frame_id_t* frame_id) {
  for (auto it = lru_list_.begin(); it != lru_list_.end(); ++it) {
    auto frame_it = frames_.find(*it);
    if (frame_it != frames_.end() && frame_it->second.evictable) {
      *frame_id = *it;
      lru_list_.erase(it);
      frames_.erase(frame_it);
      --evictable_count_;
      return true;
    }
  }
  return false;
}

void LRUReplacer::Remove(frame_id_t frame_id) {
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    return;
  }
  if (!it->second.evictable) {
    throw IOException("LRUReplacer: cannot remove a non-evictable (pinned) frame");
  }
  lru_list_.erase(it->second.pos);
  --evictable_count_;
  frames_.erase(it);
}

size_t LRUReplacer::Size() const { return evictable_count_; }

}  // namespace dbengine
