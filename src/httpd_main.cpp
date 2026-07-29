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

// Replays each non-empty, non-comment ("--" prefix) line of `path` as a
// statement against `db` before the server starts serving. The SQL catalog
// is in-memory only and doesn't persist across process restarts, so this is
// how a fresh process learns about tables a separate CLI session already
// created on disk. Runs only at startup, never in response to a request,
// keeping the network-facing API read-only.
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
