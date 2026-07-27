#include "index/internal_page.h"

#include <algorithm>

#include "buffer/buffer_pool_manager.h"

namespace dbengine {

void InternalPage::Init(page_id_t parent_id, int max_size) {
  SetPageType(PageType::INTERNAL);
  SetSize(0);
  SetMaxSize(max_size);
  SetParentPageId(parent_id);
}

KeyType InternalPage::KeyAt(int index) const { return array_[index].key; }

void InternalPage::SetKeyAt(int index, KeyType key) { array_[index].key = key; }

page_id_t InternalPage::ValueAt(int index) const { return array_[index].value; }

void InternalPage::SetValueAt(int index, page_id_t value) { array_[index].value = value; }

int InternalPage::ValueIndex(page_id_t value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (array_[i].value == value) {
      return i;
    }
  }
  return -1;
}

page_id_t InternalPage::Lookup(KeyType key) const {
  // Largest index i in [1, size) with key_[i] <= key; falls back to
  // value_[0] if key is smaller than every real separator.
  auto it = std::upper_bound(array_ + 1, array_ + GetSize(), key,
                              [](KeyType k, const MappingType& entry) { return k < entry.key; });
  return (it - 1)->value;
}

void InternalPage::PopulateNewRoot(page_id_t old_value, KeyType new_key, page_id_t new_value) {
  array_[0].value = old_value;
  array_[1] = {new_key, new_value};
  SetSize(2);
}

int InternalPage::InsertNodeAfter(page_id_t old_value, KeyType new_key, page_id_t new_value) {
  int idx = ValueIndex(old_value);
  for (int i = GetSize(); i > idx + 1; --i) {
    array_[i] = array_[i - 1];
  }
  array_[idx + 1] = {new_key, new_value};
  IncreaseSize(1);
  return GetSize();
}

void InternalPage::Remove(int index) {
  for (int i = index; i < GetSize() - 1; ++i) {
    array_[i] = array_[i + 1];
  }
  IncreaseSize(-1);
}

page_id_t InternalPage::RemoveAndReturnOnlyChild() {
  page_id_t only = array_[0].value;
  SetSize(0);
  return only;
}

void InternalPage::UpdateChildParent(page_id_t child_page_id, page_id_t new_parent_id,
                                      BufferPoolManager* bpm) {
  Page* child = bpm->FetchPage(child_page_id);
  reinterpret_cast<BPlusTreePage*>(child->GetData())->SetParentPageId(new_parent_id);
  bpm->UnpinPage(child_page_id, /*is_dirty=*/true);
}

void InternalPage::MoveHalfTo(InternalPage* recipient, page_id_t recipient_page_id,
                               BufferPoolManager* bpm) {
  int start = GetSize() / 2;
  int count = GetSize() - start;
  for (int i = 0; i < count; ++i) {
    recipient->array_[i] = array_[start + i];
    UpdateChildParent(recipient->array_[i].value, recipient_page_id, bpm);
  }
  recipient->SetSize(count);
  SetSize(start);
}

void InternalPage::MoveAllTo(InternalPage* recipient, page_id_t recipient_page_id,
                              KeyType middle_key, BufferPoolManager* bpm) {
  array_[0].key = middle_key;
  int base = recipient->GetSize();
  for (int i = 0; i < GetSize(); ++i) {
    recipient->array_[base + i] = array_[i];
    UpdateChildParent(array_[i].value, recipient_page_id, bpm);
  }
  recipient->IncreaseSize(GetSize());
  SetSize(0);
}

void InternalPage::MoveFirstToEndOf(InternalPage* recipient, page_id_t recipient_page_id,
                                     KeyType middle_key, BufferPoolManager* bpm) {
  MappingType first = array_[0];
  recipient->array_[recipient->GetSize()] = {middle_key, first.value};
  recipient->IncreaseSize(1);
  UpdateChildParent(first.value, recipient_page_id, bpm);

  for (int i = 0; i < GetSize() - 1; ++i) {
    array_[i] = array_[i + 1];
  }
  IncreaseSize(-1);
}

void InternalPage::MoveLastToFrontOf(InternalPage* recipient, page_id_t recipient_page_id,
                                      KeyType middle_key, BufferPoolManager* bpm) {
  MappingType last = array_[GetSize() - 1];
  IncreaseSize(-1);

  for (int i = recipient->GetSize(); i > 0; --i) {
    recipient->array_[i] = recipient->array_[i - 1];
  }
  recipient->array_[0].value = last.value;
  recipient->array_[1].key = middle_key;
  recipient->IncreaseSize(1);
  UpdateChildParent(last.value, recipient_page_id, bpm);
}

}  // namespace dbengine
