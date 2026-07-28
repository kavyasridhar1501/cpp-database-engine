#pragma once

#include <stdexcept>
#include <string>

namespace dbengine {

// Raised for any error in the SQL layer: lexing, parsing, planning, or
// execution (unknown table/column, arity/type mismatches, a row that
// doesn't fit MAX_VALUE_SIZE, etc). Kept separate from IOException, which
// is specifically about the storage layer underneath.
class SqlException : public std::runtime_error {
 public:
  explicit SqlException(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace dbengine
