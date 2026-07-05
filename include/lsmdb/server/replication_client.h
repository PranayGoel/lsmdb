#pragma once

#include "lsmdb/db.h"

#include <asio.hpp>
#include <memory>
#include <string>

namespace lsmdb::server {

// Runs on a follower server: connects to a primary, issues SYNC, and
// applies every REPLICATE line it receives to a local Db -- for as long as
// the connection stays open. This IS Tier 3's "ship the log, replay it on
// the follower" design, just expressed as replicated commands rather than
// raw WAL bytes: streaming the primary's actual binary WAL format across a
// process boundary would require the follower to understand that exact
// on-disk format (and stay compatible with it across versions); instead,
// the primary re-expresses each already-durable mutation as the same
// simple text command the client protocol already speaks, and the follower
// applies it through the exact same Db::put()/Db::remove() path a local
// client write would take. That's what makes a follower's copy genuinely
// durable and independently crash-recoverable (its own WAL, its own
// SSTables) rather than merely an in-memory mirror that vanishes if the
// follower process itself restarts.
class ReplicationClient : public std::enable_shared_from_this<ReplicationClient> {
 public:
  ReplicationClient(asio::io_context& io_context, std::string primary_host,
                     std::string primary_port, Db& local_db);

  // Connects and begins the async replay loop. Connection failures (primary
  // unreachable, connection dropped later) are reported to stderr and this
  // client simply stops -- Tier 3 is explicitly scoped to manual promotion,
  // not automatic reconnect or failover.
  void start();

 private:
  void read_line();

  asio::ip::tcp::socket socket_;
  asio::streambuf buffer_;
  std::string primary_host_;
  std::string primary_port_;
  Db& local_db_;
};

}  // namespace lsmdb::server
