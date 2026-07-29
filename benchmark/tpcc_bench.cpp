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

// Deliberately small relative to a real TPC-C run, enough to exercise a
// multi-table OLTP access pattern without minutes of setup time.
constexpr int64_t kNumWarehouses = 4;
constexpr int64_t kCustomersPerWarehouse = 1000;
constexpr int64_t kItemsPerWarehouse = 10000;
constexpr int64_t kOrderLinesPerOrder = 10;

std::string TempDbPath(const std::string& label) {
  return (std::filesystem::temp_directory_path() / ("dbengine_tpcc_bench_" + label + ".sql.db")).string();
}

// TPC-C's schema uses composite keys (warehouse_id, local_id); this SQL
// layer only indexes a single INTEGER column, so both are flattened into
// one primary key by arithmetic.
int64_t CustomerId(int64_t w_id, int64_t local_id) { return w_id * kCustomersPerWarehouse + local_id; }
int64_t StockId(int64_t w_id, int64_t item_id) { return w_id * kItemsPerWarehouse + item_id; }

Database& GetOrBuildDatabase(EngineType engine_type) {
  static std::unordered_map<int, std::unique_ptr<Database>> cache;
  int key = static_cast<int>(engine_type);
  auto it = cache.find(key);
  if (it != cache.end()) return *it->second;

  std::string label = engine_type == EngineType::BTREE ? "btree" : "lsm";
  std::string path = TempDbPath(label);
  std::filesystem::remove(path);
  auto db = std::make_unique<Database>(path, engine_type);

  db->Execute("CREATE TABLE warehouse (w_id INTEGER, w_name TEXT)");
  db->Execute("CREATE TABLE customer (c_id INTEGER, c_balance INTEGER)");
  db->Execute("CREATE TABLE stock (s_id INTEGER, s_quantity INTEGER)");
  db->Execute("CREATE TABLE orders (o_id INTEGER, o_c_id INTEGER, o_w_id INTEGER)");
  db->Execute(
      "CREATE TABLE order_line (ol_id INTEGER, ol_o_id INTEGER, ol_item_id INTEGER, ol_quantity INTEGER)");

  for (int64_t w = 0; w < kNumWarehouses; ++w) {
    db->Execute("INSERT INTO warehouse VALUES (" + std::to_string(w) + ", 'wh" + std::to_string(w) + "')");
    for (int64_t c = 0; c < kCustomersPerWarehouse; ++c) {
      db->Execute("INSERT INTO customer VALUES (" + std::to_string(CustomerId(w, c)) + ", 100000)");
    }
    for (int64_t s = 0; s < kItemsPerWarehouse; ++s) {
      db->Execute("INSERT INTO stock VALUES (" + std::to_string(StockId(w, s)) + ", 5000)");
    }
  }

  Database& ref = *db;
  cache[key] = std::move(db);
  return ref;
}

// Simplified TPC-C-style "New-Order" transaction: look up the customer,
// then for each order line look up/decrement stock and insert an
// order_line row, finishing with one orders insert. Not wrapped in an
// actual transaction — the SQL layer doesn't expose Begin/Commit — so this
// measures raw statement throughput, not transactional/atomicity cost.
void RunNewOrderTransaction(Database& db, std::mt19937& rng, int64_t order_id) {
  std::uniform_int_distribution<int64_t> w_dist(0, kNumWarehouses - 1);
  std::uniform_int_distribution<int64_t> c_dist(0, kCustomersPerWarehouse - 1);
  std::uniform_int_distribution<int64_t> item_dist(0, kItemsPerWarehouse - 1);
  std::uniform_int_distribution<int64_t> qty_dist(1, 10);

  int64_t w_id = w_dist(rng);
  int64_t c_id = CustomerId(w_id, c_dist(rng));
  db.Execute("SELECT c_balance FROM customer WHERE c_id = " + std::to_string(c_id));

  for (int64_t line = 0; line < kOrderLinesPerOrder; ++line) {
    int64_t item_id = item_dist(rng);
    int64_t s_id = StockId(w_id, item_id);

    auto stock_row = db.Execute("SELECT s_quantity FROM stock WHERE s_id = " + std::to_string(s_id));
    int64_t qty = stock_row.rows.empty() ? 5000 : std::get<int64_t>(stock_row.rows[0][0]);
    db.Execute("INSERT INTO stock VALUES (" + std::to_string(s_id) + ", " + std::to_string(qty - qty_dist(rng)) +
               ")");

    int64_t ol_id = order_id * (kOrderLinesPerOrder * 2) + line;
    db.Execute("INSERT INTO order_line VALUES (" + std::to_string(ol_id) + ", " + std::to_string(order_id) +
               ", " + std::to_string(item_id) + ", " + std::to_string(qty_dist(rng)) + ")");
  }

  db.Execute("INSERT INTO orders VALUES (" + std::to_string(order_id) + ", " + std::to_string(c_id) + ", " +
             std::to_string(w_id) + ")");
}

void BM_TPCC_NewOrder_BTree(benchmark::State& state) {
  Database& db = GetOrBuildDatabase(EngineType::BTREE);
  std::mt19937 rng(1);
  static int64_t next_order_id = 0;
  for (auto _ : state) {
    RunNewOrderTransaction(db, rng, next_order_id++);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TPCC_NewOrder_BTree)->Unit(benchmark::kMicrosecond);

void BM_TPCC_NewOrder_LSM(benchmark::State& state) {
  Database& db = GetOrBuildDatabase(EngineType::LSM);
  std::mt19937 rng(1);
  static int64_t next_order_id = 0;
  for (auto _ : state) {
    RunNewOrderTransaction(db, rng, next_order_id++);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TPCC_NewOrder_LSM)->Unit(benchmark::kMicrosecond);

}  // namespace
