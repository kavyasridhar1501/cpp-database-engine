#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "common/config.h"

namespace dbengine {

// DiskManager owns a single heap file and performs page-granular reads and
// writes against it. Page `i` lives at byte offset `i * PAGE_SIZE`. All I/O
// goes through pread/pwrite so callers never need to seek, and concurrent
// reads/writes to different pages don't race on a shared file offset.
//
// This class is the only thing in the engine that talks to the filesystem.
// Higher layers (buffer pool, storage engines) never open files themselves.
class DiskManager {
 public:
  explicit DiskManager(const std::string& db_file);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads PAGE_SIZE bytes for `page_id` into `page_data`. `page_data` must
  // point to a buffer of at least PAGE_SIZE bytes. Reading a page beyond the
  // current end of file zero-fills the buffer (treated as an allocated but
  // never-written page).
  void ReadPage(page_id_t page_id, char* page_data);

  // Writes PAGE_SIZE bytes from `page_data` to `page_id`, extending the file
  // if necessary. Does not implicitly fsync; call Sync for durability
  // guarantees.
  void WritePage(page_id_t page_id, const char* page_data);

  // fsyncs the underlying file descriptor.
  void Sync();

  // Allocates and returns a new page id. Does not write any data; the page
  // reads as zero-filled until the first WritePage.
  page_id_t AllocatePage();

  // Number of pages currently allocated (== file_size / PAGE_SIZE, rounded
  // up).
  size_t GetNumPages() const;

  size_t GetNumReads() const { return num_reads_.load(); }
  size_t GetNumWrites() const { return num_writes_.load(); }

 private:
  int fd_;
  std::string file_name_;
  std::atomic<page_id_t> next_page_id_{0};
  std::atomic<size_t> num_reads_{0};
  std::atomic<size_t> num_writes_{0};
};

}  // namespace dbengine
