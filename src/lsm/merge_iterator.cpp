#include "lsm/merge_iterator.h"

namespace dbengine {

LSMMergeIterator::LSMMergeIterator(std::vector<std::unique_ptr<LSMCursor>> cursors)
    : cursors_(std::move(cursors)) {
  for (size_t i = 0; i < cursors_.size(); ++i) {
    if (cursors_[i]->Valid()) {
      heap_.push({cursors_[i]->Key(), static_cast<int>(i)});
    }
  }
  Advance();
}

void LSMMergeIterator::Advance() {
  if (heap_.empty()) {
    valid_ = false;
    return;
  }

  HeapItem winner = heap_.top();
  heap_.pop();
  current_key_ = winner.key;
  current_value_ = cursors_[static_cast<size_t>(winner.priority)]->Value();

  cursors_[static_cast<size_t>(winner.priority)]->Next();
  if (cursors_[static_cast<size_t>(winner.priority)]->Valid()) {
    heap_.push({cursors_[static_cast<size_t>(winner.priority)]->Key(), winner.priority});
  }

  // Any other cursor currently sitting on the same key holds older, now-
  // shadowed data for it — discard those entries (advance past) without
  // emitting them.
  while (!heap_.empty() && heap_.top().key == current_key_) {
    HeapItem shadowed = heap_.top();
    heap_.pop();
    cursors_[static_cast<size_t>(shadowed.priority)]->Next();
    if (cursors_[static_cast<size_t>(shadowed.priority)]->Valid()) {
      heap_.push({cursors_[static_cast<size_t>(shadowed.priority)]->Key(), shadowed.priority});
    }
  }

  valid_ = true;
}

}  // namespace dbengine
