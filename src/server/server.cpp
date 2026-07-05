#include "lsmdb/server/server.h"

#include "lsmdb/server/session.h"

namespace lsmdb::server {

Server::Server(asio::io_context& io_context, unsigned short port, Db& db)
    : acceptor_(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)), db_(db) {
  do_accept();
}

unsigned short Server::local_port() const { return acceptor_.local_endpoint().port(); }

void Server::do_accept() {
  acceptor_.async_accept([this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
    if (!ec) {
      std::make_shared<Session>(std::move(socket), db_)->start();
    }
    // Keep accepting regardless of this one connection's outcome -- one
    // failed/dropped accept shouldn't stop the server from serving anyone
    // else.
    do_accept();
  });
}

}  // namespace lsmdb::server
