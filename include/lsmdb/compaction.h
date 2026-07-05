#pragma once

#include "lsmdb/sstable.h"

#include <filesystem>
#include <vector>

namespace lsmdb {

// Merges several existing SSTables into one new SSTable, keeping only the
// newest version of each key and dropping tombstones.
//
// Compaction strategy: size-tiered, not leveled. When the number of SSTables
// crosses a threshold, ALL of them get merged into one new file in a single
// pass. This is simpler than leveled compaction (RocksDB/LevelDB's default),
// which instead organizes SSTables into levels with non-overlapping key
// ranges within a level and merges just one level into the next -- leveled
// compaction gives better read amplification at scale, but its correctness
// depends on maintaining per-level key-range invariants that add real
// implementation complexity. Size-tiered's "merge everything periodically"
// is easier to get right and entirely adequate for this project's scope;
// documented as a deliberate choice, not a missing feature.
//
// Because size-tiered compaction here always merges the FULL current set of
// SSTables (there is no "older data outside this merge" in this project's
// simplified design -- see Db, once it exists, for how the SSTable list is
// managed), every compaction is implicitly a full compaction: a tombstone
// can be safely dropped once merged, since nothing outside the merge could
// still need it to shadow older data. A leveled scheme would need to track
// whether a tombstone might still be shadowing data in an even-older level
// that wasn't part of this particular merge -- another reason leveled
// compaction is meaningfully harder to get right than size-tiered.
//
// `paths_oldest_to_newest` must be given in that exact order: when the same
// key appears in more than one input SSTable, the value from whichever
// SSTable appears LATER in this list wins (it's the newer write). Passing
// them out of order silently produces the wrong winner for any key that was
// overwritten across SSTables -- there's no way to detect this from the
// files alone, so getting the ordering right is the caller's responsibility.
//
// Does not delete or otherwise modify the input files -- the caller decides
// when it's safe to remove them (e.g. after confirming the new SSTable opens
// correctly), keeping this function a pure "produce one output from N
// inputs" step that's simple to test in isolation.
void compact(const std::vector<std::filesystem::path>& paths_oldest_to_newest,
             const std::filesystem::path& output_path);

}  // namespace lsmdb
