#include "lsm/sstable.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "buffer/lru_k_replacer.h"

namespace dbengine {

namespace {

constexpr size_t kSSTableBufferPoolFrames = 8;

struct DataEntry {
  KeyType key;
  uint8_t deleted;
  uint16_t value_len;
  char value[MAX_VALUE_SIZE];
};

constexpr int kMaxEntriesPerDataPage =
    static_cast<int>((PAGE_SIZE - sizeof(int64_t)) / sizeof(DataEntry));

struct DataPage {
  int64_t count;
  DataEntry entries[kMaxEntriesPerDataPage];
};

struct IndexEntry {
  KeyType first_key;
  page_id_t page_id;
};

constexpr int kMaxIndexEntriesPerPage =
    static_cast<int>((PAGE_SIZE - sizeof(int64_t)) / sizeof(IndexEntry));

struct IndexPage {
  int64_t count;
  IndexEntry entries[kMaxIndexEntriesPerPage];
};

struct HeaderPage {
  int64_t num_entries;
  KeyType min_key;
  KeyType max_key;
  int32_t num_data_pages;
  page_id_t first_data_page_id;
  int64_t num_index_entries;
  page_id_t first_index_page_id;
  int64_t bloom_num_bytes;
  size_t bloom_num_bits;
  int32_t bloom_num_hashes;
  page_id_t first_bloom_page_id;
};

}  // namespace

void SSTable::Build(const std::string& path, const std::vector<std::pair<KeyType, LSMValue>>& entries,
                     double bloom_false_positive_rate) {
  DiskManager dm(path);

  page_id_t header_page_id = dm.AllocatePage();

  BloomFilter bloom(std::max<size_t>(entries.size(), 1), bloom_false_positive_rate);
  for (const auto& [key, value] : entries) {
    if (value.value.size() > MAX_VALUE_SIZE) {
      throw std::invalid_argument("SSTable::Build: value exceeds MAX_VALUE_SIZE");
    }
    bloom.Add(key);
  }

  std::vector<std::pair<KeyType, page_id_t>> index;
  page_id_t first_data_page_id = INVALID_PAGE_ID;
  int32_t num_data_pages = 0;
  {
    size_t i = 0;
    while (i < entries.size()) {
      page_id_t pid = dm.AllocatePage();
      if (first_data_page_id == INVALID_PAGE_ID) {
        first_data_page_id = pid;
      }
      DataPage page{};
      page.count = 0;
      KeyType page_first_key = entries[i].first;
      while (i < entries.size() && page.count < kMaxEntriesPerDataPage) {
        const auto& [key, value] = entries[i];
        DataEntry& e = page.entries[page.count];
        e.key = key;
        e.deleted = value.deleted ? 1 : 0;
        e.value_len = static_cast<uint16_t>(value.value.size());
        std::memcpy(e.value, value.value.data(), value.value.size());
        ++page.count;
        ++i;
      }
      dm.WritePage(pid, reinterpret_cast<const char*>(&page));
      index.emplace_back(page_first_key, pid);
      ++num_data_pages;
    }
  }

  page_id_t first_index_page_id = INVALID_PAGE_ID;
  {
    size_t i = 0;
    while (i < index.size()) {
      page_id_t pid = dm.AllocatePage();
      if (first_index_page_id == INVALID_PAGE_ID) {
        first_index_page_id = pid;
      }
      IndexPage page{};
      page.count = 0;
      while (i < index.size() && page.count < kMaxIndexEntriesPerPage) {
        page.entries[page.count] = IndexEntry{index[i].first, index[i].second};
        ++page.count;
        ++i;
      }
      dm.WritePage(pid, reinterpret_cast<const char*>(&page));
    }
  }

  page_id_t first_bloom_page_id = INVALID_PAGE_ID;
  const auto& bloom_bytes = bloom.ToBytes();
  {
    size_t offset = 0;
    while (offset < bloom_bytes.size()) {
      page_id_t pid = dm.AllocatePage();
      if (first_bloom_page_id == INVALID_PAGE_ID) {
        first_bloom_page_id = pid;
      }
      char buf[PAGE_SIZE] = {0};
      size_t chunk = std::min(bloom_bytes.size() - offset, PAGE_SIZE);
      std::memcpy(buf, bloom_bytes.data() + offset, chunk);
      dm.WritePage(pid, buf);
      offset += chunk;
    }
  }

  HeaderPage header{};
  header.num_entries = static_cast<int64_t>(entries.size());
  header.min_key = entries.empty() ? 0 : entries.front().first;
  header.max_key = entries.empty() ? 0 : entries.back().first;
  header.num_data_pages = num_data_pages;
  header.first_data_page_id = first_data_page_id;
  header.num_index_entries = static_cast<int64_t>(index.size());
  header.first_index_page_id = first_index_page_id;
  header.bloom_num_bytes = static_cast<int64_t>(bloom_bytes.size());
  header.bloom_num_bits = bloom.NumBits();
  header.bloom_num_hashes = bloom.NumHashes();
  header.first_bloom_page_id = first_bloom_page_id;
  dm.WritePage(header_page_id, reinterpret_cast<const char*>(&header));
}

std::shared_ptr<SSTable> SSTable::Open(const std::string& path) {
  std::shared_ptr<SSTable> sst(new SSTable());
  sst->path_ = path;
  sst->disk_manager_ = std::make_unique<DiskManager>(path);

  char buf[PAGE_SIZE];
  sst->disk_manager_->ReadPage(0, buf);
  const auto* header = reinterpret_cast<const HeaderPage*>(buf);

  sst->num_entries_ = header->num_entries;
  sst->min_key_ = header->min_key;
  sst->max_key_ = header->max_key;
  sst->num_data_pages_ = header->num_data_pages;
  sst->first_data_page_id_ = header->first_data_page_id;

  {
    int64_t remaining = header->num_index_entries;
    page_id_t pid = header->first_index_page_id;
    sst->sparse_index_.reserve(static_cast<size_t>(remaining));
    while (remaining > 0) {
      char ibuf[PAGE_SIZE];
      sst->disk_manager_->ReadPage(pid, ibuf);
      const auto* page = reinterpret_cast<const IndexPage*>(ibuf);
      for (int64_t i = 0; i < page->count; ++i) {
        sst->sparse_index_.emplace_back(page->entries[i].first_key, page->entries[i].page_id);
      }
      remaining -= page->count;
      ++pid;
    }
  }

  {
    auto num_bytes = static_cast<size_t>(header->bloom_num_bytes);
    std::vector<uint8_t> bytes(num_bytes);
    size_t offset = 0;
    page_id_t pid = header->first_bloom_page_id;
    while (offset < num_bytes) {
      char bbuf[PAGE_SIZE];
      sst->disk_manager_->ReadPage(pid, bbuf);
      size_t chunk = std::min(num_bytes - offset, PAGE_SIZE);
      std::memcpy(bytes.data() + offset, bbuf, chunk);
      offset += chunk;
      ++pid;
    }
    sst->bloom_ = std::make_unique<BloomFilter>(std::move(bytes), header->bloom_num_bits,
                                                 header->bloom_num_hashes);
  }

  sst->bpm_ = std::make_unique<BufferPoolManager>(
      kSSTableBufferPoolFrames, sst->disk_manager_.get(),
      std::make_unique<LRUKReplacer>(kSSTableBufferPoolFrames, 2));

  return sst;
}

SSTable::~SSTable() {
  bpm_.reset();
  disk_manager_.reset();
  if (obsolete_) {
    std::remove(path_.c_str());
  }
}

bool SSTable::MightContain(KeyType key) const {
  if (num_entries_ == 0 || key < min_key_ || key > max_key_) {
    return false;
  }
  return bloom_->MightContain(key);
}

int32_t SSTable::FindPagePos(KeyType key) const {
  auto it = std::upper_bound(sparse_index_.begin(), sparse_index_.end(), key,
                              [](KeyType k, const std::pair<KeyType, page_id_t>& entry) {
                                return k < entry.first;
                              });
  if (it == sparse_index_.begin()) {
    return 0;
  }
  return static_cast<int32_t>(std::distance(sparse_index_.begin(), it) - 1);
}

bool SSTable::Get(KeyType key, LSMValue* out) {
  if (!MightContain(key)) {
    return false;
  }

  int32_t page_pos = FindPagePos(key);
  page_id_t page_id = sparse_index_[static_cast<size_t>(page_pos)].second;
  Page* page = bpm_->FetchPage(page_id);
  const auto* view = reinterpret_cast<const DataPage*>(page->GetData());

  auto it = std::lower_bound(
      view->entries, view->entries + view->count, key,
      [](const DataEntry& entry, KeyType k) { return entry.key < k; });

  bool found = it != view->entries + view->count && it->key == key;
  if (found) {
    out->deleted = it->deleted != 0;
    out->value.assign(it->value, it->value_len);
  }
  bpm_->UnpinPage(page_id, false);
  return found;
}

SSTable::Iterator SSTable::NewIterator(KeyType start_key) {
  Iterator iter;
  if (num_entries_ == 0 || start_key > max_key_) {
    return iter;
  }

  int32_t page_pos = FindPagePos(std::max(start_key, min_key_));
  page_id_t page_id = sparse_index_[static_cast<size_t>(page_pos)].second;
  Page* page = bpm_->FetchPage(page_id);
  const auto* view = reinterpret_cast<const DataPage*>(page->GetData());

  auto it = std::lower_bound(
      view->entries, view->entries + view->count, start_key,
      [](const DataEntry& entry, KeyType k) { return entry.key < k; });
  int index = static_cast<int>(std::distance(view->entries, it));

  if (index >= view->count) {
    // start_key falls after this page's last entry but (per the range check
    // above) not past the table's max key, so the next page must contain it.
    bpm_->UnpinPage(page_id, false);
    ++page_pos;
    page_id = first_data_page_id_ + page_pos;
    page = bpm_->FetchPage(page_id);
    index = 0;
  }

  iter.owner_ = this;
  iter.page_ = page;
  iter.page_id_ = page_id;
  iter.index_ = index;
  iter.page_pos_ = page_pos;
  iter.LoadCurrent();
  return iter;
}

SSTable::Iterator::~Iterator() { Release(); }

void SSTable::Iterator::Release() {
  if (page_ != nullptr) {
    owner_->bpm_->UnpinPage(page_id_, false);
    page_ = nullptr;
  }
}

void SSTable::Iterator::MoveFrom(Iterator& other) {
  owner_ = other.owner_;
  page_ = other.page_;
  page_id_ = other.page_id_;
  index_ = other.index_;
  page_pos_ = other.page_pos_;
  cached_key_ = other.cached_key_;
  cached_value_ = std::move(other.cached_value_);
  other.page_ = nullptr;
}

SSTable::Iterator::Iterator(Iterator&& other) noexcept { MoveFrom(other); }

SSTable::Iterator& SSTable::Iterator::operator=(Iterator&& other) noexcept {
  if (this != &other) {
    Release();
    MoveFrom(other);
  }
  return *this;
}

void SSTable::Iterator::LoadCurrent() {
  const auto* view = reinterpret_cast<const DataPage*>(page_->GetData());
  const DataEntry& e = view->entries[index_];
  cached_key_ = e.key;
  cached_value_.deleted = e.deleted != 0;
  cached_value_.value.assign(e.value, e.value_len);
}

void SSTable::Iterator::Next() {
  const auto* view = reinterpret_cast<const DataPage*>(page_->GetData());
  ++index_;
  if (index_ >= view->count) {
    owner_->bpm_->UnpinPage(page_id_, false);
    page_ = nullptr;
    ++page_pos_;
    if (page_pos_ >= owner_->num_data_pages_) {
      page_id_ = INVALID_PAGE_ID;
      return;
    }
    page_id_ = owner_->first_data_page_id_ + page_pos_;
    page_ = owner_->bpm_->FetchPage(page_id_);
    index_ = 0;
  }
  LoadCurrent();
}

}  // namespace dbengine
