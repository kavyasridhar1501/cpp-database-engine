#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "storage/disk/disk_manager.h"
#include "wal/log_record.h"

namespace dbengine {

// An append-only, page-based write-ahead log. Every LogRecord is fixed-size
// and pages start at page 0, so a record's LSN maps directly to its (page,
// index) by division — no separate index needed for random-access
// ReadRecord(lsn), which recovery's undo phase uses to walk a transaction's
// prev_lsn chain backward.
//
// Append() only pwrites (survives process death via the OS page cache, not
// power loss). Flush() fsyncs — required before Commit() returns, so a
// transaction's COMMIT record (and everything before it) is durable against
// an actual crash.
class LogManager {
 public:
  explicit LogManager(const std::string& log_path);

  LogManager(const LogManager&) = delete;
  LogManager& operator=(const LogManager&) = delete;

  // Assigns `record.lsn` and appends it. Returns the assigned LSN.
  int64_t Append(LogRecord record);

  // fsyncs the log file.
  void Flush();

  int64_t GetNextLsn() const;
  size_t GetLogSizeBytes() const;

  LogRecord ReadRecord(int64_t lsn) const;
  std::vector<LogRecord> ReadFrom(int64_t start_lsn) const;
  std::vector<LogRecord> ReadAll() const { return ReadFrom(0); }

 private:
  std::unique_ptr<DiskManager> disk_manager_;
  mutable std::mutex mutex_;
  int64_t next_lsn_ = 0;
  page_id_t current_page_id_ = INVALID_PAGE_ID;
  alignas(8) char current_page_buf_[PAGE_SIZE]{};
};

}  // namespace dbengine
