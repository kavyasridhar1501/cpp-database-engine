cmake_minimum_required(VERSION 3.16)
project(dbengine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DBENGINE_BUILD_TESTS "Build unit tests" ON)
option(DBENGINE_BUILD_BENCHMARKS "Build benchmarks" ON)

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)

add_subdirectory(src)

if(DBENGINE_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  # Prevent GoogleTest from overriding the parent project's compiler/linker settings.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Test-only, and only for the SQLite differential test suite (Phase 7):
  # a *system* library, not vendored/FetchContent'd, and never linked into
  # dbengine_core/dbengine_cli — the "no external database libraries"
  # constraint is about what ships in the engine, not what validates it
  # against a real, mature implementation in tests. See DESIGN.md's Phase 7
  # section. Optional: if it's not installed, that one test file is simply
  # skipped (test/CMakeLists.txt) rather than failing the whole build.
  find_package(SQLite3)

  add_subdirectory(test)
endif()

if(DBENGINE_BUILD_BENCHMARKS)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.0
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_subdirectory(benchmark)
endif()
