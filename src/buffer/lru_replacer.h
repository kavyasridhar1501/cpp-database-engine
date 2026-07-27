#pragma once

#include <list>
#include <unordered_map>

#include "buffer/replacer.h"
#include "common/config.h"

namespace dbengine {

// Textbook LRU: frames are kept in a list ordered from least- to
// most-recently used; Evict() picks the least-recently-used frame that is
// currently evictable. This is *not* the replacer BufferPoolManager is meant
// to use in production (LRUKReplacer is) — it exists to give the Phase 1
// benchmark an honest baseline to beat, since plain LRU is the standard
// reference point in the buffer-replacement literature and the OS/DB
// textbooks this project follows (15-445, Hellerstein et al. §4).
class LRUReplacer : public Replacer {
 public:
  explicit LRUReplacer(size_t num_frames);

  bool Evict(frame_id_t* frame_id) override;
  void RecordAccess(frame_id_t frame_id) override;
  void SetEvictable(frame_id_t frame_id, bool evictable) override;
  void Remove(frame_id_t frame_id) override;
  size_t Size() const override;

 private:
  struct FrameState {
    std::list<frame_id_t>::iterator pos;
    bool evictable = false;
  };

  const size_t num_frames_;
  size_t evictable_count_ = 0;
  std::list<frame_id_t> lru_list_;  // front = least recently used, back = most.
  std::unordered_map<frame_id_t, FrameState> frames_;
};

}  // namespace dbengine
