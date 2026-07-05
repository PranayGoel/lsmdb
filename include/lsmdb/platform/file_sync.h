#pragma once

#include <filesystem>
#include <fstream>

namespace lsmdb::platform {

// Forces a file's contents to physical disk (not just the OS page cache),
// which is the actual durability guarantee a write-ahead log depends on --
// without this call, a "successful" write can still be lost on a crash or
// power loss, because the OS is free to buffer it in memory indefinitely.
//
// This is the ONE genuinely OS-specific operation in the whole project:
// POSIX exposes it as fsync(2) on a file descriptor; Windows exposes the
// equivalent as FlushFileBuffers on a HANDLE. Every other component in this
// codebase is written against std::fstream/std::filesystem specifically so
// this is the only place a platform #ifdef is allowed to appear -- the same
// pattern real portable systems (e.g. SQLite's os_unix.c/os_win.c split) use.
//
// Takes the OS-native path (not an already-open std::fstream) because we need
// the raw file descriptor/handle to call fsync/FlushFileBuffers directly --
// std::fstream doesn't expose one portably. Callers open+write+close (or keep
// their own std::fstream open) as normal, then call this to force durability
// at the specific point they need the guarantee (e.g. after appending a WAL
// record), not on every buffered write.
void sync_file(const std::filesystem::path& path);

// Removes a file, tolerating the transient "file is in use by another
// process" failure Windows can report immediately after the last handle to
// that file is closed. This is a well-documented flakiness pattern on
// GitHub's windows-latest runners -- most often Windows Defender's real-time
// scanner briefly opening a just-written file for inspection -- and every
// close-then-delete or close-then-reopen sequence in this codebase (WAL
// reset, compaction dropping old SSTables) hits it. POSIX's unlink(2) has no
// equivalent failure mode (a file can be removed while other handles are
// still open on it), so this collapses to a single std::filesystem::remove
// there; Windows retries briefly before giving up.
//
// Mirrors std::filesystem::remove's return contract: true if a file was
// removed, false if it didn't exist.
bool remove_file(const std::filesystem::path& path);

}  // namespace lsmdb::platform
