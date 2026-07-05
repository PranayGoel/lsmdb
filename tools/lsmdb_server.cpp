// The actual, runnable database server: opens a Db, listens on a TCP port,
// and serves the protocol documented in include/lsmdb/server/protocol.h.
//
// Prints "LISTENING <port>" as its very first line of stdout, flushed
// immediately -- both a human-readable startup confirmation and, not
// coincidentally, a machine-parseable synchronization point that
// tests/server/server_crash_recovery_test.cpp reads to learn which port an
// OS-assigned (port 0) listener actually bound to before it starts sending
// commands.
#include "lsmdb/db.h"
#include "lsmdb/server/server.h"

#include <asio.hpp>
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: lsmdb_server <data-dir> <port>\n";
    return 1;
  }

  std::string data_dir = argv[1];
  unsigned short port = static_cast<unsigned short>(std::stoi(argv[2]));

  try {
    lsmdb::Db db(data_dir);
    asio::io_context io_context;
    lsmdb::server::Server server(io_context, port, db);

    std::cout << "LISTENING " << server.local_port() << std::endl;

    // SIGINT only (not SIGTERM): asio::signal_set's SIGINT handling is
    // uniformly portable across POSIX and Windows (translated to a console
    // control handler there); SIGTERM has no real Windows equivalent, and
    // this server's crash-recovery guarantee is proven by hard-killing it
    // anyway (see the crash-recovery tests), so graceful shutdown is a
    // convenience for interactive/manual use, not something correctness
    // depends on.
    asio::signal_set signals(io_context, SIGINT);
    signals.async_wait([&io_context](const asio::error_code&, int) { io_context.stop(); });

    // Run the io_context across every available hardware thread -- Db's own
    // internal locking (see db.h's module comment) is what makes this safe;
    // the server/session layer needs no additional synchronization of its
    // own to allow genuinely concurrent client connections.
    unsigned num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < num_threads; ++i) {
      pool.emplace_back([&io_context] { io_context.run(); });
    }
    io_context.run();  // main thread also participates
    for (auto& t : pool) t.join();
  } catch (const std::exception& e) {
    std::cerr << "lsmdb_server: fatal: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
