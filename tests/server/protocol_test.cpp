#include "lsmdb/server/protocol.h"

#include "lsmdb/db.h"

#include <catch2/catch_test_macros.hpp>
#include <random>

using lsmdb::Db;
using lsmdb::server::dispatch;

namespace {

std::filesystem::path make_temp_dir() {
  std::random_device rd;
  auto dir =
      std::filesystem::temp_directory_path() / ("lsmdb_protocol_test_" + std::to_string(rd()));
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

TEST_CASE("PING replies PONG regardless of database state", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    REQUIRE(dispatch(db, "PING") == "+PONG\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("PUT then GET round-trips a value over the wire protocol", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    REQUIRE(dispatch(db, "PUT hello world") == "+OK\r\n");
    REQUIRE(dispatch(db, "GET hello") == "+world\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("GET on a missing key returns the nil sentinel, not an error", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    REQUIRE(dispatch(db, "GET missing") == "$-1\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("DELETE makes a key subsequently absent", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    dispatch(db, "PUT k v");
    REQUIRE(dispatch(db, "DELETE k") == "+OK\r\n");
    REQUIRE(dispatch(db, "GET k") == "$-1\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("a value may itself contain spaces", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    REQUIRE(dispatch(db, "PUT sentence the quick brown fox") == "+OK\r\n");
    REQUIRE(dispatch(db, "GET sentence") == "+the quick brown fox\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("an unrecognized command returns an error, not a crash", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    auto response = dispatch(db, "BOGUS foo bar");
    REQUIRE(response.rfind("-ERR", 0) == 0);  // starts with -ERR
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("PUT without a value is a malformed command, not a crash", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    auto response = dispatch(db, "PUT keyonly");
    REQUIRE(response.rfind("-ERR", 0) == 0);
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("GET without a key is a malformed command, not a crash", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    auto response = dispatch(db, "GET");
    REQUIRE(response.rfind("-ERR", 0) == 0);
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("an empty value is a valid PUT (trailing space, nothing after)", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    REQUIRE(dispatch(db, "PUT emptyval ") == "+OK\r\n");
    REQUIRE(dispatch(db, "GET emptyval") == "+\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("PUT is rejected, not applied, on a read-only replica", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    auto response = dispatch(db, "PUT k v", /*read_only=*/true);
    REQUIRE(response.rfind("-ERR", 0) == 0);
    REQUIRE_FALSE(db.get("k").has_value());  // must not have been applied
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("DELETE is rejected, not applied, on a read-only replica", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    dispatch(db, "PUT k v");  // seeded directly, not read-only, so this is unaffected
    auto response = dispatch(db, "DELETE k", /*read_only=*/true);
    REQUIRE(response.rfind("-ERR", 0) == 0);
    REQUIRE(db.get("k").has_value());  // must not have been deleted
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("GET and PING are always allowed, even on a read-only replica", "[protocol]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    dispatch(db, "PUT k v");
    REQUIRE(dispatch(db, "GET k", /*read_only=*/true) == "+v\r\n");
    REQUIRE(dispatch(db, "PING", /*read_only=*/true) == "+PONG\r\n");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("is_mutating_command correctly classifies PUT/DELETE and nothing else", "[protocol]") {
  using lsmdb::server::is_mutating_command;
  REQUIRE(is_mutating_command("PUT k v"));
  REQUIRE(is_mutating_command("DELETE k"));
  REQUIRE_FALSE(is_mutating_command("GET k"));
  REQUIRE_FALSE(is_mutating_command("PING"));
  REQUIRE_FALSE(is_mutating_command("SYNC"));
  REQUIRE_FALSE(is_mutating_command("BOGUS"));
}
