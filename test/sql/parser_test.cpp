#include "sql/parser.h"

#include <gtest/gtest.h>

#include "sql/sql_exception.h"

namespace dbengine {
namespace {

TEST(ParserTest, ParsesCreateTable) {
  Statement stmt = Parser::Parse("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER)");
  auto* create = std::get_if<CreateTableStmt>(&stmt);
  ASSERT_NE(create, nullptr);
  EXPECT_EQ(create->table_name, "users");
  ASSERT_EQ(create->columns.size(), 3u);
  EXPECT_EQ(create->columns[0].name, "id");
  EXPECT_EQ(create->columns[0].type, ColumnType::INTEGER);
  EXPECT_EQ(create->columns[1].name, "name");
  EXPECT_EQ(create->columns[1].type, ColumnType::TEXT);
}

TEST(ParserTest, RejectsCreateTableWithNonIntegerFirstColumn) {
  EXPECT_THROW(Parser::Parse("CREATE TABLE t (name TEXT)"), SqlException);
}

TEST(ParserTest, ParsesInsert) {
  Statement stmt = Parser::Parse("INSERT INTO users VALUES (1, 'alice', 30)");
  auto* insert = std::get_if<InsertStmt>(&stmt);
  ASSERT_NE(insert, nullptr);
  EXPECT_EQ(insert->table_name, "users");
  ASSERT_EQ(insert->values.size(), 3u);
  EXPECT_EQ(std::get<int64_t>(insert->values[0]), 1);
  EXPECT_EQ(std::get<std::string>(insert->values[1]), "alice");
  EXPECT_EQ(std::get<int64_t>(insert->values[2]), 30);
}

TEST(ParserTest, ParsesSelectStar) {
  Statement stmt = Parser::Parse("SELECT * FROM users");
  auto* select = std::get_if<SelectStmt>(&stmt);
  ASSERT_NE(select, nullptr);
  EXPECT_TRUE(select->select_star);
  EXPECT_EQ(select->table_name, "users");
  EXPECT_TRUE(select->where.empty());
}

TEST(ParserTest, ParsesSelectWithColumnListAndWhere) {
  Statement stmt = Parser::Parse("SELECT id, name FROM users WHERE age >= 18 AND id < 100");
  auto* select = std::get_if<SelectStmt>(&stmt);
  ASSERT_NE(select, nullptr);
  EXPECT_FALSE(select->select_star);
  EXPECT_EQ(select->columns, (std::vector<std::string>{"id", "name"}));
  ASSERT_EQ(select->where.size(), 2u);
  EXPECT_EQ(select->where[0].column, "age");
  EXPECT_EQ(select->where[0].op, CompareOp::GE);
  EXPECT_EQ(std::get<int64_t>(select->where[0].literal), 18);
  EXPECT_EQ(select->where[1].column, "id");
  EXPECT_EQ(select->where[1].op, CompareOp::LT);
}

TEST(ParserTest, ParsesDeleteWithWhere) {
  Statement stmt = Parser::Parse("DELETE FROM users WHERE id = 5");
  auto* del = std::get_if<DeleteStmt>(&stmt);
  ASSERT_NE(del, nullptr);
  EXPECT_EQ(del->table_name, "users");
  ASSERT_EQ(del->where.size(), 1u);
  EXPECT_EQ(del->where[0].op, CompareOp::EQ);
}

TEST(ParserTest, TrailingSemicolonIsOptional) {
  EXPECT_NO_THROW(Parser::Parse("SELECT * FROM users;"));
  EXPECT_NO_THROW(Parser::Parse("SELECT * FROM users"));
}

TEST(ParserTest, ThrowsOnUnknownStatement) {
  EXPECT_THROW(Parser::Parse("UPDATE users SET x = 1"), SqlException);
}

TEST(ParserTest, ThrowsOnTrailingGarbage) {
  EXPECT_THROW(Parser::Parse("SELECT * FROM users WHERE id = 1 garbage"), SqlException);
}

TEST(ParserTest, ThrowsOnMissingFrom) {
  EXPECT_THROW(Parser::Parse("SELECT * users"), SqlException);
}

}  // namespace
}  // namespace dbengine
