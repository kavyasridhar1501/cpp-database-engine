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

// Large relative to swept thread counts so write-write conflicts stay rare:
// this measures synchronization overhead, not conflict-retry cost.
constexpr int64_t kKeyRange = 10000;

// Google Benchmark instantiates one Fixture per thread, so shared state must
// be a static member, created/destroyed only by thread_index==0.
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
// transaction. Measures MVCC's per-op synchronization cost as thread count grows.
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

// Same 80/20 mix against a single std::unordered_map behind one mutex, as a
// baseline: should stay flat or degrade as threads increase since every op
// serializes on the one lock.
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
