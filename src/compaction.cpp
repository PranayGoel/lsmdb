#include "lsmdb/compaction.h"

#include <algorithm>

namespace lsmdb {

namespace {

struct TaggedEntry {
  std::string key;
  bool is_tombstone;
  std::string value;
  size_t source_index;  // position in the oldest-to-newest input list
};

}  // namespace

void compact(const std::vector<std::filesystem::path>& paths_oldest_to_newest,
             const std::filesystem::path& output_path) {
  std::vector<TaggedEntry> all;
  for (size_t source = 0; source < paths_oldest_to_newest.size(); ++source) {
    SSTable table(paths_oldest_to_newest[source]);
    for (auto& entry : table.read_all()) {
      all.push_back(TaggedEntry{std::move(entry.key), entry.is_tombstone,
                                 std::move(entry.value), source});
    }
  }

  // A STABLE sort by key alone is what makes "the last entry in each
  // same-key group is the newest" true afterward: entries are appended above
  // in source_index order (0, 1, 2, ...), and a stable sort preserves that
  // relative order among equal keys -- so within any run of entries sharing
  // a key, they stay ordered oldest-source-first, and the last one in the
  // run is guaranteed to have the highest source_index (the newest write).
  std::stable_sort(all.begin(), all.end(),
                    [](const TaggedEntry& a, const TaggedEntry& b) { return a.key < b.key; });

  std::vector<MemtableEntry> merged;
  for (size_t i = 0; i < all.size();) {
    size_t group_end = i;
    while (group_end < all.size() && all[group_end].key == all[i].key) ++group_end;

    const TaggedEntry& newest = all[group_end - 1];  // see stability note above
    if (!newest.is_tombstone) {
      // Every compaction in this project's size-tiered scheme merges the
      // FULL current SSTable set, so a tombstone can be dropped outright
      // here -- there is no older data outside this merge left for it to
      // shadow. See compaction.h's module comment for why that's specific
      // to size-tiered compaction and wouldn't hold for a leveled scheme.
      merged.push_back(MemtableEntry{newest.key, newest.value});
    }
    i = group_end;
  }

  SSTable::create(output_path, merged);
}

}  // namespace lsmdb
