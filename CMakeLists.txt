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
  src/memtable.cpp
  src/bloom_filter.cpp
  src/sstable.cpp
  src/compaction.cpp
  src/db.cpp
)
target_include_directories(lsmdb PUBLIC include)
target_link_libraries(lsmdb PUBLIC lsmdb_platform)

# Test-only helper binary, hard-killed by crash_recovery_test.cpp to prove
# durability survives a real OS-level process termination, not just a
# simulated one within the test binary itself. See tools/crash_helper.cpp.
add_executable(crash_helper tools/crash_helper.cpp)
target_link_libraries(crash_helper PRIVATE lsmdb)

# --- Tier 2: networked server, over standalone (Boost-free) Asio.
# Asio is what makes the cross-platform networking requirement possible from
# one codebase: it's a portable abstraction over BSD sockets (POSIX) vs.
# Winsock (Windows), the same relationship platform/file_sync.h has to
# fsync vs. FlushFileBuffers, just upstream and pre-built rather than
# hand-rolled -- there's no engineering value in re-deriving socket-API
# portability from scratch when a mature, widely-used library already solves
# it correctly. "Standalone" (not the Boost.Asio variant) avoids pulling in
# all of Boost for one header-only library.
include(FetchContent)
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
# Asio's own optional exception-on-error paths still need this on Windows;
# harmless (a no-op) everywhere else.
if(WIN32)
  target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00)
endif()

add_library(lsmdb_server
  src/server/protocol.cpp
  src/server/session.cpp
  src/server/server.cpp
  src/server/replication_hub.cpp
  src/server/replication_client.cpp
)
target_include_directories(lsmdb_server PUBLIC include)
target_link_libraries(lsmdb_server PUBLIC lsmdb asio)
if(WIN32)
  target_link_libraries(lsmdb_server PUBLIC ws2_32 mswsock)
endif()

add_executable(lsmdb_server_bin tools/lsmdb_server.cpp)
set_target_properties(lsmdb_server_bin PROPERTIES OUTPUT_NAME lsmdb_server)
target_link_libraries(lsmdb_server_bin PRIVATE lsmdb_server)

add_executable(lsmdb_bench tools/lsmdb_bench.cpp)
target_link_libraries(lsmdb_bench PRIVATE asio)
if(WIN32)
  target_link_libraries(lsmdb_bench PRIVATE ws2_32 mswsock)
endif()

option(LSMDB_BUILD_TESTS "Build the lsmdb test suite" ON)
if(LSMDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
