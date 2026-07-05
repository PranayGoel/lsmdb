#include "lsmdb/sstable.h"

#include "lsmdb/platform/file_sync.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace lsmdb {

namespace {

constexpr uint32_t kMagic = 0x53535442;  // "SSTB" as a little-endian uint32
constexpr size_t kFooterSize = 8 + 8 + 8 + 4;  // data_offset + bloom_offset + index_offset + magic

void put_u32(std::string& buf, uint32_t v) {
  for (int i = 0; i < 4; ++i) buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u64(std::string& buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t read_u32(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}
uint64_t read_u64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

}  // namespace

void SSTable::create(const std::filesystem::path& path, const std::vector<MemtableEntry>& entries) {
  for (size_t i = 1; i < entries.size(); ++i) {
    if (!(entries[i - 1].key < entries[i].key)) {
      throw std::runtime_error("SSTable::create: entries must be strictly sorted by key");
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("SSTable::create: failed to open " + path.string());

  BloomFilter bloom(std::max<size_t>(entries.size(), 1));
  std::string index_buf;
  put_u32(index_buf, static_cast<uint32_t>(entries.size()));

  uint64_t offset = 0;
  for (const auto& entry : entries) {
    std::string record;
    put_u32(record, static_cast<uint32_t>(entry.key.size()));
    record.append(entry.key);
    record.push_back(entry.value.has_value() ? 1 : 0);
    put_u32(record, static_cast<uint32_t>(entry.value.has_value() ? entry.value->size() : 0));
    if (entry.value.has_value()) record.append(*entry.value);

    out.write(record.data(), static_cast<std::streamsize>(record.size()));

    put_u32(index_buf, static_cast<uint32_t>(entry.key.size()));
    index_buf.append(entry.key);
    put_u64(index_buf, offset);
    put_u64(index_buf, static_cast<uint64_t>(record.size()));

    bloom.add(entry.key);
    offset += record.size();
  }

  uint64_t data_end = offset;
  std::string bloom_bytes = bloom.to_bytes();
  out.write(bloom_bytes.data(), static_cast<std::streamsize>(bloom_bytes.size()));
  uint64_t bloom_end = data_end + bloom_bytes.size();

  out.write(index_buf.data(), static_cast<std::streamsize>(index_buf.size()));
  uint64_t index_end = bloom_end + index_buf.size();

  std::string footer;
  put_u64(footer, 0);          // data section always starts at file offset 0
  put_u64(footer, data_end);   // bloom section start
  put_u64(footer, bloom_end);  // index section start
  put_u32(footer, kMagic);
  out.write(footer.data(), static_cast<std::streamsize>(footer.size()));

  if (!out) throw std::runtime_error("SSTable::create: write failed for " + path.string());
  out.close();
  platform::sync_file(path);
  (void)index_end;
}

SSTable::SSTable(std::filesystem::path path) : path_(std::move(path)) {
  in_.open(path_, std::ios::binary);
  if (!in_) throw std::runtime_error("SSTable: failed to open " + path_.string());

  in_.seekg(0, std::ios::end);
  auto file_size = static_cast<uint64_t>(in_.tellg());
  if (file_size < kFooterSize) {
    throw std::runtime_error("SSTable: file too small to contain a valid footer: " + path_.string());
  }

  std::string footer(kFooterSize, '\0');
  in_.seekg(static_cast<std::streamoff>(file_size - kFooterSize));
  in_.read(footer.data(), static_cast<std::streamsize>(kFooterSize));

  uint64_t data_offset = read_u64(footer.data());
  uint64_t bloom_offset = read_u64(footer.data() + 8);
  uint64_t index_offset = read_u64(footer.data() + 16);
  uint32_t magic = read_u32(footer.data() + 24);
  if (magic != kMagic) {
    throw std::runtime_error("SSTable: bad magic number, not a valid SSTable file: " + path_.string());
  }
  (void)data_offset;

  uint64_t bloom_len = index_offset - bloom_offset;
  std::string bloom_bytes(bloom_len, '\0');
  in_.seekg(static_cast<std::streamoff>(bloom_offset));
  in_.read(bloom_bytes.data(), static_cast<std::streamsize>(bloom_len));
  bloom_ = BloomFilter::from_bytes(bloom_bytes);

  uint64_t index_len = (file_size - kFooterSize) - index_offset;
  std::string index_bytes(index_len, '\0');
  in_.seekg(static_cast<std::streamoff>(index_offset));
  in_.read(index_bytes.data(), static_cast<std::streamsize>(index_len));

  const char* p = index_bytes.data();
  uint32_t count = read_u32(p);
  p += 4;
  index_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t key_len = read_u32(p);
    p += 4;
    std::string key(p, key_len);
    p += key_len;
    uint64_t entry_offset = read_u64(p);
    p += 8;
    uint64_t entry_length = read_u64(p);
    p += 8;
    index_.push_back(IndexEntry{std::move(key), entry_offset, entry_length});
  }
}

LookupResult SSTable::get(const std::string& key) const {
  if (!bloom_.maybe_contains(key)) {
    return LookupResult{LookupStatus::kNotFound, ""};
  }

  auto it = std::lower_bound(index_.begin(), index_.end(), key,
                              [](const IndexEntry& e, const std::string& k) { return e.key < k; });
  if (it == index_.end() || it->key != key) {
    return LookupResult{LookupStatus::kNotFound, ""};  // bloom false positive
  }

  std::lock_guard<std::mutex> lock(read_mutex_);
  std::string record(it->length, '\0');
  in_.seekg(static_cast<std::streamoff>(it->offset));
  in_.read(record.data(), static_cast<std::streamsize>(it->length));

  const char* p = record.data();
  uint32_t key_len = read_u32(p);
  p += 4 + key_len;  // skip past the key, we already know it matches
  bool is_tombstone = (*p == 0);
  p += 1;
  uint32_t value_len = read_u32(p);
  p += 4;
  if (is_tombstone) {
    return LookupResult{LookupStatus::kDeleted, ""};
  }
  return LookupResult{LookupStatus::kFound, std::string(p, value_len)};
}

std::vector<SSTableEntry> SSTable::read_all() const {
  std::lock_guard<std::mutex> lock(read_mutex_);
  std::vector<SSTableEntry> result;
  result.reserve(index_.size());
  for (const auto& idx : index_) {
    std::string record(idx.length, '\0');
    in_.seekg(static_cast<std::streamoff>(idx.offset));
    in_.read(record.data(), static_cast<std::streamsize>(idx.length));

    const char* p = record.data();
    uint32_t key_len = read_u32(p);
    p += 4 + key_len;
    bool is_tombstone = (*p == 0);
    p += 1;
    uint32_t value_len = read_u32(p);
    p += 4;
    result.push_back(SSTableEntry{idx.key, is_tombstone, is_tombstone ? "" : std::string(p, value_len)});
  }
  return result;
}

}  // namespace lsmdb
