#include <benchmark/benchmark.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "common/config.h"
#include "storage/disk/disk_manager.h"

namespace {

using dbengine::DiskManager;
using dbengine::page_id_t;
using dbengine::PAGE_SIZE;

constexpr int kNumPages = 4096;  // ~16MB working set, larger than typical page cache noise.

std::string TempDbPath(const char* label) {
  return (std::filesystem::temp_directory_path() /
          (std::string("dbengine_bench_") + label + ".db"))
      .string();
}

std::vector<page_id_t> SequentialOrder(int n) {
  std::vector<page_id_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  return order;
}

std::vector<page_id_t> RandomOrder(int n) {
  std::vector<page_id_t> order = SequentialOrder(n);
  std::mt19937 rng(7);
  std::shuffle(order.begin(), order.end(), rng);
  return order;
}

// Shared setup: a DiskManager over a fresh file with kNumPages pages
// pre-allocated (and pre-written, so read benchmarks aren't measuring
// first-touch zero-fill).
class PreparedDb {
 public:
  explicit PreparedDb(const char* label) : path_(TempDbPath(label)) {
    std::filesystem::remove(path_);
    dm_ = std::make_unique<DiskManager>(path_);
    std::vector<char> page(PAGE_SIZE, static_cast<char>(0xAB));
    for (int i = 0; i < kNumPages; ++i) {
      page_id_t pid = dm_->AllocatePage();
      dm_->WritePage(pid, page.data());
    }
    dm_->Sync();
  }

  ~PreparedDb() {
    dm_.reset();
    std::filesystem::remove(path_);
  }

  DiskManager& disk_manager() { return *dm_; }

 private:
  std::string path_;
  std::unique_ptr<DiskManager> dm_;
};

void BM_SequentialWrite(benchmark::State& state) {
  PreparedDb db("seq_write");
  auto order = SequentialOrder(kNumPages);
  std::vector<char> page(PAGE_SIZE, 0x42);
  size_t i = 0;
  for (auto _ : state) {
    db.disk_manager().WritePage(order[i % order.size()], page.data());
    ++i;
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * PAGE_SIZE);
}
BENCHMARK(BM_SequentialWrite);

void BM_RandomWrite(benchmark::State& state) {
  PreparedDb db("rand_write");
  auto order = RandomOrder(kNumPages);
  std::vector<char> page(PAGE_SIZE, 0x42);
  size_t i = 0;
  for (auto _ : state) {
    db.disk_manager().WritePage(order[i % order.size()], page.data());
    ++i;
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * PAGE_SIZE);
}
BENCHMARK(BM_RandomWrite);

void BM_SequentialRead(benchmark::State& state) {
  PreparedDb db("seq_read");
  auto order = SequentialOrder(kNumPages);
  std::vector<char> page(PAGE_SIZE, 0);
  size_t i = 0;
  for (auto _ : state) {
    db.disk_manager().ReadPage(order[i % order.size()], page.data());
    ++i;
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * PAGE_SIZE);
}
BENCHMARK(BM_SequentialRead);

void BM_RandomRead(benchmark::State& state) {
  PreparedDb db("rand_read");
  auto order = RandomOrder(kNumPages);
  std::vector<char> page(PAGE_SIZE, 0);
  size_t i = 0;
  for (auto _ : state) {
    db.disk_manager().ReadPage(order[i % order.size()], page.data());
    ++i;
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * PAGE_SIZE);
}
BENCHMARK(BM_RandomRead);

}  // namespace
