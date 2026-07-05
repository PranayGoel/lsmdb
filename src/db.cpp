#include "lsmdb/db.h"

#include "lsmdb/compaction.h"
#include "lsmdb/platform/file_sync.h"

#include <algorithm>
#include <map>

namespace lsmdb {

namespace {

std::string zero_padded(size_t id, size_t width = 6) {
  std::string digits = std::to_string(id);
  size_t pad = digits.size() < width ? width - digits.size() : 0;
  return std::string(pad, '0') + digits;
}

}  // namespace

Db::Db(std::filesystem::path directory, size_t memtable_flush_threshold_bytes,
       size_t compaction_sstable_threshold)
    : dir_(std::move(directory)),
      flush_threshold_(memtable_flush_threshold_bytes),
      compaction_threshold_(compaction_sstable_threshold),
      next_sstable_id_(0) {
  std::filesystem::create_directories(dir_);
  recover();
}

std::filesystem::path Db::wal_path() const { return dir_ / "wal.log"; }

std::filesystem::path Db::sstable_path(size_t id) const {
  return dir_ / (zero_padded(id) + ".sst");
}

void Db::recover() {
  // Step 1: load whatever SSTables already exist on disk, oldest-to-newest
  // by their numeric id -- this order is load-bearing (see db.h's comment on
  // sstables_): both get()'s newest-wins search and compact()'s conflict
  // resolution depend on it being correct.
  std::vector<std::pair<size_t, std::filesystem::path>> found;
  if (std::filesystem::exists(dir_)) {
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
      if (entry.path().extension() != ".sst") continue;
      try {
        size_t id = std::stoull(entry.path().stem().string());
        found.emplace_back(id, entry.path());
      } catch (const std::exception&) {
        continue;  // not a numerically-named SSTable file -- ignore it
      }
    }
  }
  std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });

  size_t max_id_seen = 0;
  bool any_found = false;
  for (auto& [id, path] : found) {
    sstables_.push_back(std::make_shared<SSTable>(path));
    max_id_seen = std::max(max_id_seen, id);
    any_found = true;
  }
  next_sstable_id_ = any_found ? max_id_seen + 1 : 0;

  // Step 2: replay the WAL into the (currently empty) memtable -- this is
  // the actual crash-recovery guarantee. Any Put/Delete that was durably
  // appended to the WAL but never made it into a flushed SSTable (because
  // the process died before that flush happened) gets reconstructed here,
  // in the exact order it was originally applied.
  for (const auto& record : WriteAheadLog::replay(wal_path())) {
    if (record.type == RecordType::kPut) {
      memtable_.put(record.key, record.value);
    } else {
      memtable_.remove(record.key);
    }
  }

  // Step 3: only now open the WAL for new appends -- opening it earlier
  // wouldn't be wrong (WriteAheadLog's constructor only opens for append, it
  // doesn't touch existing content), but doing it after replay keeps the
  // "read old state, then start accepting new state" ordering explicit and
  // easy to follow here rather than relying on that detail.
  wal_ = std::make_unique<WriteAheadLog>(wal_path());
}

void Db::put(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // WAL append happens BEFORE the memtable is updated -- this ordering is
  // the actual durability guarantee. If the process crashes between these
  // two lines, recovery replays the WAL and reconstructs this exact put on
  // restart. If the order were reversed (memtable first, WAL second), a
  // crash in between would lose the write entirely: the memtable holding it
  // is gone (never persisted), and the WAL never recorded it either.
  wal_->append(WalRecord{RecordType::kPut, key, value});
  memtable_.put(key, value);
  maybe_flush_locked();
}

void Db::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  wal_->append(WalRecord{RecordType::kDelete, key, ""});
  memtable_.remove(key);
  maybe_flush_locked();
}

std::optional<std::string> Db::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto mem_result = memtable_.get(key);
  if (mem_result.status == LookupStatus::kFound) return mem_result.value;
  if (mem_result.status == LookupStatus::kDeleted) return std::nullopt;

  // Not in the memtable -- search SSTables NEWEST first. This is the same
  // "most recent write wins" principle as everywhere else in this project:
  // an older SSTable might still have a stale value for this key that a
  // newer SSTable's tombstone (or newer value) has since shadowed.
  for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
    auto result = (*it)->get(key);
    if (result.status == LookupStatus::kFound) return result.value;
    if (result.status == LookupStatus::kDeleted) return std::nullopt;
    // kNotFound in this SSTable -- keep searching older ones.
  }
  return std::nullopt;  // never written, anywhere
}

std::map<std::string, std::optional<std::string>> Db::merge_all_locked() const {
  // Apply layers oldest-to-newest into one sorted map, so a later (newer)
  // layer's write for a key naturally overwrites an earlier one -- the same
  // newest-wins principle as get(), just expressed as "apply in order"
  // instead of "search backward and stop at the first hit." Simpler to read
  // for a multi-key merge across many sources; a real production
  // implementation would use each SSTable's sorted index to seek directly to
  // a requested range rather than loading every entry via read_all() and
  // filtering afterward -- documented here as the same kind of "correct but
  // not the fastest possible" scope tradeoff as read_all() itself (see
  // sstable.h).
  std::map<std::string, std::optional<std::string>> merged;
  for (const auto& sstable : sstables_) {  // oldest to newest
    for (auto& entry : sstable->read_all()) {
      merged[entry.key] = entry.is_tombstone ? std::nullopt : std::optional<std::string>(entry.value);
    }
  }
  for (const auto& entry : memtable_.entries()) {  // newest layer, applied last
    merged[entry.key] = entry.value;
  }
  return merged;
}

std::vector<std::pair<std::string, std::string>> Db::range_scan(const std::string& start,
                                                                  const std::string& end) const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::pair<std::string, std::string>> result;
  for (auto& [key, value] : merge_all_locked()) {
    if (key >= start && key < end && value.has_value()) result.emplace_back(key, *value);
  }
  return result;
}

std::vector<std::pair<std::string, std::string>> Db::all_entries() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::pair<std::string, std::string>> result;
  for (auto& [key, value] : merge_all_locked()) {
    if (value.has_value()) result.emplace_back(key, *value);  // tombstones never surface to the caller
  }
  return result;
}

size_t Db::sstable_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sstables_.size();
}

void Db::maybe_flush_locked() {
  if (memtable_.approximate_size_bytes() < flush_threshold_) return;

  auto entries = memtable_.entries();
  auto path = sstable_path(next_sstable_id_++);
  SSTable::create(path, entries);
  sstables_.push_back(std::make_shared<SSTable>(path));

  // Only clear the memtable and reset the WAL AFTER the new SSTable exists
  // and was confirmed openable (the make_shared<SSTable> line above would
  // have thrown if the just-written file were somehow invalid) -- clearing
  // first and discovering the flush failed afterward would lose data with
  // no way to recover it.
  memtable_.clear();
  wal_->reset();

  maybe_compact_locked();
}

void Db::maybe_compact_locked() {
  if (sstables_.size() < compaction_threshold_) return;

  std::vector<std::filesystem::path> old_paths;
  old_paths.reserve(sstables_.size());
  for (const auto& sstable : sstables_) old_paths.push_back(sstable->path());

  auto merged_path = sstable_path(next_sstable_id_++);
  compact(old_paths, merged_path);
  auto merged = std::make_shared<SSTable>(merged_path);  // throws before any old file is touched if this is somehow invalid

  // Dropping every shared_ptr in sstables_ destroys each SSTable object,
  // which closes its open ifstream -- required before std::filesystem::remove
  // can succeed on Windows, where a file with an open handle generally can't
  // be deleted (unlike POSIX, where unlink()ing an open file just defers the
  // actual removal until the last handle closes). Doing this in the wrong
  // order would work by accident on macOS/Linux and fail on Windows --
  // exactly the kind of platform difference this project's cross-platform
  // requirement exists to catch before it ships, not after.
  sstables_.clear();
  for (const auto& path : old_paths) platform::remove_file(path);

  sstables_.push_back(merged);
}

}  // namespace lsmdb
