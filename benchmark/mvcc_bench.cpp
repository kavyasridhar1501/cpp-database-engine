#include <benchmark/benchmark.h>

#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

#include "mvcc/mvcc_store.h"

namespace {

using dbengine::IsolationLevel;
using dbengine::KeyType;
using dbengine::MVCCStore;

// Large relative to the thread counts swept below so write-write conflicts
// stay rare — this benchmark is about synchronization overhead as threads
// scale, not about conflict-retry cost (that's exercised deliberately by
// MVCCStoreTest.ConcurrentIncrementNoLostUpdates instead).
constexpr int64_t kKeyRange = 10000;

// Shared fixture so every thread in a given ->Threads(N) configuration
// operates on the *same* store/map. Google Benchmark instantiates one
// Fixture object per thread, so the actual shared state has to be a static
// member, created/destroyed only by thread_index==0 — Benchmark places a
// barrier around SetUp/TearDown so this is race-free (see Google
// Benchmark's own multithreaded-fixture examples).
class MVCCFixture : public benchmark::Fixture {
 public:
  void SetUp(benchmark::State& state) override {
    if (state.thread_index() != 0) return;
    store_ = std::make_unique<MVCCStore>();
    int64_t txn = store_->Begin();
    for (int64_t k = 0; k < kKeyRange; ++k) store_->Write(txn, k, "init");
    store_->Commit(txn);

    coarse_map_.clear();
    for (int64_t k = 0; k < kKeyRange; ++k) coarse_map_[k] = "init";
  }

  void TearDown(benchmark::State& state) override {
    if (state.thread_index() != 0) return;
    store_.reset();
  }

  static std::unique_ptr<MVCCStore> store_;
  static std::unordered_map<KeyType, std::string> coarse_map_;
  static std::mutex coarse_map_mutex_;
};
std::unique_ptr<MVCCStore> MVCCFixture::store_;
std::unordered_map<KeyType, std::string> MVCCFixture::coarse_map_;
std::mutex MVCCFixture::coarse_map_mutex_;

// 80% point reads / 20% point writes, each its own autocommit SNAPSHOT
// transaction, uniform over kKeyRange keys. Version chains never get long
// enough to matter here (kKeyRange is large, so a given key is revisited
// rarely within one run), so this measures MVCC's per-op synchronization
// cost as thread count grows, not chain-scan cost.
BENCHMARK_DEFINE_F(MVCCFixture, ReadWriteMix)(benchmark::State& state) {
  std::mt19937 rng(static_cast<unsigned>(state.thread_index()) + 1);
  std::uniform_int_distribution<int64_t> key_dist(0, kKeyRange - 1);
  std::uniform_int_distribution<int> op_dist(0, 99);

  for (auto _ : state) {
    KeyType key = key_dist(rng);
    if (op_dist(rng) < 80) {
      int64_t txn = store_->Begin(IsolationLevel::SNAPSHOT);
      std::string value;
      benchmark::DoNotOptimize(store_->Read(txn, key, &value));
      store_->Commit(txn);
    } else {
      int64_t txn = store_->Begin(IsolationLevel::SNAPSHOT);
      store_->Write(txn, key, "v");
      store_->Commit(txn);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(MVCCFixture, ReadWriteMix)
    ->ThreadRange(1, 4)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

// Same 80/20 read/write mix and key range, but against a single
// std::unordered_map behind one std::mutex held for the whole operation —
// the "obvious" alternative to MVCC for making a table thread-safe. This is
// the baseline that makes MVCC's scaling advantage legible: it should stay
// roughly flat (or degrade) as threads increase, since every op serializes
// on the one lock regardless of which keys are touched.
BENCHMARK_DEFINE_F(MVCCFixture, CoarseLockReadWriteMix)(benchmark::State& state) {
  std::mt19937 rng(static_cast<unsigned>(state.thread_index()) + 1);
  std::uniform_int_distribution<int64_t> key_dist(0, kKeyRange - 1);
  std::uniform_int_distribution<int> op_dist(0, 99);

  for (auto _ : state) {
    KeyType key = key_dist(rng);
    bool do_read = op_dist(rng) < 80;
    std::lock_guard<std::mutex> lock(coarse_map_mutex_);
    if (do_read) {
      auto it = coarse_map_.find(key);
      benchmark::DoNotOptimize(it);
    } else {
      coarse_map_[key] = "v";
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK_REGISTER_F(MVCCFixture, CoarseLockReadWriteMix)
    ->ThreadRange(1, 4)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

}  // namespace
