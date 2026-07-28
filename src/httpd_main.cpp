#include <csignal>
#include <fstream>
#include <iostream>
#include <string>

#include "http/http_server.h"
#include "sql/database.h"

namespace {

dbengine::HttpServer* g_server = nullptr;

void HandleSignal(int) {
  if (g_server != nullptr) g_server->Stop();
}

// Runs each non-empty, non-comment ("--" prefix) line of `path` as its own
// statement against `db` before the server starts serving. This exists
// because the SQL layer's catalog is in-memory only and doesn't persist
// across a Database being reconstructed (see DESIGN.md's Phase 6 section)
// — a fresh dbengine_httpd process otherwise has no way to know a table
// exists, even though the row bytes a separate CLI session wrote are
// still sitting on disk. Loading schema/seed data this way keeps the
// server's actual network-facing API strictly read-only: nothing here
// runs in response to an HTTP request, only at local startup, driven by a
// file the operator controls.
void RunSchemaFile(dbengine::Database* db, const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("failed to open schema file: " + path);

  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    size_t start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos || line.compare(start, 2, "--") == 0) continue;
    try {
      db->Execute(line);
    } catch (const std::exception& e) {
      throw std::runtime_error("schema file '" + path + "' line " + std::to_string(line_no) + ": " + e.what());
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string db_file = "dbengine_httpd.db";
  int port = 8080;
  std::string schema_file;
  if (argc > 1) db_file = argv[1];
  if (argc > 2) port = std::stoi(argv[2]);
  if (argc > 3) schema_file = argv[3];

  try {
    dbengine::Database db(db_file, dbengine::EngineType::BTREE);
    if (!schema_file.empty()) {
      RunSchemaFile(&db, schema_file);
      std::cout << "loaded schema/seed data from " << schema_file << "\n";
    }

    dbengine::HttpServer server(&db, port);
    g_server = &server;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::cout << "dbengine_httpd listening on port " << server.GetBoundPort() << " (db: " << db_file << ")\n"
              << "  GET /health\n"
              << "  GET /query?sql=<url-encoded SELECT statement>\n";
    server.Run();
    std::cout << "dbengine_httpd shutting down\n";
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
