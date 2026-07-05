#include "lsmdb/compaction.h"

#include "lsmdb/platform/file_sync.h"

#include <catch2/catch_test_macros.hpp>
#include <random>

using lsmdb::LookupStatus;
using lsmdb::MemtableEntry;
using lsmdb::SSTable;
using lsmdb::compact;

namespace {

std::filesystem::path make_temp_path(const std::string& suffix) {
  std::random_device rd;
  return std::filesystem::temp_directory_path() /
         ("lsmdb_compaction_test_" + std::to_string(rd()) + suffix);
}

}  // namespace

TEST_CASE("compacting non-overlapping SSTables merges all entries, sorted", "[compaction]") {
  auto p1 = make_temp_path("_1.sst");
  auto p2 = make_temp_path("_2.sst");
  auto out = make_temp_path("_out.sst");

  SSTable::create(p1, {{"apple", std::string("red")}, {"cherry", std::string("dark-red")}});
  SSTable::create(p2, {{"banana", std::string("yellow")}, {"date", std::string("brown")}});

  compact({p1, p2}, out);
  {
    // Scoped so `result`'s internally-held ifstream closes before the
    // remove_file calls below -- Windows can't delete a file with an open
    // handle on it, unlike POSIX.
    SSTable result(out);
    REQUIRE(result.entry_count() == 4);
    REQUIRE(result.get("apple").value == "red");
    REQUIRE(result.get("banana").value == "yellow");
    REQUIRE(result.get("cherry").value == "dark-red");
    REQUIRE(result.get("date").value == "brown");
  }

  for (auto& p : {p1, p2, out}) lsmdb::platform::remove_file(p);
}

TEST_CASE("when a key exists in both, the entry from the newer (later-in-list) SSTable wins",
          "[compaction]") {
  auto older = make_temp_path("_older.sst");
  auto newer = make_temp_path("_newer.sst");
  auto out = make_temp_path("_out.sst");

  SSTable::create(older, {{"k", std::string("old-value")}});
  SSTable::create(newer, {{"k", std::string("new-value")}});

  compact({older, newer}, out);  // oldest-to-newest order
  {
    SSTable result(out);
    REQUIRE(result.entry_count() == 1);
    REQUIRE(result.get("k").value == "new-value");  // not "old-value"
  }

  for (auto& p : {older, newer, out}) lsmdb::platform::remove_file(p);
}

TEST_CASE("a tombstone in the newer SSTable removes the key entirely from the compacted output",
          "[compaction]") {
  auto older = make_temp_path("_older.sst");
  auto newer = make_temp_path("_newer.sst");
  auto out = make_temp_path("_out.sst");

  SSTable::create(older, {{"k", std::string("value")}});
  SSTable::create(newer, {{"k", std::nullopt}});  // deleted more recently

  compact({older, newer}, out);
  {
    SSTable result(out);
    REQUIRE(result.entry_count() == 0);  // dropped, not resurrected as a value or a tombstone
    REQUIRE(result.get("k").status == LookupStatus::kNotFound);
  }

  for (auto& p : {older, newer, out}) lsmdb::platform::remove_file(p);
}

TEST_CASE(
    "a tombstone in an OLDER SSTable does not shadow a real value written more recently",
    "[compaction]") {
  // The inverse of the above: the key was deleted, then re-written -- the
  // newer real value must win, not the older tombstone.
  auto older = make_temp_path("_older.sst");
  auto newer = make_temp_path("_newer.sst");
  auto out = make_temp_path("_out.sst");

  SSTable::create(older, {{"k", std::nullopt}});           // deleted first
  SSTable::create(newer, {{"k", std::string("reborn")}});  // then re-added

  compact({older, newer}, out);
  {
    SSTable result(out);
    REQUIRE(result.entry_count() == 1);
    auto lookup = result.get("k");
    REQUIRE(lookup.status == LookupStatus::kFound);
    REQUIRE(lookup.value == "reborn");
  }

  for (auto& p : {older, newer, out}) lsmdb::platform::remove_file(p);
}

TEST_CASE("compacting a single SSTable is a correctness-preserving passthrough", "[compaction]") {
  auto p1 = make_temp_path("_1.sst");
  auto out = make_temp_path("_out.sst");
  SSTable::create(p1, {{"a", std::string("1")}, {"b", std::string("2")}});

  compact({p1}, out);
  {
    SSTable result(out);
    REQUIRE(result.entry_count() == 2);
    REQUIRE(result.get("a").value == "1");
    REQUIRE(result.get("b").value == "2");
  }

  lsmdb::platform::remove_file(p1);
  lsmdb::platform::remove_file(out);
}

TEST_CASE("compacting an empty list of SSTables produces a valid, empty SSTable",
          "[compaction]") {
  auto out = make_temp_path("_out.sst");
  compact({}, out);
  {
    SSTable result(out);
    REQUIRE(result.entry_count() == 0);
  }
  lsmdb::platform::remove_file(out);
}

TEST_CASE("compacting three SSTables resolves a three-way overwrite to the truly newest value",
          "[compaction]") {
  auto t1 = make_temp_path("_1.sst");
  auto t2 = make_temp_path("_2.sst");
  auto t3 = make_temp_path("_3.sst");
  auto out = make_temp_path("_out.sst");

  SSTable::create(t1, {{"k", std::string("v1")}});
  SSTable::create(t2, {{"k", std::string("v2")}});
  SSTable::create(t3, {{"k", std::string("v3")}});

  compact({t1, t2, t3}, out);  // oldest-to-newest
  {
    SSTable result(out);
    REQUIRE(result.get("k").value == "v3");
  }

  for (auto& p : {t1, t2, t3, out}) lsmdb::platform::remove_file(p);
}
