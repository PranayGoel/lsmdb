#include "lsmdb/bloom_filter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lsmdb {

namespace {

// FNV-1a, 64-bit. Chosen over std::hash specifically because std::hash's
// actual algorithm is unspecified by the standard and can differ across
// standard library implementations (or even across builds) -- a bloom
// filter serialized to disk by one build and loaded by another must hash
// keys identically, or maybe_contains() silently returns wrong answers for
// data written by a different binary. FNV-1a is simple, well-known, and
// produces the same output everywhere it's implemented the same way.
uint64_t fnv1a(std::string_view data, uint64_t seed) {
  uint64_t hash = 0xcbf29ce484222325ULL ^ seed;
  constexpr uint64_t kPrime = 0x100000001b3ULL;
  for (unsigned char byte : data) {
    hash ^= byte;
    hash *= kPrime;
  }
  return hash;
}

}  // namespace

BloomFilter::BloomFilter(size_t expected_items, double fp_rate) {
  if (expected_items == 0) expected_items = 1;  // avoid a zero-size filter
  double n = static_cast<double>(expected_items);
  double m = std::ceil(-(n * std::log(fp_rate)) / (std::log(2.0) * std::log(2.0)));
  double k = std::round((m / n) * std::log(2.0));

  num_bits_ = static_cast<size_t>(std::max(m, 1.0));
  num_hashes_ = static_cast<size_t>(std::max(k, 1.0));
  bits_.assign((num_bits_ + 7) / 8, 0);
}

BloomFilter::BloomFilter(size_t num_bits, size_t num_hashes)
    : num_bits_(num_bits), num_hashes_(num_hashes), bits_((num_bits + 7) / 8, 0) {}

void BloomFilter::set_bit(size_t index) { bits_[index / 8] |= (1u << (index % 8)); }

bool BloomFilter::test_bit(size_t index) const {
  return (bits_[index / 8] & (1u << (index % 8))) != 0;
}

void BloomFilter::add(std::string_view key) {
  uint64_t h1 = fnv1a(key, 0);
  uint64_t h2 = fnv1a(key, 1);
  for (size_t i = 0; i < num_hashes_; ++i) {
    uint64_t combined = h1 + i * h2;
    set_bit(combined % num_bits_);
  }
}

bool BloomFilter::maybe_contains(std::string_view key) const {
  uint64_t h1 = fnv1a(key, 0);
  uint64_t h2 = fnv1a(key, 1);
  for (size_t i = 0; i < num_hashes_; ++i) {
    uint64_t combined = h1 + i * h2;
    if (!test_bit(combined % num_bits_)) {
      return false;  // definitely not present -- one missing bit is proof enough
    }
  }
  return true;  // every relevant bit was set -- probably present
}

std::string BloomFilter::to_bytes() const {
  std::string out;
  out.reserve(16 + bits_.size());
  auto put_u64 = [&out](uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  put_u64(static_cast<uint64_t>(num_bits_));
  put_u64(static_cast<uint64_t>(num_hashes_));
  out.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
  return out;
}

BloomFilter BloomFilter::from_bytes(std::string_view bytes) {
  if (bytes.size() < 16) {
    throw std::runtime_error("BloomFilter::from_bytes: input too short");
  }
  auto get_u64 = [&bytes](size_t offset) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    }
    return v;
  };
  size_t num_bits = static_cast<size_t>(get_u64(0));
  size_t num_hashes = static_cast<size_t>(get_u64(8));

  BloomFilter filter(num_bits, num_hashes);
  std::string_view packed = bytes.substr(16);
  if (packed.size() != filter.bits_.size()) {
    throw std::runtime_error("BloomFilter::from_bytes: packed bit array size mismatch");
  }
  std::copy(packed.begin(), packed.end(), filter.bits_.begin());
  return filter;
}

}  // namespace lsmdb
