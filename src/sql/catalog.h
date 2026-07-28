#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "sql/ast.h"
#include "sql/value.h"

namespace dbengine {

struct ColumnDef {
  std::string name;
  ColumnType type;
};

// A table's schema plus the table_id its rows are namespaced under in the
// single shared StorageEngine keyspace (see the key-encoding functions
// below). columns[0] is always the INTEGER primary key.
struct TableSchema {
  std::string name;
  uint16_t table_id;
  std::vector<ColumnDef> columns;

  int ColumnIndex(const std::string& col_name) const;
};

// This project's SQL layer multiplexes every table over *one* underlying
// StorageEngine instance instead of giving each table its own file/tree —
// the top kTableIdBits of KeyType hold a table id, the rest hold the row's
// primary key. This is a deliberate simplification (a real engine gives
// each table, and each index, its own B-tree/LSM-tree) that keeps this
// phase's scope to the SQL front-end and planner rather than per-table
// file/lifecycle management — see DESIGN.md. It also means a table can
// hold at most 2^(64-kTableIdBits) rows and a database at most
// 2^kTableIdBits tables, both generous for this project's purposes.
inline constexpr int kTableIdBits = 16;
inline constexpr int kRowKeyBits = 64 - kTableIdBits;
inline constexpr int64_t kMaxRowKey = (int64_t(1) << kRowKeyBits) - 1;
inline constexpr int kMaxTables = 1 << kTableIdBits;

inline KeyType EncodeKey(uint16_t table_id, int64_t pk) {
  return (static_cast<int64_t>(table_id) << kRowKeyBits) | (pk & kMaxRowKey);
}
inline int64_t DecodeRowKey(KeyType key) { return key & kMaxRowKey; }
inline uint16_t DecodeTableId(KeyType key) {
  return static_cast<uint16_t>((static_cast<uint64_t>(key) >> kRowKeyBits) & (kMaxTables - 1));
}

// Encodes every non-primary-key column of `values` (values[0], the primary
// key, is never stored in the payload — it's already the StorageEngine
// key) into a compact byte string: INTEGER columns as 8 raw bytes,
// TEXT columns as a 2-byte length prefix followed by the bytes themselves.
// Throws SqlException if `values` doesn't match `schema` in arity or
// per-column type, or if the encoded row would exceed MAX_VALUE_SIZE.
std::string EncodeRow(const TableSchema& schema, const std::vector<Value>& values);

// Inverse of EncodeRow: reconstructs the full row (including the primary
// key, passed in separately since it's not part of the payload) as
// `values`, one per column in schema order.
std::vector<Value> DecodeRow(const TableSchema& schema, int64_t primary_key, const std::string& payload);

// Table schemas known to this Database instance. Deliberately in-memory
// only and not persisted — see DESIGN.md's Phase 6 section for why, and
// what re-opening an existing table's data requires as a result.
class Catalog {
 public:
  const TableSchema& CreateTable(const std::string& name, const std::vector<ColumnDefAst>& columns);
  const TableSchema& GetTable(const std::string& name) const;
  bool HasTable(const std::string& name) const;

 private:
  std::unordered_map<std::string, TableSchema> tables_;
  uint16_t next_table_id_ = 0;
};

}  // namespace dbengine
