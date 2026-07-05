#include "lsmdb/memtable.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

using lsmdb::LookupStatus;
using lsmdb::Memtable;

TEST_CASE("get on an empty memtable returns kNotFound", "[memtable]") {
  Memtable m;
  auto result = m.get("missing");
  REQUIRE(result.status == LookupStatus::kNotFound);
}

TEST_CASE("put then get returns the stored value", "[memtable]") {
  Memtable m;
  m.put("k", "v1");
  auto result = m.get("k");
  REQUIRE(result.status == LookupStatus::kFound);
  REQUIRE(result.value == "v1");
}

TEST_CASE("a later put overwrites an earlier one for the same key", "[memtable]") {
  Memtable m;
  m.put("k", "v1");
  m.put("k", "v2");
  REQUIRE(m.get("k").value == "v2");
  REQUIRE(m.size() == 1);  // still one logical entry, not two
}

TEST_CASE("remove creates a tombstone distinguishable from never-written", "[memtable]") {
  Memtable m;
  m.put("k", "v1");
  m.remove("k");
  auto result = m.get("k");
  REQUIRE(result.status == LookupStatus::kDeleted);  // NOT kNotFound

  auto never_written = m.get("never-written-key");
  REQUIRE(never_written.status == LookupStatus::kNotFound);
}

TEST_CASE("removing a key that was never written still creates a tombstone", "[memtable]") {
  // This matters: if an older SSTable has a value for this key, the memtable
  // needs its own tombstone recorded so a later flush correctly shadows that
  // older on-disk value, even though this memtable never saw a Put for it.
  Memtable m;
  m.remove("k");
  REQUIRE(m.get("k").status == LookupStatus::kDeleted);
}

TEST_CASE("entries() returns everything in sorted key order, tombstones included",
          "[memtable]") {
  Memtable m;
  m.put("banana", "yellow");
  m.put("apple", "red");
  m.remove("cherry");
  m.put("cherry", "dark-red");  // re-added after the tombstone
  m.remove("date");

  auto entries = m.entries();
  REQUIRE(entries.size() == 4);
  REQUIRE(entries[0].key == "apple");
  REQUIRE(entries[1].key == "banana");
  REQUIRE(entries[2].key == "cherry");
  REQUIRE(entries[2].value.has_value());
  REQUIRE(*entries[2].value == "dark-red");
  REQUIRE(entries[3].key == "date");
  REQUIRE(!entries[3].value.has_value());  // tombstone
}

TEST_CASE("approximate_size_bytes grows on new keys and doesn't double-count overwrites",
          "[memtable]") {
  Memtable m;
  REQUIRE(m.approximate_size_bytes() == 0);
  m.put("ab", "xyz");  // 2 + 3 = 5 bytes
  REQUIRE(m.approximate_size_bytes() == 5);
  m.put("ab", "x");  // overwrite with a shorter value -- size should shrink, not grow
  REQUIRE(m.approximate_size_bytes() == 3);
}

TEST_CASE("concurrent puts from multiple threads all land correctly", "[memtable]") {
  // Exercises the shared_mutex under real contention -- run under TSan in CI
  // specifically to catch a data race here, not just to check the final count.
  Memtable m;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&m, t] {
      for (int i = 0; i < kPerThread; ++i) {
        m.put("t" + std::to_string(t) + "-" + std::to_string(i), "v");
      }
    });
  }
  for (auto& th : threads) th.join();

  REQUIRE(m.size() == static_cast<size_t>(kThreads * kPerThread));
}
