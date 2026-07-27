#include "buffer/lru_k_replacer.h"

#include <gtest/gtest.h>

#include "common/exception.h"

namespace dbengine {
namespace {

TEST(LRUKReplacerTest, EvictOnEmptyReplacerReturnsFalse) {
  LRUKReplacer replacer(5, 2);
  frame_id_t victim;
  EXPECT_FALSE(replacer.Evict(&victim));
}

TEST(LRUKReplacerTest, SingleAccessFramesAreEvictedBeforeMultiAccessFrames) {
  LRUKReplacer replacer(5, 2);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);
  replacer.RecordAccess(3);
  replacer.RecordAccess(1);  // frame 1 now has 2 accesses (finite backward distance).
  replacer.SetEvictable(1, true);
  replacer.SetEvictable(2, true);
  replacer.SetEvictable(3, true);

  // Frames 2 and 3 have only one access each (+inf backward distance) and
  // must be evicted before frame 1, which has real access history. Among
  // the +inf frames, the one touched longest ago (2) goes first.
  frame_id_t victim;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 2);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 3);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 1);
  EXPECT_FALSE(replacer.Evict(&victim));
}

TEST(LRUKReplacerTest, LargerBackwardKDistanceEvictedFirstAmongMultiAccessFrames) {
  LRUKReplacer replacer(5, 2);
  replacer.RecordAccess(0);  // t=0
  replacer.RecordAccess(1);  // t=1
  replacer.RecordAccess(0);  // t=2 -> frame 0 history [0, 2]
  replacer.RecordAccess(1);  // t=3 -> frame 1 history [1, 3]
  replacer.SetEvictable(0, true);
  replacer.SetEvictable(1, true);

  // At eviction time (t=4): frame 0's backward distance is 4-0=4, frame
  // 1's is 4-1=3. The larger distance (frame 0) is evicted first.
  frame_id_t victim;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 0);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 1);
}

TEST(LRUKReplacerTest, NonEvictableFramesAreNeverPicked) {
  LRUKReplacer replacer(5, 2);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);
  replacer.SetEvictable(1, false);
  replacer.SetEvictable(2, true);

  EXPECT_EQ(replacer.Size(), 1u);
  frame_id_t victim;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 2);
  EXPECT_FALSE(replacer.Evict(&victim));  // only frame 1 left, and it's pinned.
}

TEST(LRUKReplacerTest, TogglingEvictableUpdatesSize) {
  LRUKReplacer replacer(5, 2);
  replacer.RecordAccess(1);
  replacer.SetEvictable(1, true);
  EXPECT_EQ(replacer.Size(), 1u);
  replacer.SetEvictable(1, false);
  EXPECT_EQ(replacer.Size(), 0u);
  replacer.SetEvictable(1, true);
  EXPECT_EQ(replacer.Size(), 1u);
}

TEST(LRUKReplacerTest, RemoveThrowsOnNonEvictableFrame) {
  LRUKReplacer replacer(5, 2);
  replacer.RecordAccess(1);
  replacer.SetEvictable(1, false);
  EXPECT_THROW(replacer.Remove(1), IOException);
}

TEST(LRUKReplacerTest, RemoveOnAbsentFrameIsNoOp) {
  LRUKReplacer replacer(5, 2);
  EXPECT_NO_THROW(replacer.Remove(42));
}

TEST(LRUKReplacerTest, RemoveEvictableFrameShrinksSize) {
  LRUKReplacer replacer(5, 2);
  replacer.RecordAccess(1);
  replacer.SetEvictable(1, true);
  replacer.Remove(1);
  EXPECT_EQ(replacer.Size(), 0u);
  frame_id_t victim;
  EXPECT_FALSE(replacer.Evict(&victim));
}

TEST(LRUKReplacerTest, OutOfRangeFrameIdThrows) {
  LRUKReplacer replacer(3, 2);
  EXPECT_THROW(replacer.RecordAccess(5), IOException);
  EXPECT_THROW(replacer.RecordAccess(-1), IOException);
}

}  // namespace
}  // namespace dbengine
