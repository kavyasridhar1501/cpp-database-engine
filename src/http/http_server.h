#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "sql/database.h"

namespace dbengine {

// A minimal, read-only HTTP/1.1 server over raw POSIX sockets. Two routes:
//
//   GET /health         -> 200, body "ok"
//   GET /query?sql=...  -> runs a SELECT (only) and returns its rows as
//                          JSON; any other statement type, or a statement
//                          that fails to parse/plan/execute, gets a 400
//                          with a JSON {"error": "..."} body.
//
// Single-threaded, one connection at a time: Database isn't designed for
// concurrent StorageEngine access from multiple threads, so this keeps
// every request trivially safe without needing to reason about that.
class HttpServer {
 public:
  // `port` == 0 asks the OS to pick a free port; call GetBoundPort() after
  // construction to find out which one (used by tests to avoid collisions).
  HttpServer(Database* db, int port);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  int GetBoundPort() const { return bound_port_; }

  // Blocks, serving connections, until Stop() is called from another
  // thread. Polls the listening socket with a short timeout so it can
  // notice Stop() promptly without needing to interrupt a blocking
  // accept() from another thread.
  void Run();
  void Stop();

 private:
  void HandleConnection(int client_fd);

  Database* db_;
  int listen_fd_ = -1;
  int bound_port_ = -1;
  // Starts true (never set true again) so a Stop() called before Run()'s
  // thread is even scheduled can't be clobbered by Run() resetting it.
  std::atomic<bool> running_{true};
};

}  // namespace dbengine
