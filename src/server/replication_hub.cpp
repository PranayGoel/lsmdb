#include "lsmdb/server/replication_hub.h"

#include "lsmdb/server/session.h"

namespace lsmdb::server {

void ReplicationHub::subscribe(std::weak_ptr<Session> follower) {
  std::lock_guard<std::mutex> lock(mutex_);
  followers_.push_back(std::move(follower));
}

void ReplicationHub::publish(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto shared_message = std::make_shared<std::string>(message);

  std::vector<std::weak_ptr<Session>> still_alive;
  still_alive.reserve(followers_.size());
  for (auto& weak_follower : followers_) {
    if (auto follower = weak_follower.lock()) {
      follower->send_raw(shared_message);
      still_alive.push_back(std::move(weak_follower));
    }
    // else: expired -- the follower disconnected; drop it by simply not
    // adding it to still_alive.
  }
  followers_ = std::move(still_alive);
}

}  // namespace lsmdb::server
