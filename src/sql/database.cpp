#include "sql/database.h"

#include <variant>

#include "index/b_plus_tree_engine.h"
#include "lsm/lsm_tree_engine.h"
#include "sql/executor.h"
#include "sql/parser.h"
#include "sql/planner.h"
#include "sql/sql_exception.h"

namespace dbengine {

namespace {

constexpr size_t kDefaultBufferPoolFrames = 256;

std::unique_ptr<StorageEngine> MakeEngine(const std::string& db_path, EngineType engine_type) {
  if (engine_type == EngineType::BTREE) {
    return std::make_unique<BPlusTreeEngine>(db_path, kDefaultBufferPoolFrames);
  }
  return std::make_unique<LSMTreeEngine>(db_path);
}

}  // namespace

Database::Database(const std::string& db_path, EngineType engine_type)
    : engine_(MakeEngine(db_path, engine_type)) {}

QueryResult Database::Execute(const std::string& sql) {
  Statement stmt = Parser::Parse(sql);
  if (auto* s = std::get_if<CreateTableStmt>(&stmt)) return ExecuteCreateTable(*s);
  if (auto* s = std::get_if<InsertStmt>(&stmt)) return ExecuteInsert(*s);
  if (auto* s = std::get_if<SelectStmt>(&stmt)) return ExecuteSelect(*s);
  return ExecuteDeleteStmt(std::get<DeleteStmt>(stmt));
}

QueryResult Database::ExecuteCreateTable(const CreateTableStmt& stmt) {
  catalog_.CreateTable(stmt.table_name, stmt.columns);
  QueryResult result;
  result.message = "CREATE TABLE";
  return result;
}

QueryResult Database::ExecuteInsert(const InsertStmt& stmt) {
  const TableSchema& schema = catalog_.GetTable(stmt.table_name);

  if (stmt.values.empty() || !std::holds_alternative<int64_t>(stmt.values[0])) {
    throw SqlException("the primary key value must be an INTEGER");
  }
  int64_t pk = std::get<int64_t>(stmt.values[0]);
  if (pk < 0 || pk > kMaxRowKey) {
    throw SqlException("primary key " + std::to_string(pk) + " is out of range [0, " +
                        std::to_string(kMaxRowKey) + "]");
  }

  std::string payload = EncodeRow(schema, stmt.values);
  engine_->Put(EncodeKey(schema.table_id, pk), payload);

  QueryResult result;
  result.rows_affected = 1;
  result.message = "INSERT 1";
  return result;
}

QueryResult Database::ExecuteSelect(const SelectStmt& stmt) {
  const TableSchema& schema = catalog_.GetTable(stmt.table_name);
  Plan plan = PlanQuery(schema, stmt.where);
  std::vector<std::vector<Value>> rows = Executor::ExecuteFetch(schema, plan, engine_.get());

  std::vector<int> projection;
  QueryResult result;
  if (stmt.select_star) {
    for (const ColumnDef& col : schema.columns) {
      result.column_names.push_back(col.name);
      projection.push_back(static_cast<int>(projection.size()));
    }
  } else {
    for (const std::string& col_name : stmt.columns) {
      int idx = schema.ColumnIndex(col_name);
      if (idx < 0) throw SqlException("no such column: '" + col_name + "' on table '" + schema.name + "'");
      result.column_names.push_back(col_name);
      projection.push_back(idx);
    }
  }

  result.rows.reserve(rows.size());
  for (const std::vector<Value>& row : rows) {
    std::vector<Value> projected;
    projected.reserve(projection.size());
    for (int idx : projection) projected.push_back(row[idx]);
    result.rows.push_back(std::move(projected));
  }
  result.rows_affected = result.rows.size();
  result.message = "SELECT " + std::to_string(result.rows.size());
  return result;
}

QueryResult Database::ExecuteDeleteStmt(const DeleteStmt& stmt) {
  const TableSchema& schema = catalog_.GetTable(stmt.table_name);
  Plan plan = PlanQuery(schema, stmt.where);
  size_t deleted = Executor::ExecuteDelete(schema, plan, engine_.get());

  QueryResult result;
  result.rows_affected = deleted;
  result.message = "DELETE " + std::to_string(deleted);
  return result;
}

}  // namespace dbengine
