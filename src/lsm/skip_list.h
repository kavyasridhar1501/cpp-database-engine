#pragma once

#include <cstddef>
#include <random>
#include <utility>
#include <vector>

namespace dbengine {

// An original, header-only skip list (Pugh, "Skip Lists: A Probabilistic
// Alternative to Balanced Trees", 1990) used as the LSM-tree's memtable.
// Not thread-safe on its own — the memtable that owns one is protected by
// the LSM engine's coarse lock, consistent with this project's other
// data structures (see BufferPoolManager, BPlusTree).
//
// Header-only because it's a template, not because it's vendored
// third-party code — this implementation is original to this project.
template <typename Key, typename Value>
class SkipList {
 private:
  struct Node {
    Key key;
    Value value;
    std::vector<Node*> forward;
    Node(int level, Key k, Value v)
        : key(std::move(k)), value(std::move(v)), forward(static_cast<size_t>(level), nullptr) {}
  };

 public:
  explicit SkipList(int max_height = 16, double p = 0.5)
      : max_height_(max_height),
        p_(p),
        head_(new Node(max_height, Key{}, Value{})),
        rng_(std::random_device{}()) {}

  ~SkipList() {
    Node* node = head_->forward[0];
    while (node != nullptr) {
      Node* next = node->forward[0];
      delete node;
      node = next;
    }
    delete head_;
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts `key` with `value`, or overwrites the value of an existing
  // entry (upsert).
  void Insert(const Key& key, const Value& value) {
    std::vector<Node*> update(static_cast<size_t>(max_height_), head_);
    Node* cur = head_;
    for (int level = current_height_ - 1; level >= 0; --level) {
      while (cur->forward[static_cast<size_t>(level)] != nullptr &&
             cur->forward[static_cast<size_t>(level)]->key < key) {
        cur = cur->forward[static_cast<size_t>(level)];
      }
      update[static_cast<size_t>(level)] = cur;
    }

    Node* candidate = cur->forward[0];
    if (candidate != nullptr && candidate->key == key) {
      candidate->value = value;
      return;
    }

    int level = RandomLevel();
    if (level > current_height_) {
      for (int i = current_height_; i < level; ++i) {
        update[static_cast<size_t>(i)] = head_;
      }
      current_height_ = level;
    }

    Node* node = new Node(level, key, value);
    for (int i = 0; i < level; ++i) {
      node->forward[static_cast<size_t>(i)] = update[static_cast<size_t>(i)]->forward[static_cast<size_t>(i)];
      update[static_cast<size_t>(i)]->forward[static_cast<size_t>(i)] = node;
    }
    ++size_;
  }

  bool Find(const Key& key, Value* out) const {
    Node* cur = head_;
    for (int level = current_height_ - 1; level >= 0; --level) {
      while (cur->forward[static_cast<size_t>(level)] != nullptr &&
             cur->forward[static_cast<size_t>(level)]->key < key) {
        cur = cur->forward[static_cast<size_t>(level)];
      }
    }
    Node* candidate = cur->forward[0];
    if (candidate != nullptr && candidate->key == key) {
      *out = candidate->value;
      return true;
    }
    return false;
  }

  size_t Size() const { return size_; }

  class Iterator {
   public:
    Iterator() = default;
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { return node_->key; }
    const Value& value() const { return node_->value; }
    void Next() { node_ = node_->forward[0]; }

   private:
    friend class SkipList;
    explicit Iterator(Node* node) : node_(node) {}
    Node* node_ = nullptr;
  };

  Iterator Begin() const { return Iterator(head_->forward[0]); }

  // First entry with key >= `key`.
  Iterator LowerBound(const Key& key) const {
    Node* cur = head_;
    for (int level = current_height_ - 1; level >= 0; --level) {
      while (cur->forward[static_cast<size_t>(level)] != nullptr &&
             cur->forward[static_cast<size_t>(level)]->key < key) {
        cur = cur->forward[static_cast<size_t>(level)];
      }
    }
    return Iterator(cur->forward[0]);
  }

 private:
  int RandomLevel() {
    int level = 1;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    while (level < max_height_ && dist(rng_) < p_) {
      ++level;
    }
    return level;
  }

  int max_height_;
  double p_;
  int current_height_ = 1;
  Node* head_;
  size_t size_ = 0;
  mutable std::mt19937 rng_;
};

}  // namespace dbengine
