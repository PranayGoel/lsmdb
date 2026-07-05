#include "lsmdb/server/replication_client.h"

#include "lsmdb/server/protocol.h"

#include <iostream>
#include <istream>

namespace lsmdb::server {

ReplicationClient::ReplicationClient(asio::io_context& io_context, std::string primary_host,
                                      std::string primary_port, Db& local_db)
    : socket_(io_context),
      primary_host_(std::move(primary_host)),
      primary_port_(std::move(primary_port)),
      local_db_(local_db) {}

void ReplicationClient::start() {
  auto self = shared_from_this();
  auto resolver = std::make_shared<asio::ip::tcp::resolver>(socket_.get_executor());
  resolver->async_resolve(
      primary_host_, primary_port_,
      [this, self, resolver](const asio::error_code& ec,
                              const asio::ip::tcp::resolver::results_type& endpoints) {
        if (ec) {
          std::cerr << "lsmdb_server: replication: failed to resolve primary " << primary_host_
                     << ":" << primary_port_ << ": " << ec.message() << std::endl;
          return;
        }
        asio::async_connect(
            socket_, endpoints,
            [this, self](const asio::error_code& connect_ec, const asio::ip::tcp::endpoint&) {
              if (connect_ec) {
                std::cerr << "lsmdb_server: replication: failed to connect to primary "
                          << primary_host_ << ":" << primary_port_ << ": "
                          << connect_ec.message() << std::endl;
                return;
              }
              auto sync_command = std::make_shared<std::string>("SYNC\r\n");
              asio::async_write(socket_, asio::buffer(*sync_command),
                                 [this, self, sync_command](const asio::error_code& write_ec,
                                                             std::size_t) {
                                   if (write_ec) return;
                                   read_line();
                                 });
            });
      });
}

void ReplicationClient::read_line() {
  auto self = shared_from_this();
  asio::async_read_until(
      socket_, buffer_, "\r\n", [this, self](const asio::error_code& ec, std::size_t) {
        if (ec) {
          std::cerr << "lsmdb_server: replication: connection to primary " << primary_host_ << ":"
                     << primary_port_ << " lost: " << ec.message() << std::endl;
          return;  // no auto-reconnect -- manual promotion only, per Tier 3's
                    // explicitly documented scope.
        }

        std::istream is(&buffer_);
        std::string line;
        std::getline(is, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Expected shape: "REPLICATE PUT key value" or "REPLICATE DELETE key".
        // Anything else on this connection is unexpected and simply
        // ignored -- a well-behaved primary never sends anything else here.
        const std::string prefix = "REPLICATE ";
        if (line.rfind(prefix, 0) == 0) {
          // Applied through the ordinary dispatch() path (read_only left at
          // its default of false), the exact same route a local client
          // write would take -- this is what makes the resulting local
          // write durable via the follower's own WAL/SSTables, not just an
          // in-memory update. The response text dispatch() returns is
          // discarded: there is no client on the other end of THIS
          // connection to send it to.
          dispatch(local_db_, line.substr(prefix.size()));
        }

        read_line();
      });
}

}  // namespace lsmdb::server
