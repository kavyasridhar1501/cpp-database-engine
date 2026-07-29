#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace dbengine {

// A SQL value; must stay in lockstep with catalog.h's ColumnType.
using Value = std::variant<int64_t, std::string>;

enum class ColumnType { INTEGER, TEXT };

// Comparison across two Values of the *same* underlying type (the planner
// and executor never compare an int64_t Value against a string one — a
// type mismatch there is a SqlException raised earlier, at bind time).
inline int CompareValues(const Value& a, const Value& b) {
  if (std::holds_alternative<int64_t>(a)) {
    int64_t x = std::get<int64_t>(a);
    int64_t y = std::get<int64_t>(b);
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
  }
  const std::string& x = std::get<std::string>(a);
  const std::string& y = std::get<std::string>(b);
  if (x < y) return -1;
  if (x > y) return 1;
  return 0;
}

}  // namespace dbengine
