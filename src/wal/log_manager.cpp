#include "wal/log_manager.h"

#include <cstring>

namespace dbengine {

namespace {

constexpr int kMaxRecordsPerPage =
    static_cast<int>((PAGE_SIZE - sizeof(int64_t)) / sizeof(LogRecord));

struct LogPage {
  int64_t count;
  LogRecord records[kMaxRecordsPerPage];
};

}  // namespace

LogManager::LogManager(const std::string& log_path) {
  disk_manager_ = std::make_unique<DiskManager>(log_path);

  if (disk_manager_->GetNumPages() == 0) {
    current_page_id_ = disk_manager_->AllocatePage();
    auto* page = reinterpret_cast<LogPage*>(current_page_buf_);
    page->count = 0;
    next_lsn_ = 0;
  } else {
    current_page_id_ = static_cast<page_id_t>(disk_manager_->GetNumPages()) - 1;
    disk_manager_->ReadPage(current_page_id_, current_page_buf_);
    const auto* page = reinterpret_cast<const LogPage*>(current_page_buf_);
    next_lsn_ = static_cast<int64_t>(current_page_id_) * kMaxRecordsPerPage + page->count;
  }
}

int64_t LogManager::Append(LogRecord record) {
  std::lock_guard<std::mutex> lock(mutex_);

  record.lsn = next_lsn_;
  auto* page = reinterpret_cast<LogPage*>(current_page_buf_);
  if (page->count >= kMaxRecordsPerPage) {
    disk_manager_->WritePage(current_page_id_, current_page_buf_);
    current_page_id_ = disk_manager_->AllocatePage();
    page->count = 0;
  }
  page->records[page->count] = record;
  ++page->count;
  disk_manager_->WritePage(current_page_id_, current_page_buf_);

  ++next_lsn_;
  return record.lsn;
}

void LogManager::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  disk_manager_->Sync();
}

int64_t LogManager::GetNextLsn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_lsn_;
}

size_t LogManager::GetLogSizeBytes() const { return disk_manager_->GetNumPages() * PAGE_SIZE; }

LogRecord LogManager::ReadRecord(int64_t lsn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  page_id_t pid = static_cast<page_id_t>(lsn / kMaxRecordsPerPage);
  int index = static_cast<int>(lsn % kMaxRecordsPerPage);

  if (pid == current_page_id_) {
    const auto* page = reinterpret_cast<const LogPage*>(current_page_buf_);
    return page->records[index];
  }
  char buf[PAGE_SIZE];
  disk_manager_->ReadPage(pid, buf);
  const auto* page = reinterpret_cast<const LogPage*>(buf);
  return page->records[index];
}

std::vector<LogRecord> LogManager::ReadFrom(int64_t start_lsn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogRecord> result;
  if (start_lsn >= next_lsn_) {
    return result;
  }
  result.reserve(static_cast<size_t>(next_lsn_ - start_lsn));

  int64_t lsn = start_lsn;
  while (lsn < next_lsn_) {
    page_id_t pid = static_cast<page_id_t>(lsn / kMaxRecordsPerPage);
    char buf[PAGE_SIZE];
    const LogPage* page;
    if (pid == current_page_id_) {
      page = reinterpret_cast<const LogPage*>(current_page_buf_);
    } else {
      disk_manager_->ReadPage(pid, buf);
      page = reinterpret_cast<const LogPage*>(buf);
    }
    int idx = static_cast<int>(lsn % kMaxRecordsPerPage);
    for (; idx < page->count && lsn < next_lsn_; ++idx, ++lsn) {
      result.push_back(page->records[idx]);
    }
  }
  return result;
}

}  // namespace dbengine
