#include "lsm/sstable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <vector>

namespace dbengine {
namespace {

class SSTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_sst_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    std::filesystem::remove(file_path_);
  }

  void TearDown() override { std::filesystem::remove(file_path_); }

  std::string file_path_;
};

TEST_F(SSTableTest, BuildAndGetRoundTrip) {
  std::vector<std::pair<KeyType, LSMValue>> entries = {
      {1, LSMValue::Value("a")},
      {2, LSMValue::Value("b")},
      {3, LSMValue::Tombstone()},
      {5, LSMValue::Value("e")},
  };
  SSTable::Build(file_path_, entries);
  auto sst = SSTable::Open(file_path_);

  EXPECT_EQ(sst->NumEntries(), 4);
  EXPECT_EQ(sst->MinKey(), 1);
  EXPECT_EQ(sst->MaxKey(), 5);

  LSMValue out;
  ASSERT_TRUE(sst->Get(1, &out));
  EXPECT_FALSE(out.deleted);
  EXPECT_EQ(out.value, "a");

  ASSERT_TRUE(sst->Get(3, &out));
  EXPECT_TRUE(out.deleted);

  EXPECT_FALSE(sst->Get(4, &out));   // gap in key space, not present
  EXPECT_FALSE(sst->Get(0, &out));   // below min
  EXPECT_FALSE(sst->Get(100, &out)); // above max
}

TEST_F(SSTableTest, MightContainNeverFalseNegative) {
  std::vector<std::pair<KeyType, LSMValue>> entries;
  for (KeyType k = 0; k < 500; ++k) {
    entries.emplace_back(k, LSMValue::Value("v" + std::to_string(k)));
  }
  SSTable::Build(file_path_, entries);
  auto sst = SSTable::Open(file_path_);
  for (KeyType k = 0; k < 500; ++k) {
    EXPECT_TRUE(sst->MightContain(k));
  }
  EXPECT_FALSE(sst->MightContain(-1));
  EXPECT_FALSE(sst->MightContain(1000));
}

TEST_F(SSTableTest, IteratorWalksAllEntriesInOrder) {
  std::vector<std::pair<KeyType, LSMValue>> entries;
  for (KeyType k = 0; k < 300; k += 3) {  // sparse keys, spans many data pages
    entries.emplace_back(k, LSMValue::Value("v" + std::to_string(k)));
  }
  SSTable::Build(file_path_, entries);
  auto sst = SSTable::Open(file_path_);

  std::vector<KeyType> seen;
  for (auto it = sst->NewIterator(INVALID_KEY); it.Valid(); it.Next()) {
    seen.push_back(it.Key());
    EXPECT_EQ(it.Value().value, "v" + std::to_string(it.Key()));
  }
  ASSERT_EQ(seen.size(), entries.size());
  for (size_t i = 0; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i], entries[i].first);
  }
}

TEST_F(SSTableTest, IteratorSeeksIntoKeyGapCorrectly) {
  std::vector<std::pair<KeyType, LSMValue>> entries;
  for (KeyType k = 0; k < 300; k += 3) {
    entries.emplace_back(k, LSMValue::Value("v" + std::to_string(k)));
  }
  SSTable::Build(file_path_, entries);
  auto sst = SSTable::Open(file_path_);

  // 101 is in a gap between keys 99 and 102; seeking there must land on 102.
  auto it = sst->NewIterator(101);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.Key(), 102);

  // Seeking past the last key yields an empty iterator.
  auto end_it = sst->NewIterator(1000);
  EXPECT_FALSE(end_it.Valid());
}

TEST_F(SSTableTest, RandomizedBuildMatchesStdMapOracleAcrossManyDataPages) {
  std::mt19937 rng(99);
  std::vector<KeyType> keys(5000);
  for (size_t i = 0; i < keys.size(); ++i) {
    keys[i] = static_cast<KeyType>(i) * 2;  // even keys, guarantees no dup and gaps
  }
  std::map<KeyType, LSMValue> oracle;
  std::vector<std::pair<KeyType, LSMValue>> entries;
  std::uniform_int_distribution<int> del_dist(0, 9);
  for (KeyType k : keys) {
    LSMValue v = (del_dist(rng) == 0) ? LSMValue::Tombstone() : LSMValue::Value("val-" + std::to_string(k));
    entries.emplace_back(k, v);
    oracle[k] = v;
  }

  SSTable::Build(file_path_, entries);
  auto sst = SSTable::Open(file_path_);
  EXPECT_EQ(sst->NumEntries(), static_cast<int64_t>(oracle.size()));

  // Point lookups, including keys guaranteed absent (odd numbers).
  std::uniform_int_distribution<int64_t> query_dist(-10, keys.back() + 10);
  for (int i = 0; i < 5000; ++i) {
    KeyType q = query_dist(rng);
    LSMValue out;
    bool found = sst->Get(q, &out);
    auto oracle_it = oracle.find(q);
    ASSERT_EQ(found, oracle_it != oracle.end()) << "mismatch for key " << q;
    if (found) {
      EXPECT_EQ(out.deleted, oracle_it->second.deleted);
      if (!out.deleted) {
        EXPECT_EQ(out.value, oracle_it->second.value);
      }
    }
  }

  // Full scan matches oracle order exactly.
  auto it = sst->NewIterator(INVALID_KEY);
  auto oracle_it = oracle.begin();
  for (; it.Valid(); it.Next(), ++oracle_it) {
    ASSERT_NE(oracle_it, oracle.end());
    EXPECT_EQ(it.Key(), oracle_it->first);
    EXPECT_EQ(it.Value().deleted, oracle_it->second.deleted);
  }
  EXPECT_EQ(oracle_it, oracle.end());
}

TEST_F(SSTableTest, MarkObsoleteDeletesFileOnDestruction) {
  std::vector<std::pair<KeyType, LSMValue>> entries = {{1, LSMValue::Value("a")}};
  SSTable::Build(file_path_, entries);
  ASSERT_TRUE(std::filesystem::exists(file_path_));
  {
    auto sst = SSTable::Open(file_path_);
    sst->MarkObsolete();
  }
  EXPECT_FALSE(std::filesystem::exists(file_path_));
}

}  // namespace
}  // namespace dbengine
