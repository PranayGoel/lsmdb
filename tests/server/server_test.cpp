// Integration tests driving the server over real TCP sockets (not in-process
// function calls) -- proving the async accept/session/protocol wiring
// actually works end to end, not just that protocol::dispatch's logic is
// correct in isolation (protocol_test.cpp covers that separately).
#include "lsmdb/db.h"
#include "lsmdb/server/server.h"

#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <istream>
#include <random>
#include <thread>

using lsmdb::Db;
using lsmdb::server::Server;

namespace {

std::filesystem::path make_temp_dir() {
  std::random_device rd;
  auto dir = std::filesystem::temp_directory_path() / ("lsmdb_server_test_" + std::to_string(rd()));
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

}  // namespace

TEST_CASE("the server round-trips PUT/GET/DELETE/PING over a real TCP connection", "[server]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    asio::io_context io_context;
    Server server(io_context, /*port=*/0, db);  // 0: let the OS pick an unused port
    auto port = server.local_port();

    std::thread io_thread([&io_context] { io_context.run(); });

    {
      asio::io_context client_io;
      asio::ip::tcp::socket socket(client_io);
      asio::ip::tcp::resolver resolver(client_io);
      asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(port)));

      REQUIRE(send_command(socket, "PING") == "+PONG");
      REQUIRE(send_command(socket, "PUT hello world") == "+OK");
      REQUIRE(send_command(socket, "GET hello") == "+world");
      REQUIRE(send_command(socket, "DELETE hello") == "+OK");
      REQUIRE(send_command(socket, "GET hello") == "$-1");
    }

    io_context.stop();
    io_thread.join();
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("two concurrent client connections share a consistent view of the same database",
          "[server]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    asio::io_context io_context;
    Server server(io_context, /*port=*/0, db);
    auto port = server.local_port();

    std::thread io_thread([&io_context] { io_context.run(); });

    {
      asio::io_context client_io_a;
      asio::io_context client_io_b;
      asio::ip::tcp::socket socket_a(client_io_a);
      asio::ip::tcp::socket socket_b(client_io_b);
      asio::ip::tcp::resolver resolver_a(client_io_a);
      asio::ip::tcp::resolver resolver_b(client_io_b);
      asio::connect(socket_a, resolver_a.resolve("127.0.0.1", std::to_string(port)));
      asio::connect(socket_b, resolver_b.resolve("127.0.0.1", std::to_string(port)));

      REQUIRE(send_command(socket_a, "PUT shared value-from-a") == "+OK");
      // A second, independent connection must see the write the first one
      // made -- proving both Sessions share the one Db instance, not
      // per-connection isolated state.
      REQUIRE(send_command(socket_b, "GET shared") == "+value-from-a");
    }

    io_context.stop();
    io_thread.join();
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("a single connection can issue many commands in sequence", "[server]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    asio::io_context io_context;
    Server server(io_context, /*port=*/0, db);
    auto port = server.local_port();

    std::thread io_thread([&io_context] { io_context.run(); });

    {
      asio::io_context client_io;
      asio::ip::tcp::socket socket(client_io);
      asio::ip::tcp::resolver resolver(client_io);
      asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(port)));

      for (int i = 0; i < 50; ++i) {
        auto key = "key-" + std::to_string(i);
        auto value = "value-" + std::to_string(i);
        REQUIRE(send_command(socket, "PUT " + key + " " + value) == "+OK");
      }
      for (int i = 0; i < 50; ++i) {
        auto key = "key-" + std::to_string(i);
        auto expected = "+value-" + std::to_string(i);
        REQUIRE(send_command(socket, "GET " + key) == expected);
      }
    }

    io_context.stop();
    io_thread.join();
  }
  std::filesystem::remove_all(dir);
}
