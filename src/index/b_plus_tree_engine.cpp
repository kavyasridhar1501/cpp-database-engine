#include "index/b_plus_tree_engine.h"

#include "buffer/lru_k_replacer.h"
#include "index/internal_page.h"
#include "index/leaf_page.h"

namespace dbengine {

namespace {

class BPlusTreeStorageIterator : public StorageIterator {
 public:
  explicit BPlusTreeStorageIterator(BPlusTree::Iterator it) : it_(std::move(it)) {}

  bool IsValid() const override { return !it_.IsEnd(); }
  void Next() override { ++it_; }
  KeyType GetKey() const override { return it_.GetKey(); }
  std::string GetValue() const override { return it_.GetValue(); }

 private:
  BPlusTree::Iterator it_;
};

}  // namespace

BPlusTreeEngine::BPlusTreeEngine(const std::string& db_file, size_t buffer_pool_size) {
  disk_manager_ = std::make_unique<DiskManager>(db_file);
  bpm_ = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager_.get(),
                                              std::make_unique<LRUKReplacer>(buffer_pool_size, 2));

  page_id_t root_page_id = INVALID_PAGE_ID;
  if (disk_manager_->GetNumPages() == 0) {
    page_id_t meta_id;
    Page* meta = bpm_->NewPage(&meta_id);
    reinterpret_cast<MetaPage*>(meta->GetData())->root_page_id = INVALID_PAGE_ID;
    bpm_->UnpinPage(meta_id, true);
  } else {
    Page* meta = bpm_->FetchPage(kMetaPageId);
    root_page_id = reinterpret_cast<MetaPage*>(meta->GetData())->root_page_id;
    bpm_->UnpinPage(kMetaPageId, false);
  }

  tree_ = std::make_unique<BPlusTree>(bpm_.get(), root_page_id, LeafPage::Capacity() - 1,
                                      InternalPage::Capacity() - 1);
}

void BPlusTreeEngine::PersistRootPageIdIfChanged() {
  Page* meta = bpm_->FetchPage(kMetaPageId);
  auto* mp = reinterpret_cast<MetaPage*>(meta->GetData());
  bool changed = mp->root_page_id != tree_->GetRootPageId();
  if (changed) {
    mp->root_page_id = tree_->GetRootPageId();
  }
  bpm_->UnpinPage(kMetaPageId, changed);
}

bool BPlusTreeEngine::Get(KeyType key, std::string* value) { return tree_->GetValue(key, value); }

void BPlusTreeEngine::Put(KeyType key, const std::string& value) {
  tree_->Insert(key, value);
  PersistRootPageIdIfChanged();
}

bool BPlusTreeEngine::Delete(KeyType key) {
  bool removed = tree_->Remove(key);
  if (removed) {
    PersistRootPageIdIfChanged();
  }
  return removed;
}

std::unique_ptr<StorageIterator> BPlusTreeEngine::Scan(KeyType start_key) {
  BPlusTree::Iterator it = (start_key == INVALID_KEY) ? tree_->Begin() : tree_->Begin(start_key);
  return std::make_unique<BPlusTreeStorageIterator>(std::move(it));
}

}  // namespace dbengine
