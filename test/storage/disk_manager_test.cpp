#include "storage/disk/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <vector>

#include "common/config.h"

namespace dbengine {
namespace {

class DiskManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                   "_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    std::filesystem::remove(file_path_);
  }

  void TearDown() override { std::filesystem::remove(file_path_); }

  static std::vector<char> MakePage(char fill) { return std::vector<char>(PAGE_SIZE, fill); }

  std::string file_path_;
};

TEST_F(DiskManagerTest, WriteThenReadIsByteForByteIdentical) {
  DiskManager dm(file_path_);
  page_id_t page_id = dm.AllocatePage();

  std::vector<char> written(PAGE_SIZE);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& b : written) {
    b = static_cast<char>(dist(rng));
  }

  dm.WritePage(page_id, written.data());

  std::vector<char> read_back(PAGE_SIZE, 0);
  dm.ReadPage(page_id, read_back.data());

  EXPECT_EQ(written, read_back);
}

TEST_F(DiskManagerTest, ReadingUnwrittenAllocatedPageIsZeroFilled) {
  DiskManager dm(file_path_);
  page_id_t page_id = dm.AllocatePage();

  std::vector<char> buf(PAGE_SIZE, 0x7F);  // pre-fill with garbage to prove it gets zeroed.
  dm.ReadPage(page_id, buf.data());

  EXPECT_EQ(buf, MakePage(0));
}

TEST_F(DiskManagerTest, ReadingPastEndOfFileIsZeroFilled) {
  DiskManager dm(file_path_);
  std::vector<char> buf(PAGE_SIZE, 0x7F);
  dm.ReadPage(100, buf.data());
  EXPECT_EQ(buf, MakePage(0));
}

TEST_F(DiskManagerTest, AllocatePageReturnsSequentialIds) {
  DiskManager dm(file_path_);
  page_id_t p0 = dm.AllocatePage();
  page_id_t p1 = dm.AllocatePage();
  page_id_t p2 = dm.AllocatePage();

  EXPECT_EQ(p0, 0);
  EXPECT_EQ(p1, 1);
  EXPECT_EQ(p2, 2);
  EXPECT_EQ(dm.GetNumPages(), 3u);
}

TEST_F(DiskManagerTest, RandomizedMultiPageReadWriteMatchesReferenceMap) {
  DiskManager dm(file_path_);
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> byte_dist(0, 255);

  constexpr int kNumPages = 50;
  std::vector<page_id_t> page_ids;
  for (int i = 0; i < kNumPages; ++i) {
    page_ids.push_back(dm.AllocatePage());
  }

  // Reference oracle: page_id -> expected page contents.
  std::map<page_id_t, std::vector<char>> reference;

  // Write pages in shuffled order, some pages multiple times (last write wins).
  std::vector<page_id_t> write_order;
  for (int round = 0; round < 3; ++round) {
    write_order.insert(write_order.end(), page_ids.begin(), page_ids.end());
  }
  std::shuffle(write_order.begin(), write_order.end(), rng);

  for (page_id_t pid : write_order) {
    std::vector<char> data(PAGE_SIZE);
    for (auto& b : data) {
      b = static_cast<char>(byte_dist(rng));
    }
    dm.WritePage(pid, data.data());
    reference[pid] = data;
  }

  // Read back in a different shuffled order and verify against the oracle.
  std::vector<page_id_t> read_order = page_ids;
  std::shuffle(read_order.begin(), read_order.end(), rng);
  for (page_id_t pid : read_order) {
    std::vector<char> buf(PAGE_SIZE, 0);
    dm.ReadPage(pid, buf.data());
    EXPECT_EQ(buf, reference[pid]) << "mismatch for page " << pid;
  }
}

TEST_F(DiskManagerTest, DataPersistsAcrossManagerReopen) {
  page_id_t page_id;
  std::vector<char> written = MakePage('X');
  {
    DiskManager dm(file_path_);
    page_id = dm.AllocatePage();
    dm.WritePage(page_id, written.data());
    dm.Sync();
  }

  {
    DiskManager dm(file_path_);
    // Reopening must recognize pages already on disk.
    EXPECT_GE(dm.GetNumPages(), static_cast<size_t>(page_id) + 1);

    std::vector<char> read_back(PAGE_SIZE, 0);
    dm.ReadPage(page_id, read_back.data());
    EXPECT_EQ(read_back, written);
  }
}

TEST_F(DiskManagerTest, StatsCountReadsAndWrites) {
  DiskManager dm(file_path_);
  EXPECT_EQ(dm.GetNumReads(), 0u);
  EXPECT_EQ(dm.GetNumWrites(), 0u);

  page_id_t page_id = dm.AllocatePage();
  std::vector<char> buf = MakePage('A');

  dm.WritePage(page_id, buf.data());
  dm.WritePage(page_id, buf.data());
  EXPECT_EQ(dm.GetNumWrites(), 2u);

  dm.ReadPage(page_id, buf.data());
  EXPECT_EQ(dm.GetNumReads(), 1u);
}

}  // namespace
}  // namespace dbengine
