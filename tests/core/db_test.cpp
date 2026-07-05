#include "lsmdb/db.h"

#include <catch2/catch_test_macros.hpp>
#include <random>

using lsmdb::Db;

namespace {

std::filesystem::path make_temp_dir() {
  std::random_device rd;
  auto dir = std::filesystem::temp_directory_path() / ("lsmdb_db_test_" + std::to_string(rd()));
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

// Every TEST_CASE below scopes its Db object(s) in a nested block before
// calling remove_all(dir) -- Db holds an open WAL file handle (and, once any
// SSTables exist, open ifstreams on those too) for its whole lifetime, and
// Windows can't delete a file with an open handle on it, unlike POSIX (where
// unlinking an open file just defers the actual removal until the last
// handle closes). Letting Db stay in scope past remove_all "worked" here
// only by accident of POSIX's more permissive semantics.

TEST_CASE("get on a fresh, empty database returns nullopt", "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    REQUIRE_FALSE(db.get("missing").has_value());
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("put then get round-trips a value", "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    db.put("k", "v");
    auto result = db.get("k");
    REQUIRE(result.has_value());
    REQUIRE(*result == "v");
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("remove makes a key subsequently absent, indistinguishable from never-written",
          "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    db.put("k", "v");
    db.remove("k");
    REQUIRE_FALSE(db.get("k").has_value());
  }
  std::filesystem::remove_all(dir);
}

TEST_CASE("range_scan returns matching keys sorted, start inclusive end exclusive", "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    db.put("apple", "1");
    db.put("banana", "2");
    db.put("cherry", "3");
    db.put("date", "4");

    auto results = db.range_scan("banana", "date");  // [banana, date)
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].first == "banana");
    REQUIRE(results[1].first == "cherry");
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("range_scan omits deleted keys", "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir);
    db.put("a", "1");
    db.put("b", "2");
    db.remove("b");

    auto results = db.range_scan("a", "z");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].first == "a");
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("crossing the flush threshold produces a real SSTable and data stays correct",
          "[db]") {
  auto dir = make_temp_dir();
  {
    // A tiny threshold so a handful of writes force at least one flush.
    Db db(dir, /*memtable_flush_threshold_bytes=*/50, /*compaction_sstable_threshold=*/1000);

    for (int i = 0; i < 20; ++i) {
      db.put("key-" + std::to_string(i), "value-" + std::to_string(i));
    }

    REQUIRE(db.sstable_count() >= 1);  // at least one flush actually happened
    for (int i = 0; i < 20; ++i) {
      auto result = db.get("key-" + std::to_string(i));
      REQUIRE(result.has_value());
      REQUIRE(*result == "value-" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("a later put overwrites an earlier flushed value, even after crossing an SSTable boundary",
          "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir, /*memtable_flush_threshold_bytes=*/10, /*compaction_sstable_threshold=*/1000);

    db.put("k", "old");
    // Force a flush by writing enough other data that the threshold is crossed.
    for (int i = 0; i < 5; ++i) db.put("filler-" + std::to_string(i), "x");
    REQUIRE(db.sstable_count() >= 1);

    db.put("k", "new");  // overwrite, now sitting in the fresh memtable
    REQUIRE(*db.get("k") == "new");  // must not read the stale flushed value
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("crossing the compaction threshold reduces the SSTable count while preserving all data",
          "[db]") {
  auto dir = make_temp_dir();
  {
    // Small enough thresholds that a modest number of writes forces several
    // flushes, which in turn crosses the compaction threshold.
    Db db(dir, /*memtable_flush_threshold_bytes=*/30, /*compaction_sstable_threshold=*/3);

    for (int i = 0; i < 50; ++i) {
      db.put("key-" + std::to_string(i), "value-" + std::to_string(i));
    }

    REQUIRE(db.sstable_count() < 3);  // compaction actually ran and collapsed the set
    for (int i = 0; i < 50; ++i) {
      auto result = db.get("key-" + std::to_string(i));
      REQUIRE(result.has_value());
      REQUIRE(*result == "value-" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("reopening a Db on the same directory after a clean shutdown recovers all data",
          "[db]") {
  auto dir = make_temp_dir();
  {
    Db db(dir, /*memtable_flush_threshold_bytes=*/40, /*compaction_sstable_threshold=*/2);
    for (int i = 0; i < 30; ++i) {
      db.put("key-" + std::to_string(i), "value-" + std::to_string(i));
    }
    db.remove("key-5");
  }  // Db destructs here -- no explicit "save" step exists or is needed,
     // since every put/remove was already durable (via WAL fsync, or an
     // SSTable file) by the time the call returned.

  {
    Db reopened(dir);
    for (int i = 0; i < 30; ++i) {
      if (i == 5) {
        REQUIRE_FALSE(reopened.get("key-5").has_value());
        continue;
      }
      auto result = reopened.get("key-" + std::to_string(i));
      REQUIRE(result.has_value());
      REQUIRE(*result == "value-" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("data still sitting only in the WAL (never flushed) survives a simulated crash",
          "[db]") {
  // Simulates the "process died before ever flushing" case entirely within
  // this test binary: a huge flush threshold guarantees the writes below
  // stay in the memtable only, backed solely by the WAL, when the first Db
  // instance goes out of scope. (tests/core/crash_recovery_test.cpp covers
  // the stronger, real-process-kill version of this same guarantee.)
  auto dir = make_temp_dir();
  {
    Db db(dir, /*memtable_flush_threshold_bytes=*/1024ull * 1024, 1000);
    for (int i = 0; i < 10; ++i) {
      db.put("wal-only-" + std::to_string(i), "v" + std::to_string(i));
    }
    REQUIRE(db.sstable_count() == 0);  // confirms this data is WAL-only, not yet flushed
  }

  {
    Db recovered(dir);
    for (int i = 0; i < 10; ++i) {
      auto result = recovered.get("wal-only-" + std::to_string(i));
      REQUIRE(result.has_value());
      REQUIRE(*result == "v" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(dir);
}
