#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "common/config.h"

namespace dbengine {

enum class LogRecordType : int32_t {
  INVALID = 0,
  BEGIN = 1,
  UPDATE = 2,
  COMMIT = 3,
  ABORT = 4,
  CLR = 5,  // Compensation Log Record, written while undoing an UPDATE.
  CHECKPOINT_BEGIN = 6,
  ACTIVE_TXN = 7,  // one per transaction active at checkpoint time.
  CHECKPOINT_END = 8,
};

enum class LogOpType : int32_t { NONE = 0, PUT = 1, DELETE = 2 };

// A single WAL record, logged *logically* ("Put(key, new_value)" happened,
// with enough prior state to undo it) rather than as a physical page
// byte-image — ARIES's prescribed approach for tree-structured indexes,
// where physical undo breaks once a split/merge has changed the page layout.
//
// One struct covers every record type (a tagged union in spirit) so
// LogManager's on-disk page format stays a uniform fixed-size array, with
// no variable-length framing.
struct LogRecord {
  int64_t lsn = -1;
  int64_t txn_id = -1;
  int64_t prev_lsn = -1;  // this transaction's previous LSN, or -1.
  LogRecordType type = LogRecordType::INVALID;

  // UPDATE / CLR
  LogOpType op = LogOpType::NONE;
  KeyType key = 0;
  uint8_t has_old_value = 0;
  uint16_t old_value_len = 0;
  char old_value[MAX_VALUE_SIZE] = {};
  uint8_t has_new_value = 0;
  uint16_t new_value_len = 0;
  char new_value[MAX_VALUE_SIZE] = {};

  // CLR only: where undo should resume after this compensation (the
  // prev_lsn of the record that was just undone).
  int64_t undo_next_lsn = -1;

  // ACTIVE_TXN only.
  int64_t active_txn_id = -1;
  int64_t active_txn_last_lsn = -1;

  std::string OldValue() const { return std::string(old_value, old_value_len); }
  std::string NewValue() const { return std::string(new_value, new_value_len); }

  void SetOldValue(const std::string& v) {
    has_old_value = 1;
    old_value_len = static_cast<uint16_t>(v.size());
    std::memcpy(old_value, v.data(), v.size());
  }
  void SetNewValue(const std::string& v) {
    has_new_value = 1;
    new_value_len = static_cast<uint16_t>(v.size());
    std::memcpy(new_value, v.data(), v.size());
  }
};

}  // namespace dbengine
