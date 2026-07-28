#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "sql/database.h"

namespace dbengine {

// A minimal, read-only HTTP/1.1 server over raw POSIX sockets — no
// external web framework, consistent with this project's "no external
// libraries" rule (see DESIGN.md's Phase 8 section for the full
// rationale). Two routes:
//
//   GET /health        -> 200, body "ok"
//   GET /query?sql=...  -> runs a SELECT (only) and returns its rows as
//                          JSON; any other statement type, or a statement
//                          that fails to parse/plan/execute, gets a 400
//                          with a JSON {"error": "..."} body.
//
// Single-threaded, one connection handled at a time (Run()'s accept loop
// blocks on the next connection until the current one's response is
// written and its socket closed) — deliberately, since Database isn't
// designed for concurrent StorageEngine access from multiple threads (see
// DESIGN.md); this keeps every request trivially safe without needing to
// reason about that.
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
  // Starts true (not false) specifically so Stop() can never race Run():
  // if Stop() were called before the thread running Run() actually got
  // scheduled, Run()'s old behavior of unconditionally setting this to
  // true as its first statement would silently undo the stop signal and
  // loop forever. Initializing true here and never writing true again
  // anywhere makes Stop() a one-way, race-free transition to false
  // regardless of the relative timing between Stop() and Run() starting.
  std::atomic<bool> running_{true};
};

}  // namespace dbengine
