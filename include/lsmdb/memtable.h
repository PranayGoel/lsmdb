#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace lsmdb {

enum class LookupStatus { kNotFound, kDeleted, kFound };

struct LookupResult {
  LookupStatus status;
  std::string value;  // meaningful only when status == kFound
};

struct MemtableEntry {
  std::string key;
  std::optional<std::string> value;  // nullopt = tombstone (deleted key)
};

// The in-memory, always-sorted buffer new writes land in before they're ever
// written to an SSTable. Backed by std::map (a red-black tree) rather than a
// skip list: both give O(log n) put/get and sorted iteration, and std::map is
// the standard-library, already-correct choice -- a skip list is the more
// common textbook LSM-tree implementation choice mainly because it's easier
// to make *lock-free* for high-concurrency writes, which this project's
// simpler shared_mutex-based locking doesn't need. Documented here as a
// deliberate scope tradeoff, not an oversight: if lock-free concurrent writes
// become a real requirement, a skip list is the natural next step.
//
// A deleted key is stored as a tombstone (value = std::nullopt), not erased
// outright -- an erased key would be indistinguishable from a key that was
// never written, so a Get() for it would incorrectly fall through to check
// older SSTables and might resurrect a stale value that was deleted more
// recently than the SSTable's data. The tombstone has to survive at least
// until it's been flushed to an SSTable and compaction has had a chance to
// discard it once no older data for that key can remain.
class Memtable {
 public:
  void put(const std::string& key, const std::string& value);
  void remove(const std::string& key);

  // Only reports what's in *this* memtable -- kNotFound here does not mean
  // the key doesn't exist in the database overall; the caller (Db, once it
  // exists) is responsible for falling through to SSTables on kNotFound.
  LookupResult get(const std::string& key) const;

  // Approximate size in bytes (sum of key+value lengths seen), used to decide
  // when the memtable is full enough to flush to an SSTable. Deliberately
  // approximate -- std::map's actual per-node memory overhead isn't counted,
  // since the goal is "flush at a sane threshold," not an exact accounting.
  size_t approximate_size_bytes() const;

  size_t size() const;

  // All entries in sorted key order, tombstones included -- this is exactly
  // what gets written out when flushing to an SSTable (which must also
  // persist tombstones, so older SSTables' data for the same key stays
  // correctly shadowed until compaction can safely drop it).
  std::vector<MemtableEntry> entries() const;

  // Empties the memtable and resets the size counter -- called by Db once
  // this memtable's contents have been durably written out to a new SSTable,
  // so the same Memtable object can keep accepting new writes afterward
  // rather than Db needing to construct (and coordinate swapping in) a
  // brand-new one.
  void clear();

 private:
  mutable std::shared_mutex mutex_;
  std::map<std::string, std::optional<std::string>> data_;
  size_t approximate_bytes_ = 0;
};

}  // namespace lsmdb
