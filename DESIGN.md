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

---

## Entry 2 — Memtable

**Decision: `std::map` (a red-black tree), not a skip list.**
Skip lists are the more commonly cited textbook LSM-tree memtable structure, mainly because they're easier to make *lock-free* for very high write concurrency. This project uses a single `std::shared_mutex` instead — simpler, already-correct (it's the standard library), and entirely sufficient for the concurrency level this project targets (Tier 2's networked server handles concurrent client connections, not the kind of extreme write-throughput regime where lock-free data structures start to matter). Named explicitly as a scope tradeoff: if lock-free concurrent writes become a real requirement, a skip list is the natural next step, not a rewrite from scratch.

**Decision: tombstones (`std::optional<std::string>` = `nullopt`) instead of erasing deleted keys outright.**
This is the single most important correctness property of the memtable, and it's easy to get wrong: if `remove(key)` just erased the map entry, a `get(key)` afterward would report `kNotFound` — indistinguishable from "this key was never written at all." That distinction matters enormously once SSTables exist (next entry): `kNotFound` in the memtable has to mean "check older SSTables," while `kDeleted` has to mean "stop looking, this key is gone" — even if an older SSTable on disk still has a stale value for it. Losing that distinction would resurrect deleted data. `remove()` on a key the memtable has never seen still creates a tombstone (tested explicitly: `"removing a key that was never written still creates a tombstone"`), for exactly this reason.

**Decision: `approximate_size_bytes()` tracks a running total incrementally, corrected on overwrite, rather than summing on demand.**
Summing all entries on every call would make every `put`/`get` pay an O(n) cost just to check the flush threshold — the incremental counter keeps that check O(1). The correction-on-overwrite logic (subtract the old key+value size before adding the new one) is what `"doesn't double-count overwrites"` tests — a naive `bytes += key.size() + value.size()` on every put would make the counter drift upward forever on a workload that repeatedly overwrites the same keys, eventually triggering flushes far more often than the data volume actually warrants.

**What's tested:** basic put/get/overwrite, the tombstone-vs-never-written distinction (both directions), sorted iteration order via `entries()`, the size-accounting correction on overwrite, and — run explicitly under ThreadSanitizer in CI, not just Debug — 8 threads concurrently inserting 200 keys each with no data race and the correct final count. All 20 tests (platform + crc32 + WAL + memtable) pass under Debug, ASan, UBSan, and TSan.

**Next:** SSTables — where a full memtable gets flushed to disk, plus the per-SSTable bloom filter that keeps lookups for nonexistent keys cheap as more SSTables accumulate.

---

## Entry 3 — Bloom filter + SSTable

**Decision: FNV-1a for the bloom filter's hashing, not `std::hash`.**
`std::hash`'s actual algorithm is unspecified by the C++ standard and can differ across standard library implementations, or even between builds of the same implementation. A bloom filter serialized to an SSTable file by one binary has to hash keys identically when a *different* binary (a later version of this program, or in principle a different compiler/stdlib) loads that file back — `std::hash` doesn't guarantee that; FNV-1a, implemented directly, does.

**Decision: two hash values (Kirsch-Mitzenmacher), not k independent hash functions.**
Computing k genuinely independent hash functions per key is unnecessary — Kirsch & Mitzenmacher (2006) showed that deriving `g_i(x) = h1(x) + i*h2(x)` from just two real hashes gives asymptotically the same false-positive behavior as k independent ones. `bloom_filter_test.cpp`'s false-positive-rate test (10,000 insertions, 10,000 disjoint probes, checked against a generous margin above the 1% target) is what actually validates this holds in practice, not just in theory.

**Decision: a full per-SSTable index (every key, in memory), not a sparse block-index like RocksDB/LevelDB use.**
Real systems keep a sparse index (one entry per data *block*, not per key) specifically to bound memory use when a single SSTable holds many millions of keys — looking up a key means finding its block via the sparse index, then linearly scanning within that block. This project's full index is simpler and strictly correct, and entirely adequate at the scale a portfolio/demo project actually reaches; documented explicitly as the tradeoff it is, not a limitation being quietly hoped nobody notices. The natural next step if this needed to scale further.

**Decision: `get()`'s bloom-filter check is purely a performance optimization, never load-bearing for correctness.**
This is worth being precise about: `maybe_contains()` returning true doesn't mean the key is actually present — it means "check the index to find out." The index lookup (`std::lower_bound` against the sorted, in-memory index) is what actually determines the answer; a bloom false positive just costs one wasted index check, never a wrong answer. `"a large SSTable's bloom filter correctly skips most absent-key lookups"` tests exactly this: correctness holds for all 1000 present keys and all 10 absent probe keys regardless of whether any of the absent ones happened to trigger a bloom false positive.

**A real bug this round's own test caught, worth stating precisely (this is a bug in a test, not in the SSTable code — but it demonstrates something real about lexicographic vs. numeric ordering that's easy to get wrong anywhere sorted-string data shows up):** the first version of the 1000-key SSTable test generated keys as `"key-" + std::to_string(i)` for `i` in `[0, 1000)` and assumed that was already in sorted order. It wasn't — `"key-10"` sorts *before* `"key-2"` lexicographically, since string comparison compares character-by-character, not by parsed numeric value. `SSTable::create`'s own sorted-input check (added specifically so a caller bug like this fails loudly instead of silently corrupting the on-disk index) caught this immediately, throwing `"entries must be strictly sorted by key"` the first time the test ran. Fixed by zero-padding the generated keys (`"key-0000"`, `"key-0001"`, ...) so lexicographic order matches numeric order. Good example of a validating precondition check earning its keep on the very first real use.

**What's tested:** the bloom filter's no-false-negatives guarantee, empty-filter behavior, measured false-positive rate at 10K-key scale, byte-serialization round-tripping, and rejecting a truncated buffer; the SSTable's create-time sorted-input validation, correct get()/read_all() behavior across present keys/absent keys/tombstones, and file-format validation (bad magic number, too-small file) failing loudly rather than reading garbage. All 33 tests pass under Debug, ASan, UBSan, and TSan.

**Next:** compaction — merging multiple SSTables into fewer, larger ones, and the leveled-vs-size-tiered decision that comes with it.

---

## Entry 4 — Compaction

**Decision: size-tiered compaction, not leveled.**
Leveled compaction (RocksDB/LevelDB's default) organizes SSTables into levels with non-overlapping key ranges *within* a level, and compacts by merging one level into the next — this bounds read amplification well at large scale, but correctness depends on maintaining those per-level key-range invariants, which is real implementation complexity. Size-tiered — merge the full current set of SSTables into one new file once their count crosses a threshold — is simpler to get right and entirely adequate at this project's scale. Named as a deliberate tradeoff in `compaction.h`'s module comment, not a missing feature.

**The consequence of "always merge everything" that makes tombstone-dropping safe: every compaction in this design is implicitly a *full* compaction.**
This is worth being precise about, since it's the kind of detail that's easy to get subtly wrong: a tombstone can only be safely dropped once you're certain no *older* data for that key exists anywhere that the tombstone would otherwise need to keep shadowing. In a leveled scheme, a compaction might merge only levels 2 and 3 while level 4 still has older data for some key — dropping a tombstone there would incorrectly resurrect that older value. Because this project's size-tiered scheme always merges the *entire* current SSTable set at once, there is never any older data left outside the merge — so `compact()` can always drop tombstones outright. `compaction_test.cpp`'s two tombstone tests are what actually pin this down: a tombstone in the *newer* input correctly removes the key entirely (`"a tombstone in the newer SSTable removes the key entirely"`), and — the case that's easy to get backwards — a tombstone in an *older* input does NOT suppress a real value written more recently (`"a tombstone in an OLDER SSTable does not shadow a real value written more recently"`).

**Decision: resolve same-key conflicts via a stable sort by key, not a k-way heap merge.**
A classic LSM-tree merge uses a min-heap of per-SSTable iterators for genuine streaming (never materializing all entries in memory at once). This project instead reads every input SSTable fully into memory (`read_all()`, already a documented scope tradeoff from Entry 3) and does a single stable sort by key — entries are appended in `source_index` order (oldest input first), and a *stable* sort preserves that relative order within any run of equal keys, so the last entry in each group is guaranteed to be the one from the highest `source_index` (the newest write). This is simpler to implement correctly than a heap-based streaming merge and produces identical results at this project's scale; the true streaming k-way merge is the natural next step if SSTables need to grow larger than available memory.

**Decision: `compact()` doesn't delete or modify its input files.**
Kept as a pure "N inputs → 1 output" function specifically so it's simple to test in isolation (as the 7 tests above do) without needing to also reason about file-deletion side effects. File lifecycle — deciding when the old SSTables are safe to remove after a successful compaction — is `Db`'s responsibility once it exists, not compaction's.

**What's tested:** non-overlapping merges, same-key conflict resolution across 2 and 3 overlapping SSTables (confirming the truly newest value wins, not just "a" value), both tombstone-ordering directions described above, and the two trivial-but-worth-checking edge cases (a single-input passthrough, an empty input list). All 40 tests pass under Debug, ASan, UBSan, and TSan.

**Next:** `Db` — the class that actually ties WAL + memtable + SSTables + compaction together into one coherent `Put`/`Get`/`Delete`/`RangeScan` API, plus the crash-recovery integration test that's the real proof this all works end-to-end, not just component-by-component.

---

## Entry 5 — `Db`, and the real crash-recovery test

**Decision: WAL append happens before the memtable is updated, in both `put()` and `remove()` — never the other way around.**
This ordering *is* the durability guarantee, not an implementation detail: if the process crashes between the two lines, recovery replays the WAL and reconstructs the write on restart. If the order were reversed, a crash in that window would lose the write entirely — the memtable holding it is gone (never persisted anywhere), and the WAL never recorded it either, since it hadn't been appended yet.

**Decision: one coarse-grained `std::mutex` guards every `Db` operation, rather than relying on `Memtable`'s own finer-grained `shared_mutex` for concurrent reads.**
`Memtable` is independently thread-safe and independently tested that way (Entry 2's TSan-verified concurrent test) — but `Db` needs additional invariants across *multiple* objects at once (the memtable, the WAL, and the SSTable list must all stay consistent with each other during a flush or compaction), which a single object's internal lock can't provide. Coarse-grained locking is the simple, obviously-correct starting point; it serializes all access rather than allowing concurrent reads during a write, which is the real cost of this choice. Named explicitly as the first place to optimize if Tier 2's networked server (many concurrent client connections) makes this a real throughput bottleneck — not before, since correctness came first.

**Decision: a flush only clears the memtable and resets the WAL *after* the new SSTable has been constructed and confirmed to open correctly.**
`maybe_flush_locked()` calls `SSTable::create(...)` then immediately re-opens it via `std::make_shared<SSTable>(path)` — that constructor call would throw if the just-written file were somehow invalid, and that throw happens *before* `memtable_.clear()` or `wal_->reset()` ever run. Getting this ordering backwards (clear first, discover the flush failed after) would mean data loss with genuinely no path to recovery — the memtable would be empty, the WAL would be reset, and the SSTable that was supposed to hold that data would be broken.

**A genuine cross-platform correctness detail, not just a portability nicety: compaction must close old SSTables' file handles before deleting them.**
POSIX lets you `unlink()` a file that's still open elsewhere — the actual removal is deferred until every handle closes, and nothing breaks in the meantime. Windows generally does not allow deleting a file that has an open handle at all. `SSTable` keeps an open `std::ifstream` for its whole lifetime (for `get()`/`read_all()`), so `maybe_compact_locked()` has to `sstables_.clear()` (destroying every old `SSTable` object, which closes its ifstream via RAII) **before** calling `std::filesystem::remove()` on any of those paths. Getting this backwards would work by pure accident on macOS/Linux and fail outright on Windows — precisely the kind of bug this project's CI matrix (Entry 0) exists to catch before it ships, not after a user reports it.

**The crash-recovery test — the actual flagship artifact of this whole project.**
Two versions exist, deliberately layered:
1. `db_test.cpp`'s `"data still sitting only in the WAL... survives a simulated crash"` — a `Db` instance goes out of scope with data that was never flushed (WAL-only), then a fresh `Db` on the same directory recovers it. Cheap, fast, runs inline in the test binary — but it only proves the code behaves correctly when a C++ destructor runs, which is *not* what a real crash looks like.
2. `crash_recovery_test.cpp` — a genuinely separate OS process (`tools/crash_helper.cpp`, its own small binary) writes 25 keys, prints a completion marker once every `put()` has durably returned, and then spins forever. The test harness spawns it (`fork`+`exec` on POSIX, `CreateProcess` on Windows), blocks on a pipe until it reads the completion marker, then **hard-kills it** (`SIGKILL` / `TerminateProcess`) — no destructors run in the killed process, no cleanup, no `atexit` handlers, nothing. The test then opens a **brand-new `Db`, in this process**, on the directory the killed process was writing to, and confirms every single key survived. This is the real claim the whole WAL/fsync design has been building toward since Entry 1, proven end-to-end across an actual process boundary — not simulated, not assumed.

The process-spawn/pipe/kill code in `crash_recovery_test.cpp` is deliberately *not* folded into the engine's own `platform::sync_file`-style abstraction (Entry 0) — that abstraction is scoped to code the actual database engine needs at runtime; spawning and killing a subprocess is test infrastructure for proving a property, a different concern, kept isolated to this one test file with plain `#ifdef _WIN32` branches rather than manufacturing a shared abstraction for a single one-off use.

**What's tested:** basic Put/Get/Delete/RangeScan correctness (including range-scan's inclusive-start/exclusive-end convention and its correct omission of deleted keys), overwrites correctly shadowing stale flushed data across an SSTable boundary, flush and compaction actually triggering at their configured thresholds while preserving all data, a clean-shutdown-and-reopen recovery scenario, the WAL-only simulated-crash scenario, and — the flagship — the real cross-process hard-kill recovery test. All 52 tests (every component, end to end) pass under Debug, ASan, UBSan, and TSan, including the fork/exec-based crash test under all three sanitizers.

**Next:** Tier 2 — wrapping this engine in a real networked database server (standalone Asio, Redis-style text protocol), a benchmark client with real throughput numbers, and a recorded version of exactly this crash-and-recover demo running against the live server instead of a unit test.
