#include "storage/disk/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/exception.h"

namespace dbengine {

DiskManager::DiskManager(const std::string& db_file) : file_name_(db_file) {
  fd_ = ::open(db_file.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ == -1) {
    throw IOException("DiskManager: failed to open db file '" + db_file + "': " +
                       std::strerror(errno));
  }

  struct stat st{};
  if (::fstat(fd_, &st) == -1) {
    throw IOException("DiskManager: fstat failed for '" + db_file + "': " + std::strerror(errno));
  }
  next_page_id_.store(static_cast<page_id_t>((st.st_size + PAGE_SIZE - 1) / PAGE_SIZE));
}

DiskManager::~DiskManager() {
  if (fd_ != -1) {
    ::close(fd_);
  }
}

void DiskManager::ReadPage(page_id_t page_id, char* page_data) {
  const off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
  ssize_t bytes_read = ::pread(fd_, page_data, PAGE_SIZE, offset);
  if (bytes_read == -1) {
    throw IOException("DiskManager: read failed for page " + std::to_string(page_id) + ": " +
                       std::strerror(errno));
  }
  // Short read (e.g. page never written) is zero-filled rather than an error;
  // an allocated-but-unwritten page is legitimate.
  if (static_cast<size_t>(bytes_read) < PAGE_SIZE) {
    std::memset(page_data + bytes_read, 0, PAGE_SIZE - bytes_read);
  }
  ++num_reads_;
}

void DiskManager::WritePage(page_id_t page_id, const char* page_data) {
  const off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
  ssize_t bytes_written = ::pwrite(fd_, page_data, PAGE_SIZE, offset);
  if (bytes_written == -1 || static_cast<size_t>(bytes_written) != PAGE_SIZE) {
    throw IOException("DiskManager: write failed for page " + std::to_string(page_id) + ": " +
                       std::strerror(errno));
  }
  ++num_writes_;
}

void DiskManager::Sync() {
  if (::fsync(fd_) == -1) {
    throw IOException("DiskManager: fsync failed: " + std::string(std::strerror(errno)));
  }
}

page_id_t DiskManager::AllocatePage() { return next_page_id_.fetch_add(1); }

size_t DiskManager::GetNumPages() const { return static_cast<size_t>(next_page_id_.load()); }

}  // namespace dbengine
