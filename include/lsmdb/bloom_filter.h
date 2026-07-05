#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsmdb {

// A probabilistic set membership test: `maybe_contains(x)` returning false
// means x is *definitely not* in the set (no false negatives, ever); true
// means x is *probably* in the set, at the configured false-positive rate.
// This is what lets an SSTable Get() skip reading a file from disk entirely
// for a key that isn't in it, instead of every lookup having to check every
// SSTable on disk -- the whole point of attaching one per SSTable.
//
// Implementation: a single bit array plus the Kirsch-Mitzenmacher technique
// for simulating k independent hash functions from just two real ones
// (h1, h2 derived from splitting one 64-bit FNV-1a hash in half), rather than
// computing k genuinely independent hashes per key:
//   g_i(x) = h1(x) + i * h2(x)   for i in [0, k)
// This is a well-established, provably-good-enough approximation (Kirsch &
// Mitzenmacher, 2006) -- it avoids needing k separate hash function
// implementations while keeping the same asymptotic false-positive behavior.
class BloomFilter {
 public:
  // Default-constructs a minimal (1-bit-effective) filter -- exists solely so
  // callers like SSTable can declare a BloomFilter member and assign a real
  // one (via from_bytes) once its file has been read, without needing a
  // delegating-constructor dance. Never meaningfully used as-is.
  BloomFilter() : BloomFilter(1) {}

  // Sizes the bit array and chooses the number of hash functions from the
  // standard formulas for a target false-positive rate `fp_rate` given
  // `expected_items` insertions:
  //   m = ceil(-(n * ln(p)) / (ln(2)^2))   -- bits needed
  //   k = round((m / n) * ln(2))            -- hash functions needed
  explicit BloomFilter(size_t expected_items, double fp_rate = 0.01);

  void add(std::string_view key);
  bool maybe_contains(std::string_view key) const;

  // Serializes to a compact byte string (bit count, hash count, packed bits)
  // for embedding in an SSTable file; from_bytes reconstructs it exactly.
  std::string to_bytes() const;
  static BloomFilter from_bytes(std::string_view bytes);

 private:
  BloomFilter(size_t num_bits, size_t num_hashes);  // used by from_bytes

  size_t num_bits_;
  size_t num_hashes_;
  std::vector<uint8_t> bits_;  // packed, 8 bits per byte

  void set_bit(size_t index);
  bool test_bit(size_t index) const;
};

}  // namespace lsmdb
