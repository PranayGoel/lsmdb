#include "lsmdb/server/session.h"

#include "lsmdb/server/protocol.h"

#include <istream>

namespace lsmdb::server {

Session::Session(asio::ip::tcp::socket socket, Db& db) : socket_(std::move(socket)), db_(db) {}

void Session::start() { read_line(); }

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
  // Heap-allocated and kept alive via the completion handler's captured
  // shared_ptr: asio::async_write only requires the buffer stay valid until
  // the write completes, which is after this function returns.
  auto response = std::make_shared<std::string>(dispatch(db_, line));
  asio::async_write(socket_, asio::buffer(*response),
                     [this, self, response](const asio::error_code& ec, std::size_t) {
                       if (ec) return;
                       read_line();  // keep the connection open for the next command
                     });
}

}  // namespace lsmdb::server
