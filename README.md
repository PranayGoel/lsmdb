# lsmdb

[![CI](https://github.com/PranayGoel/lsmdb/actions/workflows/ci.yml/badge.svg)](https://github.com/PranayGoel/lsmdb/actions/workflows/ci.yml)

A from-scratch LSM-tree key-value storage engine and networked database service, in C++. Built to actually understand — not just use — the storage engine design pattern underneath RocksDB, LevelDB, and Cassandra's storage layer: a write-ahead log for durability, an in-memory sorted buffer, immutable sorted files on disk, background compaction, and bloom filters to keep reads fast as data grows.

Cross-platform by design (CMake + `std::filesystem` + `std::thread` + standalone Asio) — builds and passes its full test suite on Linux, macOS, and Windows via CI, not just the machine it was written on.

## Status

This is being built incrementally, in public, with documentation written as each piece lands — not reconstructed afterward. See [`DESIGN.md`](DESIGN.md) for the running design log (what's built, why each decision was made, what's next).

- [x] Cross-platform build/CI skeleton + platform-abstraction layer (`sync_file`)
- [x] Write-Ahead Log — durable, CRC32-checked, crash/corruption recovery tested
- [x] Memtable — sorted, tombstone-aware, thread-safe (TSan-clean under concurrent load)
- [x] SSTables + bloom filters — immutable on-disk format, full index, FNV-1a Kirsch-Mitzenmacher bloom filter (measured false-positive rate validated at 10K-key scale)
- [x] Compaction — size-tiered merge, correct tombstone/overwrite resolution across overlapping SSTables
- [x] `Db`: full `Put`/`Get`/`Delete`/`RangeScan` API, automatic flush + compaction, **crash-recovery proven against a real hard-killed OS process** (`SIGKILL`/`TerminateProcess`, not simulated)
- [x] **Tier 2 — networked server**: an async TCP server (standalone Asio) exposing the engine over a Redis-inspired text protocol, a benchmark client reporting real measured throughput under concurrent load, and the same real-process-hard-kill recovery guarantee proven again through the actual network protocol
- [ ] **Tier 3 (stretch)** — basic primary-replica log replication

## Building

Requires CMake 3.20+ and a C++20 compiler (gcc, clang, or MSVC).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Usage (library, pre-Tier-2)

```cpp
#include "lsmdb/db.h"

lsmdb::Db db("/path/to/data-dir");   // creates the directory if needed, or
                                      // recovers existing data if it already exists

db.put("hello", "world");
auto value = db.get("hello");        // std::optional<std::string> -> "world"
db.remove("hello");
auto missing = db.get("hello");      // std::nullopt

for (auto& [key, value] : db.range_scan("a", "z")) {
  // every non-deleted key in [a, z), sorted, across the memtable and every SSTable
}
```

Kill the process at any point after a `put()` call returns and reopen a `Db` on the same directory afterward — every acknowledged write is still there. This isn't a claim; it's what `tests/core/crash_recovery_test.cpp` actually does, against a real separately-spawned process, hard-killed with `SIGKILL`/`TerminateProcess`.

## Usage (networked server)

```bash
# Terminal 1: start the server
./build/lsmdb_server ./data 6380

# Terminal 2: talk to it with anything that speaks TCP text -- no custom
# client required
$ nc 127.0.0.1 6380
PING
+PONG
PUT hello world
+OK
GET hello
+world
GET missing
$-1
DELETE hello
+OK
```

Protocol: `PUT key value` / `GET key` / `DELETE key` / `PING`, each terminated by `\r\n` — Redis-inspired (including its `$-1` nil-reply convention for a missing key on `GET`) but not RESP-compatible; simple enough to drive by hand from `nc`, `telnet`, or PowerShell's `Test-NetConnection`.

Kill the server process at any point after a client's `PUT` gets its `+OK`, restart it on the same data directory, reconnect, and every acknowledged write is still there — proven for real in `tests/server/server_crash_recovery_test.cpp`, the same hard-kill guarantee as the library-level test above, exercised this time through the actual network protocol against the actual server binary this project ships.

A concurrent-load benchmark client ships alongside it:

```bash
./build/lsmdb_bench 127.0.0.1 6380 16 5000   # 16 concurrent clients, 5000 PUT+GET pairs each
```

Measured on one development machine (see [`DESIGN.md`](DESIGN.md) Entry 6 for the full run): **~25,200 ops/sec** across 160,000 total operations, zero errors under concurrent load.

## Why this project exists

Most of my other public work (see [basketball_analysis](https://github.com/PranayGoel/basketball_analysis) and [coffee_shop_customer_service_chatbot](https://github.com/PranayGoel/coffee_shop_customer_service_chatbot)) is AI/LLM application work — calling models, orchestrating agents, wiring up RAG. This project is deliberately the opposite: no LLMs, no external APIs — just the classic systems-engineering problem of "how do you make writes fast, reads fast, and data durable, all at once, at scale." Built to demonstrate the concurrency/persistence/correctness-under-failure fundamentals that AI-application work alone doesn't exercise.
