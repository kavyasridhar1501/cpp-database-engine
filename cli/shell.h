#pragma once

#include <iosfwd>
#include <string>

#include "storage/disk/disk_manager.h"

namespace dbengine {

// Minimal interactive REPL over a DiskManager. Phase 0 only exercises raw
// page read/write; later phases will extend this into a SQL shell.
class Shell {
 public:
  explicit Shell(DiskManager& disk_manager) : disk_manager_(disk_manager) {}

  // Runs the REPL against std::cin/std::cout until "exit" or EOF.
  void Run();

  // Executes a single line of input; returns false if it was an "exit".
  // Exposed separately so tests can drive the shell without stdin.
  bool ExecuteLine(const std::string& line, std::ostream& out);

 private:
  DiskManager& disk_manager_;
};

}  // namespace dbengine
