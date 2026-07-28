#include "validation/sqlite_reference.h"

#include <stdexcept>

namespace dbengine {
namespace testing_support {

SqliteReference::SqliteReference() {
  if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
    std::string msg = sqlite3_errmsg(db_);
    sqlite3_close(db_);
    throw std::runtime_error("failed to open in-memory SQLite database: " + msg);
  }
}

SqliteReference::~SqliteReference() { sqlite3_close(db_); }

void SqliteReference::Execute(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown error";
    sqlite3_free(err);
    throw std::runtime_error("SQLite error for '" + sql + "': " + msg);
  }
}

int SqliteReference::Changes() const { return sqlite3_changes(db_); }

SqliteReference::Result SqliteReference::Query(const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("SQLite prepare failed for '" + sql + "': " + sqlite3_errmsg(db_));
  }

  Result result;
  int num_cols = sqlite3_column_count(stmt);
  for (int i = 0; i < num_cols; ++i) {
    result.columns.emplace_back(sqlite3_column_name(stmt, i));
  }

  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    std::vector<Value> row;
    row.reserve(num_cols);
    for (int i = 0; i < num_cols; ++i) {
      if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER) {
        row.emplace_back(static_cast<int64_t>(sqlite3_column_int64(stmt, i)));
      } else {
        const unsigned char* text = sqlite3_column_text(stmt, i);
        row.emplace_back(std::string(text ? reinterpret_cast<const char*>(text) : ""));
      }
    }
    result.rows.push_back(std::move(row));
  }

  if (rc != SQLITE_DONE) {
    std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw std::runtime_error("SQLite step failed for '" + sql + "': " + msg);
  }
  sqlite3_finalize(stmt);
  return result;
}

}  // namespace testing_support
}  // namespace dbengine
