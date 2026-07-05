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

---

## Entry 1 — Write-Ahead Log (WAL)

**Decision: an explicit `type` byte per record, not an overloaded sentinel value.**
An earlier sketch considered using `value_len == UINT32_MAX` to mean "this is a delete." Rejected: it's a magic-number trap (an actual 4GB+ value would collide with it, however unlikely) and it makes the wire format harder to reason about. An explicit `RecordType` byte (`kPut`/`kDelete`) costs one extra byte per record and removes the ambiguity entirely.

**Decision: CRC32 over every record, checked on replay.**
A length-prefixed format alone can't distinguish "this record is genuinely 40 bytes" from "this record's length header itself got corrupted and now claims to be 40 bytes when it's actually garbage." CRC32 (the same IEEE 802.3 polynomial used by gzip/zlib/PNG — verified against the standard "123456789" → `0xCBF43926` check value in `crc32_test.cpp`) catches both a torn write (record ends early — the length says more bytes should follow than actually exist) and silent bit-level corruption of an otherwise complete-length record.

**Decision on replay: any short read or CRC mismatch stops replay entirely rather than skipping the bad record and continuing.**
This matters and is easy to get wrong: if a record's *length header itself* is what got corrupted, "skip this record and keep reading" would seek to a nonsense offset and start interpreting unrelated bytes as if they were a fresh record header — a much worse failure mode than simply stopping. Real WAL implementations (RocksDB's is the reference point here) handle a corrupted/truncated tail the same way: everything validated *before* the bad record is trusted; nothing after it is. This is exactly `wal_test.cpp`'s two most important cases — a genuinely truncated tail record (`"replay recovers every complete record and cleanly discards a torn tail record"`) and a length-valid-but-content-corrupted record (`"replay rejects a record whose bytes were corrupted"`) — both are the *expected shape of a crash*, not error conditions to propagate.

**Decision: `append()` calls `sync_file()` on every single record, not batched.**
This is the actual durability/throughput tradeoff of the whole design: fsync-per-write guarantees that once `append()` returns, that record is on physical disk, full stop — but it's also the slowest possible policy, since a real disk fsync is orders of magnitude slower than a memory write. Real production systems (e.g. Postgres's `synchronous_commit`, or MySQL's `innodb_flush_log_at_trx_commit`) make this a tunable *because* there's a genuine correctness-vs-throughput axis here: batch several appends before syncing and you can lose the unfsynced ones on a crash; fsync every time and every acknowledged write is safe but you pay a disk-round-trip per write. This project takes the simpler, safer default (sync every append) and documents the tradeoff rather than silently picking speed and hoping nobody asks; a batched/grouped-commit mode is a legitimate, named future improvement, not something being pretended not to matter.

**Decision: `open_for_append` uses `std::ios::app`, which is thread-hostile on its own — hence the `std::mutex`.**
`std::ios::app` guarantees every write seeks to end-of-file first, which handles reopening an existing file safely across process restarts, but says nothing about two threads calling `append()` concurrently. The mutex around `append()`/`reset()` is what actually makes this class safe to call from multiple threads — necessary later when Tier 2's networked server has multiple client connections writing concurrently.

**What's tested:** round-trip of Put/Delete records including empty values, reopening an existing WAL file across a simulated restart, `reset()` correctly discarding prior content, and — the two tests that matter most — the torn-write and CRC-mismatch recovery scenarios described above. All 12 tests (platform + crc32 + WAL) pass under Debug, ASan, UBSan, and TSan.

**Next:** the memtable — the in-memory structure `append()`ed records get applied to before ever touching an SSTable.
