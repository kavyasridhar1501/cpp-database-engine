#include "lsm/lsm_tree_engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <thread>

namespace dbengine {
namespace {

class LSMTreeEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_lsm_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    CleanupFiles();
  }

  void TearDown() override { CleanupFiles(); }

  // Removes the manifest file and every "<file>.sstN" it may have created.
  void CleanupFiles() {
    std::filesystem::remove(file_path_);
    std::filesystem::path dir = std::filesystem::path(file_path_).parent_path();
    std::string prefix = std::filesystem::path(file_path_).filename().string() + ".sst";
    if (!std::filesystem::exists(dir)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) {
        std::filesystem::remove(entry.path());
      }
    }
  }

  // Polls `predicate` until it's true or `timeout` elapses; returns whether
  // it succeeded. Used to wait for the background compaction thread.
  static bool WaitUntil(const std::function<bool()>& predicate,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
  }

  std::string file_path_;
};

TEST_F(LSMTreeEngineTest, PutGetDeleteRoundTrip) {
  LSMTreeEngine engine(file_path_);
  std::string value;

  EXPECT_FALSE(engine.Get(1, &value));

  engine.Put(1, "hello");
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "hello");

  engine.Put(1, "updated");
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "updated");

  EXPECT_TRUE(engine.Delete(1));
  EXPECT_FALSE(engine.Get(1, &value));
  EXPECT_FALSE(engine.Delete(1));
}

TEST_F(LSMTreeEngineTest, ScanReturnsAscendingKeys) {
  LSMTreeEngine engine(file_path_);
  for (KeyType k = 0; k < 100; ++k) {
    engine.Put(k, "v" + std::to_string(k));
  }

  auto it = engine.Scan(INVALID_KEY);
  KeyType expected = 0;
  int count = 0;
  while (it->IsValid()) {
    EXPECT_EQ(it->GetKey(), expected);
    EXPECT_EQ(it->GetValue(), "v" + std::to_string(expected));
    ++expected;
    ++count;
    it->Next();
  }
  EXPECT_EQ(count, 100);
}

TEST_F(LSMTreeEngineTest, ScanFromMidpointExcludesDeletedKeys) {
  LSMTreeEngine engine(file_path_);
  for (KeyType k = 0; k < 20; ++k) {
    engine.Put(k, "v" + std::to_string(k));
  }
  engine.Delete(10);
  engine.Delete(11);

  auto it = engine.Scan(9);
  std::vector<KeyType> seen;
  for (; it->IsValid(); it->Next()) seen.push_back(it->GetKey());
  // 9, then 10 and 11 are deleted, so the next visible key is 12.
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.front(), 9);
  EXPECT_EQ(std::find(seen.begin(), seen.end(), 10), seen.end());
  EXPECT_EQ(std::find(seen.begin(), seen.end(), 11), seen.end());
  EXPECT_NE(std::find(seen.begin(), seen.end(), 12), seen.end());
}

TEST_F(LSMTreeEngineTest, PutValueExceedingMaxSizeThrows) {
  LSMTreeEngine engine(file_path_);
  std::string too_big(MAX_VALUE_SIZE + 1, 'x');
  EXPECT_THROW(engine.Put(1, too_big), std::invalid_argument);
}

// The Phase 3 deliverable: demonstrate SSTables actually get created (via
// memtable flush) and compacted (merged into a higher tier) under normal
// write load, not just as an isolated unit test of the builder.
TEST_F(LSMTreeEngineTest, FlushAndCompactionDemo) {
  // A tiny flush threshold and tier threshold so a modest key count forces
  // several flushes and at least one compaction.
  LSMTreeEngine engine(file_path_, /*memtable_flush_threshold_bytes=*/2048,
                       /*tier_compaction_threshold=*/3);

  constexpr int kNumKeys = 2000;
  for (KeyType k = 0; k < kNumKeys; ++k) {
    engine.Put(k, "value-" + std::to_string(k));
  }

  // Compaction runs on a background thread; give it a chance to catch up.
  ASSERT_TRUE(WaitUntil([&] { return engine.NumTiers() >= 2; }))
      << "compaction never promoted any data to tier 1 within the timeout";

  // Data must still be fully correct after flushes and compactions.
  for (KeyType k = 0; k < kNumKeys; ++k) {
    std::string value;
    ASSERT_TRUE(engine.Get(k, &value)) << "missing key " << k << " after compaction";
    EXPECT_EQ(value, "value-" + std::to_string(k));
  }

  auto it = engine.Scan(INVALID_KEY);
  int count = 0;
  KeyType expected = 0;
  for (; it->IsValid(); it->Next(), ++expected, ++count) {
    ASSERT_EQ(it->GetKey(), expected);
  }
  EXPECT_EQ(count, kNumKeys);

  // Space amplification sanity: on-disk bytes shouldn't have ballooned to
  // some huge multiple of the raw data (loose bound; the point is proving
  // compaction reclaimed the pre-compaction SSTables' space, not obtaining
  // a tight ratio).
  size_t raw_bytes = static_cast<size_t>(kNumKeys) * (sizeof(KeyType) + 10);
  EXPECT_LT(engine.TotalSSTableBytesOnDisk(), raw_bytes * 20);
}

TEST_F(LSMTreeEngineTest, DataPersistsAcrossEngineReopen) {
  {
    LSMTreeEngine engine(file_path_, /*memtable_flush_threshold_bytes=*/1024,
                         /*tier_compaction_threshold=*/3);
    for (KeyType k = 0; k < 500; ++k) {
      engine.Put(k, "persisted-" + std::to_string(k));
    }
    ASSERT_TRUE(WaitUntil([&] { return engine.NumSSTables() > 0; }));
  }
  {
    LSMTreeEngine engine(file_path_);
    for (KeyType k = 0; k < 500; ++k) {
      std::string value;
      ASSERT_TRUE(engine.Get(k, &value)) << "missing key " << k << " after reopen";
      EXPECT_EQ(value, "persisted-" + std::to_string(k));
    }
  }
}

TEST_F(LSMTreeEngineTest, DeleteSurvivesFlushAndReopen) {
  {
    // A tiny threshold so even these two small puts cross it and flush.
    LSMTreeEngine engine(file_path_, /*memtable_flush_threshold_bytes=*/8,
                         /*tier_compaction_threshold=*/3);
    engine.Put(1, "a");
    engine.Put(2, "b");
    ASSERT_TRUE(WaitUntil([&] { return engine.NumSSTables() > 0; }));
    ASSERT_TRUE(engine.Delete(1));
  }
  {
    LSMTreeEngine engine(file_path_);
    std::string value;
    EXPECT_FALSE(engine.Get(1, &value));
    ASSERT_TRUE(engine.Get(2, &value));
    EXPECT_EQ(value, "b");
  }
}

// The core correctness deliverable for this engine: randomized insert/
// delete/lookup validated against a std::map oracle, with a small flush
// threshold and tier threshold so the run genuinely exercises flush,
// compaction (including background compaction racing with foreground
// operations), and tombstone handling — not just the in-memory memtable
// path.
TEST_F(LSMTreeEngineTest, RandomizedOperationsMatchStdMapOracle) {
  constexpr int kNumOps = 20000;
  constexpr int kKeyRange = 3000;

  LSMTreeEngine engine(file_path_, /*memtable_flush_threshold_bytes=*/4096,
                       /*tier_compaction_threshold=*/3);
  std::map<KeyType, std::string> oracle;

  std::mt19937 rng(20260727);
  std::uniform_int_distribution<int> key_dist(0, kKeyRange - 1);
  std::uniform_int_distribution<int> op_dist(0, 9);

  for (int op = 0; op < kNumOps; ++op) {
    KeyType key = key_dist(rng);
    int choice = op_dist(rng);

    if (choice <= 5) {
      std::string value = "val-" + std::to_string(key) + "-" + std::to_string(op);
      engine.Put(key, value);
      oracle[key] = value;
    } else if (choice <= 7) {
      bool engine_removed = engine.Delete(key);
      bool oracle_had = oracle.erase(key) > 0;
      ASSERT_EQ(engine_removed, oracle_had) << "delete mismatch for key " << key;
    } else {
      std::string engine_value;
      bool engine_found = engine.Get(key, &engine_value);
      auto oracle_it = oracle.find(key);
      bool oracle_found = oracle_it != oracle.end();
      ASSERT_EQ(engine_found, oracle_found) << "lookup presence mismatch for key " << key;
      if (oracle_found) {
        EXPECT_EQ(engine_value, oracle_it->second) << "lookup value mismatch for key " << key;
      }
    }
  }

  // Let any in-flight compaction settle before the final full sweep.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto it = engine.Scan(INVALID_KEY);
  auto oracle_it = oracle.begin();
  size_t scanned = 0;
  for (; it->IsValid(); it->Next(), ++oracle_it, ++scanned) {
    ASSERT_NE(oracle_it, oracle.end());
    EXPECT_EQ(it->GetKey(), oracle_it->first);
    EXPECT_EQ(it->GetValue(), oracle_it->second);
  }
  EXPECT_EQ(scanned, oracle.size());
  EXPECT_EQ(oracle_it, oracle.end());
}

}  // namespace
}  // namespace dbengine
