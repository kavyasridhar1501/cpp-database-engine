#pragma once

#include <cstdint>
#include <limits>

namespace dbengine {

// Fixed, compile-time page size. All disk I/O is page-granular.
inline constexpr size_t PAGE_SIZE = 4096;

using page_id_t = int64_t;
inline constexpr page_id_t INVALID_PAGE_ID = -1;

using frame_id_t = int32_t;
inline constexpr frame_id_t INVALID_FRAME_ID = -1;

}  // namespace dbengine
