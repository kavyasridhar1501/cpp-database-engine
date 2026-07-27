#include "buffer/buffer_pool_manager.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {
namespace {

class BufferPoolManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_bpm_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    std::filesystem::remove(file_path_);
  }

  void TearDown() override { std::filesystem::remove(file_path_); }

  std::unique_ptr<BufferPoolManager> MakeBpm(size_t pool_size, size_t k = 2) {
    disk_manager_ = std::make_unique<DiskManager>(file_path_);
    return std::make_unique<BufferPoolManager>(pool_size, disk_manager_.get(),
                                                std::make_unique<LRUKReplacer>(pool_size, k));
  }

  std::string file_path_;
  std::unique_ptr<DiskManager> disk_manager_;
};

TEST_F(BufferPoolManagerTest, NewPageIsPinnedAndZeroInitialized) {
  auto bpm = MakeBpm(4);
  page_id_t page_id;
  Page* page = bpm->NewPage(&page_id);
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->GetPinCount(), 1);
  EXPECT_FALSE(page->IsDirty());
  std::vector<char> zero(PAGE_SIZE, 0);
  EXPECT_EQ(std::memcmp(page->GetData(), zero.data(), PAGE_SIZE), 0);
}

TEST_F(BufferPoolManagerTest, WriteBackSurvivesEviction) {
  auto bpm = MakeBpm(2);

  page_id_t target_id;
  Page* target = bpm->NewPage(&target_id);
  ASSERT_NE(target, nullptr);
  std::memset(target->GetData(), 'Z', PAGE_SIZE);
  ASSERT_TRUE(bpm->UnpinPage(target_id, /*is_dirty=*/true));

  // Allocate enough new pages (pool size 2) to force `target` out of the
  // pool via eviction; its dirty contents must be written back first.
  for (int i = 0; i < 10; ++i) {
    page_id_t pid;
    Page* p = bpm->NewPage(&pid);
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(bpm->UnpinPage(pid, false));
  }

  Page* refetched = bpm->FetchPage(target_id);
  ASSERT_NE(refetched, nullptr);
  std::vector<char> expected(PAGE_SIZE, 'Z');
  EXPECT_EQ(std::memcmp(refetched->GetData(), expected.data(), PAGE_SIZE), 0);
  bpm->UnpinPage(target_id, false);
}

TEST_F(BufferPoolManagerTest, PinnedFramesAreNotEvictable) {
  auto bpm = MakeBpm(2);

  page_id_t p1, p2;
  ASSERT_NE(bpm->NewPage(&p1), nullptr);
  ASSERT_NE(bpm->NewPage(&p2), nullptr);
  // Both frames pinned, pool exhausted: a third page cannot be allocated.
  page_id_t p3;
  EXPECT_EQ(bpm->NewPage(&p3), nullptr);

  // Unpinning one frame frees it up for reuse.
  ASSERT_TRUE(bpm->UnpinPage(p1, false));
  EXPECT_NE(bpm->NewPage(&p3), nullptr);
}

TEST_F(BufferPoolManagerTest, DeletePageFailsWhilePinnedAndFreesFrameOnceUnpinned) {
  auto bpm = MakeBpm(2);
  page_id_t page_id;
  ASSERT_NE(bpm->NewPage(&page_id), nullptr);

  EXPECT_FALSE(bpm->DeletePage(page_id));
  ASSERT_TRUE(bpm->UnpinPage(page_id, false));
  EXPECT_TRUE(bpm->DeletePage(page_id));

  // The freed frame should be reusable immediately without tripping the
  // "pool full" path, even though only one NewPage call preceded it.
  page_id_t p2, p3;
  ASSERT_NE(bpm->NewPage(&p2), nullptr);
  ASSERT_NE(bpm->NewPage(&p3), nullptr);
}

TEST_F(BufferPoolManagerTest, FlushPageWritesDirtyDataAndClearsDirtyFlag) {
  auto bpm = MakeBpm(4);
  page_id_t page_id;
  Page* page = bpm->NewPage(&page_id);
  std::memset(page->GetData(), 'Q', PAGE_SIZE);
  ASSERT_TRUE(bpm->UnpinPage(page_id, true));

  ASSERT_TRUE(bpm->FlushPage(page_id));

  std::vector<char> buf(PAGE_SIZE, 0);
  disk_manager_->ReadPage(page_id, buf.data());
  std::vector<char> expected(PAGE_SIZE, 'Q');
  EXPECT_EQ(buf, expected);
}

// Randomized stress test: a pool much smaller than the working set forces
// continuous eviction churn. Every mutation is recorded in an in-memory
// oracle map and periodically re-verified via FetchPage, so any write-back
// or frame-reuse bug would show up as a mismatch.
TEST_F(BufferPoolManagerTest, RandomAccessStressTestForcesEvictionAndPreservesData) {
  constexpr size_t kPoolSize = 8;
  constexpr int kNumPages = 100;
  constexpr int kNumOps = 5000;

  auto bpm = MakeBpm(kPoolSize);

  std::vector<page_id_t> page_ids;
  std::unordered_map<page_id_t, std::vector<char>> oracle;

  for (int i = 0; i < kNumPages; ++i) {
    page_id_t pid;
    Page* page = bpm->NewPage(&pid);
    ASSERT_NE(page, nullptr);
    page_ids.push_back(pid);
    oracle[pid] = std::vector<char>(PAGE_SIZE, 0);
    ASSERT_TRUE(bpm->UnpinPage(pid, false));
  }

  std::mt19937 rng(2024);
  std::uniform_int_distribution<int> page_dist(0, kNumPages - 1);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::uniform_int_distribution<int> op_dist(0, 2);  // 0/1 = write, 2 = verify-only.

  for (int op = 0; op < kNumOps; ++op) {
    page_id_t pid = page_ids[static_cast<size_t>(page_dist(rng))];
    Page* page = bpm->FetchPage(pid);
    ASSERT_NE(page, nullptr) << "eviction should always free a frame when nothing is pinned "
                                 "outside this single fetch";

    if (op_dist(rng) < 2) {
      std::vector<char> data(PAGE_SIZE);
      for (auto& b : data) {
        b = static_cast<char>(byte_dist(rng));
      }
      std::memcpy(page->GetData(), data.data(), PAGE_SIZE);
      oracle[pid] = data;
      ASSERT_TRUE(bpm->UnpinPage(pid, true));
    } else {
      EXPECT_EQ(std::memcmp(page->GetData(), oracle[pid].data(), PAGE_SIZE), 0)
          << "page " << pid << " diverged from oracle after eviction/reload";
      ASSERT_TRUE(bpm->UnpinPage(pid, false));
    }
  }

  // Working set (100 pages) is far larger than the pool (8 frames), so this
  // run must have actually exercised eviction, not just filled free frames.
  EXPECT_GT(bpm->GetNumMisses(), kNumPages);

  // Final full sweep against the oracle.
  for (page_id_t pid : page_ids) {
    Page* page = bpm->FetchPage(pid);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(std::memcmp(page->GetData(), oracle[pid].data(), PAGE_SIZE), 0);
    bpm->UnpinPage(pid, false);
  }
}

}  // namespace
}  // namespace dbengine
