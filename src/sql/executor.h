#pragma once

#include <cstddef>
#include <vector>

#include "engine/storage_engine.h"
#include "sql/catalog.h"
#include "sql/planner.h"

namespace dbengine {

// Runs a Plan against a StorageEngine. Planning already picked the access
// path (point/range/full); the executor's only remaining job is to fetch
// along that path and apply the residual filter.
class Executor {
 public:
  static std::vector<std::vector<Value>> ExecuteFetch(const TableSchema& schema, const Plan& plan,
                                                        StorageEngine* engine);

  static size_t ExecuteDelete(const TableSchema& schema, const Plan& plan, StorageEngine* engine);

  static bool MatchesPredicate(const TableSchema& schema, const std::vector<Value>& row,
                                const Predicate& predicate);
};

}  // namespace dbengine
