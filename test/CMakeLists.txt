# Standalone worker executable driven by the crash-injection harness
# (test/crash/crash_recovery_test.cpp), which forks, execs, and SIGKILLs
# it at random points. Must exist as a real separate process — an
# in-process "simulated crash" can't bypass RAII/destructor cleanup the
# way an actual kill -9 does.
add_executable(dbengine_crash_worker
  crash/crash_worker.cpp
)
target_include_directories(dbengine_crash_worker
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dbengine_crash_worker
  PRIVATE
    dbengine_core
)

add_executable(dbengine_tests
  storage/disk_manager_test.cpp
  buffer/lru_k_replacer_test.cpp
  buffer/lru_replacer_test.cpp
  buffer/buffer_pool_manager_test.cpp
  index/b_plus_tree_test.cpp
  index/b_plus_tree_engine_test.cpp
  lsm/skip_list_test.cpp
  lsm/bloom_filter_test.cpp
  lsm/memtable_test.cpp
  lsm/sstable_test.cpp
  lsm/merge_iterator_test.cpp
  lsm/lsm_tree_engine_test.cpp
  wal/log_manager_test.cpp
  wal/wal_b_plus_tree_engine_test.cpp
  crash/crash_recovery_test.cpp
  mvcc/mvcc_store_test.cpp
  sql/lexer_test.cpp
  sql/parser_test.cpp
  sql/planner_test.cpp
  sql/catalog_test.cpp
  sql/database_test.cpp
  http/http_server_test.cpp
)

target_include_directories(dbengine_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/test
)

target_link_libraries(dbengine_tests
  PRIVATE
    dbengine_core
    GTest::gtest_main
)

# SQLite differential testing (Phase 7) is test-only and needs a system
# libsqlite3 (see CMakeLists.txt's find_package(SQLite3) call). Optional:
# skip it gracefully rather than failing the whole test build when it's not
# installed, rather than vendoring/FetchContent'ing a database library into
# the project just for this.
if(SQLite3_FOUND)
  target_sources(dbengine_tests PRIVATE
    validation/sqlite_reference.cpp
    validation/sqlite_differential_test.cpp
  )
  target_link_libraries(dbengine_tests PRIVATE SQLite::SQLite3)
else()
  message(STATUS "SQLite3 not found — skipping the SQLite differential test suite (test/validation/)")
endif()

target_compile_definitions(dbengine_tests
  PRIVATE
    DBENGINE_CRASH_WORKER_PATH="$<TARGET_FILE:dbengine_crash_worker>"
)
add_dependencies(dbengine_tests dbengine_crash_worker)

include(GoogleTest)
gtest_discover_tests(dbengine_tests)
