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

## Status: complete — all 8 phases

Results page (published container image, headline benchmark numbers): see
[Deployment](#deployment) below.

- `DiskManager` (`src/storage/disk/`): page-granular (4096-byte, fixed at
  compile time) reads/writes over a single heap file via `pread`/`pwrite`.
- `BufferPoolManager` (`src/buffer/`): fixed frames, pin/unpin, dirty-flag
  write-back, pluggable eviction via a `Replacer` interface.
  - `LRUKReplacer` (k=2) — the production eviction policy.
  - `LRUReplacer` — a plain-LRU baseline kept only to benchmark against.
- `StorageEngine` (`src/engine/`): the Get/Put/Delete/Scan interface both
  storage engines implement identically.
- `BPlusTree` / `BPlusTreeEngine` (`src/index/`): a disk-backed B+-tree
  behind `StorageEngine` — page-overlaid leaf/internal node layouts, insert
  with splits, delete with redistribute/merge, and a range-scan iterator
  over the linked leaf chain. Root page id persists across reopen via a
  metadata page.
- `LSMTreeEngine` (`src/lsm/`): an LSM-tree behind the same `StorageEngine` —
  a skip-list memtable flushed to immutable, Bloom-filtered, page-based
  SSTables, a background thread doing size-tiered compaction (with
  write-side backpressure so a fast writer can't starve it — see
  DESIGN.md), and a k-way merge iterator over the memtable and every live
  SSTable for reads and range scans.
- `LogManager` / `WALBPlusTreeEngine` (`src/wal/`): an append-only,
  page-based write-ahead log and an ARIES-lite recovery layer on top of the
  B+-tree — explicit multi-operation transactions
  (Begin/Put/Delete/Get/Commit/Abort), logical redo/undo with Compensation
  Log Records, sharp periodic checkpoints, and Analysis→Redo→Undo restart
  recovery that jumps straight to the last checkpoint instead of scanning
  the whole log. `LSMTreeEngine` gets a smaller, optional redo-only WAL for
  its memtable (no transactions/undo needed — see DESIGN.md for why).
- `MVCCStore` (`src/mvcc/`): an in-memory, multi-versioned key/value store
  with four isolation levels (`READ_UNCOMMITTED`, `READ_COMMITTED`,
  `SNAPSHOT`, `SERIALIZABLE_SNAPSHOT`) — per-key version chains, explicit
  `Begin`/`Read`/`Write`/`Delete`/`Commit`/`Abort` transactions,
  first-committer-wins conflict detection with deadlock-free chain locking,
  and background-safe garbage collection of versions no longer visible to
  any active snapshot. Deliberately sits beside `StorageEngine` rather than
  implementing it — see DESIGN.md for why.
- A tiny SQL front-end (`src/sql/`): a lexer, recursive-descent parser, and
  planner/executor over `CREATE TABLE` / `INSERT` / `SELECT` / `DELETE`,
  with `WHERE` as an `AND`-conjunction of column comparisons. Every table
  is multiplexed over one shared `StorageEngine` instance via a table-id
  key prefix (`src/sql/catalog.h`). The "tiny optimizer" picks one of three
  access paths per query — `POINT_LOOKUP`, `RANGE_SCAN`, or `FULL_SCAN` —
  based on whether the `WHERE` clause constrains the primary key (the only
  indexed column); see DESIGN.md for the full selection rules and what a
  real cost-based optimizer does that this one doesn't.
- A minimal CLI shell (`alloc` / `write` / `read` / `stats` / `sql`) for
  poking raw pages by hand or running one SQL statement at a time.
- **A read-only HTTP API** (`src/http/`, `dbengine_httpd`): raw POSIX
  sockets, no external web framework — `GET /health` and
  `GET /query?sql=...`, the latter rejecting anything that doesn't parse
  as a `SELECT` before it ever reaches the engine. Single-threaded (one
  connection at a time) by design — see DESIGN.md for why. An optional
  startup schema file loads table definitions/seed data before the server
  starts accepting connections, working around the SQL layer's
  non-persistent catalog without compromising the network API's read-only
  property.
- **Differential testing against a real SQLite** (`test/validation/`,
  test-only — see DESIGN.md for why this doesn't compromise the "no
  external database libraries" rule the engine itself follows): identical
  SQL statement text run against both this project's `Database` and a real
  SQLite, results compared directly, including a 2,000-operation randomized
  fuzzer, run against both `BTREE`- and `LSM`-backed tables.
- GoogleTest suite (201 tests): DiskManager, replacer, BufferPoolManager,
  B+-tree, LSM-tree, WAL/recovery, MVCC, SQL, SQLite-differential, and
  HTTP API correctness, including randomized oracle tests for each storage
  engine and for the SQL layer (run against both `BTREE`- and `LSM`-backed
  tables); a `LogManager` test suite; a `WALBPlusTreeEngine` suite covering
  transactions, live abort/undo, checkpointing, and simulated-crash
  recovery; a **crash-injection harness** that forks a real worker process,
  `SIGKILL`s it at a random point, and verifies recovered state against an
  independent oracle — 150 cycles against the B+-tree, 100 against the
  LSM-tree, every push; an `MVCCStore` suite demonstrating dirty read,
  non-repeatable read, and write skew each appearing and then vanishing
  under the appropriate isolation level, plus a real multi-threaded
  no-lost-updates stress test and GC correctness tests; a SQL suite
  covering the lexer, parser, planner's access-path selection, row
  encoding, and end-to-end query execution; the SQLite differential suite
  above; and an `HttpServer` suite covering routing, JSON responses,
  read-only enforcement, and error handling over real sockets.
- Google Benchmark suites: disk I/O; a Zipfian LRU-K-vs-LRU hit-rate curve;
  B+-tree point-lookup/range-scan/insert-throughput at 1M/10M keys; a
  head-to-head B+-tree-vs-LSM-tree comparison across write-heavy to
  read-heavy workloads; WAL recovery-time-vs-log-size (with and without
  checkpointing) plus WAL-on-vs-off throughput cost; MVCC throughput
  scaling from 1 to 4 threads against a coarse-lock baseline; a SQL
  benchmark comparing indexed vs. unindexed access paths for the same
  logical query at 1K/10K/100K rows; and TPC-C-/TPC-H-*inspired* (not
  compliant — see DESIGN.md) multi-table OLTP and large-table analytical
  scan workloads.
- GitHub Actions CI (build + test + CLI smoke test + Docker build) on every
  push, plus a GHCR image publish workflow and a GitHub Pages results-page
  deploy workflow (both `main`-only) — see [Deployment](#deployment).
- Docker image that builds the engine and runs the CLI (and carries
  `dbengine_httpd` too).

## Build

Requires CMake >= 3.16 and a C++20 compiler (GCC 12+ / Clang 15+). GoogleTest
and Google Benchmark are fetched automatically via `FetchContent` — no
external DB libraries are used anywhere in the *engine itself*. The one
exception, test-only: the Phase 7 differential test suite links a system
SQLite3 (`libsqlite3-dev` on Debian/Ubuntu) purely as a validation oracle —
see DESIGN.md for why that doesn't compromise the engine's own "no external
database libraries" rule. It's optional: if `find_package(SQLite3)` doesn't
find one, that one test file is skipped and everything else still builds.

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

## Run the HTTP API

```sh
./build/src/dbengine_httpd [db-file] [port] [schema-file]
```

`schema-file` is optional: a text file of one SQL statement per line
(`--`-prefixed lines are comments), run once at startup before the server
starts accepting connections — see DESIGN.md for why this exists (the SQL
layer's catalog doesn't persist across a `Database` restart, so a fresh
`dbengine_httpd` process otherwise has no way to know what tables exist).

```sh
$ cat seed.sql
CREATE TABLE users (id INTEGER, name TEXT, age INTEGER)
INSERT INTO users VALUES (1, 'alice', 30)

$ ./build/src/dbengine_httpd data.db 8080 seed.sql &
$ curl http://localhost:8080/health
ok
$ curl 'http://localhost:8080/query?sql=SELECT%20*%20FROM%20users'
{"columns":["id","name","age"],"rows":[[1,"alice",30]],"rows_affected":1,"message":"SELECT 1"}
```

Only `SELECT` is accepted over the API — anything else gets a 400 before
it reaches the engine.

## Deployment

- **Container image**: published to GHCR on every push to `main` via
  `.github/workflows/publish.yml`:
  ```sh
  docker pull ghcr.io/kavyasridhar1501/cpp-database-engine:latest
  ```
- **Results page**: `docs/index.html`, a static, hand-written page with
  this project's headline benchmark numbers, deployed via
  `.github/workflows/pages.yml`. Requires a one-time manual step the
  workflow can't do for you: in the repo's Settings → Pages, set *Source*
  to *GitHub Actions*.
- **Read-only HTTP API**: see above.

## Roadmap

0. Scaffolding, Disk Manager, CI & Docker
1. Buffer Pool Manager (LRU-K eviction)
2. Engine A: disk-backed B+-tree
3. Engine B: LSM-tree (memtable, SSTables, compaction, Bloom filters)
4. Write-ahead log & ARIES-style crash recovery
5. MVCC concurrency (snapshot isolation)
6. SQL front-end & tiny optimizer
7. Validation against SQLite, TPC-C/H-style workload
8. Deployment (GHCR image, results page, read-only HTTP API)

All 8 phases complete. See DESIGN.md for trade-off notes (including what
was deliberately cut and why) and BENCHMARKS.md for the full results log.
