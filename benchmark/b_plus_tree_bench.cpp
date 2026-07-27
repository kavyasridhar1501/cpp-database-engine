#include <benchmark/benchmark.h>

#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "index/b_plus_tree_engine.h"

namespace {

using dbengine::BPlusTreeEngine;
using dbengine::KeyType;

// Deliberately smaller than either dataset (1M keys is ~76MB of pages, 10M
// is ~760MB) so lookups mostly miss the buffer pool and hit real page I/O —
// the realistic case for an index that doesn't fully fit in memory, and the
// harder case worth measuring.
constexpr size_t kBufferPoolFrames = 2000;

std::string TempDbPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_bpt_bench_" + label + ".db")).string();
}

std::string ValueFor(KeyType key) { return "v" + std::to_string(key); }

// Random (not sequential) insertion order: the harder case for a B+-tree
// (splits occur throughout the key space rather than always at the
// rightmost leaf) and the fairer baseline for the Phase 3 head-to-head
// against an LSM-tree, whose write path is insensitive to key order.
std::vector<KeyType> BuildShuffledKeys(int64_t n, unsigned seed) {
  std::vector<KeyType> keys(static_cast<size_t>(n));
  std::iota(keys.begin(), keys.end(), 0);
  std::mt19937 rng(seed);
  std::shuffle(keys.begin(), keys.end(), rng);
  return keys;
}

// Lazily builds (once per key count, shared across benchmark functions and
// across Google Benchmark's internal calibration reps) a B+-tree with `n`
// keys inserted in random order, so BM_PointLookup and BM_RangeScan pay the
// ~1M/~10M-key construction cost only once each.
BPlusTreeEngine& GetOrBuildEngine(int64_t n) {
  static std::unordered_map<int64_t, std::unique_ptr<BPlusTreeEngine>> cache;
  auto it = cache.find(n);
  if (it != cache.end()) {
    return *it->second;
  }

  std::string path = TempDbPath("readonly_" + std::to_string(n));
  std::filesystem::remove(path);
  auto engine = std::make_unique<BPlusTreeEngine>(path, kBufferPoolFrames);
  for (KeyType k : BuildShuffledKeys(n, /*seed=*/123)) {
    engine->Put(k, ValueFor(k));
  }
  BPlusTreeEngine& ref = *engine;
  cache[n] = std::move(engine);
  return ref;
}

void BM_PointLookup(benchmark::State& state) {
  int64_t n = state.range(0);
  BPlusTreeEngine& engine = GetOrBuildEngine(n);
  std::mt19937 rng(99);
  std::uniform_int_distribution<int64_t> dist(0, n - 1);
  std::string value;

  for (auto _ : state) {
    KeyType key = dist(rng);
    bool found = engine.Get(key, &value);
    benchmark::DoNotOptimize(found);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointLookup)->Arg(1000000)->Arg(10000000)->Unit(benchmark::kMicrosecond);

void BM_RangeScan(benchmark::State& state) {
  int64_t n = state.range(0);
  BPlusTreeEngine& engine = GetOrBuildEngine(n);
  constexpr int kWindow = 1000;  // keys per scan, a typical range-query size.
  std::mt19937 rng(77);
  std::uniform_int_distribution<int64_t> dist(0, n - 1);
  int64_t total_scanned = 0;

  for (auto _ : state) {
    KeyType start = dist(rng);
    auto cursor = engine.Scan(start);
    int c = 0;
    while (cursor->IsValid() && c < kWindow) {
      benchmark::DoNotOptimize(cursor->GetValue());
      ++c;
      cursor->Next();
    }
    total_scanned += c;
  }
  state.SetItemsProcessed(total_scanned);
}
BENCHMARK(BM_RangeScan)->Arg(1000000)->Arg(10000000)->Unit(benchmark::kMicrosecond);

// Single-shot (->Iterations(1)): times building an N-key tree from an empty
// file via random-order inserts, so the reported items_per_second is insert
// throughput for populating a fresh index, not steady-state upsert cost
// into an already-built one.
void BM_InsertThroughput(benchmark::State& state) {
  int64_t n = state.range(0);
  std::string path = TempDbPath("insert_" + std::to_string(n));
  auto keys = BuildShuffledKeys(n, /*seed=*/456);

  for (auto _ : state) {
    std::filesystem::remove(path);
    BPlusTreeEngine engine(path, kBufferPoolFrames);
    for (KeyType k : keys) {
      engine.Put(k, ValueFor(k));
    }
  }
  std::filesystem::remove(path);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_InsertThroughput)
    ->Arg(1000000)
    ->Arg(10000000)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

}  // namespace
