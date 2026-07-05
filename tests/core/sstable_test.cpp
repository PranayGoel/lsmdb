#include "lsmdb/sstable.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <random>

using lsmdb::LookupStatus;
using lsmdb::MemtableEntry;
using lsmdb::SSTable;

namespace {

std::filesystem::path make_temp_path() {
  std::random_device rd;
  return std::filesystem::temp_directory_path() /
         ("lsmdb_sstable_test_" + std::to_string(rd()) + ".sst");
}

}  // namespace

TEST_CASE("create rejects unsorted input instead of silently reordering it", "[sstable]") {
  auto path = make_temp_path();
  std::vector<MemtableEntry> entries = {
      {"zebra", "1"},
      {"apple", "2"},  // out of order
  };
  REQUIRE_THROWS_AS(SSTable::create(path, entries), std::runtime_error);
}

TEST_CASE("an empty SSTable opens fine and reports every key absent", "[sstable]") {
  auto path = make_temp_path();
  SSTable::create(path, {});
  SSTable table(path);
  REQUIRE(table.entry_count() == 0);
  REQUIRE(table.get("anything").status == LookupStatus::kNotFound);
  std::filesystem::remove(path);
}

TEST_CASE("get returns kFound with the right value for present keys, kNotFound for absent",
          "[sstable]") {
  auto path = make_temp_path();
  std::vector<MemtableEntry> entries = {
      {"apple", std::string("red")},
      {"banana", std::string("yellow")},
      {"cherry", std::string("dark-red")},
  };
  SSTable::create(path, entries);

  SSTable table(path);
  REQUIRE(table.entry_count() == 3);

  auto apple = table.get("apple");
  REQUIRE(apple.status == LookupStatus::kFound);
  REQUIRE(apple.value == "red");

  auto banana = table.get("banana");
  REQUIRE(banana.status == LookupStatus::kFound);
  REQUIRE(banana.value == "yellow");

  REQUIRE(table.get("definitely-not-present-xyz").status == LookupStatus::kNotFound);

  std::filesystem::remove(path);
}

TEST_CASE("a tombstone entry persists and reports kDeleted, not kNotFound", "[sstable]") {
  auto path = make_temp_path();
  std::vector<MemtableEntry> entries = {
      {"alive", std::string("v1")},
      {"dead", std::nullopt},  // tombstone
  };
  SSTable::create(path, entries);

  SSTable table(path);
  REQUIRE(table.get("alive").status == LookupStatus::kFound);
  REQUIRE(table.get("dead").status == LookupStatus::kDeleted);  // not kNotFound

  std::filesystem::remove(path);
}

TEST_CASE("read_all returns every entry in sorted order, tombstones included", "[sstable]") {
  auto path = make_temp_path();
  std::vector<MemtableEntry> entries = {
      {"a", std::string("1")},
      {"b", std::nullopt},
      {"c", std::string("3")},
  };
  SSTable::create(path, entries);

  SSTable table(path);
  auto all = table.read_all();
  REQUIRE(all.size() == 3);
  REQUIRE(all[0].key == "a");
  REQUIRE_FALSE(all[0].is_tombstone);
  REQUIRE(all[0].value == "1");
  REQUIRE(all[1].key == "b");
  REQUIRE(all[1].is_tombstone);
  REQUIRE(all[2].key == "c");
  REQUIRE(all[2].value == "3");

  std::filesystem::remove(path);
}

TEST_CASE("opening a file with a bad magic number fails loudly, not silently", "[sstable]") {
  auto path = make_temp_path();
  {
    // 28 bytes of garbage -- exactly kFooterSize, so it passes the
    // size check but fails the magic-number check specifically.
    std::ofstream out(path, std::ios::binary);
    std::string garbage(28, 'x');
    out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
  REQUIRE_THROWS_AS(SSTable{path}, std::runtime_error);
  std::filesystem::remove(path);
}

TEST_CASE("opening a file too small to hold a footer fails loudly", "[sstable]") {
  auto path = make_temp_path();
  {
    std::ofstream out(path, std::ios::binary);
    out << "too short";
  }
  REQUIRE_THROWS_AS(SSTable{path}, std::runtime_error);
  std::filesystem::remove(path);
}

namespace {
// Zero-padded so lexicographic string order matches numeric order (plain
// "key-" + to_string(i) sorts "key-10" before "key-2" -- a real mistake this
// exact helper was added to avoid after hitting it in the first version of
// this test, which SSTable::create's own sorted-input check correctly caught).
std::string padded_key(int i) {
  std::string n = std::to_string(i);
  return "key-" + std::string(4 - n.size(), '0') + n;
}
}  // namespace

TEST_CASE("a large SSTable's bloom filter correctly skips most absent-key lookups", "[sstable]") {
  auto path = make_temp_path();
  std::vector<MemtableEntry> entries;
  for (int i = 0; i < 1000; ++i) {
    entries.push_back({padded_key(i), std::string("value-" + std::to_string(i))});
  }
  SSTable::create(path, entries);
  SSTable table(path);

  // Every inserted key must be found -- correctness, not just "usually works."
  for (int i = 0; i < 1000; ++i) {
    auto result = table.get(padded_key(i));
    REQUIRE(result.status == LookupStatus::kFound);
    REQUIRE(result.value == "value-" + std::to_string(i));
  }
  // And absent keys must still correctly report kNotFound, regardless of
  // whether the bloom filter flagged them as a possible false positive --
  // the index check after the bloom check is what guarantees this.
  for (int i = 1000; i < 1010; ++i) {
    REQUIRE(table.get("key-" + std::to_string(i)).status == LookupStatus::kNotFound);
  }

  std::filesystem::remove(path);
}
