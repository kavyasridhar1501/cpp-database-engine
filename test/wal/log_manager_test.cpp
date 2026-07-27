#include "wal/log_manager.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace dbengine {
namespace {

class LogManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_log_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    std::filesystem::remove(file_path_);
  }
  void TearDown() override { std::filesystem::remove(file_path_); }

  std::string file_path_;
};

TEST_F(LogManagerTest, AppendAssignsSequentialLsns) {
  LogManager log(file_path_);
  LogRecord r1;
  r1.txn_id = 1;
  r1.type = LogRecordType::BEGIN;
  int64_t lsn1 = log.Append(r1);
  LogRecord r2;
  r2.txn_id = 1;
  r2.type = LogRecordType::COMMIT;
  int64_t lsn2 = log.Append(r2);

  EXPECT_EQ(lsn1, 0);
  EXPECT_EQ(lsn2, 1);
  EXPECT_EQ(log.GetNextLsn(), 2);
}

TEST_F(LogManagerTest, ReadRecordReturnsWhatWasAppended) {
  LogManager log(file_path_);
  LogRecord r;
  r.txn_id = 42;
  r.type = LogRecordType::UPDATE;
  r.op = LogOpType::PUT;
  r.key = 7;
  r.SetNewValue("hello");
  int64_t lsn = log.Append(r);

  LogRecord read_back = log.ReadRecord(lsn);
  EXPECT_EQ(read_back.txn_id, 42);
  EXPECT_EQ(read_back.type, LogRecordType::UPDATE);
  EXPECT_EQ(read_back.op, LogOpType::PUT);
  EXPECT_EQ(read_back.key, 7);
  EXPECT_EQ(read_back.NewValue(), "hello");
}

TEST_F(LogManagerTest, ReadFromReturnsCorrectRangeAcrossManyPages) {
  LogManager log(file_path_);
  constexpr int kNumRecords = 500;  // spans many pages given ~18 records/page.
  for (int i = 0; i < kNumRecords; ++i) {
    LogRecord r;
    r.txn_id = i;
    r.type = LogRecordType::UPDATE;
    r.op = LogOpType::PUT;
    r.key = i;
    r.SetNewValue("v" + std::to_string(i));
    log.Append(r);
  }

  auto all = log.ReadAll();
  ASSERT_EQ(all.size(), static_cast<size_t>(kNumRecords));
  for (int i = 0; i < kNumRecords; ++i) {
    EXPECT_EQ(all[static_cast<size_t>(i)].lsn, i);
    EXPECT_EQ(all[static_cast<size_t>(i)].key, i);
    EXPECT_EQ(all[static_cast<size_t>(i)].NewValue(), "v" + std::to_string(i));
  }

  auto partial = log.ReadFrom(250);
  ASSERT_EQ(partial.size(), 250u);
  EXPECT_EQ(partial.front().lsn, 250);
  EXPECT_EQ(partial.back().lsn, kNumRecords - 1);
}

TEST_F(LogManagerTest, StatePersistsAcrossReopenAndAppendContinuesLsnSequence) {
  {
    LogManager log(file_path_);
    for (int i = 0; i < 30; ++i) {
      LogRecord r;
      r.type = LogRecordType::UPDATE;
      r.key = i;
      log.Append(r);
    }
  }
  {
    LogManager log(file_path_);
    EXPECT_EQ(log.GetNextLsn(), 30);
    LogRecord r;
    r.type = LogRecordType::UPDATE;
    r.key = 999;
    int64_t lsn = log.Append(r);
    EXPECT_EQ(lsn, 30);

    auto all = log.ReadAll();
    ASSERT_EQ(all.size(), 31u);
    EXPECT_EQ(all[30].key, 999);
  }
}

TEST_F(LogManagerTest, FlushDoesNotThrow) {
  LogManager log(file_path_);
  LogRecord r;
  r.type = LogRecordType::COMMIT;
  log.Append(r);
  EXPECT_NO_THROW(log.Flush());
}

}  // namespace
}  // namespace dbengine
