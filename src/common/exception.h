#pragma once

#include <stdexcept>
#include <string>

namespace dbengine {

// Raised for any I/O failure in the storage layer (short read/write,
// failed open, etc.). Kept as a single type for now; refine as engine
// phases need more granular error handling.
class IOException : public std::runtime_error {
 public:
  explicit IOException(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace dbengine
