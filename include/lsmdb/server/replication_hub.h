#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lsmdb::server {

class Session;

// Tracks every currently-subscribed replication follower connection on a
// primary server, and broadcasts each mutating client command to all of
// them as it happens. A follower subscribes by sending the SYNC command
// over an ordinary client connection (see Session::handle_line); from that
// point, Session repurposes that same connection into a one-way push stream
// of REPLICATE lines it receives from here.
//
// Thread-safe: subscribe()/publish() can run concurrently from whichever
// io_context thread happens to be handling a given Session's completion
// handler, since the server's io_context typically runs across multiple
// hardware threads (see tools/lsmdb_server.cpp).
class ReplicationHub {
 public:
  void subscribe(std::weak_ptr<Session> follower);

  // Sends `message` (expected to already end in \r\n) to every currently
  // subscribed follower. A follower whose weak_ptr has expired (its Session
  // was destroyed, e.g. on disconnect) is silently dropped from the list --
  // a disconnected follower simply stops receiving further updates, which
  // is the correct, simple behavior for Tier 3's explicitly-scoped
  // no-auto-reconnect design.
  void publish(const std::string& message);

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<Session>> followers_;
};

}  // namespace lsmdb::server
