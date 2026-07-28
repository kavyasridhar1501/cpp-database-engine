#pragma once

#include <iosfwd>
#include <memory>
#include <string>

#include "sql/database.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {

// Interactive REPL: raw page access (alloc/write/read/stats, from Phase 0)
// plus a `sql` command (Phase 6) that runs one statement at a time against
// a Database. The SQL layer deliberately uses a *separate* file
// (db_path + ".sql") rather than sharing db_path with the raw page
// commands above — those write directly through DiskManager with no
// notion of the SQL layer's table-multiplexed key space, so sharing a file
// would let the two corrupt each other.
class Shell {
 public:
  Shell(DiskManager& disk_manager, std::string db_path) : disk_manager_(disk_manager), db_path_(std::move(db_path)) {}

  // Runs the REPL against std::cin/std::cout until "exit" or EOF.
  void Run();

  // Executes a single line of input; returns false if it was an "exit".
  // Exposed separately so tests can drive the shell without stdin.
  bool ExecuteLine(const std::string& line, std::ostream& out);

 private:
  // The SQL Database is created lazily, on the first `sql` command, so a
  // session that never touches SQL never pays for opening a second engine.
  Database& GetOrCreateDatabase();

  DiskManager& disk_manager_;
  std::string db_path_;
  std::unique_ptr<Database> database_;
};

}  // namespace dbengine
