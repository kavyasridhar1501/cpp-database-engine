#include "lsm/memtable.h"

#include <gtest/gtest.h>

namespace dbengine {
namespace {

TEST(MemTableTest, PutThenGetReturnsValue) {
  MemTable mt;
  mt.Put(1, "hello");
  LSMValue out;
  ASSERT_TRUE(mt.Get(1, &out));
  EXPECT_FALSE(out.deleted);
  EXPECT_EQ(out.value, "hello");
}

TEST(MemTableTest, GetMissingKeyReturnsFalse) {
  MemTable mt;
  LSMValue out;
  EXPECT_FALSE(mt.Get(42, &out));
}

TEST(MemTableTest, DeleteRecordsTombstoneNotRemoval) {
  MemTable mt;
  mt.Put(1, "hello");
  mt.Delete(1);
  LSMValue out;
  ASSERT_TRUE(mt.Get(1, &out));  // entry still present...
  EXPECT_TRUE(out.deleted);      // ...but marked deleted.
  EXPECT_EQ(mt.EntryCount(), 1u);
}

TEST(MemTableTest, ApproximateSizeGrowsWithPuts) {
  MemTable mt;
  EXPECT_EQ(mt.ApproximateSizeBytes(), 0u);
  mt.Put(1, "hello");
  EXPECT_GT(mt.ApproximateSizeBytes(), 0u);
  size_t after_one = mt.ApproximateSizeBytes();
  mt.Put(2, "world");
  EXPECT_GT(mt.ApproximateSizeBytes(), after_one);
}

TEST(MemTableTest, IteratorWalksInSortedOrderIncludingTombstones) {
  MemTable mt;
  mt.Put(3, "c");
  mt.Put(1, "a");
  mt.Delete(2);
  mt.Put(4, "d");

  std::vector<KeyType> keys;
  std::vector<bool> deleted_flags;
  for (auto it = mt.Begin(); it.Valid(); it.Next()) {
    keys.push_back(it.key());
    deleted_flags.push_back(it.value().deleted);
  }
  EXPECT_EQ(keys, (std::vector<KeyType>{1, 2, 3, 4}));
  EXPECT_EQ(deleted_flags, (std::vector<bool>{false, true, false, false}));
}

TEST(MemTableTest, LowerBoundSkipsEntriesBeforeKey) {
  MemTable mt;
  for (KeyType k : {0, 10, 20, 30}) {
    mt.Put(k, std::to_string(k));
  }
  auto it = mt.LowerBound(15);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), 20);
}

}  // namespace
}  // namespace dbengine
