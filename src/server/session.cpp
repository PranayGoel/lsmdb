#include "lsmdb/server/session.h"

#include "lsmdb/server/protocol.h"

#include <istream>

namespace lsmdb::server {

Session::Session(asio::ip::tcp::socket socket, Db& db, ReplicationHub& hub, bool read_only)
    : socket_(std::move(socket)),
      strand_(asio::make_strand(socket_.get_executor())),
      db_(db),
      hub_(hub),
      read_only_(read_only) {}

void Session::start() { read_line(); }

void Session::send_raw(std::shared_ptr<std::string> message) {
  auto self = shared_from_this();
  asio::post(strand_, [this, self, message] {
    asio::async_write(socket_, asio::buffer(*message),
                       [self, message](const asio::error_code&, std::size_t) {
                         // Errors are silently ignored here: if the
                         // follower disconnected, this Session is on its
                         // way to destruction anyway (the read side will
                         // see the same error and stop), and
                         // ReplicationHub::publish() will simply find this
                         // Session's weak_ptr expired on its next call --
                         // no retry logic is warranted for a single missed
                         // replication message given Tier 3's explicit
                         // no-auto-reconnect scope.
                       });
  });
}

void Session::read_line() {
  auto self = shared_from_this();
  asio::async_read_until(
      socket_, buffer_, "\r\n",
      [this, self](const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
        if (ec) return;  // client disconnected, or a real socket error --
                          // either way this Session simply stops here; the
                          // shared_ptr chain ending is what destroys it,
                          // closing the socket via ~tcp::socket.

        std::istream is(&buffer_);
        std::string line;
        std::getline(is, line);
        // getline() stops at the '\n' asio::async_read_until matched on but
        // leaves the preceding '\r' in place (it only strips the final '\n'
        // itself) -- strip it explicitly so dispatch() sees a clean line.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        handle_line(line);
      });
}

void Session::handle_line(const std::string& line) {
  auto self = shared_from_this();

  if (line == "SYNC") {
    // Send a full snapshot of everything the primary currently has, as a
    // sequence of REPLICATE PUT lines -- the exact same wire shape as a
    // live-forwarded write below, so the follower's single read loop needs
    // no special-casing for "snapshot" vs. "ongoing update." Subscribing
    // AFTER queuing the snapshot writes (not before) would risk missing a
    // write that lands in the gap between reading the snapshot and
    // registering as a follower; subscribing FIRST means the worst case is
    // a write that's both in the snapshot and separately published once
    // more -- applying the same PUT/DELETE twice is harmless (idempotent),
    // while missing one outright would not be.
    hub_.subscribe(self);
    for (auto& [key, value] : db_.all_entries()) {
      send_raw(std::make_shared<std::string>("REPLICATE PUT " + key + " " + value + "\r\n"));
    }
    read_line();  // a well-behaved follower sends nothing further, but keep
                  // reading regardless -- harmless either way, and it's what
                  // notices the connection dropping.
    return;
  }

  auto response = std::make_shared<std::string>(dispatch(db_, line, read_only_));
  // Forward to any subscribed followers only once dispatch() confirms the
  // write actually applied (response starts with +OK) -- a rejected or
  // malformed command (including one rejected for being sent to a
  // read-only replica) must never be replicated onward.
  if (is_mutating_command(line) && response->rfind("+OK", 0) == 0) {
    hub_.publish("REPLICATE " + line + "\r\n");
  }

  asio::post(strand_, [this, self, response] {
    asio::async_write(socket_, asio::buffer(*response),
                       [this, self, response](const asio::error_code& ec, std::size_t) {
                         if (ec) return;
                         read_line();
                       });
  });
}

}  // namespace lsmdb::server
