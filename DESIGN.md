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

---

## Entry 5.1 — a real cross-platform bug the CI matrix actually caught

Worth recording precisely, since it's exactly the kind of thing this project's whole cross-platform setup (Entry 0) exists to catch: after pushing Entry 5's `Db` work, CI failed on **Linux and Windows, and every sanitizer leg (which runs on Ubuntu), while passing on macOS** — the build broke everywhere except the machine it was written on.

**Root cause:** `memtable.cpp` calls `std::unique_lock` but only `#include`s `memtable.h`, which itself includes `<shared_mutex>` — not `<mutex>`. On macOS's libc++, `<shared_mutex>` happens to transitively pull in `<mutex>` as an implementation detail, so `std::unique_lock` resolved anyway and the local build looked completely fine. On Linux's libstdc++ (used by both the plain Ubuntu build and, not coincidentally, every sanitizer leg — they all run on `ubuntu-latest`), that transitive include doesn't happen, and the build failed with `'unique_lock' is not a member of 'std'`.

**Why this is a meaningfully different bug than the ones documented so far:** every earlier "fixed by X" entry in this log describes a logic bug caught by a test. This one is a *portability* bug that a correct, fully-passing local test suite could never have caught, because the local machine's standard library happened to paper over it. The CI matrix's actual job — proving behavior on more than one machine — is precisely what surfaced it, on the very first push where it mattered. It's also a clean illustration of the difference between "a header transitively includes something as an implementation detail" (fragile, standard-library-specific, not a contract) and "a header explicitly and intentionally includes what it needs" (portable, because it's a decision this project controls, not an accident of a specific vendor's header layout) — see the `<filesystem>` audit below, which is an example of the *safe* version of relying on a header's own declared includes.

**Fix:** added `#include <mutex>` directly to `memtable.h`, next to the existing `<shared_mutex>` — one line.

**Follow-up audit (before pushing the fix, not after):** rather than fixing just this one instance and hoping nothing else had the same problem, every source file was checked for `std::mutex`/`std::unique_lock`/`std::lock_guard` usage against which files actually `#include <mutex>` directly. Every other `.cpp` file in the project turned out to be fine — `wal.cpp`, `sstable.cpp`, and `db.cpp` all get `<mutex>` through their own header (`wal.h`/`sstable.h`/`db.h`), each of which *directly* includes `<mutex>` itself (not through a further transitive chain) — that's the safe pattern described above, not the fragile one. A parallel audit of `std::filesystem::path` usage across every test file found the same "included via a directly-including header" shape, confirmed safe for the same reason. `memtable.h`'s missing `<mutex>` was the one genuine gap.

**The honest process lesson, stated plainly:** this bug shipped to `main` in the first place because CI wasn't checked after every push during this stretch of the build — local Debug/ASan/UBSan/TSan builds all passed (on this one machine, with this one standard library), which is necessary but not sufficient evidence of correctness for a project whose entire premise includes "works on Windows and Linux too." The fix going forward: treat a push as unverified until the CI matrix itself reports green, not just the local build.

---

## Entry 5.2 — the other cross-platform bug the CI matrix caught: deleting a file that's still open

After Entry 5.1's fix, `ubuntu-latest`, `macos-latest`, and all three sanitizer legs went green — but `windows-latest` still failed, at the *test* step, not the build. The actual exception, retrieved from the raw CI log (not just the truncated summary line Catch2 prints first):

```
remove: The process cannot access the file because it is being used by
another process.: "C:\Users\RUNNER~1\...\lsmdb_wal_test_291156071.wal"
```

**Root cause — a real bug, not flakiness, in every test's own cleanup, not the engine.** `tests/core/wal_test.cpp`'s `"reset() discards prior records..."` test constructs a `WriteAheadLog wal(path)`, exercises it, then calls `std::filesystem::remove(path)` — while `wal` is *still in scope*, holding its `out_` ofstream open, because the object isn't destructed until the enclosing `TEST_CASE` function returns at the closing brace, which happens *after* the remove call. The identical shape — construct a file-owning object (`WriteAheadLog`, `SSTable`, or `Db`, each of which holds an open file handle for its entire lifetime by design), then delete or `remove_all` its backing file/directory while that object is still alive — was present in every test in `wal_test.cpp`, `sstable_test.cpp`, `compaction_test.cpp`, `db_test.cpp`, and `crash_recovery_test.cpp`.

This "worked" on macOS and Linux purely because POSIX's `unlink()` doesn't care whether a file is still open elsewhere — the directory entry is removed immediately and the underlying inode's storage is reclaimed only once the last open handle closes, invisibly to the caller. Windows has no equivalent: deleting a file (or a directory containing one) while any handle to it is open, without that handle having been explicitly opened with `FILE_SHARE_DELETE`, fails outright. `std::ofstream`/`std::ifstream` don't request that sharing flag. This is the exact same category of bug as Entry 5.1 — a real behavioral difference between platforms that a fully-passing macOS/Linux test suite structurally cannot surface, because the thing that makes it a bug (an object outliving the file it holds open) is invisible until you run somewhere that actually enforces the constraint.

**Two distinct fixes, for two distinct causes bundled into the same symptom:**

1. **Test structure — the real bug.** Every test that constructed a file-owning object and then deleted its backing path now scopes that object in a nested `{ }` block so its destructor (and the file-handle close it performs) runs *before* the delete, matching how these objects are actually used in production — nothing in real usage ever deletes a database's files while a live `Db` still has them open; only the tests' own cleanup pattern violated that.
2. **A genuinely transient case, handled separately.** Even with objects correctly scoped, the OS's own delayed handle-release timing, or (a well-documented GitHub Actions `windows-latest` flakiness pattern) Windows Defender's real-time scanner briefly opening a just-written file for inspection, can still cause a delete to fail immediately after a file was legitimately and fully closed. This is transient by nature, so it calls for a different remedy than scoping: a new `platform::remove_file()` was added alongside `sync_file()` — the same abstraction point (Entry 0's rule: all OS-specific behavior lives in `platform/file_sync.h`) — that retries `std::filesystem::remove()` up to 10 times, 50ms apart, before giving up and rethrowing. POSIX's implementation is a pass-through (unlink has no such failure mode to retry against); every engine call site that removes a just-closed file (`WriteAheadLog::reset()`, `Db::maybe_compact_locked()`'s old-SSTable cleanup) and every test's own per-file cleanup now goes through it instead of calling `std::filesystem::remove()` directly.

**Why both fixes were needed, not just one:** the retry alone would not have fixed the `wal_test.cpp` failure, because that failure wasn't transient — `wal`'s handle was never going to close within any retry window, since the object itself wasn't destructed yet. Conversely, scoping alone doesn't address the possibility of a genuinely transient lock on a file that *has* actually been closed, which is a real, separate failure mode worth being defensive against on Windows CI runners regardless. Diagnosing which of the two was actually responsible for this specific failure required reading the full, untruncated exception message from the raw CI log rather than trusting Catch2's summary line — the summary alone ("unexpected exception") gives no signal on which of these two very different root causes is in play.

With this fix, all 52 tests pass on `ubuntu-latest`, `macos-latest`, `windows-latest`, and all three sanitizer legs — Tier 1 is genuinely, verifiably cross-platform, not just claimed to be.

---

## Entry 6 — Tier 2: the networked server, over standalone Asio

**Decision: standalone (Boost-free) Asio, not hand-rolled sockets, for the networking layer.**
The cross-platform requirement extends to networking exactly the way it already applies to durability (Entry 0): POSIX exposes BSD sockets, Windows exposes Winsock, and they're similar but not identical (different headers, different init/teardown calls, different error-code conventions). Asio is a mature, portable abstraction over that difference — the same relationship `platform::sync_file` has to `fsync`/`FlushFileBuffers`, just upstream and pre-built rather than hand-derived, because there's no engineering value in re-solving socket-API portability from scratch when a widely-used library already does it correctly. "Standalone" Asio (fetched directly from its own GitHub repo, not via Boost) avoids pulling in all of Boost for one header-only library — `ASIO_STANDALONE` is the compile definition that tells Asio's headers to skip every Boost dependency.

**Decision: a genuine text protocol, Redis-inspired but not RESP-compatible — `PUT key value`, `GET key`, `DELETE key`, `PING`.**
The goal was a protocol simple enough to drive by hand from `nc`, `telnet`, or PowerShell's `Test-NetConnection` with zero custom tooling — a real, demoable, cross-platform-testable interface, not something that only this project's own client can speak. `GET` on a missing key replies with Redis's classic `$-1` nil sentinel specifically because it's instantly recognizable to anyone who's used `redis-cli`, a small but real signal of familiarity with how production key-value stores actually communicate "not found." Full RESP-protocol compliance was deliberately out of scope — it buys binary-safety and multi-bulk replies this project's demo doesn't need, at the cost of a meaningfully more complex parser, for no benefit here.

**Decision: protocol parsing/dispatch (`server::dispatch`) is a pure function, entirely independent of any socket type.**
`dispatch(Db&, const std::string& line) -> std::string` takes one already-received line and a `Db&`, and returns the exact response bytes. This is what makes `tests/server/protocol_test.cpp` possible without ever opening a real socket — every command's semantics (PUT/GET/DELETE/PING, malformed-input handling, values-with-spaces) gets a fast, isolated unit test. The real-socket path (`Session`, `Server`) is tested separately, over actual TCP, in `tests/server/server_test.cpp` — deliberately layered the same way Tier 1's WAL-only vs. real-process-kill crash tests are (Entry 5): a cheap, precise unit test for the logic, plus a slower, end-to-end test proving the wiring around it actually works.

**Decision: async I/O via `enable_shared_from_this`, not one thread per connection.**
Each `Session` keeps itself alive across its own chain of `async_read_until`/`async_write` calls by capturing `shared_from_this()` in every completion handler — the standard Asio idiom for object lifetime under callback-based async I/O. A `Session` is destroyed (closing its socket via RAII) the moment neither its read nor write chain has a pending operation left, which happens naturally on client disconnect or any socket error, with no explicit cleanup code required anywhere. `Db`'s own internal mutex (Entry 5) is what makes running the shared `io_context` across every hardware thread (`tools/lsmdb_server.cpp`) safe for genuinely concurrent client connections — the server layer adds no locking of its own, and doesn't need to.

**Decision: `Server`'s port parameter accepts `0` to mean "let the OS choose."**
`asio::ip::tcp::acceptor` treats port 0 as "bind an ephemeral port," and `Server::local_port()` (reading `acceptor_.local_endpoint().port()` after construction) exposes whatever the OS actually picked. Every test that spins up a real `Server` uses this instead of a hardcoded port number, which is what makes the test suite safe to run concurrently (e.g. multiple CI jobs, or a developer running tests while another instance is already listening on some fixed "obvious" port) without ever colliding.

**The flagship artifact: `tests/server/server_crash_recovery_test.cpp` — the Tier 2 version of Entry 5's real hard-kill crash test.**
Spawns the actual `lsmdb_server` binary this project ships (not a test double) as a real, separate OS process, listening on an OS-assigned port it announces via a machine-parseable `LISTENING <port>` first line of stdout. A real TCP client connects and issues ten `PUT`s, each acknowledged with `+OK`. The server process is then hard-killed (`SIGKILL`/`TerminateProcess` — the same technique as Entry 5's engine-level test, in this file rather than folded into the shared platform abstraction, for the same "test infrastructure vs. engine code" reason documented there) with zero chance to flush anything beyond what each `put()` already did synchronously through the WAL. A fresh `lsmdb_server` process is then spawned on the same data directory (a new OS-assigned port, since immediately reusing the just-killed one isn't guaranteed available on every platform), and a new client confirms every one of the ten keys is still there. This is the actual end-to-end claim of the whole project, proven across both a real process boundary *and* the real network protocol — not simulated, not assumed, not just "the engine underneath is durable, trust that the server exposes it correctly too."

**A real, measured throughput number, not a claimed one:** `tools/lsmdb_bench.cpp` drove 16 concurrent client connections, 5,000 PUT+GET pairs each (160,000 total operations, each worker writing and reading back its own disjoint key range so the number reflects genuine concurrent throughput rather than lock contention on a shared hot key), against a locally running `lsmdb_server` on this development machine:

```
clients: 16, ops/client: 5000 (PUT+GET each)
total operations: 160000
elapsed: 6.3382s
throughput: 25243.8 ops/sec
put errors: 0, get errors: 0
```

Zero errors under concurrent load is as important a result here as the throughput figure itself — a benchmark that only measures speed and never checks correctness isn't a very convincing artifact. (This number is specific to one unremarkable development laptop and one specific configuration; it's reported as a demonstrated, reproducible measurement — `lsmdb_bench <host> <port> <clients> <ops>` against any running server — not a performance claim meant to generalize.)

Manually exercised with real `nc`, confirming the protocol is exactly as usable by hand as intended:
```
$ printf 'PING\r\nPUT demo-key demo value with spaces\r\nGET demo-key\r\nGET missing-key\r\nDELETE demo-key\r\nGET demo-key\r\n' | nc 127.0.0.1 7891
+PONG
+OK
+demo value with spaces
$-1
+OK
$-1
```

**What's tested:** protocol-logic correctness in isolation (`protocol_test.cpp`: all four commands, malformed input, values containing spaces, empty values), the real-socket server wiring (`server_test.cpp`: round-trip over an actual TCP connection, two independent connections sharing one consistent database view, many sequential commands on one connection), and the flagship real-process-kill-and-restart recovery test described above — all passing on Linux, macOS, and Windows, and under ASan/UBSan/TSan.

**Next:** Tier 3 (stretch, only if time remains) — basic primary-replica log replication: one primary streams its WAL to a follower, which replays it; killing the primary and confirming the follower already has the data, with manual promotion (no automatic failover/leader-election required for this to be a genuine, honest talking point).

---

## Entry 7 — Tier 3 (stretch): primary-replica replication

**Decision: replicate re-expressed commands, not raw WAL bytes.**
The plan's original framing ("stream the WAL, replay it on the follower") suggests literally shipping the primary's binary WAL file across the wire. Implemented instead: whenever a client's `PUT`/`DELETE` is accepted by a primary, the exact same command is re-sent, verbatim, to every subscribed follower as `REPLICATE PUT key value` / `REPLICATE DELETE key`. A follower applies each one through its own `Db::put()`/`Db::remove()` — the identical code path a local client write takes, meaning a follower's own WAL and SSTables are just as durable and crash-recoverable as the primary's, not an in-memory mirror that vanishes if the follower itself restarts. This is functionally the same idea (ship every mutation, replay it in order) without requiring the follower to parse the primary's exact binary WAL framing across a process (and potential version) boundary — a real, simpler, and more robust design for what the plan explicitly scopes as "not full consensus," and it reuses `server::dispatch()` (Entry 6) instead of inventing a second command-application path that could drift from the first.

**Decision: a follower subscribes over an ordinary client connection via a new `SYNC` command, not a separate replication port.**
Any existing TCP connection to a primary can become a replication stream — `Session::handle_line` recognizes `SYNC` as a request to (a) send a full snapshot of every currently-live key, then (b) keep pushing every future mutation to that same connection, indefinitely. This means any `lsmdb_server`, whether or not it's itself a follower of some other primary, can also act as a replication source for a downstream follower of its own — chained replication falls out of the design for free, without a special case for it.

**Decision: `Db::all_entries()` — reusing `range_scan`'s merge logic, factored out, rather than inventing a "return everything" boundary hack.**
A follower joining needs the primary's *entire* current state as a starting snapshot, not just writes that happen to occur after it subscribes. `range_scan(start, end)` already merges every SSTable and the memtable into one sorted view and filters by range — but `std::string` has no safe "largest possible key" sentinel to fake a range covering everything (an arbitrary key can contain any byte, including `0xFF`). Instead, the actual merge logic was extracted into a private `Db::merge_all_locked()`, and both `range_scan()` (which filters the result by range) and the new public `all_entries()` (which doesn't) build on top of it — real, existing duplication factored out because two callers needed it now, not a speculative abstraction for a hypothetical future one.

**Decision: read-only enforcement lives in `server::dispatch()` itself, as a `read_only` parameter, not as a separate check layered in `Session`.**
A replica must reject ordinary client `PUT`/`DELETE` — all its data is supposed to arrive only via replication from its primary, never from a client that connected directly to the wrong server. Putting this check inside `dispatch()` (checked before validating the rest of the command, so a replica rejects a malformed write with the same "read-only" message it gives a well-formed one, matching how a real replica behaves) keeps the single source of truth for "what can this command actually do" in one place, and means `ReplicationClient` applying an incoming `REPLICATE` line — which must always be allowed to write, since that's the entire point of being a follower — simply calls `dispatch()` at its default (`read_only = false`) rather than needing a second, parallel code path.

**A genuine, new concurrency correctness detail Tier 3 introduces that Tier 2 didn't have: a `Session`'s socket can now receive concurrent writes from two different threads, and needs a strand to stay safe.**
Before replication, a `Session` only ever wrote its own request's response, and never issued another write until that one completed and the next command was read — one write in flight at a time, always initiated from within that same Session's own completion-handler chain. Replication breaks that invariant: `ReplicationHub::publish()` can push a `REPLICATE` line onto a **different** Session's socket (the subscribed follower's) from **whichever io_context thread happens to be running the publishing Session's own write** — and since `tools/lsmdb_server.cpp` runs the shared `io_context` across every hardware thread (Entry 6), that could genuinely be a different OS thread than the one about to process the follower Session's own next request. Asio's threading model requires a single socket's operations never be initiated concurrently from multiple threads without explicit synchronization; two unsynchronized `async_write` calls racing on the same socket is a real data race, not just a logical ordering concern. Fixed by giving every `Session` its own `asio::strand`, with every write (`send_raw()`'s push path and the normal request-response reply) dispatched through it via `asio::post` — the strand guarantees no two writes to that Session's socket ever execute concurrently, regardless of which thread queues them.

**An accepted, documented, narrow race — not hidden as if it didn't exist:** `Session::handle_line`'s `SYNC` handler subscribes the follower to `ReplicationHub` **before** reading `db_.all_entries()` for the snapshot (not after). Subscribing first guarantees no write is ever missed in the gap between "read the snapshot" and "start receiving live updates" — the worst case is a write that lands in that exact window being present in *both* the snapshot and a separately published live message, applied twice on the follower. For `PUT`/`DELETE`, applying the identical operation twice is idempotent and harmless; the alternative ordering (snapshot first, subscribe second) would instead risk a write being missed *entirely*, which is the failure mode that actually matters. A real production replication protocol would resolve this with sequence numbers or log offsets; accepted here as an honest, bounded tradeoff appropriate to a feature the plan itself scopes as "not full consensus."

**Explicitly narrower test scope than Tiers 1 and 2, and why that's the right call, not a shortcut:** `tests/server/replication_test.cpp` proves the actual new claim Tier 3 introduces — data flows from primary to follower (both the pre-existing snapshot and live writes), and the follower's copy survives being reopened completely independent of the primary — by closing and reopening the follower's `Db` directly, in-process, rather than spawning a third layer of separate-process-plus-`SIGKILL` test infrastructure like Entry 5's and Entry 6's flagship tests. That's a deliberate scope decision, not a gap: `tests/core/crash_recovery_test.cpp` already proves "a `Db` survives a real hard-killed OS process," and `tests/server/server_crash_recovery_test.cpp` already proves "a server exposes that guarantee correctly over the network" — Tier 3 doesn't need to re-derive either of those from scratch a third time; it only needs to prove the one thing that's actually new here, which the in-process test does directly and completely.

**A real, manually-run two-process demo** (primary on port 7901, follower on port 7902 started with `--replica-of 127.0.0.1 7901`), matching exactly what the automated test proves, run for real against the actual `lsmdb_server` binary:

```
$ printf 'PUT alpha one\r\nPUT beta two\r\n' | nc 127.0.0.1 7901
+OK
+OK
$ ./lsmdb_server ./follower-data 7902 --replica-of 127.0.0.1 7901
LISTENING 7902
REPLICA-OF 127.0.0.1 7901

$ printf 'GET alpha\r\nGET beta\r\nPUT should-fail x\r\n' | nc 127.0.0.1 7902
+one
+two
-ERR this server is a read-only replica

$ printf 'PUT gamma three\r\n' | nc 127.0.0.1 7901     # written to the primary AFTER the follower subscribed
+OK
$ printf 'GET gamma\r\n' | nc 127.0.0.1 7902            # ... and immediately visible on the follower
+three

# now the primary process is killed outright
$ printf 'PUT x y\r\n' | nc 127.0.0.1 7901; echo $?
1                                                        # connection refused -- the primary is gone

$ printf 'GET alpha\r\nGET beta\r\nGET gamma\r\n' | nc 127.0.0.1 7902
+one
+two
+three                                                    # the follower, a completely independent
                                                           # process, still has everything
```

**What's tested:** the read-only rejection and mutation-classification logic in isolation (`protocol_test.cpp`), and the full snapshot-plus-live-replication-plus-independent-durability flow described above (`replication_test.cpp`) — all 70 tests across every tier passing on Linux, macOS, and Windows, and under ASan/UBSan/TSan.

**This closes out the plan's full scope: Tier 1 (storage engine), Tier 2 (networked server), and Tier 3 (stretch: replication) are all built, tested, and documented as they were built.**
