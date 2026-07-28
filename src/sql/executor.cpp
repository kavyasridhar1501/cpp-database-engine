#include "sql/executor.h"

#include <limits>

namespace dbengine {

namespace {

int CompareOpMatches(CompareOp op, int cmp) {
  switch (op) {
    case CompareOp::EQ:
      return cmp == 0;
    case CompareOp::LT:
      return cmp < 0;
    case CompareOp::LE:
      return cmp <= 0;
    case CompareOp::GT:
      return cmp > 0;
    case CompareOp::GE:
      return cmp >= 0;
  }
  return false;
}

}  // namespace

bool Executor::MatchesPredicate(const TableSchema& schema, const std::vector<Value>& row,
                                 const Predicate& predicate) {
  for (const Comparison& cmp : predicate) {
    int idx = schema.ColumnIndex(cmp.column);
    if (idx < 0 || !CompareOpMatches(cmp.op, CompareValues(row[idx], cmp.literal))) return false;
  }
  return true;
}

std::vector<std::vector<Value>> Executor::ExecuteFetch(const TableSchema& schema, const Plan& plan,
                                                         StorageEngine* engine) {
  std::vector<std::vector<Value>> results;

  if (plan.access_path == AccessPathType::POINT_LOOKUP) {
    std::string payload;
    if (engine->Get(EncodeKey(schema.table_id, plan.point_key), &payload)) {
      std::vector<Value> row = DecodeRow(schema, plan.point_key, payload);
      if (MatchesPredicate(schema, row, plan.residual)) results.push_back(std::move(row));
    }
    return results;
  }

  int64_t start = plan.access_path == AccessPathType::RANGE_SCAN ? plan.range_lo : 0;
  int64_t hi = plan.access_path == AccessPathType::RANGE_SCAN ? plan.range_hi
                                                                : std::numeric_limits<int64_t>::max();

  auto it = engine->Scan(EncodeKey(schema.table_id, start));
  while (it->IsValid()) {
    KeyType key = it->GetKey();
    if (DecodeTableId(key) != schema.table_id) break;  // ran into the next table's key range
    int64_t row_key = DecodeRowKey(key);
    if (row_key > hi) break;

    std::vector<Value> row = DecodeRow(schema, row_key, it->GetValue());
    if (MatchesPredicate(schema, row, plan.residual)) results.push_back(std::move(row));
    it->Next();
  }
  return results;
}

size_t Executor::ExecuteDelete(const TableSchema& schema, const Plan& plan, StorageEngine* engine) {
  if (plan.access_path == AccessPathType::POINT_LOOKUP) {
    std::string payload;
    if (!engine->Get(EncodeKey(schema.table_id, plan.point_key), &payload)) return 0;
    std::vector<Value> row = DecodeRow(schema, plan.point_key, payload);
    if (!MatchesPredicate(schema, row, plan.residual)) return 0;
    return engine->Delete(EncodeKey(schema.table_id, plan.point_key)) ? 1 : 0;
  }

  // Collect keys to delete first, fully draining the scan, rather than
  // deleting while iterating — StorageIterator makes no promise about
  // surviving a concurrent mutation of the structure it's walking.
  int64_t start = plan.access_path == AccessPathType::RANGE_SCAN ? plan.range_lo : 0;
  int64_t hi = plan.access_path == AccessPathType::RANGE_SCAN ? plan.range_hi
                                                                : std::numeric_limits<int64_t>::max();

  std::vector<KeyType> to_delete;
  auto it = engine->Scan(EncodeKey(schema.table_id, start));
  while (it->IsValid()) {
    KeyType key = it->GetKey();
    if (DecodeTableId(key) != schema.table_id) break;
    int64_t row_key = DecodeRowKey(key);
    if (row_key > hi) break;

    std::vector<Value> row = DecodeRow(schema, row_key, it->GetValue());
    if (MatchesPredicate(schema, row, plan.residual)) to_delete.push_back(key);
    it->Next();
  }

  size_t deleted = 0;
  for (KeyType key : to_delete) {
    if (engine->Delete(key)) ++deleted;
  }
  return deleted;
}

}  // namespace dbengine
