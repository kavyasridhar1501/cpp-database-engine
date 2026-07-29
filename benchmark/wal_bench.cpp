#include <benchmark/benchmark.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "wal/wal_b_plus_tree_engine.h"

namespace {

using dbengine::KeyType;
using dbengine::WALBPlusTreeEngine;

constexpr size_t kBufferPoolFrames = 64;
constexpr int64_t kKeyRange = 1000;  // keeps the tree itself small; the log is what grows.

std::string TempPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_wal_bench_" + label + ".db")).string();
}

void RemoveEngineFiles(const std::string& path) {
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".wal");
}

// Builds (once, untimed) a WAL-enabled engine whose log holds `num_ops`
// UPDATE records under a single transaction (one fsync total), keeping
// setup fast enough to sweep large log sizes.
void BuildLoggedEngineNoCheckpoint(const std::string& path, int64_t num_ops) {
  RemoveEngineFiles(path);
  WALBPlusTreeEngine engine(path, kBufferPoolFrames, /*enable_wal=*/true,
                            /*checkpoint_interval_txns=*/static_cast<int>(num_ops) + 10);
  int64_t txn = engine.Begin();
  for (int64_t i = 0; i < num_ops; ++i) {
    engine.Put(txn, i % kKeyRange, "v" + std::to_string(i));
  }
  engine.Commit(txn);
}

// Builds (once, untimed) a WAL-enabled engine with `num_ops` committed
// single-key Puts, checkpointing every 100 commits — costs num_ops
// individual fsyncs during setup, so this sweeps a smaller range than the
// single-transaction version above.
void BuildLoggedEngineWithCheckpoints(const std::string& path, int64_t num_ops) {
  RemoveEngineFiles(path);
  WALBPlusTreeEngine engine(path, kBufferPoolFrames, /*enable_wal=*/true,
                            /*checkpoint_interval_txns=*/100);
  for (int64_t i = 0; i < num_ops; ++i) {
    engine.Put(i % kKeyRange, "v" + std::to_string(i));
  }
}

// Recovery time as a function of log size, with checkpointing disabled —
// Redo must replay the entire log every time, so this isolates how
// recovery scales with raw log size.
void BM_RecoveryTime_NoCheckpoint(benchmark::State& state) {
  int64_t num_ops = state.range(0);
  std::string path = TempPath("recovery_nockpt_" + std::to_string(num_ops));
  BuildLoggedEngineNoCheckpoint(path, num_ops);

  for (auto _ : state) {
    auto t0 = std::chrono::steady_clock::now();
    WALBPlusTreeEngine engine(path, kBufferPoolFrames, /*enable_wal=*/true);
    auto t1 = std::chrono::steady_clock::now();
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
    benchmark::DoNotOptimize(engine);
  }
  RemoveEngineFiles(path);
}
BENCHMARK(BM_RecoveryTime_NoCheckpoint)
    ->Arg(1000)
    ->Arg(5000)
    ->Arg(10000)
    ->Arg(20000)
    ->Arg(50000)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// Same idea, but checkpointing every 100 committed transactions: recovery
// time should stay roughly flat as num_ops grows, unlike the version above.
void BM_RecoveryTime_PeriodicCheckpoints(benchmark::State& state) {
  int64_t num_ops = state.range(0);
  std::string path = TempPath("recovery_ckpt_" + std::to_string(num_ops));
  BuildLoggedEngineWithCheckpoints(path, num_ops);

  for (auto _ : state) {
    auto t0 = std::chrono::steady_clock::now();
    WALBPlusTreeEngine engine(path, kBufferPoolFrames, /*enable_wal=*/true);
    auto t1 = std::chrono::steady_clock::now();
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
    benchmark::DoNotOptimize(engine);
  }
  RemoveEngineFiles(path);
}
BENCHMARK(BM_RecoveryTime_PeriodicCheckpoints)
    ->Arg(1000)
    ->Arg(3000)
    ->Arg(5000)
    ->Arg(7500)
    ->Arg(10000)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// WAL on vs off: the throughput cost of logging + fsyncing every commit.
// Single-shot (->Iterations(1)) since each run builds a fresh engine.
void BM_PutThroughput_WalOn(benchmark::State& state) {
  int64_t num_ops = state.range(0);
  std::string path = TempPath("walcost_on_" + std::to_string(num_ops));
  for (auto _ : state) {
    RemoveEngineFiles(path);
    WALBPlusTreeEngine engine(path, kBufferPoolFrames, /*enable_wal=*/true);
    for (int64_t i = 0; i < num_ops; ++i) {
      engine.Put(i % kKeyRange, "v" + std::to_string(i));
    }
  }
  RemoveEngineFiles(path);
  state.SetItemsProcessed(state.iterations() * num_ops);
}
BENCHMARK(BM_PutThroughput_WalOn)->Arg(2000)->Iterations(1)->Unit(benchmark::kMillisecond);

void BM_PutThroughput_WalOff(benchmark::State& state) {
  int64_t num_ops = state.range(0);
  std::string path = TempPath("walcost_off_" + std::to_string(num_ops));
  for (auto _ : state) {
    RemoveEngineFiles(path);
    WALBPlusTreeEngine engine(path, kBufferPoolFrames, /*enable_wal=*/false);
    for (int64_t i = 0; i < num_ops; ++i) {
      engine.Put(i % kKeyRange, "v" + std::to_string(i));
    }
  }
  RemoveEngineFiles(path);
  state.SetItemsProcessed(state.iterations() * num_ops);
}
BENCHMARK(BM_PutThroughput_WalOff)->Arg(2000)->Iterations(1)->Unit(benchmark::kMillisecond);

}  // namespace
