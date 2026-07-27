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
)

target_include_directories(dbengine_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(dbengine_tests
  PRIVATE
    dbengine_core
    GTest::gtest_main
)

target_compile_definitions(dbengine_tests
  PRIVATE
    DBENGINE_CRASH_WORKER_PATH="$<TARGET_FILE:dbengine_crash_worker>"
)
add_dependencies(dbengine_tests dbengine_crash_worker)

include(GoogleTest)
gtest_discover_tests(dbengine_tests)
