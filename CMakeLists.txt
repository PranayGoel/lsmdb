cmake_minimum_required(VERSION 3.20)
project(lsmdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizers: opt-in via -DLSMDB_SANITIZE=address|undefined|thread. Kept as a build
# option (not always-on) since ASan/UBSan and TSan can't be combined in one build,
# and MSVC's sanitizer support differs from gcc/clang -- CI runs each variant
# separately on the platforms that support it rather than assuming this flag is
# portable everywhere.
set(LSMDB_SANITIZE "" CACHE STRING "Sanitizer to build with: address, undefined, thread, or empty")
if(LSMDB_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=${LSMDB_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${LSMDB_SANITIZE})
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Platform abstraction: the ONE place OS-specific code is allowed to live.
# Every other component (WAL, memtable, SSTable, compaction, server) is written
# against std::filesystem, std::thread, and this header -- never against
# POSIX or Win32 APIs directly. See include/lsmdb/platform/file_sync.h.
add_library(lsmdb_platform
  src/platform/file_sync.cpp
)
target_include_directories(lsmdb_platform PUBLIC include)

add_library(lsmdb
  src/crc32.cpp
  src/wal.cpp
  # Populated incrementally: memtable.cpp, sstable.cpp, bloom_filter.cpp,
  # compaction.cpp, db.cpp
)
target_include_directories(lsmdb PUBLIC include)
target_link_libraries(lsmdb PUBLIC lsmdb_platform)

option(LSMDB_BUILD_TESTS "Build the lsmdb test suite" ON)
if(LSMDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
