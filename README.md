# lsmdb

A from-scratch LSM-tree key-value storage engine and networked database service, in C++. Built to actually understand — not just use — the storage engine design pattern underneath RocksDB, LevelDB, and Cassandra's storage layer: a write-ahead log for durability, an in-memory sorted buffer, immutable sorted files on disk, background compaction, and bloom filters to keep reads fast as data grows.

Cross-platform by design (CMake + `std::filesystem` + `std::thread` + standalone Asio) — builds and passes its full test suite on Linux, macOS, and Windows via CI, not just the machine it was written on.

## Status

This is being built incrementally, in public, with documentation written as each piece lands — not reconstructed afterward. See [`DESIGN.md`](DESIGN.md) for the running design log (what's built, why each decision was made, what's next).

- [ ] **Tier 1 — storage engine**: WAL, memtable, SSTables, bloom filters, compaction, crash recovery, core `Put`/`Get`/`Delete`/`RangeScan` API
- [ ] **Tier 2 — networked server**: TCP server over the engine, a benchmark client with real throughput numbers, a recorded crash-and-recover demo
- [ ] **Tier 3 (stretch)** — basic primary-replica log replication

Currently in progress: the cross-platform build/CI skeleton and the platform-abstraction layer (`sync_file`) that everything else builds on.

## Building

Requires CMake 3.20+ and a C++20 compiler (gcc, clang, or MSVC).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Why this project exists

Most of my other public work (see [basketball_analysis](https://github.com/PranayGoel/basketball_analysis) and [coffee_shop_customer_service_chatbot](https://github.com/PranayGoel/coffee_shop_customer_service_chatbot)) is AI/LLM application work — calling models, orchestrating agents, wiring up RAG. This project is deliberately the opposite: no LLMs, no external APIs — just the classic systems-engineering problem of "how do you make writes fast, reads fast, and data durable, all at once, at scale." Built to demonstrate the concurrency/persistence/correctness-under-failure fundamentals that AI-application work alone doesn't exercise.
