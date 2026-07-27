#include "index/leaf_page.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dbengine {

void LeafPage::Init(page_id_t parent_id, int max_size) {
  SetPageType(PageType::LEAF);
  SetSize(0);
  SetMaxSize(max_size);
  SetParentPageId(parent_id);
  next_page_id_ = INVALID_PAGE_ID;
}

KeyType LeafPage::KeyAt(int index) const { return array_[index].key; }

std::string LeafPage::ValueAt(int index) const {
  return std::string(array_[index].value, array_[index].value_len);
}

page_id_t LeafPage::GetNextPageId() const { return next_page_id_; }

void LeafPage::SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

int LeafPage::KeyIndex(KeyType key) const {
  auto it = std::lower_bound(array_, array_ + GetSize(), key,
                              [](const Entry& entry, KeyType k) { return entry.key < k; });
  return static_cast<int>(it - array_);
}

bool LeafPage::Lookup(KeyType key, std::string* value) const {
  int idx = KeyIndex(key);
  if (idx >= GetSize() || array_[idx].key != key) {
    return false;
  }
  *value = ValueAt(idx);
  return true;
}

int LeafPage::Insert(KeyType key, const std::string& value) {
  if (value.size() > MAX_VALUE_SIZE) {
    throw std::invalid_argument("LeafPage::Insert: value exceeds MAX_VALUE_SIZE");
  }

  int idx = KeyIndex(key);
  if (idx < GetSize() && array_[idx].key == key) {
    array_[idx].value_len = static_cast<uint16_t>(value.size());
    std::memcpy(array_[idx].value, value.data(), value.size());
    return GetSize();
  }

  for (int i = GetSize(); i > idx; --i) {
    array_[i] = array_[i - 1];
  }
  array_[idx].key = key;
  array_[idx].value_len = static_cast<uint16_t>(value.size());
  std::memcpy(array_[idx].value, value.data(), value.size());
  IncreaseSize(1);
  return GetSize();
}

bool LeafPage::Remove(KeyType key) {
  int idx = KeyIndex(key);
  if (idx >= GetSize() || array_[idx].key != key) {
    return false;
  }
  for (int i = idx; i < GetSize() - 1; ++i) {
    array_[i] = array_[i + 1];
  }
  IncreaseSize(-1);
  return true;
}

void LeafPage::MoveHalfTo(LeafPage* recipient, page_id_t recipient_page_id) {
  int start = GetSize() / 2;
  int count = GetSize() - start;
  for (int i = 0; i < count; ++i) {
    recipient->array_[i] = array_[start + i];
  }
  recipient->SetSize(count);
  SetSize(start);

  recipient->SetNextPageId(GetNextPageId());
  SetNextPageId(recipient_page_id);
}

void LeafPage::MoveAllTo(LeafPage* recipient) {
  int base = recipient->GetSize();
  for (int i = 0; i < GetSize(); ++i) {
    recipient->array_[base + i] = array_[i];
  }
  recipient->IncreaseSize(GetSize());
  recipient->SetNextPageId(GetNextPageId());
  SetSize(0);
}

void LeafPage::MoveFirstToEndOf(LeafPage* recipient) {
  recipient->array_[recipient->GetSize()] = array_[0];
  recipient->IncreaseSize(1);

  for (int i = 0; i < GetSize() - 1; ++i) {
    array_[i] = array_[i + 1];
  }
  IncreaseSize(-1);
}

void LeafPage::MoveLastToFrontOf(LeafPage* recipient) {
  Entry last = array_[GetSize() - 1];
  IncreaseSize(-1);

  for (int i = recipient->GetSize(); i > 0; --i) {
    recipient->array_[i] = recipient->array_[i - 1];
  }
  recipient->array_[0] = last;
  recipient->IncreaseSize(1);
}

}  // namespace dbengine
