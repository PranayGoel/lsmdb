#pragma once

#include <cstdint>
#include <string_view>

namespace lsmdb {

// Standard IEEE 802.3 CRC-32 (the same polynomial gzip/zlib/PNG use), computed
// over a byte range. Used to detect corrupted or torn (partially-written)
// WAL records -- a crash mid-append can leave a record with a correct length
// prefix but garbage/truncated bytes, and CRC32 catches that far more
// reliably than "does the length look sane" alone.
uint32_t crc32(std::string_view data);

}  // namespace lsmdb
