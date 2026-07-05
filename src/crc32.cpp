#include "lsmdb/crc32.h"

#include <array>

namespace lsmdb {

namespace {

// Precomputed lookup table for the reflected IEEE 802.3 polynomial
// (0xEDB88320), built once at static-init time. Table-based CRC32 is the
// standard approach -- computing it bit-by-bit per byte would be correct but
// roughly 8x slower, and this runs on every single WAL append.
std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

const std::array<uint32_t, 256> kTable = make_table();

}  // namespace

uint32_t crc32(std::string_view data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char byte : data) {
    crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace lsmdb
