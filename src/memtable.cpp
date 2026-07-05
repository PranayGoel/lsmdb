#include "lsmdb/memtable.h"

namespace lsmdb {

void Memtable::put(const std::string& key, const std::string& value) {
  std::unique_lock lock(mutex_);
  auto it = data_.find(key);
  if (it != data_.end()) {
    // Replacing an existing entry (value or tombstone) -- adjust the
    // approximate size by the delta, not just adding the new size, so
    // repeated overwrites of the same key don't make approximate_bytes_
    // drift arbitrarily upward.
    approximate_bytes_ -= key.size() + (it->second ? it->second->size() : 0);
    it->second = value;
  } else {
    data_.emplace(key, value);
  }
  approximate_bytes_ += key.size() + value.size();
}

void Memtable::remove(const std::string& key) {
  std::unique_lock lock(mutex_);
  auto it = data_.find(key);
  if (it != data_.end()) {
    approximate_bytes_ -= key.size() + (it->second ? it->second->size() : 0);
    it->second = std::nullopt;
  } else {
    data_.emplace(key, std::nullopt);
  }
  approximate_bytes_ += key.size();  // tombstones still cost the key's bytes
}

LookupResult Memtable::get(const std::string& key) const {
  std::shared_lock lock(mutex_);
  auto it = data_.find(key);
  if (it == data_.end()) {
    return LookupResult{LookupStatus::kNotFound, ""};
  }
  if (!it->second.has_value()) {
    return LookupResult{LookupStatus::kDeleted, ""};
  }
  return LookupResult{LookupStatus::kFound, *it->second};
}

size_t Memtable::approximate_size_bytes() const {
  std::shared_lock lock(mutex_);
  return approximate_bytes_;
}

size_t Memtable::size() const {
  std::shared_lock lock(mutex_);
  return data_.size();
}

std::vector<MemtableEntry> Memtable::entries() const {
  std::shared_lock lock(mutex_);
  std::vector<MemtableEntry> result;
  result.reserve(data_.size());
  for (const auto& [key, value] : data_) {
    result.push_back(MemtableEntry{key, value});
  }
  return result;
}

void Memtable::clear() {
  std::unique_lock lock(mutex_);
  data_.clear();
  approximate_bytes_ = 0;
}

}  // namespace lsmdb
