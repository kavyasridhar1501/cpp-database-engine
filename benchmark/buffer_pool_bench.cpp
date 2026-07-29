#include <benchmark/benchmark.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "buffer/lru_k_replacer.h"
#include "buffer/lru_replacer.h"
#include "common/config.h"
#include "storage/disk/disk_manager.h"

namespace {

using dbengine::BufferPoolManager;
using dbengine::DiskManager;
using dbengine::LRUKReplacer;
using dbengine::LRUReplacer;
using dbengine::Page;
using dbengine::page_id_t;
using dbengine::Replacer;

// Generates `length` page accesses over [0, num_pages) following a Zipfian
// distribution: a small number of "hot" low-numbered pages absorb most of the traffic.
std::vector<page_id_t> BuildZipfianTrace(int num_pages, int length, double skew, unsigned seed) {
  std::vector<double> weights(static_cast<size_t>(num_pages));
  for (int i = 0; i < num_pages; ++i) {
    weights[static_cast<size_t>(i)] = 1.0 / std::pow(i + 1, skew);
  }
  std::discrete_distribution<int> dist(weights.begin(), weights.end());
  std::mt19937 rng(seed);

  std::vector<page_id_t> trace(static_cast<size_t>(length));
  for (auto& pid : trace) {
    pid = dist(rng);
  }
  return trace;
}

// Same Zipfian hot-page traffic, with a cold sequential scan injected every
// `scan_interval` accesses — tests resistance to "sequential flooding",
// where plain LRU evicts hot pages because recency can't tell a page
// touched once from one touched constantly.
std::vector<page_id_t> BuildZipfianWithScanTrace(int hot_pages, int cold_pages, int length,
                                                  double skew, int scan_interval, int scan_length,
                                                  unsigned seed) {
  std::vector<double> weights(static_cast<size_t>(hot_pages));
  for (int i = 0; i < hot_pages; ++i) {
    weights[static_cast<size_t>(i)] = 1.0 / std::pow(i + 1, skew);
  }
  std::discrete_distribution<int> dist(weights.begin(), weights.end());
  std::mt19937 rng(seed);

  std::vector<page_id_t> trace;
  trace.reserve(static_cast<size_t>(length) +
                static_cast<size_t>(length / scan_interval + 1) * static_cast<size_t>(scan_length));

  int since_scan = 0;
  int scan_cursor = 0;
  for (int i = 0; i < length; ++i) {
    trace.push_back(dist(rng));
    if (++since_scan >= scan_interval) {
      for (int j = 0; j < scan_length; ++j) {
        trace.push_back(hot_pages + ((scan_cursor + j) % cold_pages));
      }
      scan_cursor += scan_length;
      since_scan = 0;
    }
  }
  return trace;
}

const std::vector<page_id_t>& ZipfianTrace() {
  static const std::vector<page_id_t> trace = BuildZipfianTrace(
      /*num_pages=*/10000, /*length=*/200000, /*skew=*/0.99, /*seed=*/42);
  return trace;
}
constexpr int kZipfianNumPages = 10000;

const std::vector<page_id_t>& ZipfianWithScanTrace() {
  static const std::vector<page_id_t> trace =
      BuildZipfianWithScanTrace(/*hot_pages=*/2000, /*cold_pages=*/2000, /*length=*/100000,
                                /*skew=*/0.99, /*scan_interval=*/2000, /*scan_length=*/300,
                                /*seed=*/42);
  return trace;
}
constexpr int kZipfianWithScanNumPages = 4000;  // hot_pages + cold_pages

std::string TempDbPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_bpm_bench_" + label + ".db")).string();
}

// Replays `trace` through a fresh BufferPoolManager and reports hit rate as
// a custom counter. Uses ->Iterations(1): averaging over multiple replays
// would conflate a cold-start first pass with mostly-hit later ones.
void RunTrace(benchmark::State& state, const std::string& label, const std::vector<page_id_t>& trace,
              int num_pages, const std::function<std::unique_ptr<Replacer>(size_t)>& make_replacer) {
  const auto pool_size = static_cast<size_t>(state.range(0));
  const std::string path = TempDbPath(label + "_" + std::to_string(pool_size));

  size_t hits = 0;
  size_t misses = 0;
  for (auto _ : state) {
    std::filesystem::remove(path);
    DiskManager dm(path);
    for (int i = 0; i < num_pages; ++i) {
      dm.AllocatePage();
    }
    BufferPoolManager bpm(pool_size, &dm, make_replacer(pool_size));

    for (page_id_t pid : trace) {
      Page* page = bpm.FetchPage(pid);
      if (page == nullptr) {
        state.SkipWithError("BufferPoolManager returned nullptr (no evictable frame)");
        break;
      }
      benchmark::DoNotOptimize(page->GetData());
      bpm.UnpinPage(pid, false);
    }
    hits = bpm.GetNumHits();
    misses = bpm.GetNumMisses();
  }
  std::filesystem::remove(path);

  const size_t total = hits + misses;
  const double hit_rate = total > 0 ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
  state.counters["HitRate"] = hit_rate;
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(trace.size()));
}

void BM_Zipfian_LRUK(benchmark::State& state) {
  RunTrace(state, "zipf_lruk", ZipfianTrace(), kZipfianNumPages,
           [](size_t pool_size) { return std::make_unique<LRUKReplacer>(pool_size, 2); });
}
BENCHMARK(BM_Zipfian_LRUK)
    ->RangeMultiplier(2)
    ->Range(8, 2048)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_Zipfian_LRU(benchmark::State& state) {
  RunTrace(state, "zipf_lru", ZipfianTrace(), kZipfianNumPages,
           [](size_t pool_size) { return std::make_unique<LRUReplacer>(pool_size); });
}
BENCHMARK(BM_Zipfian_LRU)
    ->RangeMultiplier(2)
    ->Range(8, 2048)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_ZipfianWithScan_LRUK(benchmark::State& state) {
  RunTrace(state, "zipfscan_lruk", ZipfianWithScanTrace(), kZipfianWithScanNumPages,
           [](size_t pool_size) { return std::make_unique<LRUKReplacer>(pool_size, 2); });
}
BENCHMARK(BM_ZipfianWithScan_LRUK)
    ->RangeMultiplier(2)
    ->Range(8, 1024)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_ZipfianWithScan_LRU(benchmark::State& state) {
  RunTrace(state, "zipfscan_lru", ZipfianWithScanTrace(), kZipfianWithScanNumPages,
           [](size_t pool_size) { return std::make_unique<LRUReplacer>(pool_size); });
}
BENCHMARK(BM_ZipfianWithScan_LRU)
    ->RangeMultiplier(2)
    ->Range(8, 1024)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

}  // namespace
