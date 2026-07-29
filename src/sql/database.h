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

// SQL front-end entry point: parses, plans, and executes one statement per
// call against a single shared StorageEngine (see catalog.h for the
// table-id key-prefixing scheme). The catalog is in-memory only, so it does
// not persist across a Database being destroyed and reopened on the same file.
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
