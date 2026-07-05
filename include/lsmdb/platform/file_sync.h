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

}  // namespace lsmdb::platform
