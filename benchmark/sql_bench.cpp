#include <benchmark/benchmark.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

#include "sql/database.h"

namespace {

using dbengine::Database;
using dbengine::EngineType;
using dbengine::QueryResult;

std::string TempDbPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_sql_bench_" + label + ".sql.db")).string();
}

// `id` is the primary key (0..n-1, indexed by construction — every
// StorageEngine key is this column); `tag` holds the identical values but
// isn't indexed by anything, so a predicate on it forces the planner's
// FULL_SCAN path. That makes "WHERE id = X" and "WHERE tag = X" return the
// exact same single row — the only difference this benchmark measures is
// the access path the planner picked to find it.
Database& GetOrBuildDatabase(int64_t n) {
  static std::unordered_map<int64_t, std::unique_ptr<Database>> cache;
  auto it = cache.find(n);
  if (it != cache.end()) return *it->second;

  std::string path = TempDbPath(std::to_string(n));
  std::filesystem::remove(path);
  auto db = std::make_unique<Database>(path, EngineType::BTREE);
  db->Execute("CREATE TABLE t (id INTEGER, tag INTEGER, payload TEXT)");
  for (int64_t i = 0; i < n; ++i) {
    db->Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ", 'row')");
  }

  Database& ref = *db;
  cache[n] = std::move(db);
  return ref;
}

// Indexed access: the planner sees "id = X" (the primary key) and picks
// POINT_LOOKUP — one StorageEngine::Get() regardless of table size.
void BM_PointLookup_IndexedPredicate(benchmark::State& state) {
  int64_t n = state.range(0);
  Database& db = GetOrBuildDatabase(n);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> dist(0, n - 1);

  for (auto _ : state) {
    int64_t key = dist(rng);
    QueryResult result = db.Execute("SELECT * FROM t WHERE id = " + std::to_string(key));
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointLookup_IndexedPredicate)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kMicrosecond);

// Same query shape, same single-row result, but on a column the planner
// has no index for — FULL_SCAN, so cost grows with table size instead of
// staying flat. This is the comparison that makes the "tiny optimizer"
// concrete: it's not just that POINT_LOOKUP is fast, it's that picking the
// wrong access path for the same logical query gets proportionally worse
// as the table grows.
void BM_PointLookup_UnindexedPredicate(benchmark::State& state) {
  int64_t n = state.range(0);
  Database& db = GetOrBuildDatabase(n);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> dist(0, n - 1);

  for (auto _ : state) {
    int64_t key = dist(rng);
    QueryResult result = db.Execute("SELECT * FROM t WHERE tag = " + std::to_string(key));
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointLookup_UnindexedPredicate)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kMicrosecond);

// A bounded range on the primary key (RANGE_SCAN) versus the same
// selectivity forced into a FULL_SCAN by putting the bound on `tag`
// instead — RANGE_SCAN should cost roughly the fixed ~100-row window
// regardless of n, while the unindexed version still pays for the whole
// table.
void BM_RangeScan_IndexedPredicate(benchmark::State& state) {
  int64_t n = state.range(0);
  Database& db = GetOrBuildDatabase(n);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> dist(0, n - 101);

  for (auto _ : state) {
    int64_t lo = dist(rng);
    QueryResult result = db.Execute("SELECT * FROM t WHERE id >= " + std::to_string(lo) + " AND id < " +
                                     std::to_string(lo + 100));
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RangeScan_IndexedPredicate)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kMicrosecond);

void BM_RangeScan_UnindexedPredicate(benchmark::State& state) {
  int64_t n = state.range(0);
  Database& db = GetOrBuildDatabase(n);
  std::mt19937 rng(42);
  std::uniform_int_distribution<int64_t> dist(0, n - 101);

  for (auto _ : state) {
    int64_t lo = dist(rng);
    QueryResult result = db.Execute("SELECT * FROM t WHERE tag >= " + std::to_string(lo) + " AND tag < " +
                                     std::to_string(lo + 100));
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RangeScan_UnindexedPredicate)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
