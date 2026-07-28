#include "sql/planner.h"

#include <gtest/gtest.h>
#include <limits>

#include "sql/sql_exception.h"

namespace dbengine {
namespace {

TableSchema MakeSchema() {
  TableSchema schema;
  schema.name = "t";
  schema.table_id = 0;
  schema.columns = {
      {"id", ColumnType::INTEGER},
      {"name", ColumnType::TEXT},
      {"age", ColumnType::INTEGER},
  };
  return schema;
}

TEST(PlannerTest, NoWhereClauseIsFullScan) {
  Plan plan = PlanQuery(MakeSchema(), {});
  EXPECT_EQ(plan.access_path, AccessPathType::FULL_SCAN);
  EXPECT_TRUE(plan.residual.empty());
}

TEST(PlannerTest, EqualityOnPrimaryKeyIsPointLookup) {
  Predicate where = {Comparison{"id", CompareOp::EQ, Value(int64_t{5})}};
  Plan plan = PlanQuery(MakeSchema(), where);
  EXPECT_EQ(plan.access_path, AccessPathType::POINT_LOOKUP);
  EXPECT_EQ(plan.point_key, 5);
  EXPECT_TRUE(plan.residual.empty());
}

TEST(PlannerTest, PointLookupLeavesOtherPredicatesAsResidual) {
  Predicate where = {
      Comparison{"id", CompareOp::EQ, Value(int64_t{5})},
      Comparison{"age", CompareOp::GT, Value(int64_t{10})},
  };
  Plan plan = PlanQuery(MakeSchema(), where);
  EXPECT_EQ(plan.access_path, AccessPathType::POINT_LOOKUP);
  EXPECT_EQ(plan.point_key, 5);
  ASSERT_EQ(plan.residual.size(), 1u);
  EXPECT_EQ(plan.residual[0].column, "age");
}

TEST(PlannerTest, PredicateOnlyOnNonKeyColumnIsFullScan) {
  Predicate where = {Comparison{"age", CompareOp::EQ, Value(int64_t{10})}};
  Plan plan = PlanQuery(MakeSchema(), where);
  EXPECT_EQ(plan.access_path, AccessPathType::FULL_SCAN);
  ASSERT_EQ(plan.residual.size(), 1u);
}

TEST(PlannerTest, BoundedRangeOnPrimaryKeyIsRangeScan) {
  Predicate where = {
      Comparison{"id", CompareOp::GT, Value(int64_t{5})},
      Comparison{"id", CompareOp::LE, Value(int64_t{20})},
  };
  Plan plan = PlanQuery(MakeSchema(), where);
  EXPECT_EQ(plan.access_path, AccessPathType::RANGE_SCAN);
  EXPECT_EQ(plan.range_lo, 6);
  EXPECT_EQ(plan.range_hi, 20);
}

TEST(PlannerTest, OpenLowerBoundRangeScan) {
  Predicate where = {Comparison{"id", CompareOp::GE, Value(int64_t{5})}};
  Plan plan = PlanQuery(MakeSchema(), where);
  EXPECT_EQ(plan.access_path, AccessPathType::RANGE_SCAN);
  EXPECT_EQ(plan.range_lo, 5);
  EXPECT_EQ(plan.range_hi, std::numeric_limits<int64_t>::max());
}

TEST(PlannerTest, OpenUpperBoundRangeScan) {
  Predicate where = {Comparison{"id", CompareOp::LT, Value(int64_t{5})}};
  Plan plan = PlanQuery(MakeSchema(), where);
  EXPECT_EQ(plan.access_path, AccessPathType::RANGE_SCAN);
  EXPECT_EQ(plan.range_lo, 0);
  EXPECT_EQ(plan.range_hi, 4);
}

TEST(PlannerTest, ThrowsOnUnknownColumn) {
  Predicate where = {Comparison{"bogus", CompareOp::EQ, Value(int64_t{1})}};
  EXPECT_THROW(PlanQuery(MakeSchema(), where), SqlException);
}

TEST(PlannerTest, ThrowsOnTypeMismatch) {
  Predicate where = {Comparison{"id", CompareOp::EQ, Value(std::string("oops"))}};
  EXPECT_THROW(PlanQuery(MakeSchema(), where), SqlException);
}

}  // namespace
}  // namespace dbengine
