add_executable(dbengine_tests
  storage/disk_manager_test.cpp
  buffer/lru_k_replacer_test.cpp
  buffer/lru_replacer_test.cpp
  buffer/buffer_pool_manager_test.cpp
  index/b_plus_tree_test.cpp
  index/b_plus_tree_engine_test.cpp
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

include(GoogleTest)
gtest_discover_tests(dbengine_tests)
