#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/storage_engine.h"
#include "sql/ast.h"
#include "sql/catalog.h"
#include "sql/value.h"

namespace dbengine {

enum class EngineType { BTREE, LSM };

struct QueryResult {
  std::vector<std::string> column_names;    // SELECT only
  std::vector<std::vector<Value>> rows;     // SELECT only
  size_t rows_affected = 0;                 // INSERT/DELETE
  std::string message;                      // human-readable summary, e.g. "SELECT 3", "INSERT 1"
};

// The SQL front-end's entry point: parses a statement, plans it, executes
// it against a single StorageEngine shared by every table in this
// database (see catalog.h for the table-id key-prefixing scheme that makes
// that possible), and returns a QueryResult. One statement per call —
// there is no multi-statement batching.
//
// The catalog is in-memory only and does not persist across a Database
// being destroyed and reconstructed on the same file — see DESIGN.md.
class Database {
 public:
  explicit Database(const std::string& db_path, EngineType engine_type = EngineType::BTREE);

  QueryResult Execute(const std::string& sql);

 private:
  QueryResult ExecuteCreateTable(const CreateTableStmt& stmt);
  QueryResult ExecuteInsert(const InsertStmt& stmt);
  QueryResult ExecuteSelect(const SelectStmt& stmt);
  QueryResult ExecuteDeleteStmt(const DeleteStmt& stmt);

  Catalog catalog_;
  std::unique_ptr<StorageEngine> engine_;
};

}  // namespace dbengine
