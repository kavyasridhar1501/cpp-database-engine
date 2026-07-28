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

// A single large fact table standing in for TPC-H's `lineitem` — this
// project's SQL layer has no joins or aggregates (see DESIGN.md's Phase 6
// section), so a literal multi-table TPC-H query is out of reach. What IS
// measurable, and is arguably TPC-H's defining characteristic before its
// GROUP BY, is a selective filter over a large table with no usable index:
// Q1 ("pricing summary report... shipped as of a given date") reduces to
// exactly that. `l_shipdate` is deliberately scattered relative to
// `l_orderkey` (the primary key) via a multiplier coprime with the spread,
// so no insertion-order or key-range trick lets the planner do anything
// cheaper than a real FULL_SCAN — the honest worst case this benchmark is
// meant to characterize.
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

// state.range(0) is the target selectivity as a percent (10/50/90) of
// kShipdateSpread, i.e. roughly what fraction of the table's rows match.
// Since l_shipdate isn't the primary key, the planner always falls back to
// FULL_SCAN regardless — the point of this sweep is to show that a
// full-scan query's cost tracks table size, not result size: unlike the
// indexed-vs-unindexed comparison in Phase 6's benchmark (where the
// planner's choice of access path is the variable), here the access path
// is fixed (FULL_SCAN either way) and only the residual filter's
// selectivity changes.
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
