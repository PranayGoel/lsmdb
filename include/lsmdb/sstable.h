#pragma once

#include "lsmdb/bloom_filter.h"
#include "lsmdb/memtable.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace lsmdb {

// An immutable, sorted, on-disk file produced by flushing a full memtable
// (or by compaction merging several existing SSTables into one). "Immutable"
// is the key property: once written, an SSTable file is never modified in
// place -- only ever read, or eventually replaced wholesale by a compaction
// that merges it (and others) into a brand new file. This is what makes
// concurrent reads trivially safe without per-read locking against writers.
//
// On-disk layout (all integers little-endian):
//   [data section]   sorted entries: key_len(4) key tombstone(1) value_len(4) value
//   [bloom section]  BloomFilter::to_bytes() output, sized for this SSTable's entry count
//   [index section]  entry_count(4), then per entry: key_len(4) key offset(8) length(8)
//                     (offset/length point at that entry's bytes in the data section)
//   [footer, fixed 28 bytes]  data_offset(8) bloom_offset(8) index_offset(8) magic(4)
//
// The index holds every key (a "full index," not RocksDB/LevelDB's sparse
// block-index-plus-restart-points design) -- deliberately simpler, correct,
// and entirely adequate at the data volumes this project targets; a sparse
// index trading a bit of lookup precision for a much smaller in-memory
// footprint is the natural next step if this needs to scale to SSTables with
// many millions of keys. Documented as a scope choice, not an oversight.
struct SSTableEntry {
  std::string key;
  bool is_tombstone;
  std::string value;  // empty and unused when is_tombstone
};

class SSTable {
 public:
  // Writes a brand-new SSTable file from already-sorted entries (as produced
  // by Memtable::entries(), or by compaction's merge step). Throws if
  // `entries` isn't sorted by key -- callers are expected to guarantee this,
  // since re-sorting here would hide a caller bug instead of surfacing it.
  static void create(const std::filesystem::path& path,
                      const std::vector<MemtableEntry>& entries);

  // Opens an existing SSTable file: reads the footer, loads the bloom filter
  // and full index into memory, keeps the data section on disk (read on
  // demand via get()/read_all()).
  explicit SSTable(std::filesystem::path path);

  // Looks up a single key. kNotFound here means "not in this SSTable" --
  // exactly like Memtable::get(), the caller is responsible for falling
  // through to older SSTables. The bloom filter check happens first and
  // avoids ever touching the data section on disk for a key that's
  // definitely absent.
  LookupResult get(const std::string& key) const;

  // Reads every entry from the data section, in sorted order, including
  // tombstones. Used by compaction's merge step. Loads the whole data
  // section into memory at once -- simple and correct at this project's
  // target scale; a real system would stream this instead for SSTables too
  // large to fit in memory, same tradeoff as the full-index decision above.
  std::vector<SSTableEntry> read_all() const;

  const std::filesystem::path& path() const { return path_; }
  size_t entry_count() const { return index_.size(); }

 private:
  struct IndexEntry {
    std::string key;
    uint64_t offset;
    uint64_t length;
  };

  std::filesystem::path path_;
  BloomFilter bloom_;
  std::vector<IndexEntry> index_;  // sorted by key -- binary-searched in get()
  mutable std::ifstream in_;
  mutable std::mutex read_mutex_;  // serializes seeks on the single shared
                                    // ifstream; see DESIGN.md for why a mutex
                                    // (not per-call reopen or pread) was the
                                    // chosen tradeoff here
};

}  // namespace lsmdb
