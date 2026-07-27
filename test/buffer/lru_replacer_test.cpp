#include "buffer/lru_replacer.h"

#include <gtest/gtest.h>

#include "common/exception.h"

namespace dbengine {
namespace {

TEST(LRUReplacerTest, EvictOnEmptyReplacerReturnsFalse) {
  LRUReplacer replacer(5);
  frame_id_t victim;
  EXPECT_FALSE(replacer.Evict(&victim));
}

TEST(LRUReplacerTest, EvictOrderIsLeastRecentlyUsedFirst) {
  LRUReplacer replacer(5);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);
  replacer.RecordAccess(3);
  replacer.SetEvictable(1, true);
  replacer.SetEvictable(2, true);
  replacer.SetEvictable(3, true);

  frame_id_t victim;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 1);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 2);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 3);
  EXPECT_FALSE(replacer.Evict(&victim));
}

TEST(LRUReplacerTest, ReAccessMovesFrameToMostRecentlyUsedEnd) {
  LRUReplacer replacer(5);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);
  replacer.RecordAccess(3);
  replacer.RecordAccess(1);  // 1 is now most-recently used; order is 2, 3, 1.
  replacer.SetEvictable(1, true);
  replacer.SetEvictable(2, true);
  replacer.SetEvictable(3, true);

  frame_id_t victim;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 2);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 3);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 1);
}

TEST(LRUReplacerTest, NonEvictableFramesAreNeverPicked) {
  LRUReplacer replacer(5);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);
  replacer.SetEvictable(1, false);
  replacer.SetEvictable(2, true);

  EXPECT_EQ(replacer.Size(), 1u);
  frame_id_t victim;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 2);
  EXPECT_FALSE(replacer.Evict(&victim));
}

TEST(LRUReplacerTest, TogglingEvictableUpdatesSize) {
  LRUReplacer replacer(5);
  replacer.RecordAccess(1);
  replacer.SetEvictable(1, true);
  EXPECT_EQ(replacer.Size(), 1u);
  replacer.SetEvictable(1, false);
  EXPECT_EQ(replacer.Size(), 0u);
}

TEST(LRUReplacerTest, RemoveThrowsOnNonEvictableFrame) {
  LRUReplacer replacer(5);
  replacer.RecordAccess(1);
  replacer.SetEvictable(1, false);
  EXPECT_THROW(replacer.Remove(1), IOException);
}

TEST(LRUReplacerTest, RemoveOnAbsentFrameIsNoOp) {
  LRUReplacer replacer(5);
  EXPECT_NO_THROW(replacer.Remove(42));
}

TEST(LRUReplacerTest, OutOfRangeFrameIdThrows) {
  LRUReplacer replacer(3);
  EXPECT_THROW(replacer.RecordAccess(5), IOException);
  EXPECT_THROW(replacer.RecordAccess(-1), IOException);
}

}  // namespace
}  // namespace dbengine
