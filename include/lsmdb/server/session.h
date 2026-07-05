#pragma once

#include "lsmdb/db.h"

#include <asio.hpp>
#include <memory>

namespace lsmdb::server {

// One TCP client connection's async read-dispatch-write loop.
//
// Owned via shared_ptr and inherits enable_shared_from_this: this is the
// standard Asio session-lifetime pattern. A bare Session* or reference would
// dangle the moment the function that created it returns, since the actual
// I/O it kicks off completes later, asynchronously, on whichever thread is
// running the io_context -- keeping a shared_ptr to itself alive inside each
// pending async operation's completion handler is what keeps the Session
// (and its socket) alive for exactly as long as there's I/O in flight on it,
// no longer.
class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(asio::ip::tcp::socket socket, Db& db);

  // Kicks off this session's read loop. Must be called on a Session held by
  // shared_ptr (see Server::do_accept) -- calling shared_from_this() before
  // any shared_ptr owns this object is undefined behavior.
  void start();

 private:
  void read_line();
  void handle_line(const std::string& line);

  asio::ip::tcp::socket socket_;
  asio::streambuf buffer_;
  // Referenced, never owned: Server (and therefore Db) outlives every
  // Session for the whole process lifetime, since a Session is only ever
  // destroyed on client disconnect or a socket error, well before shutdown.
  Db& db_;
};

}  // namespace lsmdb::server
