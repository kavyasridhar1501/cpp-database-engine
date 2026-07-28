#pragma once

#include <cstdint>
#include <limits>

#include "sql/ast.h"
#include "sql/catalog.h"

namespace dbengine {

enum class AccessPathType { POINT_LOOKUP, RANGE_SCAN, FULL_SCAN };

// What the executor should do to satisfy a WHERE clause: this project's
// "tiny optimizer" is entirely about picking one of these three, since the
// primary key is the only indexed column (see catalog.h — KeyType is a
// single int64_t across every storage engine, so there's no secondary
// index to consider). See DESIGN.md for the selection rules.
struct Plan {
  AccessPathType access_path = AccessPathType::FULL_SCAN;

  // POINT_LOOKUP
  int64_t point_key = 0;

  // RANGE_SCAN: [range_lo, range_hi] inclusive on both ends. Defaults span
  // the full representable primary-key range.
  int64_t range_lo = 0;
  int64_t range_hi = std::numeric_limits<int64_t>::max();

  // Every comparison not already implied by the chosen access path (always
  // *all* comparisons for FULL_SCAN); the executor applies these as a
  // post-fetch filter regardless of access path, so a plan is correct even
  // if this ends up including redundant checks.
  Predicate residual;
};

// Chooses an access path for `where` against `schema`:
//   1. An EQ comparison on the primary key -> POINT_LOOKUP.
//   2. Otherwise, any LT/LE/GT/GE bound on the primary key -> RANGE_SCAN
//      over the intersection of all such bounds.
//   3. Otherwise -> FULL_SCAN.
Plan PlanQuery(const TableSchema& schema, const Predicate& where);

}  // namespace dbengine
