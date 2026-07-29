#include <benchmark/benchmark.h>

#include <filesystem>
#include <memory>
#include <string>

#include "sql/database.h"

namespace {

using dbengine::Database;
using dbengine::EngineType;
using dbengine::QueryResult;

constexpr int64_t kNumRows = 300000;
constexpr int64_t kShipdateBase = 20200101;
constexpr int64_t kShipdateSpread = 1000;  // l_shipdate spans [kShipdateBase, kShipdateBase + kShipdateSpread)

std::string TempDbPath() {
  return (std::filesystem::temp_directory_path() / "dbengine_tpch_bench.sql.db").string();
}

// Single large fact table standing in for TPC-H's `lineitem`, approximating
// Q1's selective filter over a large table with no usable index.
// `l_shipdate` is scattered relative to `l_orderkey` (the primary key) via a
// multiplier coprime with the spread, so the planner can't avoid a real
// FULL_SCAN.
Database& GetOrBuildDatabase() {
  static std::unique_ptr<Database> db;
  if (db) return *db;

  std::string path = TempDbPath();
  std::filesystem::remove(path);
  db = std::make_unique<Database>(path, EngineType::BTREE);
  db->Execute(
      "CREATE TABLE lineitem (l_orderkey INTEGER, l_quantity INTEGER, l_extendedprice INTEGER, "
      "l_shipdate INTEGER)");

  for (int64_t i = 0; i < kNumRows; ++i) {
    int64_t shipdate = kShipdateBase + ((i * 7919) % kShipdateSpread);
    int64_t quantity = 1 + (i % 50);
    int64_t extendedprice = 1000 + (i % 100000);
    db->Execute("INSERT INTO lineitem VALUES (" + std::to_string(i) + ", " + std::to_string(quantity) + ", " +
                std::to_string(extendedprice) + ", " + std::to_string(shipdate) + ")");
  }
  return *db;
}

// state.range(0) is the target selectivity as a percent (10/50/90). Access
// path is fixed (FULL_SCAN either way); shows cost tracks table size, not result size.
void BM_TPCH_Q1Style_ShipdateFilter(benchmark::State& state) {
  Database& db = GetOrBuildDatabase();
  int64_t percent = state.range(0);
  int64_t threshold = kShipdateBase + (kShipdateSpread * percent) / 100;

  QueryResult result;
  for (auto _ : state) {
    result = db.Execute("SELECT l_extendedprice FROM lineitem WHERE l_shipdate <= " + std::to_string(threshold));
    benchmark::DoNotOptimize(result);
  }
  state.counters["matched_rows"] = static_cast<double>(result.rows.size());
  state.SetItemsProcessed(state.iterations() * kNumRows);  // rows scanned, not rows returned
}
BENCHMARK(BM_TPCH_Q1Style_ShipdateFilter)->Arg(10)->Arg(50)->Arg(90)->Unit(benchmark::kMillisecond);

}  // namespace
