// Tier 3's actual claim, tested honestly and directly: a follower server,
// once subscribed to a primary, ends up with a real, independently durable
// copy of the primary's data -- both what already existed before it
// subscribed (the SYNC snapshot) and everything written afterward (the live
// replication stream) -- and that copy survives the follower's own Db being
// closed and reopened, completely independent of the primary.
//
// This deliberately doesn't re-run a hard-OS-process-kill test here:
// tests/core/crash_recovery_test.cpp and
// tests/server/server_crash_recovery_test.cpp already separately prove "a
// Db survives a real process kill" and "a server exposes that durability
// correctly over the network." Tier 3 only needs to newly prove that data
// actually flows from primary to follower and lands somewhere durable,
// which is exactly what closing and reopening the follower's Db below
// checks -- an honest, deliberately narrower scope for what is explicitly a
// stretch goal, documented as such rather than silently skipped.
#include "lsmdb/db.h"
#include "lsmdb/server/replication_client.h"
#include "lsmdb/server/server.h"

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <istream>
#include <random>
#include <thread>

using lsmdb::Db;
using lsmdb::server::ReplicationClient;
using lsmdb::server::Server;

namespace {

std::filesystem::path make_temp_dir(const std::string& tag) {
  std::random_device rd;
  auto dir = std::filesystem::temp_directory_path() /
             ("lsmdb_replication_test_" + tag + "_" + std::to_string(rd()));
  std::filesystem::create_directories(dir);
  return dir;
}

std::string send_command(asio::ip::tcp::socket& socket, const std::string& command) {
  std::string line = command + "\r\n";
  asio::write(socket, asio::buffer(line));
  asio::streambuf buffer;
  asio::read_until(socket, buffer, "\r\n");
  std::istream is(&buffer);
  std::string response;
  std::getline(is, response);
  if (!response.empty() && response.back() == '\r') response.pop_back();
  return response;
}

// Polls `predicate` until it returns true or `timeout` elapses. Replication
// is asynchronous, so a test can't assume a primary's write has already
// propagated to the follower the instant the primary acknowledges it; this
// bounds how long a test waits before concluding replication genuinely
// failed rather than merely being slow.
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

TEST_CASE(
    "a follower receives a full snapshot on SYNC, then every subsequent primary write, and "
    "its own copy survives independently once the primary is gone",
    "[replication]") {
  auto primary_dir = make_temp_dir("primary");
  auto follower_dir = make_temp_dir("follower");

  {
    Db primary_db(primary_dir);
    asio::io_context primary_io;
    Server primary_server(primary_io, /*port=*/0, primary_db);
    auto primary_port = primary_server.local_port();
    std::thread primary_thread([&primary_io] { primary_io.run(); });

    // Written to the primary BEFORE the follower ever subscribes -- proves
    // the SYNC snapshot backfills pre-existing data, not just future writes.
    {
      asio::io_context client_io;
      asio::ip::tcp::socket socket(client_io);
      asio::ip::tcp::resolver resolver(client_io);
      asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(primary_port)));
      REQUIRE(send_command(socket, "PUT preexisting value-from-before-subscribe") == "+OK");
    }

    {
      Db follower_db(follower_dir);
      asio::io_context follower_io;
      Server follower_server(follower_io, /*port=*/0, follower_db, /*read_only=*/true);
      auto follower_port = follower_server.local_port();
      std::thread follower_thread([&follower_io] { follower_io.run(); });

      std::make_shared<ReplicationClient>(follower_io, "127.0.0.1", std::to_string(primary_port),
                                           follower_db)
          ->start();

      REQUIRE(wait_until([&] { return follower_db.get("preexisting").has_value(); }));
      REQUIRE(*follower_db.get("preexisting") == "value-from-before-subscribe");

      // Now write something NEW to the primary, after the follower already
      // subscribed -- the live-forwarding path, not the snapshot.
      {
        asio::io_context client_io;
        asio::ip::tcp::socket socket(client_io);
        asio::ip::tcp::resolver resolver(client_io);
        asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(primary_port)));
        REQUIRE(send_command(socket, "PUT live-write arrived-after-subscribe") == "+OK");
      }
      REQUIRE(wait_until([&] { return follower_db.get("live-write").has_value(); }));
      REQUIRE(*follower_db.get("live-write") == "arrived-after-subscribe");

      // The follower is read-only: its data must only ever arrive via
      // replication from the primary, never from a client that connected
      // to the wrong server.
      {
        asio::io_context client_io;
        asio::ip::tcp::socket socket(client_io);
        asio::ip::tcp::resolver resolver(client_io);
        asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(follower_port)));
        auto response = send_command(socket, "PUT should-be-rejected x");
        REQUIRE(response.rfind("-ERR", 0) == 0);
      }

      follower_io.stop();
      follower_thread.join();
    }  // follower_db destructs here -- what it received is only durable if
       // it actually persisted through Db::put(), not merely mirrored in
       // memory.

    primary_io.stop();
    primary_thread.join();
  }  // primary_db destructs here too -- the primary is now completely gone.

  // The actual Tier 3 claim: reopen a brand-new Db on the follower's
  // directory alone -- no primary, no server, no replication client running
  // at all -- and confirm both the snapshot-backfilled key and the
  // live-replicated key are still there. This is what "the follower
  // survives the primary being killed" means in practice: the data was
  // genuinely written durably to the follower's own WAL/SSTables via the
  // same Db::put() path any local write takes, not merely held in memory by
  // a process that happened to still be running.
  {
    Db reopened_follower(follower_dir);
    REQUIRE(reopened_follower.get("preexisting").has_value());
    REQUIRE(*reopened_follower.get("preexisting") == "value-from-before-subscribe");
    REQUIRE(reopened_follower.get("live-write").has_value());
    REQUIRE(*reopened_follower.get("live-write") == "arrived-after-subscribe");
  }

  std::filesystem::remove_all(primary_dir);
  std::filesystem::remove_all(follower_dir);
}
