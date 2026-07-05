#pragma once

#include "lsmdb/db.h"

#include <asio.hpp>

namespace lsmdb::server {

// Accepts incoming TCP connections and hands each one off to its own
// Session. Owns nothing about the database itself -- Db is constructed and
// owned by the caller (main() in tools/lsmdb_server.cpp, or a test) for the
// whole process/test lifetime; every Session holds only a reference to it,
// never its own copy.
class Server {
 public:
  // port == 0 lets the OS pick an unused port -- call local_port() after
  // construction to find out which one it chose. Useful for tests, which
  // shouldn't hardcode a port number that might already be in use.
  Server(asio::io_context& io_context, unsigned short port, Db& db);

  unsigned short local_port() const;

 private:
  void do_accept();

  asio::ip::tcp::acceptor acceptor_;
  Db& db_;
};

}  // namespace lsmdb::server
