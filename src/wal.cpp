#include "lsmdb/wal.h"

#include "lsmdb/crc32.h"
#include "lsmdb/platform/file_sync.h"

#include <cstring>
#include <stdexcept>

namespace lsmdb {

namespace {

void put_u32(std::string& buf, uint32_t v) {
  // Little-endian, explicit byte-by-byte -- avoids any assumption about the
  // host's native endianness (memcpy'ing a uint32_t directly would silently
  // break on a big-endian host; there are none we target today, but writing
  // it out explicitly costs nothing and removes the assumption entirely).
  buf.push_back(static_cast<char>(v & 0xFF));
  buf.push_back(static_cast<char>((v >> 8) & 0xFF));
  buf.push_back(static_cast<char>((v >> 16) & 0xFF));
  buf.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// Reads a little-endian uint32 from an istream. Returns false (leaving `out`
// unspecified) if fewer than 4 bytes remain -- the normal, expected shape of
// hitting the end of a WAL that was mid-write when the process died.
bool read_u32(std::istream& in, uint32_t& out) {
  char buf[4];
  in.read(buf, 4);
  if (in.gcount() != 4) return false;
  out = static_cast<uint32_t>(static_cast<unsigned char>(buf[0])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buf[1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buf[2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(buf[3])) << 24);
  return true;
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::filesystem::path path) : path_(std::move(path)) {
  open_for_append();
}

void WriteAheadLog::open_for_append() {
  // std::ios::app: every write() call seeks to end-of-file first, so this is
  // safe even if the file already has content from a prior process run.
  out_.open(path_, std::ios::binary | std::ios::app);
  if (!out_) {
    throw std::runtime_error("WriteAheadLog: failed to open " + path_.string());
  }
}

void WriteAheadLog::append(const WalRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string buf;
  buf.push_back(static_cast<char>(record.type));
  put_u32(buf, static_cast<uint32_t>(record.key.size()));
  buf.append(record.key);
  put_u32(buf, static_cast<uint32_t>(record.value.size()));
  buf.append(record.value);

  uint32_t crc = crc32(buf);
  put_u32(buf, crc);

  out_.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  if (!out_) {
    throw std::runtime_error("WriteAheadLog: write failed for " + path_.string());
  }
  out_.flush();  // pushes from the C++ stream buffer into the OS -- still not
                 // durable yet, sync_file below is what actually forces it to disk
  platform::sync_file(path_);
}

std::vector<WalRecord> WriteAheadLog::replay(const std::filesystem::path& path) {
  std::vector<WalRecord> records;
  if (!std::filesystem::exists(path)) {
    return records;  // fresh DB, nothing to replay
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("WriteAheadLog::replay: failed to open " + path.string());
  }

  while (true) {
    // Everything from here to the next `continue`/`break` reads one record.
    // Any short read at any step means we've hit a torn/partial write from a
    // crash mid-append -- that is the expected, correct place to stop, not
    // an error to propagate.
    char type_byte;
    in.read(&type_byte, 1);
    if (in.gcount() != 1) break;  // clean EOF between records -- normal end

    uint32_t key_len;
    if (!read_u32(in, key_len)) break;

    std::string key(key_len, '\0');
    in.read(key.data(), key_len);
    if (static_cast<uint32_t>(in.gcount()) != key_len) break;

    uint32_t value_len;
    if (!read_u32(in, value_len)) break;

    std::string value(value_len, '\0');
    in.read(value.data(), value_len);
    if (static_cast<uint32_t>(in.gcount()) != value_len) break;

    uint32_t stored_crc;
    if (!read_u32(in, stored_crc)) break;

    // Recompute the CRC over exactly the bytes it was computed over on write.
    std::string buf;
    buf.push_back(type_byte);
    put_u32(buf, key_len);
    buf.append(key);
    put_u32(buf, value_len);
    buf.append(value);
    if (crc32(buf) != stored_crc) break;  // corrupted record -- stop, discard it

    RecordType type = static_cast<RecordType>(type_byte);
    if (type != RecordType::kPut && type != RecordType::kDelete) break;  // corrupted type byte

    records.push_back(WalRecord{type, std::move(key), std::move(value)});
  }

  return records;
}

void WriteAheadLog::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  out_.close();
  std::filesystem::remove(path_);
  open_for_append();
}

}  // namespace lsmdb
