// Forks the standalone crash_worker (see crash_worker.cpp), lets it run
// unpaced for a random short duration, SIGKILLs it, reopens the database to
// trigger recovery, and verifies every key matches what the side-effect log
// proves was durable. Run hundreds of times to sweep random kill points
// across the operation sequence.

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <string>
#include <thread>

#include "lsm/lsm_tree_engine.h"
#include "wal/wal_b_plus_tree_engine.h"

#ifndef DBENGINE_CRASH_WORKER_PATH
#error "DBENGINE_CRASH_WORKER_PATH must be defined by CMake (see test/CMakeLists.txt)"
#endif

namespace dbengine {
namespace {

constexpr int64_t kKeyRange = 50;

void CleanupEngineFiles(const std::string& db_path) {
  std::filesystem::remove(db_path);
  std::filesystem::path dir = std::filesystem::path(db_path).parent_path();
  if (dir.empty()) dir = ".";
  std::string prefix = std::filesystem::path(db_path).filename().string();
  if (!std::filesystem::exists(dir)) return;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().rfind(prefix, 0) == 0) {
      std::filesystem::remove(entry.path());
    }
  }
}

// At most the last A/C bracket in the side log can be left dangling (an 'A'
// with no matching 'C'); that one key's outcome is genuinely ambiguous, but
// every other key's last 'C' value must match the DB exactly.
struct CrashOracle {
  std::map<int64_t, std::string> last_confirmed;
  bool has_ambiguous = false;
  int64_t ambiguous_key = -1;
  std::string ambiguous_value;
};

CrashOracle ParseSideLog(const std::string& side_log_path) {
  CrashOracle oracle;
  std::ifstream side_log(side_log_path);
  char marker;
  int64_t key;
  std::string value;
  bool attempt_open = false;
  int64_t attempt_key = -1;
  std::string attempt_value;

  while (side_log >> marker >> key >> value) {
    if (marker == 'A') {
      attempt_open = true;
      attempt_key = key;
      attempt_value = value;
    } else if (marker == 'C') {
      oracle.last_confirmed[key] = value;
      attempt_open = false;
    }
  }
  if (attempt_open) {
    oracle.has_ambiguous = true;
    oracle.ambiguous_key = attempt_key;
    oracle.ambiguous_value = attempt_value;
  }
  return oracle;
}

CrashOracle RunOneCrashCycle(const std::string& worker_path, const std::string& engine_type,
                             const std::string& db_path, const std::string& side_log_path,
                             std::mt19937& rng) {
  std::filesystem::remove(side_log_path);

  pid_t pid = fork();
  if (pid == -1) {
    ADD_FAILURE() << "fork() failed";
    return {};
  }
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull != -1) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    std::string key_range_str = std::to_string(kKeyRange);
    execl(worker_path.c_str(), worker_path.c_str(), engine_type.c_str(), db_path.c_str(),
          side_log_path.c_str(), key_range_str.c_str(), static_cast<char*>(nullptr));
    _exit(127);  // exec failed
  }

  std::uniform_int_distribution<int> delay_ms(1, 50);
  std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms(rng)));
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);

  return ParseSideLog(side_log_path);
}

// Exact match for every unambiguous key; one of two legitimate outcomes for
// the at-most-one ambiguous key.
template <typename Engine>
void VerifyAgainstOracle(Engine& engine, const CrashOracle& oracle, int cycle) {
  for (int64_t k = 0; k < kKeyRange; ++k) {
    std::string value;
    bool found = engine.Get(k, &value);
    auto it = oracle.last_confirmed.find(k);
    bool is_ambiguous = oracle.has_ambiguous && oracle.ambiguous_key == k;

    if (!is_ambiguous) {
      if (it != oracle.last_confirmed.end()) {
        ASSERT_TRUE(found) << "cycle " << cycle << ": key " << k
                            << " should be present (last confirmed '" << it->second
                            << "') but recovery lost it";
        ASSERT_EQ(value, it->second) << "cycle " << cycle << ": key " << k;
      } else {
        ASSERT_FALSE(found) << "cycle " << cycle << ": key " << k
                             << " was never confirmed but is present after recovery";
      }
      continue;
    }

    // Ambiguous key: the in-flight write may or may not have landed.
    if (!found) {
      ASSERT_EQ(it, oracle.last_confirmed.end())
          << "cycle " << cycle << ": key " << k
          << " (ambiguous) is absent, but an earlier write to it was already confirmed as '"
          << it->second << "' — that confirmed write must never be lost";
      continue;
    }
    bool matches_prior_confirmed = it != oracle.last_confirmed.end() && value == it->second;
    bool matches_in_flight_attempt = value == oracle.ambiguous_value;
    ASSERT_TRUE(matches_prior_confirmed || matches_in_flight_attempt)
        << "cycle " << cycle << ": key " << k << " (ambiguous) has value '" << value
        << "', which matches neither the prior confirmed value nor the in-flight attempt '"
        << oracle.ambiguous_value << "'";
  }
}

}  // namespace

TEST(CrashRecoveryTest, BPlusTreeSurvivesRepeatedRandomKills) {
  const std::string worker_path = DBENGINE_CRASH_WORKER_PATH;
  const std::string db_path =
      (std::filesystem::temp_directory_path() / "dbengine_crash_bpt.db").string();
  const std::string side_log_path =
      (std::filesystem::temp_directory_path() / "dbengine_crash_bpt_side.log").string();

  std::mt19937 rng(std::random_device{}());
  constexpr int kNumCycles = 150;

  for (int cycle = 0; cycle < kNumCycles; ++cycle) {
    CleanupEngineFiles(db_path);
    CrashOracle oracle = RunOneCrashCycle(worker_path, "bplustree", db_path, side_log_path, rng);

    WALBPlusTreeEngine engine(db_path, 16, /*enable_wal=*/true);
    VerifyAgainstOracle(engine, oracle, cycle);

    // Sentinel key: only written by aborted txns, so it must never be
    // visible regardless of when the kill landed relative to the abort.
    std::string poison_value;
    ASSERT_FALSE(engine.Get(kKeyRange, &poison_value))
        << "cycle " << cycle << ": an aborted transaction's write survived recovery";
  }

  std::filesystem::remove(side_log_path);
  CleanupEngineFiles(db_path);
}

TEST(CrashRecoveryTest, LSMTreeSurvivesRepeatedRandomKills) {
  const std::string worker_path = DBENGINE_CRASH_WORKER_PATH;
  const std::string db_path =
      (std::filesystem::temp_directory_path() / "dbengine_crash_lsm.db").string();
  const std::string side_log_path =
      (std::filesystem::temp_directory_path() / "dbengine_crash_lsm_side.log").string();

  std::mt19937 rng(std::random_device{}());
  constexpr int kNumCycles = 100;

  for (int cycle = 0; cycle < kNumCycles; ++cycle) {
    CleanupEngineFiles(db_path);
    CrashOracle oracle = RunOneCrashCycle(worker_path, "lsm", db_path, side_log_path, rng);

    LSMTreeEngine engine(db_path, 1 << 20, 4, /*enable_wal=*/true);
    VerifyAgainstOracle(engine, oracle, cycle);
  }

  std::filesystem::remove(side_log_path);
  CleanupEngineFiles(db_path);
}

}  // namespace dbengine
