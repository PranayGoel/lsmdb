#pragma once

#include "lsmdb/memtable.h"
#include "lsmdb/sstable.h"
#include "lsmdb/wal.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lsmdb {

// Ties the write-ahead log, memtable, SSTables, and compaction together into
// the actual Put/Get/Delete/RangeScan API a user of this database calls.
// This is also where the crash-recovery guarantee the whole project is built
// around actually gets exercised end-to-end: opening a Db on a directory
// that already has data replays its WAL and loads its existing SSTables,
// reconstructing exactly the state that existed before the process last
// stopped (cleanly or otherwise).
//
// Concurrency model: one mutex guards every operation (put/remove/get/
// range_scan/the internal flush and compaction triggers). This is a
// deliberately simple, coarse-grained choice -- correct and easy to reason
// about, at the cost of serializing all access rather than allowing, say,
// concurrent reads during a write. A finer-grained design (e.g. relying on
// Memtable's own internal shared_mutex for concurrent reads, with narrower
// locking just around the SSTable list and flush/compaction bookkeeping) is
// the natural next step if throughput under concurrent load ever becomes the
// bottleneck; Tier 2's networked server is what will actually exercise this
// under real concurrent client connections and is where that tradeoff would
// first start to matter in practice.
class Db {
 public:
  // Opens (or creates) a database rooted at `directory`. If the directory
  // already contains a WAL and/or SSTables from a previous run, replays and
  // loads them -- this constructor IS the crash-recovery path, not a
  // separate mode.
  explicit Db(std::filesystem::path directory, size_t memtable_flush_threshold_bytes = 4 * 1024 * 1024,
              size_t compaction_sstable_threshold = 4);

  void put(const std::string& key, const std::string& value);
  void remove(const std::string& key);

  // Returns std::nullopt for both "never written" and "deleted" -- the
  // public API deliberately doesn't distinguish those two internally-tracked
  // states (LookupStatus::kNotFound vs kDeleted); from a caller's point of
  // view, a deleted key not existing is exactly the same outcome as a key
  // that was never written.
  std::optional<std::string> get(const std::string& key) const;

  // Inclusive of `start`, exclusive of `end`, matching common range-query
  // convention (e.g. Python slicing, many database range-scan APIs). Merges
  // the memtable and every SSTable, newest-wins on overlapping keys, and
  // omits deleted keys from the result -- a caller never sees a tombstone.
  std::vector<std::pair<std::string, std::string>> range_scan(const std::string& start,
                                                                const std::string& end) const;

  // Exposed for tests/diagnostics -- not part of the normal usage flow.
  size_t sstable_count() const;

 private:
  std::filesystem::path dir_;
  size_t flush_threshold_;
  size_t compaction_threshold_;
  size_t next_sstable_id_;

  mutable std::mutex mutex_;
  std::unique_ptr<WriteAheadLog> wal_;
  Memtable memtable_;
  // Ordered oldest-to-newest -- matches compact()'s required input order and
  // is also exactly the order get()/range_scan() must search in *reverse*
  // (newest first) to correctly resolve overlapping keys.
  std::vector<std::shared_ptr<SSTable>> sstables_;

  void recover();  // called once, from the constructor
  void maybe_flush_locked();      // call with mutex_ already held
  void maybe_compact_locked();    // call with mutex_ already held
  std::filesystem::path wal_path() const;
  std::filesystem::path sstable_path(size_t id) const;
};

}  // namespace lsmdb
