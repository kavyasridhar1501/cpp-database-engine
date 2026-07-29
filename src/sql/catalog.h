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
// shared StorageEngine keyspace. columns[0] is always the INTEGER primary key.
struct TableSchema {
  std::string name;
  uint16_t table_id;
  std::vector<ColumnDef> columns;

  int ColumnIndex(const std::string& col_name) const;
};

// Every table is multiplexed over one underlying StorageEngine instance:
// the top kTableIdBits of KeyType hold the table id, the rest hold the row's
// primary key. Caps a table at 2^(64-kTableIdBits) rows and a database at
// 2^kTableIdBits tables.
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

// Encodes non-primary-key columns of `values` (values[0] is never stored in
// the payload — it's already the StorageEngine key) as INTEGER = 8 raw
// bytes, TEXT = 2-byte length prefix + bytes. Throws SqlException on
// arity/type mismatch or if the row would exceed MAX_VALUE_SIZE.
std::string EncodeRow(const TableSchema& schema, const std::vector<Value>& values);

// Inverse of EncodeRow; primary_key is passed separately since it's not part of the payload.
std::vector<Value> DecodeRow(const TableSchema& schema, int64_t primary_key, const std::string& payload);

// Table schemas known to this Database instance. In-memory only, not persisted.
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
