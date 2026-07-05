# Design log

Written as each piece is built, not reconstructed from memory afterward — each entry captures the decision and its tradeoff while it's still fresh. Newest entries at the bottom.

---

## Entry 0 — project skeleton, build system, cross-platform strategy

**Decision: CMake as the build system, targeting gcc/clang/MSVC from one `CMakeLists.txt`.**
Why: this project needs to build and run on both a macOS/Linux development machine and a Windows laptop. CMake is the de facto standard for portable C++ across all three toolchains — no separate Makefile/Visual-Studio-project maintenance, no drift between platforms.

**Decision: isolate all OS-specific code behind one platform-abstraction header, `include/lsmdb/platform/file_sync.h`.**
The only genuinely OS-specific operation this project needs is forcing a file write to physical disk — POSIX calls this `fsync`, Windows calls the equivalent `FlushFileBuffers`. Every other component (WAL, memtable, SSTable, compaction, the eventual network server) is written against `std::filesystem`, `std::fstream`, and `std::thread` — all portable — specifically so this is the *only* place a `#ifdef _WIN32` is allowed to appear anywhere in the codebase. This is the same pattern real portable systems use (e.g. SQLite splits OS-specific code into `os_unix.c`/`os_win.c` behind one interface) — it keeps the platform risk contained and auditable in one file instead of scattered through every component.

**Decision: `std::filesystem::path` takes an already-existing file, not an open stream, in `sync_file`.**
`std::fstream` doesn't expose a portable way to get at the underlying OS file descriptor/handle needed to call `fsync`/`FlushFileBuffers` directly. So `sync_file` re-opens the path by name to get that handle. This means callers must close (or otherwise flush) their own write before calling `sync_file` — a real constraint on the WAL's design (entry to follow when the WAL lands), not a limitation to work around silently.

**Decision: sanitizers (ASan/UBSan/TSan) as an opt-in CMake flag (`-DLSMDB_SANITIZE=...`), not always-on.**
ASan/UBSan and TSan instrument code in mutually incompatible ways and can't be combined in a single build — CI runs each as a separate leg (Linux only, since MSVC's sanitizer story differs enough to need separate handling later if/when it matters) rather than pretending one flag covers everything everywhere.

**Decision: Catch2 (v3, via CMake `FetchContent`) for testing, not Google Test.**
Either would work; Catch2's header-mostly, no-external-dependency-install story is simpler to keep working identically across all three CI runners without also having to vendor/build a second project's build system.

**Decision: a placeholder empty `.cpp` file for the `lsmdb` library target.**
CMake needs at least one source file per target to compile; this keeps the build graph correct from commit 1, before any real component exists, and gets removed the moment the first real file (`wal.cpp`) lands.

**What's tested at this point:** `sync_file` succeeds on a real file and throws (rather than silently no-op-ing) on a nonexistent one — a small but real check that the durability primitive everything else depends on actually does something and fails loudly when it can't.

**Next:** the write-ahead log itself, built directly on top of `sync_file`.
