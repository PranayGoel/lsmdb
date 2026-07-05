#include "lsmdb/wal.h"

#include "lsmdb/platform/file_sync.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <random>

using lsmdb::RecordType;
using lsmdb::WalRecord;
using lsmdb::WriteAheadLog;

namespace {

std::filesystem::path make_temp_path() {
  std::random_device rd;
  return std::filesystem::temp_directory_path() /
         ("lsmdb_wal_test_" + std::to_string(rd()) + ".wal");
}

}  // namespace

TEST_CASE("replay of a nonexistent file returns empty, not an error", "[wal]") {
  auto path = make_temp_path();  // never created
  REQUIRE(WriteAheadLog::replay(path).empty());
}

TEST_CASE("append then replay round-trips Put and Delete records in order", "[wal]") {
  auto path = make_temp_path();
  {
    WriteAheadLog wal(path);
    wal.append(WalRecord{RecordType::kPut, "k1", "v1"});
    wal.append(WalRecord{RecordType::kPut, "k2", "v2"});
    wal.append(WalRecord{RecordType::kDelete, "k1", ""});
  }

  auto records = WriteAheadLog::replay(path);
  REQUIRE(records.size() == 3);
  REQUIRE(records[0].type == RecordType::kPut);
  REQUIRE(records[0].key == "k1");
  REQUIRE(records[0].value == "v1");
  REQUIRE(records[1].key == "k2");
  REQUIRE(records[2].type == RecordType::kDelete);
  REQUIRE(records[2].key == "k1");

  lsmdb::platform::remove_file(path);
}

TEST_CASE("append survives and replays an empty value", "[wal]") {
  auto path = make_temp_path();
  {
    WriteAheadLog wal(path);
    wal.append(WalRecord{RecordType::kPut, "empty-value-key", ""});
  }
  auto records = WriteAheadLog::replay(path);
  REQUIRE(records.size() == 1);
  REQUIRE(records[0].value.empty());
  lsmdb::platform::remove_file(path);
}

TEST_CASE("a WAL reopened on the same path appends after existing records", "[wal]") {
  auto path = make_temp_path();
  {
    WriteAheadLog wal(path);
    wal.append(WalRecord{RecordType::kPut, "a", "1"});
  }
  {
    WriteAheadLog wal(path);  // simulates process restart, same file
    wal.append(WalRecord{RecordType::kPut, "b", "2"});
  }
  auto records = WriteAheadLog::replay(path);
  REQUIRE(records.size() == 2);
  REQUIRE(records[0].key == "a");
  REQUIRE(records[1].key == "b");
  lsmdb::platform::remove_file(path);
}

TEST_CASE("reset() discards prior records so replay comes back empty", "[wal]") {
  auto path = make_temp_path();
  {
    // Scoped so `wal`'s open file handle (held for its whole lifetime, even
    // across reset()) closes before remove_file runs below -- Windows can't
    // delete a file with an open handle on it, unlike POSIX.
    WriteAheadLog wal(path);
    wal.append(WalRecord{RecordType::kPut, "k", "v"});
    REQUIRE(WriteAheadLog::replay(path).size() == 1);

    wal.reset();
    REQUIRE(WriteAheadLog::replay(path).empty());

    // and the log is still usable after reset
    wal.append(WalRecord{RecordType::kPut, "k2", "v2"});
    auto records = WriteAheadLog::replay(path);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].key == "k2");
  }

  lsmdb::platform::remove_file(path);
}

TEST_CASE(
    "replay recovers every complete record and cleanly discards a torn tail record "
    "-- this is the actual crash-recovery guarantee the whole engine depends on",
    "[wal]") {
  auto path = make_temp_path();
  {
    WriteAheadLog wal(path);
    wal.append(WalRecord{RecordType::kPut, "safe1", "value1"});
    wal.append(WalRecord{RecordType::kPut, "safe2", "value2"});
  }

  // Simulate a crash mid-append: append a third record's bytes directly to
  // the file, then truncate it partway through -- exactly what a process
  // dying mid-write() leaves behind. We don't go through WriteAheadLog::append
  // here on purpose, since that always writes a complete, valid record; we
  // need to construct the specific "torn write" shape by hand.
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    std::string garbage = "\x01\x05\x00\x00\x00truncated-key-but-no-value-or-crc";
    out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }

  auto records = WriteAheadLog::replay(path);
  REQUIRE(records.size() == 2);  // exactly the two complete records survive
  REQUIRE(records[0].key == "safe1");
  REQUIRE(records[1].key == "safe2");

  lsmdb::platform::remove_file(path);
}

TEST_CASE("replay rejects a record whose bytes were corrupted after writing (CRC mismatch)",
          "[wal]") {
  auto path = make_temp_path();
  {
    WriteAheadLog wal(path);
    wal.append(WalRecord{RecordType::kPut, "k", "v"});
  }

  // Flip a byte inside the key to simulate on-disk bit rot / a torn write
  // that happened to leave a length-plausible but content-corrupted record.
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(5);  // byte offset into the key ("k"), well past the header fields
    char corrupted = 'X';
    f.write(&corrupted, 1);
  }

  auto records = WriteAheadLog::replay(path);
  REQUIRE(records.empty());  // CRC mismatch -- the corrupted record is discarded, not trusted

  lsmdb::platform::remove_file(path);
}
