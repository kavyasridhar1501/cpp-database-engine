#include "index/b_plus_tree.h"

#include <stdexcept>

namespace dbengine {

BPlusTree::BPlusTree(BufferPoolManager* bpm, page_id_t root_page_id, int leaf_max_size,
                     int internal_max_size)
    : bpm_(bpm),
      root_page_id_(root_page_id),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  if (leaf_max_size_ < 3 || leaf_max_size_ > LeafPage::Capacity() - 1) {
    throw std::invalid_argument("BPlusTree: leaf_max_size out of range");
  }
  if (internal_max_size_ < 3 || internal_max_size_ > InternalPage::Capacity() - 1) {
    throw std::invalid_argument("BPlusTree: internal_max_size out of range");
  }
}

// ---------------------------------------------------------------------------
// Iterator
// ---------------------------------------------------------------------------

BPlusTree::Iterator::~Iterator() { Release(); }

void BPlusTree::Iterator::Release() {
  if (page_ != nullptr) {
    bpm_->UnpinPage(page_id_, false);
    page_ = nullptr;
  }
}

void BPlusTree::Iterator::MoveFrom(Iterator& other) {
  bpm_ = other.bpm_;
  page_id_ = other.page_id_;
  page_ = other.page_;
  index_ = other.index_;
  other.page_ = nullptr;
  other.page_id_ = INVALID_PAGE_ID;
}

BPlusTree::Iterator::Iterator(Iterator&& other) noexcept { MoveFrom(other); }

BPlusTree::Iterator& BPlusTree::Iterator::operator=(Iterator&& other) noexcept {
  if (this != &other) {
    Release();
    MoveFrom(other);
  }
  return *this;
}

KeyType BPlusTree::Iterator::GetKey() const {
  return reinterpret_cast<LeafPage*>(page_->GetData())->KeyAt(index_);
}

std::string BPlusTree::Iterator::GetValue() const {
  return reinterpret_cast<LeafPage*>(page_->GetData())->ValueAt(index_);
}

BPlusTree::Iterator& BPlusTree::Iterator::operator++() {
  LeafPage* leaf = reinterpret_cast<LeafPage*>(page_->GetData());
  ++index_;
  if (index_ >= leaf->GetSize()) {
    page_id_t next = leaf->GetNextPageId();
    bpm_->UnpinPage(page_id_, false);
    page_ = nullptr;
    if (next != INVALID_PAGE_ID) {
      page_id_ = next;
      page_ = bpm_->FetchPage(next);
      index_ = 0;
    } else {
      page_id_ = INVALID_PAGE_ID;
    }
  }
  return *this;
}

BPlusTree::Iterator BPlusTree::Begin() {
  std::lock_guard<std::mutex> lock(latch_);
  if (IsEmpty()) {
    return Iterator();
  }
  PageGuard guard = FindLeafPageGuard(INVALID_KEY, /*leftmost=*/true);
  page_id_t page_id = guard.page_id();
  Page* page = guard.Release();
  return Iterator(bpm_, page_id, page, 0);
}

BPlusTree::Iterator BPlusTree::Begin(KeyType key) {
  std::lock_guard<std::mutex> lock(latch_);
  if (IsEmpty()) {
    return Iterator();
  }
  PageGuard guard = FindLeafPageGuard(key);
  LeafPage* leaf = AsLeaf(guard.page());
  int idx = leaf->KeyIndex(key);
  if (idx >= leaf->GetSize()) {
    page_id_t next = leaf->GetNextPageId();
    guard.Reset();
    if (next == INVALID_PAGE_ID) {
      return Iterator();
    }
    Page* next_page = bpm_->FetchPage(next);
    return Iterator(bpm_, next, next_page, 0);
  }
  page_id_t page_id = guard.page_id();
  Page* page = guard.Release();
  return Iterator(bpm_, page_id, page, idx);
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

BPlusTree::PageGuard BPlusTree::FindLeafPageGuard(KeyType key, bool leftmost) const {
  page_id_t cur = root_page_id_;
  Page* page = bpm_->FetchPage(cur);
  BPlusTreePage* node = AsTreePage(page);
  while (!node->IsLeafPage()) {
    InternalPage* internal = reinterpret_cast<InternalPage*>(node);
    page_id_t child = leftmost ? internal->ValueAt(0) : internal->Lookup(key);
    Page* child_page = bpm_->FetchPage(child);
    bpm_->UnpinPage(cur, false);
    cur = child;
    page = child_page;
    node = AsTreePage(page);
  }
  return PageGuard(bpm_, cur, page);
}

bool BPlusTree::GetValue(KeyType key, std::string* value) {
  std::lock_guard<std::mutex> lock(latch_);
  if (IsEmpty()) {
    return false;
  }
  PageGuard leaf_guard = FindLeafPageGuard(key);
  return AsLeaf(leaf_guard.page())->Lookup(key, value);
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

bool BPlusTree::Insert(KeyType key, const std::string& value) {
  std::lock_guard<std::mutex> lock(latch_);

  if (IsEmpty()) {
    page_id_t new_page_id;
    Page* page = bpm_->NewPage(&new_page_id);
    if (page == nullptr) {
      throw std::runtime_error("BPlusTree::Insert: buffer pool exhausted creating root");
    }
    LeafPage* leaf = AsLeaf(page);
    leaf->Init(INVALID_PAGE_ID, leaf_max_size_);
    leaf->Insert(key, value);
    bpm_->UnpinPage(new_page_id, true);
    root_page_id_ = new_page_id;
    return true;
  }

  PageGuard leaf_guard = FindLeafPageGuard(key);
  LeafPage* leaf = AsLeaf(leaf_guard.page());
  int old_size = leaf->GetSize();
  int new_size = leaf->Insert(key, value);
  bool inserted_new = new_size > old_size;
  leaf_guard.SetDirty();

  if (new_size > leaf_max_size_) {
    page_id_t new_leaf_id;
    Page* new_page = bpm_->NewPage(&new_leaf_id);
    if (new_page == nullptr) {
      throw std::runtime_error("BPlusTree::Insert: buffer pool exhausted splitting leaf");
    }
    LeafPage* new_leaf = AsLeaf(new_page);
    new_leaf->Init(leaf->GetParentPageId(), leaf_max_size_);
    leaf->MoveHalfTo(new_leaf, new_leaf_id);
    KeyType middle_key = new_leaf->KeyAt(0);
    page_id_t leaf_id = leaf_guard.page_id();

    PageGuard new_leaf_guard(bpm_, new_leaf_id, new_page);
    new_leaf_guard.SetDirty();

    InsertIntoParent(leaf_id, middle_key, new_leaf_id);
  }

  return inserted_new;
}

void BPlusTree::InsertIntoParent(page_id_t left_id, KeyType middle_key, page_id_t right_id) {
  PageGuard left_guard(bpm_, left_id, bpm_->FetchPage(left_id));
  BPlusTreePage* left_node = AsTreePage(left_guard.page());
  page_id_t parent_id = left_node->GetParentPageId();

  if (parent_id == INVALID_PAGE_ID) {
    page_id_t new_root_id;
    Page* new_root_page = bpm_->NewPage(&new_root_id);
    if (new_root_page == nullptr) {
      throw std::runtime_error("BPlusTree::InsertIntoParent: buffer pool exhausted creating root");
    }
    InternalPage* new_root = AsInternal(new_root_page);
    new_root->Init(INVALID_PAGE_ID, internal_max_size_);
    new_root->PopulateNewRoot(left_id, middle_key, right_id);
    bpm_->UnpinPage(new_root_id, true);

    left_node->SetParentPageId(new_root_id);
    left_guard.SetDirty();

    Page* right_page = bpm_->FetchPage(right_id);
    AsTreePage(right_page)->SetParentPageId(new_root_id);
    bpm_->UnpinPage(right_id, true);

    root_page_id_ = new_root_id;
    return;
  }

  PageGuard parent_guard(bpm_, parent_id, bpm_->FetchPage(parent_id));
  InternalPage* parent = AsInternal(parent_guard.page());
  int new_size = parent->InsertNodeAfter(left_id, middle_key, right_id);
  parent_guard.SetDirty();

  if (new_size > internal_max_size_) {
    page_id_t new_internal_id;
    Page* new_internal_page = bpm_->NewPage(&new_internal_id);
    if (new_internal_page == nullptr) {
      throw std::runtime_error("BPlusTree::InsertIntoParent: buffer pool exhausted splitting internal node");
    }
    InternalPage* new_internal = AsInternal(new_internal_page);
    new_internal->Init(parent->GetParentPageId(), internal_max_size_);
    parent->MoveHalfTo(new_internal, new_internal_id, bpm_);
    KeyType up_key = new_internal->KeyAt(0);
    bpm_->UnpinPage(new_internal_id, true);

    InsertIntoParent(parent_id, up_key, new_internal_id);
  }
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

bool BPlusTree::Remove(KeyType key) {
  std::lock_guard<std::mutex> lock(latch_);
  if (IsEmpty()) {
    return false;
  }

  PageGuard leaf_guard = FindLeafPageGuard(key);
  LeafPage* leaf = AsLeaf(leaf_guard.page());
  if (!leaf->Remove(key)) {
    return false;
  }
  leaf_guard.SetDirty();
  page_id_t leaf_id = leaf_guard.page_id();

  HandleUnderflow(leaf_id, std::move(leaf_guard));
  return true;
}

void BPlusTree::HandleRootUnderflow(page_id_t node_id, PageGuard node_guard) {
  BPlusTreePage* node = AsTreePage(node_guard.page());

  if (node->IsLeafPage()) {
    if (node->GetSize() == 0) {
      node_guard.Reset();
      bpm_->DeletePage(node_id);
      root_page_id_ = INVALID_PAGE_ID;
    }
    return;
  }

  InternalPage* internal = reinterpret_cast<InternalPage*>(node);
  if (internal->GetSize() == 1) {
    page_id_t only_child = internal->RemoveAndReturnOnlyChild();
    node_guard.Reset();
    bpm_->DeletePage(node_id);

    Page* child_page = bpm_->FetchPage(only_child);
    AsTreePage(child_page)->SetParentPageId(INVALID_PAGE_ID);
    bpm_->UnpinPage(only_child, true);

    root_page_id_ = only_child;
  }
}

void BPlusTree::HandleUnderflow(page_id_t node_id, PageGuard node_guard) {
  BPlusTreePage* node = AsTreePage(node_guard.page());

  if (node->IsRootPage()) {
    HandleRootUnderflow(node_id, std::move(node_guard));
    return;
  }
  if (node->GetSize() >= node->GetMinSize()) {
    return;
  }

  page_id_t parent_id = node->GetParentPageId();
  PageGuard parent_guard(bpm_, parent_id, bpm_->FetchPage(parent_id));
  InternalPage* parent = AsInternal(parent_guard.page());
  int idx = parent->ValueIndex(node_id);
  bool has_left = idx > 0;
  bool has_right = idx < parent->GetSize() - 1;

  if (node->IsLeafPage()) {
    LeafPage* leaf = reinterpret_cast<LeafPage*>(node);

    if (has_left) {
      page_id_t left_id = parent->ValueAt(idx - 1);
      PageGuard left_guard(bpm_, left_id, bpm_->FetchPage(left_id));
      LeafPage* left = AsLeaf(left_guard.page());
      if (left->GetSize() > left->GetMinSize()) {
        left->MoveLastToFrontOf(leaf);
        parent->SetKeyAt(idx, leaf->KeyAt(0));
        left_guard.SetDirty();
        node_guard.SetDirty();
        parent_guard.SetDirty();
        return;
      }
    }
    if (has_right) {
      page_id_t right_id = parent->ValueAt(idx + 1);
      PageGuard right_guard(bpm_, right_id, bpm_->FetchPage(right_id));
      LeafPage* right = AsLeaf(right_guard.page());
      if (right->GetSize() > right->GetMinSize()) {
        right->MoveFirstToEndOf(leaf);
        parent->SetKeyAt(idx + 1, right->KeyAt(0));
        right_guard.SetDirty();
        node_guard.SetDirty();
        parent_guard.SetDirty();
        return;
      }
    }
    if (has_left) {
      page_id_t left_id = parent->ValueAt(idx - 1);
      PageGuard left_guard(bpm_, left_id, bpm_->FetchPage(left_id));
      LeafPage* left = AsLeaf(left_guard.page());
      leaf->MoveAllTo(left);
      left_guard.SetDirty();
      parent->Remove(idx);
      parent_guard.SetDirty();
      node_guard.Reset();
      bpm_->DeletePage(node_id);
      left_guard.Reset();
      parent_guard.Reset();
      HandleUnderflow(parent_id, PageGuard(bpm_, parent_id, bpm_->FetchPage(parent_id)));
      return;
    }
    // No left sibling: a non-root node always has at least one sibling, so
    // the right sibling must exist.
    page_id_t right_id = parent->ValueAt(idx + 1);
    PageGuard right_guard(bpm_, right_id, bpm_->FetchPage(right_id));
    LeafPage* right = AsLeaf(right_guard.page());
    right->MoveAllTo(leaf);
    node_guard.SetDirty();
    parent->Remove(idx + 1);
    parent_guard.SetDirty();
    right_guard.Reset();
    bpm_->DeletePage(right_id);
    node_guard.Reset();
    parent_guard.Reset();
    HandleUnderflow(parent_id, PageGuard(bpm_, parent_id, bpm_->FetchPage(parent_id)));
    return;
  }

  // Internal node underflow: same structure, but redistribute/merge need
  // the parent's separator key and BufferPoolManager to fix up moved
  // children's parent pointers.
  InternalPage* internal = reinterpret_cast<InternalPage*>(node);

  if (has_left) {
    page_id_t left_id = parent->ValueAt(idx - 1);
    PageGuard left_guard(bpm_, left_id, bpm_->FetchPage(left_id));
    InternalPage* left = AsInternal(left_guard.page());
    if (left->GetSize() > left->GetMinSize()) {
      KeyType new_separator = left->KeyAt(left->GetSize() - 1);
      left->MoveLastToFrontOf(internal, node_id, parent->KeyAt(idx), bpm_);
      parent->SetKeyAt(idx, new_separator);
      left_guard.SetDirty();
      node_guard.SetDirty();
      parent_guard.SetDirty();
      return;
    }
  }
  if (has_right) {
    page_id_t right_id = parent->ValueAt(idx + 1);
    PageGuard right_guard(bpm_, right_id, bpm_->FetchPage(right_id));
    InternalPage* right = AsInternal(right_guard.page());
    if (right->GetSize() > right->GetMinSize()) {
      KeyType new_separator = right->KeyAt(1);
      right->MoveFirstToEndOf(internal, node_id, parent->KeyAt(idx + 1), bpm_);
      parent->SetKeyAt(idx + 1, new_separator);
      right_guard.SetDirty();
      node_guard.SetDirty();
      parent_guard.SetDirty();
      return;
    }
  }
  if (has_left) {
    page_id_t left_id = parent->ValueAt(idx - 1);
    PageGuard left_guard(bpm_, left_id, bpm_->FetchPage(left_id));
    InternalPage* left = AsInternal(left_guard.page());
    internal->MoveAllTo(left, left_id, parent->KeyAt(idx), bpm_);
    left_guard.SetDirty();
    parent->Remove(idx);
    parent_guard.SetDirty();
    node_guard.Reset();
    bpm_->DeletePage(node_id);
    left_guard.Reset();
    parent_guard.Reset();
    HandleUnderflow(parent_id, PageGuard(bpm_, parent_id, bpm_->FetchPage(parent_id)));
    return;
  }
  page_id_t right_id = parent->ValueAt(idx + 1);
  PageGuard right_guard(bpm_, right_id, bpm_->FetchPage(right_id));
  InternalPage* right = AsInternal(right_guard.page());
  right->MoveAllTo(internal, node_id, parent->KeyAt(idx + 1), bpm_);
  node_guard.SetDirty();
  parent->Remove(idx + 1);
  parent_guard.SetDirty();
  right_guard.Reset();
  bpm_->DeletePage(right_id);
  node_guard.Reset();
  parent_guard.Reset();
  HandleUnderflow(parent_id, PageGuard(bpm_, parent_id, bpm_->FetchPage(parent_id)));
}

}  // namespace dbengine
