#include "sql/catalog.h"

#include <gtest/gtest.h>

#include "sql/sql_exception.h"

namespace dbengine {
namespace {

TEST(CatalogTest, CreateAndGetTable) {
  Catalog catalog;
  std::vector<ColumnDefAst> cols = {{"id", ColumnType::INTEGER}, {"name", ColumnType::TEXT}};
  const TableSchema& schema = catalog.CreateTable("t", cols);
  EXPECT_EQ(schema.name, "t");
  EXPECT_EQ(schema.table_id, 0);

  EXPECT_TRUE(catalog.HasTable("t"));
  EXPECT_FALSE(catalog.HasTable("nope"));
  EXPECT_EQ(&catalog.GetTable("t"), &schema);
}

TEST(CatalogTest, DistinctTablesGetDistinctIds) {
  Catalog catalog;
  const TableSchema& a = catalog.CreateTable("a", {{"id", ColumnType::INTEGER}});
  const TableSchema& b = catalog.CreateTable("b", {{"id", ColumnType::INTEGER}});
  EXPECT_NE(a.table_id, b.table_id);
}

TEST(CatalogTest, ThrowsOnDuplicateTable) {
  Catalog catalog;
  catalog.CreateTable("t", {{"id", ColumnType::INTEGER}});
  EXPECT_THROW(catalog.CreateTable("t", {{"id", ColumnType::INTEGER}}), SqlException);
}

TEST(CatalogTest, ThrowsOnUnknownTable) {
  Catalog catalog;
  EXPECT_THROW(catalog.GetTable("nope"), SqlException);
}

TEST(CatalogTest, ColumnIndexFindsAndMisses) {
  TableSchema schema;
  schema.columns = {{"id", ColumnType::INTEGER}, {"name", ColumnType::TEXT}};
  EXPECT_EQ(schema.ColumnIndex("id"), 0);
  EXPECT_EQ(schema.ColumnIndex("name"), 1);
  EXPECT_EQ(schema.ColumnIndex("bogus"), -1);
}

TEST(CatalogTest, EncodeDecodeRowRoundTrips) {
  TableSchema schema;
  schema.name = "t";
  schema.table_id = 0;
  schema.columns = {
      {"id", ColumnType::INTEGER},
      {"name", ColumnType::TEXT},
      {"age", ColumnType::INTEGER},
  };
  std::vector<Value> values = {Value(int64_t{7}), Value(std::string("alice")), Value(int64_t{30})};

  std::string payload = EncodeRow(schema, values);
  std::vector<Value> decoded = DecodeRow(schema, 7, payload);

  ASSERT_EQ(decoded.size(), 3u);
  EXPECT_EQ(std::get<int64_t>(decoded[0]), 7);
  EXPECT_EQ(std::get<std::string>(decoded[1]), "alice");
  EXPECT_EQ(std::get<int64_t>(decoded[2]), 30);
}

TEST(CatalogTest, EncodeRowRoundTripsEmptyString) {
  TableSchema schema;
  schema.columns = {{"id", ColumnType::INTEGER}, {"name", ColumnType::TEXT}};
  std::vector<Value> values = {Value(int64_t{1}), Value(std::string(""))};
  std::string payload = EncodeRow(schema, values);
  std::vector<Value> decoded = DecodeRow(schema, 1, payload);
  EXPECT_EQ(std::get<std::string>(decoded[1]), "");
}

TEST(CatalogTest, ThrowsOnArityMismatch) {
  TableSchema schema;
  schema.columns = {{"id", ColumnType::INTEGER}, {"name", ColumnType::TEXT}};
  std::vector<Value> values = {Value(int64_t{1})};  // missing "name"
  EXPECT_THROW(EncodeRow(schema, values), SqlException);
}

TEST(CatalogTest, ThrowsOnColumnTypeMismatch) {
  TableSchema schema;
  schema.name = "t";
  schema.columns = {{"id", ColumnType::INTEGER}, {"age", ColumnType::INTEGER}};
  std::vector<Value> values = {Value(int64_t{1}), Value(std::string("not an int"))};
  EXPECT_THROW(EncodeRow(schema, values), SqlException);
}

TEST(CatalogTest, ThrowsWhenRowExceedsMaxValueSize) {
  TableSchema schema;
  schema.name = "t";
  schema.columns = {{"id", ColumnType::INTEGER}, {"big", ColumnType::TEXT}};
  std::vector<Value> values = {Value(int64_t{1}), Value(std::string(MAX_VALUE_SIZE, 'x'))};
  EXPECT_THROW(EncodeRow(schema, values), SqlException);
}

TEST(CatalogTest, KeyEncodingRoundTrips) {
  KeyType key = EncodeKey(42, 12345);
  EXPECT_EQ(DecodeTableId(key), 42);
  EXPECT_EQ(DecodeRowKey(key), 12345);
}

TEST(CatalogTest, DifferentTableIdsProduceDisjointKeyRanges) {
  KeyType a_last = EncodeKey(5, kMaxRowKey);
  KeyType b_first = EncodeKey(6, 0);
  EXPECT_LT(a_last, b_first);
}

}  // namespace
}  // namespace dbengine
