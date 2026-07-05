#include "lsmdb/bloom_filter.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

using lsmdb::BloomFilter;

TEST_CASE("a key that was added is always reported as maybe-present", "[bloom]") {
  // No false negatives is the one guarantee a bloom filter must never break --
  // this is what makes it safe for an SSTable to skip a whole file read on
  // "definitely not here."
  BloomFilter filter(100);
  filter.add("hello");
  filter.add("world");
  REQUIRE(filter.maybe_contains("hello"));
  REQUIRE(filter.maybe_contains("world"));
}

TEST_CASE("an empty filter reports everything as definitely absent", "[bloom]") {
  BloomFilter filter(100);
  REQUIRE_FALSE(filter.maybe_contains("anything"));
}

TEST_CASE("false-positive rate stays close to the configured target at real scale",
          "[bloom]") {
  // Not testing an exact number (that would be flaky by nature -- it's a
  // probabilistic structure) but confirming the actual measured rate is in
  // the right ballpark for the configured 1% target, using a large enough
  // sample that a wildly wrong implementation (e.g. a hashing bug collapsing
  // to far fewer effective bits) would fail this test reliably.
  constexpr size_t kInserted = 10000;
  BloomFilter filter(kInserted, 0.01);
  for (size_t i = 0; i < kInserted; ++i) {
    filter.add("inserted-key-" + std::to_string(i));
  }

  size_t false_positives = 0;
  constexpr size_t kProbes = 10000;
  for (size_t i = 0; i < kProbes; ++i) {
    // Keys guaranteed disjoint from the inserted set (different prefix).
    if (filter.maybe_contains("absent-key-" + std::to_string(i))) {
      ++false_positives;
    }
  }
  double rate = static_cast<double>(false_positives) / static_cast<double>(kProbes);
  REQUIRE(rate < 0.03);  // generous margin above the 1% target -- this is a
                         // sanity bound, not a strict statistical test
}

TEST_CASE("to_bytes/from_bytes round-trips a filter's exact membership behavior",
          "[bloom]") {
  BloomFilter filter(50);
  filter.add("a");
  filter.add("b");
  filter.add("c");

  std::string serialized = filter.to_bytes();
  BloomFilter restored = BloomFilter::from_bytes(serialized);

  REQUIRE(restored.maybe_contains("a"));
  REQUIRE(restored.maybe_contains("b"));
  REQUIRE(restored.maybe_contains("c"));
}

TEST_CASE("from_bytes rejects a truncated buffer instead of reading out of bounds",
          "[bloom]") {
  REQUIRE_THROWS_AS(BloomFilter::from_bytes("short"), std::runtime_error);
}
