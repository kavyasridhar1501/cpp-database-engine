add_executable(dbengine_bench
  disk_manager_bench.cpp
  buffer_pool_bench.cpp
  b_plus_tree_bench.cpp
  engine_comparison_bench.cpp
)

target_include_directories(dbengine_bench
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(dbengine_bench
  PRIVATE
    dbengine_core
    benchmark::benchmark
    benchmark::benchmark_main
)
