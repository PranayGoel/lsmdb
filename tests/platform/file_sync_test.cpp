#include "lsmdb/platform/file_sync.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <random>

namespace {

// Unique-per-test-run temp file so parallel/repeated test runs don't collide.
std::filesystem::path make_temp_path() {
  std::random_device rd;
  return std::filesystem::temp_directory_path() /
         ("lsmdb_sync_test_" + std::to_string(rd()) + ".tmp");
}

}  // namespace

TEST_CASE("sync_file succeeds on a real, existing file", "[platform]") {
  auto path = make_temp_path();
  {
    std::ofstream out(path, std::ios::binary);
    out << "durability matters";
  }
  REQUIRE_NOTHROW(lsmdb::platform::sync_file(path));
  std::filesystem::remove(path);
}

TEST_CASE("sync_file throws on a nonexistent file rather than silently succeeding",
          "[platform]") {
  auto path = make_temp_path();  // deliberately never created
  REQUIRE_THROWS_AS(lsmdb::platform::sync_file(path), std::runtime_error);
}
