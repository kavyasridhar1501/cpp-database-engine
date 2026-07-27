#include <benchmark/benchmark.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "engine/storage_engine.h"
#include "index/b_plus_tree_engine.h"
#include "lsm/lsm_tree_engine.h"

namespace {

using dbengine::BPlusTreeEngine;
using dbengine::KeyType;
using dbengine::LSMTreeEngine;
using dbengine::StorageEngine;

// Same buffer pool size used for both engines' comparisons in Phase 2 —
// deliberately small relative to the dataset (see DESIGN.md), so both
// engines are compared under the same "index doesn't fully fit in RAM"
// condition rather than one benefiting from a bigger effective cache.
constexpr size_t kBufferPoolFrames = 2000;
constexpr size_t kLSMFlushThresholdBytes = 256 * 1024;
constexpr int kLSMTierThreshold = 4;

constexpr int64_t kNumKeys = 100000;
constexpr int kNumOps = 30000;
constexpr double kZipfSkew = 0.99;

std::string TempPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_cmp_bench_" + label + ".db")).string();
}

// Removes an engine's file(s): the plain path for a B+-tree, plus every
// "<path>.sstN" for an LSM-tree.
void RemoveEngineFiles(const std::string& path) {
  std::filesystem::remove(path);
  std::filesystem::path dir = std::filesystem::path(path).parent_path();
  std::string prefix = std::filesystem::path(path).filename().string() + ".sst";
  if (!std::filesystem::exists(dir)) return;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().rfind(prefix, 0) == 0) {
      std::filesystem::remove(entry.path());
    }
  }
}

std::vector<KeyType> BuildShuffledKeys(int64_t n, unsigned seed) {
  std::vector<KeyType> keys(static_cast<size_t>(n));
  std::iota(keys.begin(), keys.end(), 0);
  std::mt19937 rng(seed);
  std::shuffle(keys.begin(), keys.end(), rng);
  return keys;
}

std::string ValueFor(KeyType key) { return "v" + std::to_string(key); }

enum class OpType { kRead, kWrite };
struct Op {
  OpType type;
  KeyType key;
};

// Zipfian-distributed keys (skewed popularity, matching the Phase 1
// benchmark's methodology), mixed read/write per `write_fraction`.
std::vector<Op> GenerateWorkload(int num_ops, int64_t key_range, double write_fraction, unsigned seed) {
  std::vector<double> weights(static_cast<size_t>(key_range));
  for (int64_t i = 0; i < key_range; ++i) {
    weights[static_cast<size_t>(i)] = 1.0 / std::pow(static_cast<double>(i + 1), kZipfSkew);
  }
  std::discrete_distribution<int64_t> key_dist(weights.begin(), weights.end());
  std::bernoulli_distribution write_dist(write_fraction);
  std::mt19937 rng(seed);

  std::vector<Op> ops(static_cast<size_t>(num_ops));
  for (auto& op : ops) {
    op.type = write_dist(rng) ? OpType::kWrite : OpType::kRead;
    op.key = key_dist(rng);
  }
  return ops;
}

struct EngineStats {
  size_t disk_reads;
  size_t disk_writes;
  size_t on_disk_bytes;
};

template <typename EngineT>
std::unique_ptr<EngineT> MakeEngine(const std::string& path);

template <>
std::unique_ptr<BPlusTreeEngine> MakeEngine<BPlusTreeEngine>(const std::string& path) {
  return std::make_unique<BPlusTreeEngine>(path, kBufferPoolFrames);
}

template <>
std::unique_ptr<LSMTreeEngine> MakeEngine<LSMTreeEngine>(const std::string& path) {
  return std::make_unique<LSMTreeEngine>(path, kLSMFlushThresholdBytes, kLSMTierThreshold);
}

template <typename EngineT>
EngineStats GetStats(EngineT& engine);

template <>
EngineStats GetStats<BPlusTreeEngine>(BPlusTreeEngine& engine) {
  return {engine.GetDiskReadCount(), engine.GetDiskWriteCount(), engine.OnDiskBytes()};
}

template <>
EngineStats GetStats<LSMTreeEngine>(LSMTreeEngine& engine) {
  return {engine.GetDiskReadCount(), engine.GetDiskWriteCount(), engine.TotalSSTableBytesOnDisk()};
}

// LSM-tree compaction runs on a background thread; give it a moment to
// settle so amplification numbers reflect a steady state rather than a
// snapshot mid-compaction. No-op (just a wasted, harmless sleep) for the
// B+-tree, which has no background work.
template <typename EngineT>
void SettleBackgroundWork() {
  if constexpr (std::is_same_v<EngineT, LSMTreeEngine>) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
}

template <typename EngineT>
std::unique_ptr<EngineT> BuildPopulatedEngine(const std::string& path) {
  RemoveEngineFiles(path);
  auto engine = MakeEngine<EngineT>(path);
  for (KeyType k : BuildShuffledKeys(kNumKeys, /*seed=*/123)) {
    engine->Put(k, ValueFor(k));
  }
  SettleBackgroundWork<EngineT>();
  return engine;
}

// Runs `ops` once (registered with ->Iterations(1)) against a freshly
// populated engine and reports throughput, average latency, and read/write
// amplification (disk reads/writes per op, and total on-disk bytes versus
// the logical dataset size) as Google Benchmark counters.
template <typename EngineT>
void RunPointWorkload(benchmark::State& state, const std::string& label, double write_fraction) {
  std::string path = TempPath(label);
  auto engine = BuildPopulatedEngine<EngineT>(path);
  auto ops = GenerateWorkload(kNumOps, kNumKeys, write_fraction, /*seed=*/456);

  EngineStats before = GetStats<EngineT>(*engine);
  auto t0 = std::chrono::steady_clock::now();

  for (auto _ : state) {
    std::string value;
    for (const auto& op : ops) {
      if (op.type == OpType::kWrite) {
        engine->Put(op.key, "updated-" + std::to_string(op.key));
      } else {
        benchmark::DoNotOptimize(engine->Get(op.key, &value));
      }
    }
  }

  auto t1 = std::chrono::steady_clock::now();
  SettleBackgroundWork<EngineT>();
  EngineStats after = GetStats<EngineT>(*engine);

  double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
  double total_ops = static_cast<double>(state.iterations()) * static_cast<double>(ops.size());

  state.counters["OpsPerSec"] = total_ops / elapsed_sec;
  state.counters["AvgLatencyUs"] = (elapsed_sec / total_ops) * 1e6;
  state.counters["DiskReadsPerOp"] =
      static_cast<double>(after.disk_reads - before.disk_reads) / total_ops;
  state.counters["DiskWritesPerOp"] =
      static_cast<double>(after.disk_writes - before.disk_writes) / total_ops;
  state.counters["OnDiskMB"] = static_cast<double>(after.on_disk_bytes) / (1024.0 * 1024.0);

  double logical_bytes = static_cast<double>(kNumKeys) * (sizeof(KeyType) + 8);
  state.counters["SpaceAmp"] = static_cast<double>(after.on_disk_bytes) / logical_bytes;

  // Destroy the engine (an LSM engine's destructor does one final flush,
  // which creates one more file) before removing files, or that last file
  // would be orphaned.
  engine.reset();
  RemoveEngineFiles(path);
}

template <typename EngineT>
void RunRangeScanWorkload(benchmark::State& state, const std::string& label) {
  constexpr int kNumScans = 500;
  constexpr int kWindow = 100;

  std::string path = TempPath(label);
  auto engine = BuildPopulatedEngine<EngineT>(path);

  std::mt19937 rng(789);
  std::uniform_int_distribution<int64_t> dist(0, kNumKeys - 1);

  EngineStats before = GetStats<EngineT>(*engine);
  int64_t total_scanned = 0;
  auto t0 = std::chrono::steady_clock::now();

  for (auto _ : state) {
    for (int i = 0; i < kNumScans; ++i) {
      KeyType start = dist(rng);
      auto cursor = engine->Scan(start);
      int c = 0;
      while (cursor->IsValid() && c < kWindow) {
        benchmark::DoNotOptimize(cursor->GetValue());
        ++c;
        cursor->Next();
      }
      total_scanned += c;
    }
  }

  auto t1 = std::chrono::steady_clock::now();
  SettleBackgroundWork<EngineT>();
  EngineStats after = GetStats<EngineT>(*engine);

  double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
  double total_scans = static_cast<double>(state.iterations()) * kNumScans;

  state.counters["ScansPerSec"] = total_scans / elapsed_sec;
  state.counters["KeysPerSec"] = static_cast<double>(total_scanned) / elapsed_sec;
  state.counters["DiskReadsPerScan"] =
      static_cast<double>(after.disk_reads - before.disk_reads) / total_scans;

  engine.reset();
  RemoveEngineFiles(path);
}

// Sweeps write fraction across the write-heavy (90%) / read-heavy (95%
// reads, i.e. 5% writes) range the Phase 3 brief asks for, plus enough
// intermediate points (10/30/50/70%) to actually locate where the two
// engines' throughput curves cross, rather than only reporting the two
// endpoints. state.range(0) is the write percentage.
void BM_MixedWorkload_BPlusTree(benchmark::State& state) {
  int write_pct = static_cast<int>(state.range(0));
  RunPointWorkload<BPlusTreeEngine>(state, "mixed_bptree_" + std::to_string(write_pct),
                                    write_pct / 100.0);
}
BENCHMARK(BM_MixedWorkload_BPlusTree)
    ->Arg(5)
    ->Arg(10)
    ->Arg(30)
    ->Arg(50)
    ->Arg(70)
    ->Arg(90)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_MixedWorkload_LSM(benchmark::State& state) {
  int write_pct = static_cast<int>(state.range(0));
  RunPointWorkload<LSMTreeEngine>(state, "mixed_lsm_" + std::to_string(write_pct),
                                  write_pct / 100.0);
}
BENCHMARK(BM_MixedWorkload_LSM)
    ->Arg(5)
    ->Arg(10)
    ->Arg(30)
    ->Arg(50)
    ->Arg(70)
    ->Arg(90)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_RangeScan_BPlusTree(benchmark::State& state) {
  RunRangeScanWorkload<BPlusTreeEngine>(state, "range_scan_bptree");
}
BENCHMARK(BM_RangeScan_BPlusTree)->Iterations(1)->Unit(benchmark::kMillisecond);

void BM_RangeScan_LSM(benchmark::State& state) {
  RunRangeScanWorkload<LSMTreeEngine>(state, "range_scan_lsm");
}
BENCHMARK(BM_RangeScan_LSM)->Iterations(1)->Unit(benchmark::kMillisecond);

}  // namespace
