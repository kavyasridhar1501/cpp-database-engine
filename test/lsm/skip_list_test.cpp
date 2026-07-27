#include "lsm/skip_list.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace dbengine {
namespace {

TEST(SkipListTest, FindOnEmptyListFails) {
  SkipList<int64_t, std::string> list;
  std::string out;
  EXPECT_FALSE(list.Find(1, &out));
  EXPECT_EQ(list.Size(), 0u);
}

TEST(SkipListTest, InsertThenFind) {
  SkipList<int64_t, std::string> list;
  list.Insert(5, "five");
  std::string out;
  ASSERT_TRUE(list.Find(5, &out));
  EXPECT_EQ(out, "five");
  EXPECT_FALSE(list.Find(6, &out));
}

TEST(SkipListTest, InsertExistingKeyOverwritesValue) {
  SkipList<int64_t, std::string> list;
  list.Insert(1, "a");
  list.Insert(1, "b");
  EXPECT_EQ(list.Size(), 1u);
  std::string out;
  ASSERT_TRUE(list.Find(1, &out));
  EXPECT_EQ(out, "b");
}

TEST(SkipListTest, IteratorWalksInSortedOrder) {
  SkipList<int64_t, std::string> list;
  std::vector<int64_t> keys = {5, 1, 4, 2, 3};
  for (int64_t k : keys) {
    list.Insert(k, std::to_string(k));
  }
  std::vector<int64_t> seen;
  for (auto it = list.Begin(); it.Valid(); it.Next()) {
    seen.push_back(it.key());
  }
  EXPECT_EQ(seen, (std::vector<int64_t>{1, 2, 3, 4, 5}));
}

TEST(SkipListTest, LowerBoundFindsFirstKeyGreaterOrEqual) {
  SkipList<int64_t, std::string> list;
  for (int64_t k : {0, 10, 20, 30, 40}) {
    list.Insert(k, std::to_string(k));
  }
  auto it = list.LowerBound(15);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 20);

  auto end_it = list.LowerBound(1000);
  EXPECT_FALSE(end_it.Valid());

  auto exact_it = list.LowerBound(20);
  ASSERT_TRUE(exact_it.Valid());
  EXPECT_EQ(exact_it.key(), 20);
}

TEST(SkipListTest, RandomizedInsertAndFindMatchesStdMapOracle) {
  SkipList<int64_t, std::string> list;
  std::map<int64_t, std::string> oracle;

  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> key_dist(0, 2000);

  for (int i = 0; i < 5000; ++i) {
    int64_t key = key_dist(rng);
    std::string value = "v" + std::to_string(i);
    list.Insert(key, value);
    oracle[key] = value;
  }

  EXPECT_EQ(list.Size(), oracle.size());

  for (const auto& [key, value] : oracle) {
    std::string out;
    ASSERT_TRUE(list.Find(key, &out));
    EXPECT_EQ(out, value);
  }

  std::vector<int64_t> seen;
  for (auto it = list.Begin(); it.Valid(); it.Next()) {
    seen.push_back(it.key());
  }
  std::vector<int64_t> expected_keys;
  for (const auto& [key, value] : oracle) {
    expected_keys.push_back(key);
  }
  EXPECT_EQ(seen, expected_keys);
}

}  // namespace
}  // namespace dbengine
