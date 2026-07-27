# cpp-database-engine

A disk-backed database engine built from scratch in C++20, as a head-to-head
comparison of two storage engines (B+-tree vs. LSM-tree) behind one interface,
plus ARIES-style crash recovery. Architecture is informed by CMU 15-445 and
Hellerstein, Stonebraker & Hamilton's *Architecture of a Database System*
(cited in [DESIGN.md](DESIGN.md)); the code itself is original, not derived
from BusTub.

Built in phases, each with its own tests and benchmark numbers — see
[DESIGN.md](DESIGN.md) for trade-off notes and [BENCHMARKS.md](BENCHMARKS.md)
for results.

## Status: Phase 0 — Scaffolding & Disk Manager

- `DiskManager` (`src/storage/disk/`): page-granular (4096-byte, fixed at
  compile time) reads/writes over a single heap file via `pread`/`pwrite`.
- A minimal CLI shell (`alloc` / `write` / `read` / `stats`) for poking pages
  by hand.
- GoogleTest suite validating byte-for-byte read/write correctness,
  zero-fill semantics, and persistence across reopen.
- Google Benchmark suite for sequential vs. random page I/O.
- GitHub Actions CI (build + test + CLI smoke test + Docker build) on every
  push.
- Docker image that builds the engine and runs the CLI.

## Build

Requires CMake >= 3.16 and a C++20 compiler (GCC 12+ / Clang 15+). GoogleTest
and Google Benchmark are fetched automatically via `FetchContent` — no
external DB libraries are used anywhere in this project.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

## Benchmark

```sh
./build/benchmark/dbengine_bench
```

See [BENCHMARKS.md](BENCHMARKS.md) for how to reproduce and interpret results.

## Run the CLI

```sh
./build/src/dbengine_cli [path-to-db-file]
```

```
db> alloc
allocated page 0
db> write 0 hello world
wrote 11 bytes to page 0
db> read 0
page 0: hello world
db> stats
pages allocated: 1
disk reads:      1
disk writes:     1
```

## Run via Docker

```sh
docker build -t dbengine .
docker run --rm -it -v dbengine-data:/home/dbengine/data dbengine
```

## Roadmap

0. Scaffolding, Disk Manager, CI & Docker *(current)*
1. Buffer Pool Manager (CLOCK / LRU-K eviction)
2. Engine A: disk-backed B+-tree
3. Engine B: LSM-tree (memtable, SSTables, compaction, Bloom filters)
4. Write-ahead log & ARIES-style crash recovery
5. MVCC concurrency (snapshot isolation)
6. SQL front-end & cost-based optimizer
7. Validation against SQLite, TPC-C/H-style workload
8. Deployment (GHCR image, results page, optional read-only HTTP API)
