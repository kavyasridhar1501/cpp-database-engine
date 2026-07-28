// Differential testing (Phase 7): the same SQL statement text, executed
// against both this project's Database and a real SQLite, must produce the
// same results. This project's tiny grammar (src/sql/parser.h) is a strict
// subset of real SQL — CREATE TABLE/INSERT/SELECT/DELETE with an
// AND-conjunction WHERE all parse and mean the same thing in SQLite — so no
// translation layer is needed, and any mismatch is either a genuine bug or
// one of this project's documented, deliberate scope limitations (see the
// negative-primary-key and MAX_VALUE_SIZE guards below, both of which the
// generators here simply avoid rather than asserting SQLite is "wrong").
//
// One real semantic gap this file *does* bridge: this project's grammar
// has no PRIMARY KEY / uniqueness syntax at all — column[0] is *always*
// treated as the primary key implicitly, and every StorageEngine::Put is
// an upsert (see DESIGN.md's Phase 6 section). Bare `id INTEGER` in
// standard SQL is just a plain column with no special uniqueness — SQLite
// happily stores multiple rows with the same "id" unless told otherwise —
// so CREATE TABLE and INSERT text sent to SQLite is adjusted (first
// column gets "PRIMARY KEY", INSERT becomes "INSERT OR REPLACE") to match
// what this project's grammar means unconditionally. This isn't a
// translation layer for the *language* (still identical syntax otherwise)
// — it's making the two sides agree on what "the same schema" means before
// asking whether they compute the same answer.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>

#include "sql/database.h"
#include "validation/sqlite_reference.h"

namespace dbengine {
namespace {

using testing_support::SqliteReference;

class SqliteDifferentialTest : public ::testing::TestWithParam<EngineType> {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_diff_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    CleanupFiles();
  }

  void TearDown() override { CleanupFiles(); }

  void CleanupFiles() {
    std::filesystem::remove(file_path_);
    std::filesystem::path dir = std::filesystem::path(file_path_).parent_path();
    std::string prefix = std::filesystem::path(file_path_).filename().string();
    if (!std::filesystem::exists(dir)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) {
        std::filesystem::remove(entry.path());
      }
    }
  }

  Database MakeDatabase() { return Database(file_path_, GetParam()); }

  std::string file_path_;
};

// This project's grammar requires column[0] to be INTEGER and always
// treats it as the primary key; plain "INTEGER" in real SQL carries no
// such meaning. Rewrites the *first* INTEGER (always the primary key
// column's type, by this grammar's own rule) to "INTEGER PRIMARY KEY" so
// SQLite enforces the same one-row-per-key invariant this project's
// StorageEngine::Put always has.
std::string SqliteCreateTable(const std::string& our_create) {
  size_t pos = our_create.find("INTEGER");
  return our_create.substr(0, pos) + "INTEGER PRIMARY KEY" +
         our_create.substr(pos + std::string("INTEGER").size());
}

// Put() is an upsert; plain SQLite INSERT is not (it errors on a duplicate
// primary key). "INSERT OR REPLACE" is SQLite's upsert-by-primary-key form
// — the direct match for this project's semantics.
std::string SqliteInsert(const std::string& our_insert) {
  return "INSERT OR REPLACE" + our_insert.substr(std::string("INSERT").size());
}

std::vector<std::vector<std::string>> Canonicalize(const std::vector<std::vector<Value>>& rows) {
  std::vector<std::vector<std::string>> out;
  out.reserve(rows.size());
  for (const auto& row : rows) {
    std::vector<std::string> r;
    r.reserve(row.size());
    for (const auto& v : row) {
      if (std::holds_alternative<int64_t>(v)) {
        r.push_back(std::to_string(std::get<int64_t>(v)));
      } else {
        r.push_back(std::get<std::string>(v));
      }
    }
    out.push_back(std::move(r));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// SELECT results are unordered without an ORDER BY clause (neither engine
// promises one, and this grammar has no ORDER BY at all) — compare as sets.
void ExpectSameRows(const QueryResult& ours, const SqliteReference::Result& theirs) {
  EXPECT_EQ(ours.column_names, theirs.columns);
  EXPECT_EQ(Canonicalize(ours.rows), Canonicalize(theirs.rows));
}

TEST_P(SqliteDifferentialTest, CreateInsertSelectMatchesSqlite) {
  Database db = MakeDatabase();
  SqliteReference sqlite;

  const std::string create = "CREATE TABLE users (id INTEGER, name TEXT, age INTEGER)";
  db.Execute(create);
  sqlite.Execute(SqliteCreateTable(create));

  for (const std::string insert : {
           "INSERT INTO users VALUES (1, 'alice', 30)",
           "INSERT INTO users VALUES (2, 'bob', 25)",
           "INSERT INTO users VALUES (3, 'carol', 40)",
       }) {
    db.Execute(insert);
    sqlite.Execute(SqliteInsert(insert));
  }

  ExpectSameRows(db.Execute("SELECT * FROM users"), sqlite.Query("SELECT * FROM users"));
  ExpectSameRows(db.Execute("SELECT * FROM users WHERE id = 2"), sqlite.Query("SELECT * FROM users WHERE id = 2"));
  ExpectSameRows(db.Execute("SELECT name FROM users WHERE age >= 30"),
                 sqlite.Query("SELECT name FROM users WHERE age >= 30"));
}

TEST_P(SqliteDifferentialTest, RangeScanMatchesSqlite) {
  Database db = MakeDatabase();
  SqliteReference sqlite;

  const std::string create = "CREATE TABLE t (id INTEGER, val INTEGER)";
  db.Execute(create);
  sqlite.Execute(SqliteCreateTable(create));
  for (int64_t i = 0; i < 50; ++i) {
    std::string insert = "INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 3) + ")";
    db.Execute(insert);
    sqlite.Execute(SqliteInsert(insert));
  }

  const std::string query = "SELECT * FROM t WHERE id > 10 AND id <= 25";
  ExpectSameRows(db.Execute(query), sqlite.Query(query));
}

TEST_P(SqliteDifferentialTest, NonIndexedFilterMatchesSqlite) {
  Database db = MakeDatabase();
  SqliteReference sqlite;

  const std::string create = "CREATE TABLE t (id INTEGER, val INTEGER)";
  db.Execute(create);
  sqlite.Execute(SqliteCreateTable(create));
  for (int64_t i = 0; i < 50; ++i) {
    std::string insert = "INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 5) + ")";
    db.Execute(insert);
    sqlite.Execute(SqliteInsert(insert));
  }

  const std::string query = "SELECT id FROM t WHERE val = 3";
  ExpectSameRows(db.Execute(query), sqlite.Query(query));
}

TEST_P(SqliteDifferentialTest, DeleteRowCountMatchesSqlite) {
  Database db = MakeDatabase();
  SqliteReference sqlite;

  const std::string create = "CREATE TABLE t (id INTEGER, val INTEGER)";
  db.Execute(create);
  sqlite.Execute(SqliteCreateTable(create));
  for (int64_t i = 0; i < 30; ++i) {
    std::string insert = "INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")";
    db.Execute(insert);
    sqlite.Execute(SqliteInsert(insert));
  }

  const std::string del = "DELETE FROM t WHERE val >= 20";
  QueryResult our_result = db.Execute(del);
  sqlite.Execute(del);
  EXPECT_EQ(our_result.rows_affected, static_cast<size_t>(sqlite.Changes()));

  ExpectSameRows(db.Execute("SELECT * FROM t"), sqlite.Query("SELECT * FROM t"));
}

// The main fuzzer: a long randomized sequence of INSERT/DELETE/SELECT over
// a small key range, applying the *identical* generated statement text to
// both engines and checking every SELECT along the way. Primary keys are
// generated non-negative (this project's rule, not SQL's) and TEXT payloads
// stay well under MAX_VALUE_SIZE — both documented Phase 6 scope limits,
// not things this test is trying to catch SQLite disagreeing on.
TEST_P(SqliteDifferentialTest, RandomizedInsertDeleteSelectMatchesSqlite) {
  Database db = MakeDatabase();
  SqliteReference sqlite;

  const std::string create = "CREATE TABLE t (id INTEGER, v TEXT)";
  db.Execute(create);
  sqlite.Execute(SqliteCreateTable(create));

  std::mt19937 rng(20260728);
  std::uniform_int_distribution<int64_t> key_dist(0, 199);
  std::uniform_int_distribution<int> op_dist(0, 2);

  for (int iter = 0; iter < 2000; ++iter) {
    int64_t key = key_dist(rng);
    int op = op_dist(rng);

    if (op == 0) {
      std::string sql = "INSERT INTO t VALUES (" + std::to_string(key) + ", 'v" + std::to_string(iter) + "')";
      db.Execute(sql);
      sqlite.Execute(SqliteInsert(sql));
    } else if (op == 1) {
      std::string sql = "DELETE FROM t WHERE id = " + std::to_string(key);
      QueryResult our_result = db.Execute(sql);
      sqlite.Execute(sql);
      EXPECT_EQ(our_result.rows_affected, static_cast<size_t>(sqlite.Changes()));
    } else {
      std::string sql = "SELECT * FROM t WHERE id = " + std::to_string(key);
      ExpectSameRows(db.Execute(sql), sqlite.Query(sql));
    }
  }

  ExpectSameRows(db.Execute("SELECT * FROM t"), sqlite.Query("SELECT * FROM t"));
}

INSTANTIATE_TEST_SUITE_P(BTreeAndLSM, SqliteDifferentialTest,
                          ::testing::Values(EngineType::BTREE, EngineType::LSM),
                          [](const ::testing::TestParamInfo<EngineType>& info) {
                            return info.param == EngineType::BTREE ? "BTree" : "LSM";
                          });

}  // namespace
}  // namespace dbengine
