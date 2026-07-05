# lsmdb

[![CI](https://github.com/PranayGoel/lsmdb/actions/workflows/ci.yml/badge.svg)](https://github.com/PranayGoel/lsmdb/actions/workflows/ci.yml)

A key-value storage engine written from scratch in C++, using the same LSM-tree design that RocksDB, LevelDB, and Cassandra are built on. I wanted to actually understand how these things work under the hood instead of just importing one and calling it a day, so this has a write-ahead log, an in-memory sorted buffer, immutable sorted files on disk, background compaction, and bloom filters — the whole pipeline.

It's cross-platform (Linux, macOS, Windows), and CI actually builds and runs the full test suite on all three, not just the machine I wrote it on.

## Status

I built this in public and wrote the design notes as I went instead of writing them up after the fact — see [`DESIGN.md`](DESIGN.md) for the full log of what got built, why, and what tripped me up along the way.

- [x] Cross-platform build setup + CI, with a platform abstraction layer for the one bit of OS-specific code (`fsync`/`FlushFileBuffers`)
- [x] Write-ahead log — CRC32-checked, tested against crashes and corrupted writes
- [x] Memtable — sorted, tombstone-aware, thread-safe (ran it under TSan to be sure)
- [x] SSTables + bloom filters — immutable on-disk format with a full index and an FNV-1a bloom filter
- [x] Compaction — size-tiered, merges overlapping SSTables and resolves tombstones/overwrites correctly
- [x] `Db`: the actual Put/Get/Delete/RangeScan API, with automatic flush + compaction. Crash recovery is tested by literally `SIGKILL`-ing a separate process mid-write and reopening the data directory afterward
- [x] Tier 2 — a networked server on top of the engine (async TCP, standalone Asio), plus a benchmark client and the same crash-recovery test but this time through the wire protocol
- [x] Tier 3 (stretch goal) — basic primary/replica replication

## Building

You need CMake 3.20+ and a C++20 compiler (gcc, clang, or MSVC all work).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Using it as a library

```cpp
#include "lsmdb/db.h"

lsmdb::Db db("/path/to/data-dir");   // creates the directory if it doesn't exist,
                                      // or picks up existing data if it does

db.put("hello", "world");
auto value = db.get("hello");        // std::optional<std::string> -> "world"
db.remove("hello");
auto missing = db.get("hello");      // std::nullopt

for (auto& [key, value] : db.range_scan("a", "z")) {
  // every live key in [a, z), sorted, across the memtable and every SSTable
}
```

You can kill the process right after a `put()` returns, reopen a `Db` on the same directory, and the write will still be there. `tests/core/crash_recovery_test.cpp` proves this for real — it spawns a separate process, hard-kills it, then opens a fresh `Db` on the same data and checks everything survived.

## Running it as a server

```bash
# start it
./build/lsmdb_server ./data 6380

# talk to it from another terminal with anything that speaks plain TCP
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

The protocol is `PUT key value` / `GET key` / `DELETE key` / `PING`, each line ending in `\r\n`. It's loosely inspired by Redis (the `$-1` for a missing key is a straight lift from `redis-cli`) but it's not RESP-compliant — just simple enough to poke at with `nc`, `telnet`, or PowerShell's `Test-NetConnection` without needing a real client.

Same crash guarantee as above, just proven through the network this time in `tests/server/server_crash_recovery_test.cpp`: kill the server after it acknowledges a write, restart it, reconnect, and the data's there.

There's also a small benchmark client:

```bash
./build/lsmdb_bench 127.0.0.1 6380 16 5000   # 16 concurrent clients, 5000 PUT+GET pairs each
```

On my laptop that gets roughly **25,200 ops/sec** across 160k total operations with zero errors under concurrent load (full numbers in [`DESIGN.md`](DESIGN.md), Entry 6).

## Replication (the stretch goal)

Any server can act as a primary. Point a second one at it and it becomes a follower:

```bash
# the primary
./build/lsmdb_server ./primary-data 7901

# a follower, replicating from it
./build/lsmdb_server ./follower-data 7902 --replica-of 127.0.0.1 7901
```

The follower pulls a full snapshot of whatever the primary already had, then gets every write after that in real time — and it writes everything through the same durable path a normal client write would use, so it's a real copy on disk, not just something held in memory. It also refuses direct writes from clients (`-ERR this server is a read-only replica`), since the whole point is that its data only comes from the primary. Kill the primary outright and the follower keeps everything it had. There's a full walkthrough of this with real terminal output in `DESIGN.md`, Entry 7.

This is intentionally basic — manual promotion, no leader election, no failover. Wanted the real primary/replica idea without pretending I'd built Raft.

## Why I built this

The rest of my public projects ([basketball_analysis](https://github.com/PranayGoel/basketball_analysis), [coffee_shop_customer_service_chatbot](https://github.com/PranayGoel/coffee_shop_customer_service_chatbot)) are all AI/LLM stuff — calling models, wiring up agents, RAG pipelines. This one's the opposite on purpose. No LLMs, no APIs, just the actual systems problem of making writes fast, reads fast, and data durable at the same time. It's the kind of concurrency-and-correctness-under-failure work that doesn't really come up when you're mostly gluing API calls together, and I wanted something in my portfolio that shows I can do it.
