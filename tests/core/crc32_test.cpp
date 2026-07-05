#include "lsmdb/crc32.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("crc32 matches the well-known check value for the ASCII test string", "[crc32]") {
  // "123456789" -> 0xCBF43926 is the standard CRC-32/ISO-HDLC check value
  // published for this exact polynomial -- the canonical sanity check used
  // to confirm a CRC32 implementation matches the widely-used variant
  // (the same one zlib/gzip/PNG use), not just "some" CRC32 variant.
  REQUIRE(lsmdb::crc32("123456789") == 0xCBF43926u);
}

TEST_CASE("crc32 of empty input is 0", "[crc32]") {
  REQUIRE(lsmdb::crc32("") == 0u);
}

TEST_CASE("crc32 is sensitive to a single bit flip", "[crc32]") {
  REQUIRE(lsmdb::crc32("hello") != lsmdb::crc32("hellp"));
}
