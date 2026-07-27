#include "buffer/buffer_pool_manager.h"

#include <numeric>

namespace dbengine {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                                       std::unique_ptr<Replacer> replacer)
    : pool_size_(pool_size),
      disk_manager_(disk_manager),
      replacer_(std::move(replacer)),
      pages_(pool_size) {
  for (frame_id_t i = 0; i < static_cast<frame_id_t>(pool_size_); ++i) {
    free_list_.push_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() { FlushAllPages(); }

bool BufferPoolManager::AcquireFrame(frame_id_t* frame_id) {
  if (!free_list_.empty()) {
    *frame_id = free_list_.front();
    free_list_.pop_front();
    return true;
  }

  if (!replacer_->Evict(frame_id)) {
    return false;
  }

  Page& victim = pages_[*frame_id];
  if (victim.is_dirty_) {
    disk_manager_->WritePage(victim.page_id_, victim.data_);
  }
  page_table_.erase(victim.page_id_);
  victim.ResetMemory();
  return true;
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  frame_id_t frame_id;
  if (!AcquireFrame(&frame_id)) {
    return nullptr;
  }

  *page_id = disk_manager_->AllocatePage();
  Page& page = pages_[frame_id];
  page.page_id_ = *page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;

  page_table_[*page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  return &page;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second;
    Page& page = pages_[frame_id];
    ++page.pin_count_;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);
    ++num_hits_;
    return &page;
  }

  ++num_misses_;

  frame_id_t frame_id;
  if (!AcquireFrame(&frame_id)) {
    return nullptr;
  }

  Page& page = pages_[frame_id];
  disk_manager_->ReadPage(page_id, page.data_);
  page.page_id_ = page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;

  page_table_[page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);
  return &page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }
  frame_id_t frame_id = it->second;
  Page& page = pages_[frame_id];
  if (page.pin_count_ <= 0) {
    return false;
  }
  if (is_dirty) {
    page.is_dirty_ = true;
  }
  if (--page.pin_count_ == 0) {
    replacer_->SetEvictable(frame_id, true);
  }
  return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }
  Page& page = pages_[it->second];
  disk_manager_->WritePage(page.page_id_, page.data_);
  page.is_dirty_ = false;
  return true;
}

void BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::mutex> lock(latch_);
  for (const auto& [page_id, frame_id] : page_table_) {
    Page& page = pages_[frame_id];
    if (page.is_dirty_) {
      disk_manager_->WritePage(page_id, page.data_);
      page.is_dirty_ = false;
    }
  }
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return true;
  }
  frame_id_t frame_id = it->second;
  Page& page = pages_[frame_id];
  if (page.pin_count_ > 0) {
    return false;
  }

  replacer_->SetEvictable(frame_id, true);  // Remove() requires evictable.
  replacer_->Remove(frame_id);
  page_table_.erase(it);
  page.ResetMemory();
  free_list_.push_back(frame_id);
  return true;
}

}  // namespace dbengine
