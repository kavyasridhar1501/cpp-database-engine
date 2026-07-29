#pragma once

#include <string>
#include <variant>
#include <vector>

#include "sql/value.h"

namespace dbengine {

struct ColumnDefAst {
  std::string name;
  ColumnType type;
};

// column[0] must be INTEGER: it's the primary key, and the only column the
// underlying StorageEngine can index (KeyType is a fixed int64_t).
struct CreateTableStmt {
  std::string table_name;
  std::vector<ColumnDefAst> columns;
};

struct InsertStmt {
  std::string table_name;
  std::vector<Value> values;  // one per column, in schema order
};

enum class CompareOp { EQ, LT, LE, GT, GE };

struct Comparison {
  std::string column;
  CompareOp op;
  Value literal;
};

// AND-conjunction of comparisons; no OR or sub-expressions. Empty means no WHERE clause.
using Predicate = std::vector<Comparison>;

struct SelectStmt {
  std::string table_name;
  bool select_star = true;
  std::vector<std::string> columns;  // used only if !select_star
  Predicate where;
};

struct DeleteStmt {
  std::string table_name;
  Predicate where;
};

using Statement = std::variant<CreateTableStmt, InsertStmt, SelectStmt, DeleteStmt>;

}  // namespace dbengine
