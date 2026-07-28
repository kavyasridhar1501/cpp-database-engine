#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "storage/disk/disk_manager.h"
#include "wal/log_record.h"

namespace dbengine {

// An append-only, page-based write-ahead log. Records are packed into
// fixed-size pages (like LeafPage/SSTable data pages — every LogRecord is
// the same size, so no slotted/variable-length framing is needed) in a
// dedicated file, always starting at page 0, so a record's LSN maps
// directly to its (page, index) by simple division — no separate index
// structure needed to support random-access ReadRecord(lsn), which
// recovery's undo phase relies on to walk a transaction's prev_lsn chain
// backward.
//
// Append() writes the record's page via DiskManager (a pwrite — durable
// against this process being killed, since the write lands in the OS page
// cache regardless of fsync; see DESIGN.md). Flush() additionally fsyncs,
// which is what's needed for durability against a *power loss/OS crash*
// rather than just this process dying — the golden rule this project
// enforces is "a transaction's COMMIT record (and everything before it) is
// fsynced before Commit() returns to the caller."
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
