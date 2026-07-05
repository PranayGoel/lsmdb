#pragma once

#include "lsmdb/db.h"
#include "lsmdb/server/replication_hub.h"

#include <asio.hpp>
#include <memory>

namespace lsmdb::server {

// One TCP client connection's async read-dispatch-write loop -- and, if the
// connection ever sends SYNC, a replication follower's one-way push stream
// instead.
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
  Session(asio::ip::tcp::socket socket, Db& db, ReplicationHub& hub, bool read_only);

  // Kicks off this session's read loop. Must be called on a Session held by
  // shared_ptr (see Server::do_accept) -- calling shared_from_this() before
  // any shared_ptr owns this object is undefined behavior.
  void start();

  // Pushes `message` directly to this connection's socket, bypassing the
  // normal read_line()/handle_line() request-response cycle -- how
  // ReplicationHub::publish() forwards a replicated write to a subscribed
  // follower, and how a fresh SYNC subscriber receives its initial
  // snapshot. `message` is expected to already end in \r\n.
  //
  // Dispatched through the write strand (see below) rather than calling
  // asio::async_write directly: once a Session can be a replication
  // follower, a write to its socket can be initiated from two different
  // places -- this Session's own handle_line() on a normal request, or a
  // completely different Session's thread calling into the shared
  // ReplicationHub, which may be running on a different io_context worker
  // thread entirely (tools/lsmdb_server.cpp runs the io_context across
  // every hardware thread). Asio requires a single socket's operations not
  // be initiated concurrently from multiple threads without explicit
  // synchronization; the strand provides exactly that.
  void send_raw(std::shared_ptr<std::string> message);

 private:
  void read_line();
  void handle_line(const std::string& line);

  asio::ip::tcp::socket socket_;
  asio::strand<asio::any_io_executor> strand_;
  asio::streambuf buffer_;
  // Referenced, never owned: Server (and therefore Db and ReplicationHub)
  // outlives every Session for the whole process lifetime, since a Session
  // is only ever destroyed on client disconnect or a socket error, well
  // before shutdown.
  Db& db_;
  ReplicationHub& hub_;
  bool read_only_;
};

}  // namespace lsmdb::server
