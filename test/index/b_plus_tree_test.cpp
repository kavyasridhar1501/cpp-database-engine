#include "index/b_plus_tree.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_k_replacer.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {
namespace {

class BPlusTreeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_bpt_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    std::filesystem::remove(file_path_);
  }

  void TearDown() override { std::filesystem::remove(file_path_); }

  std::unique_ptr<BPlusTree> MakeTree(size_t pool_size, int leaf_max_size, int internal_max_size) {
    disk_manager_ = std::make_unique<DiskManager>(file_path_);
    bpm_ = std::make_unique<BufferPoolManager>(pool_size, disk_manager_.get(),
                                                std::make_unique<LRUKReplacer>(pool_size, 2));
    return std::make_unique<BPlusTree>(bpm_.get(), INVALID_PAGE_ID, leaf_max_size, internal_max_size);
  }

  std::vector<std::pair<KeyType, std::string>> ScanAll(BPlusTree* tree) {
    std::vector<std::pair<KeyType, std::string>> result;
    for (auto it = tree->Begin(); !it.IsEnd(); ++it) {
      result.emplace_back(it.GetKey(), it.GetValue());
    }
    return result;
  }

  std::string file_path_;
  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> bpm_;
};

TEST_F(BPlusTreeTest, EmptyTreeLookupAndScanFail) {
  auto tree = MakeTree(16, 4, 4);
  EXPECT_TRUE(tree->IsEmpty());
  std::string value;
  EXPECT_FALSE(tree->GetValue(42, &value));
  EXPECT_TRUE(tree->Begin().IsEnd());
  EXPECT_FALSE(tree->Remove(42));
}

TEST_F(BPlusTreeTest, InsertThenGetSingleKey) {
  auto tree = MakeTree(16, 4, 4);
  EXPECT_TRUE(tree->Insert(1, "hello"));
  std::string value;
  ASSERT_TRUE(tree->GetValue(1, &value));
  EXPECT_EQ(value, "hello");
}

TEST_F(BPlusTreeTest, InsertExistingKeyUpdatesValueAndReturnsFalse) {
  auto tree = MakeTree(16, 4, 4);
  EXPECT_TRUE(tree->Insert(1, "first"));
  EXPECT_FALSE(tree->Insert(1, "second"));
  std::string value;
  ASSERT_TRUE(tree->GetValue(1, &value));
  EXPECT_EQ(value, "second");
}

TEST_F(BPlusTreeTest, ValueExceedingMaxSizeThrows) {
  auto tree = MakeTree(16, 4, 4);
  std::string too_big(MAX_VALUE_SIZE + 1, 'x');
  EXPECT_THROW(tree->Insert(1, too_big), std::invalid_argument);
}

TEST_F(BPlusTreeTest, InsertBeyondLeafCapacityForcesSplitAndAllKeysStillLookUp) {
  auto tree = MakeTree(16, /*leaf_max_size=*/4, /*internal_max_size=*/4);
  for (KeyType k = 0; k < 20; ++k) {
    tree->Insert(k, "v" + std::to_string(k));
  }
  for (KeyType k = 0; k < 20; ++k) {
    std::string value;
    ASSERT_TRUE(tree->GetValue(k, &value)) << "missing key " << k;
    EXPECT_EQ(value, "v" + std::to_string(k));
  }
  // 20 keys with leaf_max_size 4 guarantees at least one split happened;
  // the root must no longer be the original single leaf.
  auto scanned = ScanAll(tree.get());
  ASSERT_EQ(scanned.size(), 20u);
  for (KeyType k = 0; k < 20; ++k) {
    EXPECT_EQ(scanned[static_cast<size_t>(k)].first, k);
  }
}

TEST_F(BPlusTreeTest, InsertManyKeysForcesMultiLevelInternalSplits) {
  auto tree = MakeTree(32, /*leaf_max_size=*/4, /*internal_max_size=*/4);
  constexpr int kNumKeys = 500;
  for (KeyType k = 0; k < kNumKeys; ++k) {
    ASSERT_TRUE(tree->Insert(k, std::to_string(k)));
  }
  for (KeyType k = 0; k < kNumKeys; ++k) {
    std::string value;
    ASSERT_TRUE(tree->GetValue(k, &value)) << "missing key " << k;
    EXPECT_EQ(value, std::to_string(k));
  }
  auto scanned = ScanAll(tree.get());
  ASSERT_EQ(scanned.size(), static_cast<size_t>(kNumKeys));
  for (int i = 0; i < kNumKeys; ++i) {
    EXPECT_EQ(scanned[static_cast<size_t>(i)].first, i);
  }
}

TEST_F(BPlusTreeTest, DeleteNonexistentKeyReturnsFalse) {
  auto tree = MakeTree(16, 4, 4);
  tree->Insert(1, "a");
  EXPECT_FALSE(tree->Remove(999));
}

TEST_F(BPlusTreeTest, InsertThenDeleteAllKeysEmptiesTree) {
  auto tree = MakeTree(16, /*leaf_max_size=*/4, /*internal_max_size=*/4);
  constexpr int kNumKeys = 100;
  for (KeyType k = 0; k < kNumKeys; ++k) {
    tree->Insert(k, std::to_string(k));
  }
  // Delete in a different order than insertion to exercise redistribute and
  // merge from both left and right siblings, not just a simple sequential
  // drain.
  std::vector<KeyType> keys(kNumKeys);
  for (int i = 0; i < kNumKeys; ++i) keys[static_cast<size_t>(i)] = i;
  std::mt19937 rng(7);
  std::shuffle(keys.begin(), keys.end(), rng);

  for (KeyType k : keys) {
    ASSERT_TRUE(tree->Remove(k)) << "failed to remove " << k;
  }
  EXPECT_TRUE(tree->IsEmpty());
  for (KeyType k = 0; k < kNumKeys; ++k) {
    std::string value;
    EXPECT_FALSE(tree->GetValue(k, &value));
  }
}

TEST_F(BPlusTreeTest, ScanFromMidpointReturnsCorrectSuffix) {
  auto tree = MakeTree(16, 4, 4);
  for (KeyType k = 0; k < 50; k += 2) {  // even keys only: 0, 2, 4, ..., 48
    tree->Insert(k, std::to_string(k));
  }
  auto it = tree->Begin(21);  // first even key >= 21 is 22.
  ASSERT_FALSE(it.IsEnd());
  EXPECT_EQ(it.GetKey(), 22);

  std::vector<KeyType> seen;
  for (; !it.IsEnd(); ++it) {
    seen.push_back(it.GetKey());
  }
  ASSERT_EQ(seen.size(), 14u);  // 22, 24, ..., 48
  EXPECT_EQ(seen.front(), 22);
  EXPECT_EQ(seen.back(), 48);
}

TEST_F(BPlusTreeTest, ScanFromKeyBeyondAllEntriesIsEmpty) {
  auto tree = MakeTree(16, 4, 4);
  for (KeyType k = 0; k < 10; ++k) {
    tree->Insert(k, std::to_string(k));
  }
  EXPECT_TRUE(tree->Begin(1000).IsEnd());
}

// The core correctness deliverable: a randomized sequence of insert/delete/
// lookup operations, validated at every step against an in-memory
// std::map oracle, with a buffer pool small enough (relative to the tree's
// page count) to force real eviction and reload from disk, and node sizes
// small enough to force frequent splits, merges, and redistributes.
TEST_F(BPlusTreeTest, RandomizedOperationsMatchStdMapOracle) {
  constexpr int kNumOps = 20000;
  constexpr int kKeyRange = 3000;

  auto tree = MakeTree(/*pool_size=*/40, /*leaf_max_size=*/5, /*internal_max_size=*/5);
  std::map<KeyType, std::string> oracle;

  std::mt19937 rng(20260727);
  std::uniform_int_distribution<int> key_dist(0, kKeyRange - 1);
  std::uniform_int_distribution<int> op_dist(0, 9);  // 0-5 insert, 6-7 delete, 8-9 lookup.

  for (int op = 0; op < kNumOps; ++op) {
    KeyType key = key_dist(rng);
    int choice = op_dist(rng);

    if (choice <= 5) {
      std::string value = "val-" + std::to_string(key) + "-" + std::to_string(op);
      bool tree_is_new = tree->Insert(key, value);
      bool oracle_is_new = !oracle.contains(key);
      ASSERT_EQ(tree_is_new, oracle_is_new) << "insert-newness mismatch for key " << key;
      oracle[key] = value;
    } else if (choice <= 7) {
      bool tree_removed = tree->Remove(key);
      bool oracle_had = oracle.erase(key) > 0;
      ASSERT_EQ(tree_removed, oracle_had) << "delete mismatch for key " << key;
    } else {
      std::string tree_value;
      bool tree_found = tree->GetValue(key, &tree_value);
      auto oracle_it = oracle.find(key);
      bool oracle_found = oracle_it != oracle.end();
      ASSERT_EQ(tree_found, oracle_found) << "lookup presence mismatch for key " << key;
      if (oracle_found) {
        EXPECT_EQ(tree_value, oracle_it->second) << "lookup value mismatch for key " << key;
      }
    }
  }

  // Full range scan must produce exactly the oracle's sorted contents.
  auto scanned = ScanAll(tree.get());
  ASSERT_EQ(scanned.size(), oracle.size());
  auto oracle_it = oracle.begin();
  for (const auto& [key, value] : scanned) {
    ASSERT_NE(oracle_it, oracle.end());
    EXPECT_EQ(key, oracle_it->first);
    EXPECT_EQ(value, oracle_it->second);
    ++oracle_it;
  }
  EXPECT_EQ(oracle_it, oracle.end());
}

}  // namespace
}  // namespace dbengine
