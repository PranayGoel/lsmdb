include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(lsmdb_tests
  platform/file_sync_test.cpp
  core/crc32_test.cpp
  core/wal_test.cpp
  core/memtable_test.cpp
  core/bloom_filter_test.cpp
  core/sstable_test.cpp
  core/compaction_test.cpp
  core/db_test.cpp
  core/crash_recovery_test.cpp
  server/protocol_test.cpp
  server/server_test.cpp
  server/server_crash_recovery_test.cpp
)
target_link_libraries(lsmdb_tests PRIVATE lsmdb lsmdb_server asio Catch2::Catch2WithMain)
# Paths to the crash-test helper binaries, baked in at compile time so the
# crash-recovery tests can spawn them without guessing a relative path that
# would differ between single-config (Make/Ninja) and multi-config
# (Visual Studio, which nests exe output under a Debug/Release subfolder)
# generators.
target_compile_definitions(lsmdb_tests PRIVATE
  CRASH_HELPER_PATH="$<TARGET_FILE:crash_helper>"
  LSMDB_SERVER_PATH="$<TARGET_FILE:lsmdb_server_bin>")

include(CTest)
include(Catch)
catch_discover_tests(lsmdb_tests)
