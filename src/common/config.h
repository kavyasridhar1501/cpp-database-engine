#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dbengine {

// Fixed, compile-time page size. All disk I/O is page-granular.
inline constexpr size_t PAGE_SIZE = 4096;

using page_id_t = int64_t;
inline constexpr page_id_t INVALID_PAGE_ID = -1;

using frame_id_t = int32_t;
inline constexpr frame_id_t INVALID_FRAME_ID = -1;

// Keys are fixed-size 64-bit integers across every storage engine. This
// keeps node/entry layouts simple (plain arrays, no variable-length key
// encoding) and matches how the benchmarks in this project are framed
// ("N keys" of a single comparable type).
using KeyType = int64_t;
inline constexpr KeyType INVALID_KEY = std::numeric_limits<KeyType>::min();

// Values are caller-provided byte strings capped at MAX_VALUE_SIZE. The cap
// is enforced by both storage engines (not just the B+-tree, which needs it
// for its fixed-size leaf slots) so head-to-head comparisons stay
// apples-to-apples.
inline constexpr size_t MAX_VALUE_SIZE = 64;

}  // namespace dbengine
