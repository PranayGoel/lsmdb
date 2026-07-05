#include "lsmdb/server/protocol.h"

namespace lsmdb::server {

namespace {

std::string ok() { return "+OK\r\n"; }
std::string err(const std::string& message) { return "-ERR " + message + "\r\n"; }

}  // namespace

std::string dispatch(Db& db, const std::string& line) {
  auto first_space = line.find(' ');
  std::string verb = line.substr(0, first_space);
  std::string rest = (first_space == std::string::npos) ? "" : line.substr(first_space + 1);

  if (verb == "PING") {
    return "+PONG\r\n";
  }

  if (verb == "GET") {
    if (rest.empty()) return err("GET requires a key");
    auto result = db.get(rest);
    if (!result.has_value()) return "$-1\r\n";
    return "+" + *result + "\r\n";
  }

  if (verb == "DELETE") {
    if (rest.empty()) return err("DELETE requires a key");
    db.remove(rest);
    return ok();
  }

  if (verb == "PUT") {
    auto key_end = rest.find(' ');
    if (key_end == std::string::npos) return err("PUT requires a key and a value");
    std::string key = rest.substr(0, key_end);
    if (key.empty()) return err("PUT requires a non-empty key");
    std::string value = rest.substr(key_end + 1);
    db.put(key, value);
    return ok();
  }

  return err("unknown command '" + verb + "'");
}

}  // namespace lsmdb::server
