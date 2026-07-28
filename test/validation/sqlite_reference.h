#pragma once

#include <string>
#include <vector>

#include <sqlite3.h>

#include "sql/value.h"

namespace dbengine {
namespace testing_support {

// A thin wrapper around a real, system-installed SQLite3 (test-only — see
// DESIGN.md's Phase 7 section for why linking it here doesn't violate this
// project's "no external database libraries" rule, which is about what
// ships in the engine). Used purely as a ground-truth oracle: this
// project's tiny SQL grammar (see src/sql/parser.h) is a strict subset of
// real SQL, so the *same* statement text can be run against both `Database`
// and a real SQLite and the results compared directly, no translation
// layer needed.
class SqliteReference {
 public:
  SqliteReference();
  ~SqliteReference();

  SqliteReference(const SqliteReference&) = delete;
  SqliteReference& operator=(const SqliteReference&) = delete;

  // Runs a statement with no result set (CREATE TABLE / INSERT / DELETE).
  // Throws std::runtime_error on failure.
  void Execute(const std::string& sql);

  // Rows affected by the most recently executed INSERT/DELETE.
  int Changes() const;

  struct Result {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
  };

  // Runs a SELECT and collects every result row. Throws std::runtime_error
  // on failure.
  Result Query(const std::string& sql);

 private:
  sqlite3* db_ = nullptr;
};

}  // namespace testing_support
}  // namespace dbengine
