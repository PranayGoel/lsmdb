#pragma once

#include "lsmdb/db.h"
#include "lsmdb/server/replication_hub.h"

#include <asio.hpp>

namespace lsmdb::server {

// Accepts incoming TCP connections and hands each one off to its own
// Session. Owns nothing about the database itself -- Db is constructed and
// owned by the caller (main() in tools/lsmdb_server.cpp, or a test) for the
// whole process/test lifetime; every Session holds only a reference to it,
// never its own copy.
//
// Also owns the ReplicationHub every Session on this server shares: any
// lsmdb_server can act as a replication primary for downstream followers
// (a Session becomes a follower by sending SYNC), regardless of whether
// this particular server is itself also a follower of some other primary
// (see `read_only` below and tools/lsmdb_server.cpp's --replica-of
// handling) -- chained replication falls out of this for free, without
// needing to special-case it.
class Server {
 public:
  // port == 0 lets the OS pick an unused port -- call local_port() after
  // construction to find out which one it chose. Useful for tests, which
  // shouldn't hardcode a port number that might already be in use.
  //
  // read_only, when true, makes every Session on this server reject
  // ordinary client PUT/DELETE commands (see protocol.h) -- this is how a
  // Tier 3 replica server ensures its data only ever arrives via
  // replication from its primary, never from a client that connected
  // directly to the wrong server.
  Server(asio::io_context& io_context, unsigned short port, Db& db, bool read_only = false);

  unsigned short local_port() const;

 private:
  void do_accept();

  asio::ip::tcp::acceptor acceptor_;
  Db& db_;
  ReplicationHub replication_hub_;
  bool read_only_;
};

}  // namespace lsmdb::server
