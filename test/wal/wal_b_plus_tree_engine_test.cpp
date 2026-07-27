#include "wal/wal_b_plus_tree_engine.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>

namespace dbengine {
namespace {

class WALBPlusTreeEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_wal_bpt_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
                   ".db"))
                     .string();
    CleanupFiles();
  }
  void TearDown() override { CleanupFiles(); }

  void CleanupFiles() {
    std::filesystem::remove(file_path_);
    std::filesystem::remove(file_path_ + ".wal");
  }

  std::string file_path_;
};

TEST_F(WALBPlusTreeEngineTest, StorageEngineInterfaceRoundTrip) {
  WALBPlusTreeEngine engine(file_path_, 16);
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

TEST_F(WALBPlusTreeEngineTest, MultiOpTransactionCommitMakesAllEffectsVisible) {
  WALBPlusTreeEngine engine(file_path_, 16);
  int64_t txn = engine.Begin();
  engine.Put(txn, 1, "a");
  engine.Put(txn, 2, "b");
  engine.Delete(txn, 1);
  engine.Commit(txn);

  std::string value;
  EXPECT_FALSE(engine.Get(1, &value));
  ASSERT_TRUE(engine.Get(2, &value));
  EXPECT_EQ(value, "b");
}

TEST_F(WALBPlusTreeEngineTest, AbortOfFreshInsertRemovesIt) {
  WALBPlusTreeEngine engine(file_path_, 16);
  int64_t txn = engine.Begin();
  engine.Put(txn, 1, "poison");
  engine.Abort(txn);

  std::string value;
  EXPECT_FALSE(engine.Get(1, &value));
}

TEST_F(WALBPlusTreeEngineTest, AbortOfUpdateRestoresOldValue) {
  WALBPlusTreeEngine engine(file_path_, 16);
  engine.Put(1, "original");

  int64_t txn = engine.Begin();
  engine.Put(txn, 1, "poison");
  engine.Abort(txn);

  std::string value;
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "original");
}

TEST_F(WALBPlusTreeEngineTest, AbortOfDeleteRestoresKey) {
  WALBPlusTreeEngine engine(file_path_, 16);
  engine.Put(1, "original");

  int64_t txn = engine.Begin();
  ASSERT_TRUE(engine.Delete(txn, 1));
  engine.Abort(txn);

  std::string value;
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "original");
}

TEST_F(WALBPlusTreeEngineTest, AbortUndoesMultipleOperationsInReverseOrder) {
  WALBPlusTreeEngine engine(file_path_, 16);
  engine.Put(2, "b-original");

  int64_t txn = engine.Begin();
  engine.Put(txn, 1, "a");
  engine.Put(txn, 2, "b-poison");
  engine.Put(txn, 1, "a-poison");  // second write to same key within the txn
  engine.Abort(txn);

  std::string value;
  EXPECT_FALSE(engine.Get(1, &value));  // key 1 never existed before the txn
  ASSERT_TRUE(engine.Get(2, &value));
  EXPECT_EQ(value, "b-original");
}

TEST_F(WALBPlusTreeEngineTest, OperationsOnUnknownTransactionThrow) {
  WALBPlusTreeEngine engine(file_path_, 16);
  EXPECT_THROW(engine.Put(999, 1, "x"), std::runtime_error);
  EXPECT_THROW(engine.Delete(999, 1), std::runtime_error);
  EXPECT_THROW(engine.Commit(999), std::runtime_error);
  EXPECT_THROW(engine.Abort(999), std::runtime_error);
}

TEST_F(WALBPlusTreeEngineTest, CommittingTwiceThrows) {
  WALBPlusTreeEngine engine(file_path_, 16);
  int64_t txn = engine.Begin();
  engine.Put(txn, 1, "a");
  engine.Commit(txn);
  EXPECT_THROW(engine.Commit(txn), std::runtime_error);
}

TEST_F(WALBPlusTreeEngineTest, PutValueExceedingMaxSizeThrows) {
  WALBPlusTreeEngine engine(file_path_, 16);
  std::string too_big(MAX_VALUE_SIZE + 1, 'x');
  EXPECT_THROW(engine.Put(1, too_big), std::invalid_argument);
}

TEST_F(WALBPlusTreeEngineTest, CheckpointTriggersAfterConfiguredCommitCount) {
  WALBPlusTreeEngine engine(file_path_, 16, /*enable_wal=*/true, /*checkpoint_interval_txns=*/3);
  EXPECT_EQ(engine.GetCheckpointCount(), 0u);
  for (int i = 0; i < 3; ++i) {
    engine.Put(i, "v");
  }
  EXPECT_EQ(engine.GetCheckpointCount(), 1u);
  for (int i = 3; i < 6; ++i) {
    engine.Put(i, "v");
  }
  EXPECT_EQ(engine.GetCheckpointCount(), 2u);
}

TEST_F(WALBPlusTreeEngineTest, WalDisabledStillSupportsAbortViaInMemoryUndo) {
  WALBPlusTreeEngine engine(file_path_, 16, /*enable_wal=*/false);
  EXPECT_FALSE(engine.IsWALEnabled());
  EXPECT_EQ(engine.GetLogSizeBytes(), 0u);

  engine.Put(1, "original");
  int64_t txn = engine.Begin();
  engine.Put(txn, 1, "poison");
  engine.Abort(txn);

  std::string value;
  ASSERT_TRUE(engine.Get(1, &value));
  EXPECT_EQ(value, "original");
}

// Simulates a crash in-process: destroy the engine with a transaction still
// active (never committed or aborted — exactly what a `kill -9` would leave
// behind), then reopen the same files and verify recovery reconstructs the
// correct state: every committed transaction's effects present, the
// never-committed one's effects entirely absent.
TEST_F(WALBPlusTreeEngineTest, RecoveryRollsBackTransactionNeverCommittedBeforeCrash) {
  {
    WALBPlusTreeEngine engine(file_path_, 16);
    engine.Put(1, "committed-before");

    int64_t txn = engine.Begin();
    engine.Put(txn, 2, "should-not-survive");
    engine.Put(txn, 1, "should-also-be-undone");
    // Deliberately never Commit or Abort — simulates the process dying here.
  }
  {
    WALBPlusTreeEngine engine(file_path_, 16);
    std::string value;
    ASSERT_TRUE(engine.Get(1, &value));
    EXPECT_EQ(value, "committed-before");
    EXPECT_FALSE(engine.Get(2, &value));
  }
}

TEST_F(WALBPlusTreeEngineTest, RecoveryKeepsCommittedTransactionAcrossCrash) {
  {
    WALBPlusTreeEngine engine(file_path_, 16);
    int64_t txn = engine.Begin();
    engine.Put(txn, 1, "a");
    engine.Put(txn, 2, "b");
    engine.Commit(txn);
    // A second, uncommitted transaction that should NOT survive.
    int64_t txn2 = engine.Begin();
    engine.Put(txn2, 3, "c");
  }
  {
    WALBPlusTreeEngine engine(file_path_, 16);
    std::string value;
    ASSERT_TRUE(engine.Get(1, &value));
    EXPECT_EQ(value, "a");
    ASSERT_TRUE(engine.Get(2, &value));
    EXPECT_EQ(value, "b");
    EXPECT_FALSE(engine.Get(3, &value));
  }
}

TEST_F(WALBPlusTreeEngineTest, RecoveryAfterCheckpointOnlyReplaysSinceCheckpoint) {
  {
    WALBPlusTreeEngine engine(file_path_, 16, /*enable_wal=*/true, /*checkpoint_interval_txns=*/2);
    engine.Put(1, "a");  // txn 1 committed
    engine.Put(2, "b");  // txn 2 committed -> triggers a checkpoint
    ASSERT_GE(engine.GetCheckpointCount(), 1u);
    engine.Put(3, "c");  // after the checkpoint
    int64_t txn = engine.Begin();
    engine.Put(txn, 4, "should-not-survive");
    // crash: txn never resolved.
  }
  {
    WALBPlusTreeEngine engine(file_path_, 16);
    std::string value;
    ASSERT_TRUE(engine.Get(1, &value));
    EXPECT_EQ(value, "a");
    ASSERT_TRUE(engine.Get(2, &value));
    EXPECT_EQ(value, "b");
    ASSERT_TRUE(engine.Get(3, &value));
    EXPECT_EQ(value, "c");
    EXPECT_FALSE(engine.Get(4, &value));
  }
}

// The core correctness deliverable: randomized transactions (mix of commit
// and abort) validated against a std::map oracle, then a simulated crash
// (destroy with one final transaction left dangling) and a fresh reopen to
// confirm recovery reconstructs exactly the oracle's committed state.
TEST_F(WALBPlusTreeEngineTest, RandomizedTransactionsMatchOracleAcrossSimulatedCrash) {
  constexpr int kNumTxns = 500;
  constexpr int kKeyRange = 100;

  std::map<KeyType, std::string> oracle;
  std::mt19937 rng(20260727);
  std::uniform_int_distribution<int> key_dist(0, kKeyRange - 1);
  std::bernoulli_distribution commit_dist(0.8);  // 80% commit, 20% abort.

  {
    WALBPlusTreeEngine engine(file_path_, 16, /*enable_wal=*/true, /*checkpoint_interval_txns=*/17);

    for (int t = 0; t < kNumTxns; ++t) {
      int64_t txn = engine.Begin();
      std::map<KeyType, std::string> tentative = oracle;

      int num_ops = 1 + static_cast<int>(t % 3);
      for (int op = 0; op < num_ops; ++op) {
        KeyType key = key_dist(rng);
        if ((t + op) % 4 == 0 && tentative.count(key)) {
          engine.Delete(txn, key);
          tentative.erase(key);
        } else {
          std::string value = "v-" + std::to_string(t) + "-" + std::to_string(op);
          engine.Put(txn, key, value);
          tentative[key] = value;
        }
      }

      if (commit_dist(rng)) {
        engine.Commit(txn);
        oracle = tentative;
      } else {
        engine.Abort(txn);
        // oracle unchanged.
      }
    }
    // Leave the engine mid-way through one final, never-resolved
    // transaction to simulate a crash.
    int64_t dangling = engine.Begin();
    engine.Put(dangling, key_dist(rng), "should-not-survive-the-crash");
  }

  // Reopen: recovery must reconstruct exactly the oracle's state.
  {
    WALBPlusTreeEngine engine(file_path_, 16);
    for (KeyType k = 0; k < kKeyRange; ++k) {
      std::string value;
      bool found = engine.Get(k, &value);
      auto it = oracle.find(k);
      ASSERT_EQ(found, it != oracle.end()) << "presence mismatch for key " << k;
      if (found) {
        EXPECT_EQ(value, it->second) << "value mismatch for key " << k;
      }
    }
  }
}

}  // namespace
}  // namespace dbengine
