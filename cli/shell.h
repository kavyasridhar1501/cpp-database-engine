#pragma once

#include <iosfwd>
#include <memory>
#include <string>

#include "sql/database.h"
#include "storage/disk/disk_manager.h"

namespace dbengine {

// SQL layer uses a separate file (db_path + ".sql"): raw page commands
// write through DiskManager with no notion of the SQL table-key space,
// so sharing a file would let the two corrupt each other.
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
