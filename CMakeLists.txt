cmake_minimum_required(VERSION 3.16)
project(lsm_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

option(LSM_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(LSM_ENABLE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(LSM_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(LSM_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
else()
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
endif()

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(lsm_core STATIC
  src/util/crc32c.cpp
  src/util/histogram.cpp
  src/util/zipf.cpp
  src/util/thread_pool.cpp
  src/memtable/memtable_lockfree.cpp
  src/memtable/memtable_global_lock.cpp
  src/memtable/memtable_sharded.cpp
  src/wal/wal_writer.cpp
  src/wal/wal_reader.cpp
  src/sstable/bloom.cpp
  src/sstable/block.cpp
  src/sstable/sstable_builder.cpp
  src/sstable/sstable_reader.cpp
  src/version/version.cpp
  src/version/version_set.cpp
  src/version/manifest.cpp
  src/compaction/compaction_picker.cpp
  src/compaction/compaction_job.cpp
  src/compaction/compaction_pool.cpp
  src/db/db_impl.cpp
  src/db/flush.cpp
  src/db/recovery.cpp
  bench/workload_gen.cpp
  bench/metrics.cpp
)
target_link_libraries(lsm_core PUBLIC Threads::Threads)

add_executable(lsm_bench bench/bench_main.cpp)
target_link_libraries(lsm_bench PRIVATE lsm_core)

add_executable(lsm_cli tools/lsm_cli.cpp)
target_link_libraries(lsm_cli PRIVATE lsm_core)

add_executable(crash_child tests/crash_child.cpp)
target_link_libraries(crash_child PRIVATE lsm_core)

add_executable(test_all tests/test_all.cpp)
target_link_libraries(test_all PRIVATE lsm_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test_all)
