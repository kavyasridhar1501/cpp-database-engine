#include "sql/database.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <random>
#include <string>

#include "sql/sql_exception.h"

namespace dbengine {
namespace {

class DatabaseTest : public ::testing::TestWithParam<EngineType> {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_sql_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    CleanupFiles();
  }

  void TearDown() override { CleanupFiles(); }

  void CleanupFiles() {
    std::filesystem::remove(file_path_);
    std::filesystem::remove(file_path_ + ".sql");
    std::filesystem::path dir = std::filesystem::path(file_path_).parent_path();
    std::string prefix = std::filesystem::path(file_path_).filename().string();
    if (!std::filesystem::exists(dir)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) {
        std::filesystem::remove(entry.path());
      }
    }
  }

  Database MakeDatabase() { return Database(file_path_ + ".sql", GetParam()); }

  std::string file_path_;
};

TEST_P(DatabaseTest, CreateInsertSelectRoundTrip) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER)");
  db.Execute("INSERT INTO users VALUES (1, 'alice', 30)");
  db.Execute("INSERT INTO users VALUES (2, 'bob', 25)");

  QueryResult result = db.Execute("SELECT * FROM users WHERE id = 1");
  EXPECT_EQ(result.message, "SELECT 1");
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(result.rows[0][0]), 1);
  EXPECT_EQ(std::get<std::string>(result.rows[0][1]), "alice");
  EXPECT_EQ(std::get<int64_t>(result.rows[0][2]), 30);
}

TEST_P(DatabaseTest, SelectStarReturnsAllColumnsInSchemaOrder) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER, name TEXT)");
  db.Execute("INSERT INTO t VALUES (1, 'x')");
  QueryResult result = db.Execute("SELECT * FROM t");
  EXPECT_EQ(result.column_names, (std::vector<std::string>{"id", "name"}));
}

TEST_P(DatabaseTest, SelectProjectsRequestedColumnsOnly) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER, name TEXT, age INTEGER)");
  db.Execute("INSERT INTO t VALUES (1, 'alice', 30)");
  QueryResult result = db.Execute("SELECT name FROM t WHERE id = 1");
  EXPECT_EQ(result.column_names, (std::vector<std::string>{"name"}));
  ASSERT_EQ(result.rows.size(), 1u);
  ASSERT_EQ(result.rows[0].size(), 1u);
  EXPECT_EQ(std::get<std::string>(result.rows[0][0]), "alice");
}

TEST_P(DatabaseTest, UpdateSemanticsViaReinsert) {
  // No UPDATE statement in this tiny grammar (see DESIGN.md); Put()
  // semantics are upsert, so re-INSERTing an existing primary key
  // overwrites the row — this test documents and locks in that behavior.
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER, name TEXT)");
  db.Execute("INSERT INTO t VALUES (1, 'first')");
  db.Execute("INSERT INTO t VALUES (1, 'second')");
  QueryResult result = db.Execute("SELECT * FROM t");
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(result.rows[0][1]), "second");
}

TEST_P(DatabaseTest, DeleteRemovesMatchingRowsOnly) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER, age INTEGER)");
  for (int64_t i = 0; i < 10; ++i) {
    db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
  }
  QueryResult del = db.Execute("DELETE FROM t WHERE age >= 50");
  EXPECT_EQ(del.rows_affected, 5u);  // ages 50,60,70,80,90

  QueryResult remaining = db.Execute("SELECT * FROM t");
  EXPECT_EQ(remaining.rows.size(), 5u);
  for (const auto& row : remaining.rows) {
    EXPECT_LT(std::get<int64_t>(row[1]), 50);
  }
}

TEST_P(DatabaseTest, RangeScanRespectsBounds) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER)");
  for (int64_t i = 0; i < 20; ++i) {
    db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ")");
  }
  QueryResult result = db.Execute("SELECT * FROM t WHERE id > 5 AND id <= 10");
  ASSERT_EQ(result.rows.size(), 5u);  // 6,7,8,9,10
  for (const auto& row : result.rows) {
    int64_t id = std::get<int64_t>(row[0]);
    EXPECT_GT(id, 5);
    EXPECT_LE(id, 10);
  }
}

TEST_P(DatabaseTest, MultipleTablesShareOneEngineWithoutInterference) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE a (id INTEGER, v TEXT)");
  db.Execute("CREATE TABLE b (id INTEGER, v TEXT)");
  db.Execute("INSERT INTO a VALUES (1, 'from_a')");
  db.Execute("INSERT INTO b VALUES (1, 'from_b')");

  QueryResult ra = db.Execute("SELECT * FROM a");
  QueryResult rb = db.Execute("SELECT * FROM b");
  ASSERT_EQ(ra.rows.size(), 1u);
  ASSERT_EQ(rb.rows.size(), 1u);
  EXPECT_EQ(std::get<std::string>(ra.rows[0][1]), "from_a");
  EXPECT_EQ(std::get<std::string>(rb.rows[0][1]), "from_b");
}

TEST_P(DatabaseTest, ThrowsOnUnknownTable) {
  Database db = MakeDatabase();
  EXPECT_THROW(db.Execute("SELECT * FROM nope"), SqlException);
}

TEST_P(DatabaseTest, ThrowsOnRowTooLargeForMaxValueSize) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER, big TEXT)");
  std::string huge(MAX_VALUE_SIZE, 'x');
  EXPECT_THROW(db.Execute("INSERT INTO t VALUES (1, '" + huge + "')"), SqlException);
}

TEST_P(DatabaseTest, ThrowsOnNegativePrimaryKey) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER)");
  EXPECT_THROW(db.Execute("INSERT INTO t VALUES (-1)"), SqlException);
}

// Randomized oracle test: sequential INSERT/DELETE/SELECT-by-id operations
// against the SQL layer, checked against a std::map<int64_t, std::string>
// mirroring what the single TEXT column should hold for each primary key —
// same testing philosophy as every other storage-engine phase in this
// project (see DESIGN.md).
TEST_P(DatabaseTest, RandomizedOracleAgainstStdMap) {
  Database db = MakeDatabase();
  db.Execute("CREATE TABLE t (id INTEGER, v TEXT)");
  std::map<int64_t, std::string> oracle;

  std::mt19937 rng(777);
  std::uniform_int_distribution<int64_t> key_dist(0, 199);
  std::uniform_int_distribution<int> op_dist(0, 2);

  for (int iter = 0; iter < 2000; ++iter) {
    int64_t key = key_dist(rng);
    int op = op_dist(rng);

    if (op == 0) {  // insert/overwrite
      std::string value = "v" + std::to_string(iter);
      db.Execute("INSERT INTO t VALUES (" + std::to_string(key) + ", '" + value + "')");
      oracle[key] = value;
    } else if (op == 1) {  // delete
      QueryResult del = db.Execute("DELETE FROM t WHERE id = " + std::to_string(key));
      EXPECT_EQ(del.rows_affected, oracle.count(key));
      oracle.erase(key);
    } else {  // point select
      QueryResult result = db.Execute("SELECT v FROM t WHERE id = " + std::to_string(key));
      auto it = oracle.find(key);
      if (it == oracle.end()) {
        EXPECT_TRUE(result.rows.empty());
      } else {
        ASSERT_EQ(result.rows.size(), 1u);
        EXPECT_EQ(std::get<std::string>(result.rows[0][0]), it->second);
      }
    }
  }

  QueryResult all = db.Execute("SELECT * FROM t");
  EXPECT_EQ(all.rows.size(), oracle.size());
}

INSTANTIATE_TEST_SUITE_P(BTreeAndLSM, DatabaseTest, ::testing::Values(EngineType::BTREE, EngineType::LSM),
                          [](const ::testing::TestParamInfo<EngineType>& info) {
                            return info.param == EngineType::BTREE ? "BTree" : "LSM";
                          });

}  // namespace
}  // namespace dbengine
