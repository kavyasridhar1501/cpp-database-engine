#include "mvcc/mvcc_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace dbengine {
namespace {

// Convenience: read a key via its own short-lived transaction. Isolation
// level doesn't matter for a single read with nothing concurrent.
std::string ReadOnce(MVCCStore* store, KeyType key, bool* found) {
  int64_t txn = store->Begin();
  std::string value;
  *found = store->Read(txn, key, &value);
  store->Abort(txn);  // read-only; Abort vs Commit makes no difference here.
  return value;
}

void PutCommitted(MVCCStore* store, KeyType key, const std::string& value) {
  int64_t txn = store->Begin();
  store->Write(txn, key, value);
  ASSERT_TRUE(store->Commit(txn));
}

// ---------------------------------------------------------------------
// Basic transaction lifecycle
// ---------------------------------------------------------------------

TEST(MVCCStoreTest, BasicPutGetCommit) {
  MVCCStore store;
  PutCommitted(&store, 1, "hello");

  bool found = false;
  std::string value = ReadOnce(&store, 1, &found);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "hello");
}

TEST(MVCCStoreTest, ReadMissingKeyReturnsFalse) {
  MVCCStore store;
  bool found = false;
  ReadOnce(&store, 42, &found);
  EXPECT_FALSE(found);
}

TEST(MVCCStoreTest, ReadYourOwnWrites) {
  MVCCStore store;
  int64_t txn = store.Begin();
  store.Write(txn, 1, "in-progress");

  std::string value;
  ASSERT_TRUE(store.Read(txn, 1, &value));
  EXPECT_EQ(value, "in-progress");

  // Not visible to anyone else yet.
  bool found = false;
  ReadOnce(&store, 1, &found);
  EXPECT_FALSE(found);

  ASSERT_TRUE(store.Commit(txn));
  found = false;
  std::string committed_value = ReadOnce(&store, 1, &found);
  EXPECT_TRUE(found);
  EXPECT_EQ(committed_value, "in-progress");
}

TEST(MVCCStoreTest, AbortDiscardsWrites) {
  MVCCStore store;
  PutCommitted(&store, 1, "original");

  int64_t txn = store.Begin();
  store.Write(txn, 1, "should not stick");
  store.Abort(txn);

  bool found = false;
  std::string value = ReadOnce(&store, 1, &found);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "original");
}

TEST(MVCCStoreTest, DeleteReturnsWhetherKeyExisted) {
  MVCCStore store;

  int64_t txn1 = store.Begin();
  EXPECT_FALSE(store.Delete(txn1, 1));  // never existed
  store.Commit(txn1);

  PutCommitted(&store, 1, "value");

  int64_t txn2 = store.Begin();
  EXPECT_TRUE(store.Delete(txn2, 1));  // existed
  ASSERT_TRUE(store.Commit(txn2));

  bool found = false;
  ReadOnce(&store, 1, &found);
  EXPECT_FALSE(found);
}

TEST(MVCCStoreTest, WriteWriteConflictAbortsSecondCommitter) {
  MVCCStore store;
  PutCommitted(&store, 1, "base");

  int64_t txn1 = store.Begin(IsolationLevel::SNAPSHOT);
  int64_t txn2 = store.Begin(IsolationLevel::SNAPSHOT);

  store.Write(txn1, 1, "from txn1");
  store.Write(txn2, 1, "from txn2");

  EXPECT_TRUE(store.Commit(txn1));
  EXPECT_FALSE(store.Commit(txn2));  // conflicts with txn1's commit

  bool found = false;
  std::string value = ReadOnce(&store, 1, &found);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "from txn1");
}

// ---------------------------------------------------------------------
// Anomaly demonstrations. Interleavings are forced by call order on a
// single thread (not racing timers), so these are deterministic.
// ---------------------------------------------------------------------

TEST(MVCCStoreTest, DirtyReadUnderReadUncommitted) {
  MVCCStore store;
  int64_t writer = store.Begin(IsolationLevel::SNAPSHOT);
  store.Write(writer, 1, "uncommitted");

  int64_t reader = store.Begin(IsolationLevel::READ_UNCOMMITTED);
  std::string value;
  ASSERT_TRUE(store.Read(reader, 1, &value));
  EXPECT_EQ(value, "uncommitted");  // dirty read: saw data that never committed
  store.Abort(reader);

  store.Abort(writer);

  bool found = false;
  ReadOnce(&store, 1, &found);
  EXPECT_FALSE(found);  // and indeed it never did commit
}

TEST(MVCCStoreTest, DirtyReadVanishesUnderReadCommitted) {
  MVCCStore store;
  int64_t writer = store.Begin(IsolationLevel::SNAPSHOT);
  store.Write(writer, 1, "uncommitted");

  int64_t reader = store.Begin(IsolationLevel::READ_COMMITTED);
  std::string value;
  EXPECT_FALSE(store.Read(reader, 1, &value));  // no committed version exists
  store.Abort(reader);
  store.Abort(writer);
}

TEST(MVCCStoreTest, DirtyReadVanishesUnderSnapshot) {
  MVCCStore store;
  int64_t writer = store.Begin(IsolationLevel::SNAPSHOT);
  store.Write(writer, 1, "uncommitted");

  int64_t reader = store.Begin(IsolationLevel::SNAPSHOT);
  std::string value;
  EXPECT_FALSE(store.Read(reader, 1, &value));
  store.Abort(reader);
  store.Abort(writer);
}

TEST(MVCCStoreTest, NonRepeatableReadUnderReadCommitted) {
  MVCCStore store;
  PutCommitted(&store, 1, "v1");

  int64_t reader = store.Begin(IsolationLevel::READ_COMMITTED);
  std::string first;
  ASSERT_TRUE(store.Read(reader, 1, &first));
  EXPECT_EQ(first, "v1");

  PutCommitted(&store, 1, "v2");  // another txn commits in between

  std::string second;
  ASSERT_TRUE(store.Read(reader, 1, &second));
  EXPECT_EQ(second, "v2");
  EXPECT_NE(first, second);  // same txn, same key, two different answers
  store.Abort(reader);
}

TEST(MVCCStoreTest, NonRepeatableReadVanishesUnderSnapshot) {
  MVCCStore store;
  PutCommitted(&store, 1, "v1");

  int64_t reader = store.Begin(IsolationLevel::SNAPSHOT);
  std::string first;
  ASSERT_TRUE(store.Read(reader, 1, &first));
  EXPECT_EQ(first, "v1");

  PutCommitted(&store, 1, "v2");

  std::string second;
  ASSERT_TRUE(store.Read(reader, 1, &second));
  EXPECT_EQ(second, first);  // still "v1": repeatable within the snapshot
  store.Abort(reader);
}

// Classic two-key write skew: two on-call doctors, invariant "at least one
// is on call" (true). Each transaction independently checks both keys and
// takes only one of them off call, believing the other covers it.
TEST(MVCCStoreTest, WriteSkewUnderSnapshot) {
  MVCCStore store;
  PutCommitted(&store, /*doctor_a=*/1, "on_call");
  PutCommitted(&store, /*doctor_b=*/2, "on_call");

  int64_t txn1 = store.Begin(IsolationLevel::SNAPSHOT);
  int64_t txn2 = store.Begin(IsolationLevel::SNAPSHOT);

  std::string a, b;
  ASSERT_TRUE(store.Read(txn1, 1, &a));
  ASSERT_TRUE(store.Read(txn1, 2, &b));
  store.Write(txn1, 1, "off_call");  // txn1 takes doctor A off, sees B covering

  ASSERT_TRUE(store.Read(txn2, 1, &a));
  ASSERT_TRUE(store.Read(txn2, 2, &b));
  store.Write(txn2, 2, "off_call");  // txn2 takes doctor B off, sees A covering

  EXPECT_TRUE(store.Commit(txn1));
  EXPECT_TRUE(store.Commit(txn2));  // plain SI does NOT catch this

  bool found;
  std::string final_a = ReadOnce(&store, 1, &found);
  std::string final_b = ReadOnce(&store, 2, &found);
  EXPECT_EQ(final_a, "off_call");
  EXPECT_EQ(final_b, "off_call");  // invariant violated: nobody on call
}

TEST(MVCCStoreTest, WriteSkewVanishesUnderSerializableSnapshot) {
  MVCCStore store;
  PutCommitted(&store, 1, "on_call");
  PutCommitted(&store, 2, "on_call");

  int64_t txn1 = store.Begin(IsolationLevel::SERIALIZABLE_SNAPSHOT);
  int64_t txn2 = store.Begin(IsolationLevel::SERIALIZABLE_SNAPSHOT);

  std::string a, b;
  ASSERT_TRUE(store.Read(txn1, 1, &a));
  ASSERT_TRUE(store.Read(txn1, 2, &b));
  store.Write(txn1, 1, "off_call");

  ASSERT_TRUE(store.Read(txn2, 1, &a));
  ASSERT_TRUE(store.Read(txn2, 2, &b));
  store.Write(txn2, 2, "off_call");

  bool txn1_committed = store.Commit(txn1);
  bool txn2_committed = store.Commit(txn2);
  ASSERT_TRUE(txn1_committed);
  EXPECT_FALSE(txn2_committed);  // caught: txn2 read what txn1 then wrote

  bool found;
  std::string final_a = ReadOnce(&store, 1, &found);
  std::string final_b = ReadOnce(&store, 2, &found);
  // Exactly one doctor went off call: the invariant survives.
  EXPECT_TRUE((final_a == "off_call" && final_b == "on_call") ||
              (final_a == "on_call" && final_b == "off_call"));
}

// ---------------------------------------------------------------------
// Concurrency stress: real threads, real races, proving no lost updates.
// ---------------------------------------------------------------------

TEST(MVCCStoreTest, ConcurrentIncrementNoLostUpdates) {
  MVCCStore store;
  PutCommitted(&store, 1, "0");

  constexpr int kThreads = 4;
  constexpr int kIncrementsPerThread = 200;

  auto worker = [&store]() {
    for (int i = 0; i < kIncrementsPerThread; ++i) {
      while (true) {
        int64_t txn = store.Begin(IsolationLevel::SNAPSHOT);
        std::string current;
        ASSERT_TRUE(store.Read(txn, 1, &current));
        int next = std::stoi(current) + 1;
        store.Write(txn, 1, std::to_string(next));
        if (store.Commit(txn)) break;  // else: conflict, retry
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker);
  for (auto& t : threads) t.join();

  bool found = false;
  std::string final_value = ReadOnce(&store, 1, &found);
  ASSERT_TRUE(found);
  EXPECT_EQ(std::stoi(final_value), kThreads * kIncrementsPerThread);
}

// ---------------------------------------------------------------------
// Garbage collection
// ---------------------------------------------------------------------

TEST(MVCCStoreTest, GarbageCollectionReclaimsOldVersions) {
  MVCCStore store;
  for (int i = 0; i < 5; ++i) {
    PutCommitted(&store, 1, "v" + std::to_string(i));
  }
  EXPECT_GT(store.GetVersionCount(1), 1u);

  size_t removed = store.RunGarbageCollection();
  EXPECT_GT(removed, 0u);
  EXPECT_EQ(store.GetVersionCount(1), 1u);

  bool found = false;
  std::string value = ReadOnce(&store, 1, &found);
  EXPECT_TRUE(found);
  EXPECT_EQ(value, "v4");  // GC never touches the latest version
}

TEST(MVCCStoreTest, GarbageCollectionRespectsActiveSnapshot) {
  MVCCStore store;
  PutCommitted(&store, 1, "v0");

  int64_t long_running = store.Begin(IsolationLevel::SNAPSHOT);
  std::string seen_by_snapshot;
  ASSERT_TRUE(store.Read(long_running, 1, &seen_by_snapshot));
  EXPECT_EQ(seen_by_snapshot, "v0");

  for (int i = 1; i <= 3; ++i) {
    PutCommitted(&store, 1, "v" + std::to_string(i));
  }

  store.RunGarbageCollection();
  // v0 must survive: long_running's snapshot still needs it.
  EXPECT_EQ(store.GetVersionCount(1), 2u);  // v0 (still visible) + v3 (latest)

  std::string still_v0;
  ASSERT_TRUE(store.Read(long_running, 1, &still_v0));
  EXPECT_EQ(still_v0, "v0");

  store.Abort(long_running);
  store.RunGarbageCollection();
  EXPECT_EQ(store.GetVersionCount(1), 1u);
}

// ---------------------------------------------------------------------
// Randomized oracle test: single-threaded, one txn per operation
// (autocommit), checked against std::map. Concurrency correctness is
// covered separately above; this is about Get/Put/Delete semantics.
// ---------------------------------------------------------------------

TEST(MVCCStoreTest, RandomizedOracleAgainstStdMap) {
  MVCCStore store;
  std::map<KeyType, std::string> oracle;

  std::mt19937 rng(12345);
  std::uniform_int_distribution<KeyType> key_dist(0, 49);
  std::uniform_int_distribution<int> op_dist(0, 2);

  for (int iter = 0; iter < 5000; ++iter) {
    KeyType key = key_dist(rng);
    int op = op_dist(rng);

    if (op == 0) {  // Put
      std::string value = "val" + std::to_string(iter);
      int64_t txn = store.Begin();
      store.Write(txn, key, value);
      ASSERT_TRUE(store.Commit(txn));
      oracle[key] = value;
    } else if (op == 1) {  // Delete
      int64_t txn = store.Begin();
      bool existed = store.Delete(txn, key);
      ASSERT_TRUE(store.Commit(txn));
      EXPECT_EQ(existed, oracle.count(key) > 0);
      oracle.erase(key);
    } else {  // Get
      bool found = false;
      std::string value = ReadOnce(&store, key, &found);
      auto it = oracle.find(key);
      if (it == oracle.end()) {
        EXPECT_FALSE(found);
      } else {
        EXPECT_TRUE(found);
        EXPECT_EQ(value, it->second);
      }
    }
  }
}

}  // namespace
}  // namespace dbengine
