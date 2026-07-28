#include "sql/planner.h"

#include <algorithm>

#include "sql/sql_exception.h"

namespace dbengine {

namespace {

void ValidateComparison(const TableSchema& schema, const Comparison& cmp) {
  int idx = schema.ColumnIndex(cmp.column);
  if (idx < 0) {
    throw SqlException("no such column: '" + cmp.column + "' on table '" + schema.name + "'");
  }
  ColumnType col_type = schema.columns[idx].type;
  bool literal_is_int = std::holds_alternative<int64_t>(cmp.literal);
  if ((col_type == ColumnType::INTEGER) != literal_is_int) {
    throw SqlException("type mismatch comparing column '" + cmp.column + "'");
  }
}

}  // namespace

Plan PlanQuery(const TableSchema& schema, const Predicate& where) {
  for (const Comparison& cmp : where) ValidateComparison(schema, cmp);

  const std::string& pk_name = schema.columns[0].name;

  // Pass 1: an EQ on the primary key beats everything else — a point
  // lookup is the cheapest access path available.
  for (size_t i = 0; i < where.size(); ++i) {
    const Comparison& cmp = where[i];
    if (cmp.column == pk_name && cmp.op == CompareOp::EQ) {
      Plan plan;
      plan.access_path = AccessPathType::POINT_LOOKUP;
      plan.point_key = std::get<int64_t>(cmp.literal);
      for (size_t j = 0; j < where.size(); ++j) {
        if (j != i) plan.residual.push_back(where[j]);
      }
      return plan;
    }
  }

  // Pass 2: intersect every LT/LE/GT/GE bound on the primary key into a
  // single range. Bounds not on the primary key stay in the residual
  // filter; bounds that are on the primary key are still added to the
  // residual too (cheap, and correctness doesn't depend on omitting them).
  int64_t lo = 0;
  int64_t hi = std::numeric_limits<int64_t>::max();
  bool found_bound = false;
  for (const Comparison& cmp : where) {
    if (cmp.column != pk_name) continue;
    int64_t v = std::get<int64_t>(cmp.literal);
    switch (cmp.op) {
      case CompareOp::LT:
        hi = std::min(hi, v == std::numeric_limits<int64_t>::min() ? v : v - 1);
        found_bound = true;
        break;
      case CompareOp::LE:
        hi = std::min(hi, v);
        found_bound = true;
        break;
      case CompareOp::GT:
        lo = std::max(lo, v == std::numeric_limits<int64_t>::max() ? v : v + 1);
        found_bound = true;
        break;
      case CompareOp::GE:
        lo = std::max(lo, v);
        found_bound = true;
        break;
      case CompareOp::EQ:
        break;  // handled in pass 1
    }
  }

  if (found_bound) {
    Plan plan;
    plan.access_path = AccessPathType::RANGE_SCAN;
    plan.range_lo = std::max<int64_t>(lo, 0);
    plan.range_hi = hi;
    plan.residual = where;
    return plan;
  }

  Plan plan;
  plan.access_path = AccessPathType::FULL_SCAN;
  plan.residual = where;
  return plan;
}

}  // namespace dbengine
