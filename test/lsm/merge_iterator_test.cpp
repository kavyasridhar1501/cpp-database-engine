#include "lsm/merge_iterator.h"

#include <gtest/gtest.h>

#include <vector>

namespace dbengine {
namespace {

// A trivial in-memory cursor over an explicit (key, value) list, for
// testing the merge algorithm in isolation from MemTable/SSTable.
class VectorCursor : public LSMCursor {
 public:
  explicit VectorCursor(std::vector<std::pair<KeyType, LSMValue>> entries)
      : entries_(std::move(entries)) {}

  bool Valid() const override { return pos_ < entries_.size(); }
  KeyType Key() const override { return entries_[pos_].first; }
  const LSMValue& Value() const override { return entries_[pos_].second; }
  void Next() override { ++pos_; }

 private:
  std::vector<std::pair<KeyType, LSMValue>> entries_;
  size_t pos_ = 0;
};

std::vector<std::unique_ptr<LSMCursor>> MakeCursors(
    std::vector<std::vector<std::pair<KeyType, LSMValue>>> sources) {
  std::vector<std::unique_ptr<LSMCursor>> cursors;
  for (auto& src : sources) {
    cursors.push_back(std::make_unique<VectorCursor>(std::move(src)));
  }
  return cursors;
}

TEST(MergeIteratorTest, SingleSourcePassesThroughUnchanged) {
  auto cursors = MakeCursors({{{1, LSMValue::Value("a")}, {2, LSMValue::Value("b")}}});
  LSMMergeIterator it(std::move(cursors));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.Key(), 1);
  EXPECT_EQ(it.Value().value, "a");
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.Key(), 2);
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(MergeIteratorTest, InterleavesDisjointKeysInSortedOrder) {
  auto cursors = MakeCursors({
      {{1, LSMValue::Value("a")}, {3, LSMValue::Value("c")}},
      {{2, LSMValue::Value("b")}, {4, LSMValue::Value("d")}},
  });
  LSMMergeIterator it(std::move(cursors));
  std::vector<KeyType> keys;
  for (; it.Valid(); it.Next()) keys.push_back(it.Key());
  EXPECT_EQ(keys, (std::vector<KeyType>{1, 2, 3, 4}));
}

TEST(MergeIteratorTest, HigherPrioritySourceWinsOnDuplicateKey) {
  // Source 0 (highest priority, e.g. "memtable") shadows source 1's value
  // for key 1.
  auto cursors = MakeCursors({
      {{1, LSMValue::Value("new")}},
      {{1, LSMValue::Value("old")}, {2, LSMValue::Value("b")}},
  });
  LSMMergeIterator it(std::move(cursors));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.Key(), 1);
  EXPECT_EQ(it.Value().value, "new");
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.Key(), 2);
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(MergeIteratorTest, TombstoneFromHigherPrioritySourceShadowsOlderValue) {
  auto cursors = MakeCursors({
      {{1, LSMValue::Tombstone()}},
      {{1, LSMValue::Value("stale")}},
  });
  LSMMergeIterator it(std::move(cursors));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.Key(), 1);
  EXPECT_TRUE(it.Value().deleted);  // merge iterator surfaces tombstones as-is;
  it.Next();                       // filtering them out is the caller's job.
  EXPECT_FALSE(it.Valid());
}

TEST(MergeIteratorTest, ThreeWayMergeWithMixedOverlap) {
  auto cursors = MakeCursors({
      {{2, LSMValue::Value("mem-2")}, {5, LSMValue::Value("mem-5")}},
      {{1, LSMValue::Value("t0-1")}, {2, LSMValue::Value("t0-2")}, {3, LSMValue::Value("t0-3")}},
      {{3, LSMValue::Value("t1-3")}, {4, LSMValue::Value("t1-4")}},
  });
  LSMMergeIterator it(std::move(cursors));
  std::vector<std::pair<KeyType, std::string>> result;
  for (; it.Valid(); it.Next()) {
    result.emplace_back(it.Key(), it.Value().value);
  }
  std::vector<std::pair<KeyType, std::string>> expected = {
      {1, "t0-1"}, {2, "mem-2"}, {3, "t0-3"}, {4, "t1-4"}, {5, "mem-5"},
  };
  EXPECT_EQ(result, expected);
}

TEST(MergeIteratorTest, EmptyCursorsProduceInvalidIterator) {
  auto cursors = MakeCursors({{}, {}});
  LSMMergeIterator it(std::move(cursors));
  EXPECT_FALSE(it.Valid());
}

}  // namespace
}  // namespace dbengine
