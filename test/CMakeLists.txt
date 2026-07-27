add_executable(dbengine_tests
  storage/disk_manager_test.cpp
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
