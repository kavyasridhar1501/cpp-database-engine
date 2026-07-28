add_library(dbengine_core
  storage/disk/disk_manager.cpp
  buffer/lru_k_replacer.cpp
  buffer/lru_replacer.cpp
  buffer/buffer_pool_manager.cpp
  index/internal_page.cpp
  index/leaf_page.cpp
  index/b_plus_tree.cpp
  index/b_plus_tree_engine.cpp
  lsm/bloom_filter.cpp
  lsm/sstable.cpp
  lsm/merge_iterator.cpp
  lsm/lsm_tree_engine.cpp
  wal/log_manager.cpp
  wal/wal_b_plus_tree_engine.cpp
  mvcc/mvcc_store.cpp
  sql/lexer.cpp
  sql/parser.cpp
  sql/catalog.cpp
  sql/planner.cpp
  sql/executor.cpp
  sql/database.cpp
)

target_include_directories(dbengine_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(dbengine_cli
  main.cpp
  ${CMAKE_SOURCE_DIR}/cli/shell.cpp
)

target_include_directories(dbengine_cli
  PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(dbengine_cli
  PRIVATE
    dbengine_core
)
