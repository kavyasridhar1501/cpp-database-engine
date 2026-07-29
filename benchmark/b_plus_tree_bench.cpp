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

// Deliberately smaller than either dataset so lookups mostly miss the buffer
// pool and hit real page I/O.
constexpr size_t kBufferPoolFrames = 2000;

std::string TempDbPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_bpt_bench_" + label + ".db")).string();
}

std::string ValueFor(KeyType key) { return "v" + std::to_string(key); }

// Random insertion order: the harder case for a B+-tree (splits occur
// throughout the key space, not just at the rightmost leaf).
std::vector<KeyType> BuildShuffledKeys(int64_t n, unsigned seed) {
  std::vector<KeyType> keys(static_cast<size_t>(n));
  std::iota(keys.begin(), keys.end(), 0);
  std::mt19937 rng(seed);
  std::shuffle(keys.begin(), keys.end(), rng);
  return keys;
}

// Builds the tree once per key count and caches it so BM_PointLookup and
// BM_RangeScan don't pay construction cost per iteration.
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

// Insert throughput for populating a fresh index (not steady-state upsert cost).
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
