// Standalone worker for the crash-injection harness (see
// crash_recovery_test.cpp). Repeatedly commits (and, for the B+-tree,
// occasionally aborts) transactions in a tight loop with no artificial
// delay, so an external SIGKILL lands at a genuinely random point in the
// operation sequence. Every confirmed commit is recorded to a separate,
// independently-fsynced side-effect log *after* the engine call returns,
// so the harness has an oracle for "what was actually durable" that
// doesn't depend on the (possibly killed) engine itself.
//
// Usage: dbengine_crash_worker <bplustree|lsm> <db_path> <side_log_path> <key_range>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "lsm/lsm_tree_engine.h"
#include "wal/wal_b_plus_tree_engine.h"

namespace {

// Writes one bracketing line ('A'ttempt before the engine call, 'C'onfirm
// after it returns) rather than a single post-hoc record. A kill can land
// between the engine call returning and this line reaching disk, so a bare
// post-hoc log has an inherent race — the DB can legitimately be one write
// ahead of it. Bracketing turns that into a detectable "dangling A with no
// matching C" at the tail of the log instead of an unexplained mismatch;
// see crash_recovery_test.cpp's verifier for how it's used.
void AppendSideLogLine(FILE* side_log, char marker, int64_t key, const std::string& value) {
  fprintf(side_log, "%c %lld %s\n", marker, static_cast<long long>(key), value.c_str());
  fflush(side_log);
  fsync(fileno(side_log));
}

int RunBPlusTree(const std::string& db_path, const std::string& side_log_path, int64_t key_range) {
  dbengine::WALBPlusTreeEngine engine(db_path, /*buffer_pool_size=*/16, /*enable_wal=*/true,
                                       /*checkpoint_interval_txns=*/20);
  FILE* side_log = fopen(side_log_path.c_str(), "a");
  if (side_log == nullptr) return 1;

  std::mt19937 rng(static_cast<unsigned>(getpid()) ^ static_cast<unsigned>(time(nullptr)));
  std::uniform_int_distribution<int> decision(0, 9);

  const int64_t kPoisonKey = key_range;  // outside the normal key range.
  int64_t counter = 0;
  while (true) {
    if (decision(rng) == 0) {
      // A transaction that must never be visible after recovery, whether
      // this process is killed before, during, or after its Abort() call.
      int64_t txn = engine.Begin();
      engine.Put(txn, kPoisonKey, "POISON-" + std::to_string(counter));
      engine.Abort(txn);
    } else {
      int64_t key = counter % key_range;
      std::string value = "v" + std::to_string(counter);
      AppendSideLogLine(side_log, 'A', key, value);
      int64_t txn = engine.Begin();
      engine.Put(txn, key, value);
      engine.Commit(txn);
      AppendSideLogLine(side_log, 'C', key, value);
    }
    ++counter;
  }
  return 0;
}

int RunLSM(const std::string& db_path, const std::string& side_log_path, int64_t key_range) {
  dbengine::LSMTreeEngine engine(db_path, /*memtable_flush_threshold_bytes=*/4096,
                                  /*tier_compaction_threshold=*/4, /*enable_wal=*/true);
  FILE* side_log = fopen(side_log_path.c_str(), "a");
  if (side_log == nullptr) return 1;

  int64_t counter = 0;
  while (true) {
    int64_t key = counter % key_range;
    std::string value = "v" + std::to_string(counter);
    AppendSideLogLine(side_log, 'A', key, value);
    engine.Put(key, value);
    AppendSideLogLine(side_log, 'C', key, value);
    ++counter;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s <bplustree|lsm> <db_path> <side_log_path> <key_range>\n", argv[0]);
    return 2;
  }
  std::string engine_type = argv[1];
  std::string db_path = argv[2];
  std::string side_log_path = argv[3];
  int64_t key_range = std::stoll(argv[4]);

  if (engine_type == "bplustree") {
    return RunBPlusTree(db_path, side_log_path, key_range);
  }
  if (engine_type == "lsm") {
    return RunLSM(db_path, side_log_path, key_range);
  }
  fprintf(stderr, "unknown engine type '%s'\n", engine_type.c_str());
  return 2;
}
