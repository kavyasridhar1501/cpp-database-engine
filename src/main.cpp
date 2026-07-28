#include <iostream>

#include "cli/shell.h"
#include "storage/disk/disk_manager.h"

int main(int argc, char** argv) {
  std::string db_file = "dbengine.db";
  if (argc > 1) {
    db_file = argv[1];
  }

  try {
    dbengine::DiskManager disk_manager(db_file);
    dbengine::Shell shell(disk_manager, db_file);
    shell.Run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
