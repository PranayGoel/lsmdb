#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace lsmdb {

enum class RecordType : uint8_t { kPut = 1, kDelete = 2 };

struct WalRecord {
  RecordType type;
  std::string key;
  std::string value;  // empty and unused for kDelete
};

// Write-Ahead Log: every Put/Delete is appended here, durably (fsync'd via
// platform::sync_file), *before* it's applied to the in-memory memtable --
// this is the actual durability guarantee the whole engine depends on. If the
// process crashes between "wrote to memtable" and "flushed memtable to an
// SSTable," replaying this log on startup reconstructs exactly the memtable
// state that existed right before the crash. Once a memtable is successfully
// flushed to an SSTable, its WAL is no longer needed for recovery and gets
// reset (see reset()) -- this is what keeps the WAL from growing forever.
//
// On-disk record format (all integers little-endian, native to x86/ARM):
//   [1 byte type][4 byte key_len][key_len bytes key]
//   [4 byte value_len][value_len bytes value][4 byte crc32]
// The CRC32 covers everything before it in the record. On replay, a length
// that doesn't leave enough bytes for the rest of the record, OR a CRC that
// doesn't match, means we've hit a torn write from a crash mid-append --
// replay stops there and returns everything successfully validated before
// it. This mirrors how real WAL implementations (e.g. RocksDB's) handle a
// truncated/corrupted tail record: it is not an error, it is *the expected
// shape of a crash*, and the correct behavior is to discard the incomplete
// tail, not to throw.
class WriteAheadLog {
 public:
  explicit WriteAheadLog(std::filesystem::path path);

  // Appends one record, durably, before returning -- the caller may safely
  // apply the record to the memtable immediately after this returns.
  void append(const WalRecord& record);

  // Replays every valid record currently in the log file, in the order they
  // were appended. Used both at startup (crash recovery) and by tests.
  // Returns an empty vector if the file doesn't exist yet (a fresh DB).
  static std::vector<WalRecord> replay(const std::filesystem::path& path);

  // Discards the log's contents and starts fresh -- called after the
  // memtable built from this log has been successfully flushed to an
  // SSTable, since the WAL is no longer needed to recover that data.
  void reset();

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  std::ofstream out_;
  std::mutex mutex_;  // serializes concurrent appends (the networked server
                      // in Tier 2 will call append() from multiple threads)

  void open_for_append();
};

}  // namespace lsmdb
