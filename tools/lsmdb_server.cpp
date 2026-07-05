// The actual, runnable database server: opens a Db, listens on a TCP port,
// and serves the protocol documented in include/lsmdb/server/protocol.h.
// Optionally runs as a Tier 3 replica of another running lsmdb_server via
// --replica-of <primary-host> <primary-port>: a replica rejects ordinary
// client writes (see Server's read_only parameter) and instead applies
// every write replicated from its primary through ReplicationClient.
//
// Prints "LISTENING <port>" as its very first line of stdout, flushed
// immediately -- both a human-readable startup confirmation and, not
// coincidentally, a machine-parseable synchronization point that
// tests/server/server_crash_recovery_test.cpp reads to learn which port an
// OS-assigned (port 0) listener actually bound to before it starts sending
// commands.
#include "lsmdb/db.h"
#include "lsmdb/server/replication_client.h"
#include "lsmdb/server/server.h"

#include <asio.hpp>
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: lsmdb_server <data-dir> <port> [--replica-of <primary-host> "
                 "<primary-port>]\n";
    return 1;
  }

  std::string data_dir = argv[1];
  unsigned short port = static_cast<unsigned short>(std::stoi(argv[2]));

  bool is_replica = false;
  std::string primary_host;
  std::string primary_port;
  if (argc >= 6 && std::string(argv[3]) == "--replica-of") {
    is_replica = true;
    primary_host = argv[4];
    primary_port = argv[5];
  }

  try {
    lsmdb::Db db(data_dir);
    asio::io_context io_context;
    lsmdb::server::Server server(io_context, port, db, /*read_only=*/is_replica);

    std::cout << "LISTENING " << server.local_port() << std::endl;

    // Kept alive by its own shared_from_this() chain (same pattern as
    // Session) -- once started, it needs nothing further from main().
    if (is_replica) {
      std::cout << "REPLICA-OF " << primary_host << " " << primary_port << std::endl;
      std::make_shared<lsmdb::server::ReplicationClient>(io_context, primary_host, primary_port,
                                                           db)
          ->start();
    }

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
