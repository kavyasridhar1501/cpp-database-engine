# Benchmarks

Results log, phase by phase. All numbers are from Google Benchmark
(`benchmark/`) and are indicative, not authoritative. Re-run on your own
hardware before citing a number.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/benchmark/dbengine_bench
```

- `--benchmark_filter=<regex>` runs a subset.
- `--benchmark_repetitions=N` for statistical stability.
- `--benchmark_out=results.json --benchmark_out_format=json` for
  machine-readable output.

## Phase 0: Disk Manager

**What's measured**: `ReadPage`/`WritePage` latency and throughput over a
16 MiB heap file (4096 pages x 4096 bytes). Pages are pre-allocated (and
pre-written, for reads) before timing starts. "Sequential" walks page IDs
in order; "random" walks a fixed shuffle. Both hit the OS page cache (no
`O_DIRECT`), so this measures the `pread`/`pwrite` + cache path.

Environment: 4-core sandbox, containerized (not bare metal). Numbers are
for trend-tracking across phases, not an absolute hardware claim.

| Benchmark | Time/op | Throughput |
|---|---|---|
| `BM_SequentialWrite` | 565 ns | 6.75 GiB/s |
| `BM_RandomWrite` | 605 ns | 6.30 GiB/s |
| `BM_SequentialRead` | 458 ns | 8.33 GiB/s |
| `BM_RandomRead` | 516 ns | 7.39 GiB/s |

Takeaways:
- Sequential beats random for both reads and writes, but by only 7-12%.
  This 16 MiB working set fits comfortably in page cache, so there's no
  real seek cost yet. The gap should widen once Phase 1 tests a working
  set that doesn't fit in RAM.
- Reads are consistently faster than writes. Matches `pwrite` going
  through dirty-page bookkeeping while `pread` on a cached page is close
  to a memcpy.
- These numbers are the baseline for Phase 1's hit-rate benchmark and
  Phase 2's point-lookup/insert numbers: both get judged against "how much
  better than raw page I/O," not in isolation.

## Phase 1: Buffer Pool Manager (LRU-K vs plain LRU)

**What's measured**: cache hit rate vs buffer pool size, for
`LRUKReplacer` (k=2) vs `LRUReplacer`, replaying the same fixed access
trace through both. Each data point is a single deterministic replay
against a fresh `DiskManager` and `BufferPoolManager`.

Two traces:
- **Zipfian**: 200,000 accesses over 10,000 pages, skew 0.99 (YCSB
  default).
- **ZipfianWithScan**: 100,000 Zipfian accesses over a 2,000-page hot set,
  with a 300-page cold scan injected every 2,000 accesses. Standard
  workload for exposing sequential-flooding vulnerability (O'Neil, O'Neil
  & Weikum, SIGMOD 1993).

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='Zipfian'
```

**Zipfian trace** (`BM_Zipfian_LRUK` vs `BM_Zipfian_LRU`):

| Pool size | LRU-K hit rate | LRU hit rate | LRU-K advantage |
|---|---|---|---|
| 8 | 24.0% | 10.4% | +13.6 pts |
| 16 | 31.4% | 16.8% | +14.6 pts |
| 32 | 38.8% | 24.5% | +14.3 pts |
| 64 | 46.2% | 32.5% | +13.7 pts |
| 128 | 52.6% | 40.6% | +12.0 pts |
| 256 | 58.5% | 49.0% | +9.5 pts |
| 512 | 65.2% | 57.6% | +7.6 pts |
| 1024 | 72.3% | 66.5% | +5.8 pts |
| 2048 | 79.6% | 75.9% | +3.8 pts |

**Zipfian + scan pollution** (`BM_ZipfianWithScan_LRUK` vs `_LRU`):

| Pool size | LRU-K hit rate | LRU hit rate | LRU-K advantage |
|---|---|---|---|
| 8 | 25.1% | 12.6% | +12.5 pts |
| 16 | 33.2% | 20.0% | +13.2 pts |
| 32 | 40.9% | 28.2% | +12.7 pts |
| 64 | 47.8% | 36.6% | +11.2 pts |
| 128 | 54.8% | 45.0% | +9.8 pts |
| 256 | 61.7% | 52.8% | +8.9 pts |
| 512 | 69.0% | 60.5% | +8.5 pts |
| 1024 | 76.8% | 70.5% | +6.3 pts |

Takeaways:
- LRU-K beats plain LRU at every pool size tested, on both traces.
- The advantage is largest at small pool sizes (+13-15 points at 8-64
  frames) and narrows as the pool grows large enough to hold most of the
  hot set. LRU-K's edge is not letting one recent touch buy a scarce
  cache slot, and that matters most when the cache is scarce.
- The scan-pollution trace doesn't show a bigger gap than plain Zipfian.
  At these parameters (300-page scan every 2,000 accesses, 2,000-page hot
  set), the scan isn't large enough relative to the hot set to fully flush
  it under LRU either. A bigger scan-to-hot-set ratio would likely widen
  the gap.
- `BM_Zipfian_LRUK` slows down more than `BM_Zipfian_LRU` as pool size
  grows (63ms to 729ms from size 8 to 2048, vs LRU's flat ~50-65ms). Both
  replacers scan their evictable set on every eviction; LRU-K's per-frame
  work is heavier. A scaling limitation, not a correctness issue, and it
  doesn't affect the hit-rate result above.

## Phase 2: B+-Tree (Engine A)

**What's measured**: point-lookup latency, range-scan throughput, and
insert throughput at 1M and 10M keys via `BPlusTreeEngine`. Keys insert in
shuffled order for all three benchmarks. Buffer pool is fixed at 2,000
frames (8MB) for both scales: ~10.5% of the 1M-key dataset, ~1.05% of the
10M-key one.

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='PointLookup|RangeScan|InsertThroughput'
```
The 10M-key insert run alone takes several minutes. Filter it out with
`--benchmark_filter='PointLookup|RangeScan'` for a quick check.

| Benchmark | 1M keys | 10M keys |
|---|---|---|
| Point lookup (random key) | 23.5 us/op (42.5k ops/s) | 25.8 us/op (38.8k ops/s) |
| Range scan (1,000-key window) | 546 us/scan (1.83M keys/s) | 599 us/scan (1.67M keys/s) |
| Insert throughput (fresh tree, shuffled) | 18.67s total (53.6k ops/s) | 276.2s total (36.2k ops/s) |

Takeaways:
- Point lookup barely changes across a 10x increase in data (23.5us to
  25.8us, +10%). A lookup costs one page fetch per tree level, and both
  1M and 10M keys fit in a 3-level tree at this fanout.
- Range scan is similarly flat (1.83M to 1.67M keys/s), dominated by
  walking the leaf chain, a cost per key that doesn't depend on depth.
- Insert throughput drops substantially at scale (53.6k to 36.2k ops/s,
  -32%). The algorithmic cost per insert is still O(log N); the slowdown
  is that the fixed 2,000-frame pool is a shrinking fraction of a growing
  dataset, so more of each insert's page touches miss the pool and pay
  for real I/O plus an LRU-K eviction. This is the baseline Phase 3's
  LSM-tree needs to beat.

## Phase 3: Head-to-Head, B+-Tree vs LSM-Tree

**What's measured**: both engines populated with the same 100,000 keys
(shuffled order), same methodology as Phase 2, driven through a
Zipfian-distributed mixed read/write workload swept across write
fractions from 5% to 90%, plus a dedicated range-scan workload (500 scans
of a 100-key window). Reported: throughput, latency, disk reads/op (read
amplification), and on-disk bytes vs logical size (space amplification).

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='MixedWorkload|RangeScan_(BPlusTree|LSM)'
```

| Write % | B+-tree ops/s | LSM ops/s | B+-tree reads/op | LSM reads/op |
|---|---|---|---|---|
| 5 | 541k | 1.19M | 0.106 | 0.401 |
| 10 | 538k | 1.27M | 0.106 | 0.370 |
| 30 | 560k | 1.50M | 0.106 | 0.263 |
| 50 | 550k | 1.58M | 0.106 | 0.198 |
| 70 | 550k | 1.85M | 0.106 | 0.119 |
| 90 | 530k | 1.97M | 0.106 | 0.042 |

| Write % | B+-tree space amp | LSM space amp |
|---|---|---|
| 5-30 | 7.56x | 4.84x |
| 50-70 | 7.56x | 5.34x |
| 90 | 7.56x | 5.64x |

Range scan (100-key window): B+-tree 51.8k scans/s (1.238 reads/scan),
LSM 88.2k scans/s (3.852 reads/scan).

Takeaways:
- No throughput crossover in this range. The LSM-tree wins at every write
  fraction tested, and the gap widens as writes increase (1.19M to 1.97M
  ops/s for LSM, flat around 530-560k for the B+-tree). A B+-tree write is
  a tree descent plus real page I/O; an LSM write is an in-memory
  skip-list insert, paid for later at flush/compaction time.
- The classic crossover shows up in read amplification instead, and it
  does cross in the tested range. B+-tree reads/op is flat at ~0.106
  regardless of workload. LSM's is workload-dependent: 0.401 at 5% writes
  down to 0.042 at 90%. The two lines cross around 75-85% writes. This is
  the textbook LSM trade-off (cheap writes, amplified reads) made
  measurable, and it's why Bloom filters matter: at 5% writes the LSM-tree
  still averages only 0.4 pages/op because a Bloom filter miss costs
  nothing.
- Space amplification is higher for the B+-tree (7.56x) than the LSM-tree
  (4.84-5.64x) at every write fraction. Both engines pay the same
  fixed-value-size tax (every ~8-byte test value padded to 64 bytes),
  which accounts for roughly 4-5x of both numbers. The B+-tree's extra
  overhead comes from page fill factor: random-order inserts leave leaf
  pages 65-70% full on average, while the LSM-tree's SSTable builder
  bulk-packs sorted data at close to 100% fill.
- LSM space amplification grows with write fraction (4.84x at 5% writes to
  5.64x at 90%), because more writes mean more not-yet-compacted stale
  data sitting in tier 0 at any given moment. The B+-tree's is roughly
  workload-independent since updates overwrite in place.
- Range scan: the LSM-tree is faster in raw throughput (88.2k vs 51.8k
  scans/s) despite reading more disk pages per scan (3.852 vs 1.238). A
  single LSM scan fans out across the memtable and multiple SSTables, each
  with its own small page-cache-friendly pool, so more total touches but
  each one cheaper on average at this scale (100k keys, few tiers, warm
  page cache). This trend would likely reverse at larger scale or heavier
  write load.

## Phase 4: Write-Ahead Log & ARIES-Style Recovery

**What's measured**: recovery time (the constructor's cost reopening a
database with an existing log) as a function of log size, with and
without periodic checkpointing, plus the throughput cost of running with
the WAL on vs off. All runs use a 64-frame buffer pool and a 1,000-key
range, so what's varying is log size, not tree size.

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='RecoveryTime|PutThroughput'
```

| Log size (ops) | Recovery, no checkpoint | Recovery, checkpoint every 100 txns |
|---|---|---|
| 1,000 | 0.44 ms | 0.016 ms |
| 5,000 | 1.93 ms | 0.015 ms |
| 10,000 | 3.96 ms | 0.016 ms |
| 20,000 | 8.93 ms | not swept |
| 50,000 | 22.2 ms | not swept |

The checkpointed sweep stops at 10,000 because it needs one real
fsync per operation to trigger periodic checkpointing; the no-checkpoint
variant batches all setup writes into one commit to reach larger log
sizes. Both are pre-crash setup cost, not part of what's timed.

| | Ops/sec | Time for 2,000 Puts |
|---|---|---|
| WAL on (log + fsync every commit) | 9,659/s | 1,519 ms |
| WAL off | 1,017,820/s | 1.96 ms |

Takeaways:
- Recovery time scales linearly with log size when there's nothing to
  bound it (0.44ms to 22.2ms from 1,000 to 50,000 ops, ~50x more log, ~50x
  more recovery time). Expected: redo replays every record from the
  redo-start point forward.
- With periodic checkpoints, recovery time is flat regardless of log size
  (0.015-0.016ms across the whole sweep). Getting this required fixing a
  real bug first: recovery correctly replayed only the log since the last
  checkpoint, but located that checkpoint by scanning the whole log from
  the start. This benchmark caught it directly, since the "with
  checkpoints" line was scaling linearly too. Fix: persist the checkpoint
  LSN in the metadata page. See DESIGN.md.
- The WAL's throughput cost is dominated by fsync, not logging itself. A
  ~105x difference between on and off (9.66k vs 1.02M ops/s) for identical
  work. This is the expected cost of the durability guarantee. The
  standard mitigation, not implemented here, is group commit: batching
  multiple commits into one fsync.

## Phase 5: MVCC Concurrency

**What's measured**: throughput of `MVCCStore` under an 80% read / 20%
write point workload over 10,000 keys, swept across 1, 2, and 4 threads,
each running its own autocommit `SNAPSHOT` transaction per operation.
Compared against a baseline with the same workload backed by a plain
`std::unordered_map` behind one mutex.

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='MVCCFixture' \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

`real_time` mode so wall-clock reflects actual parallelism. `cv` up to
~20% at 2 threads, sandbox scheduler noise.

| Threads | MVCC ops/sec | Coarse-lock ops/sec |
|---|---|---|
| 1 | 375k/s | 23.8M/s |
| 2 | 572k/s | 3.20M/s |
| 4 | 554k/s | 1.39M/s |

Takeaways:
- MVCC throughput holds roughly flat from 1 to 4 threads. The coarse-lock
  baseline collapses ~17x over the same range (23.8M/s to 1.39M/s).
  Fine-grained per-key locking lets independent transactions on different
  keys make progress in parallel; a single global mutex serializes every
  operation regardless of which keys are touched.
- MVCC's absolute per-operation throughput is far lower than the
  coarse-lock baseline's, roughly 60x slower single-threaded. Expected,
  not a bug: every autocommit op here pays for a full `Begin`/`Commit`
  pair (heap allocation, atomic counters, shard mutex, version-chain
  machinery) against a workload that's just `unordered_map::find` for the
  baseline. The fair comparison is scaling shape, not the absolute number.
- Getting the flat line required fixing a real bottleneck. The initial
  implementation used one mutex for the whole active-transaction table.
  Since every autocommit op does one `Begin` and one `Commit` against that
  table, it was contended on nearly every operation regardless of key.
  Measured with that version, throughput fell from 1 to 4 threads (450k/s
  to 168k/s) instead of holding steady. Fix: shard the table into 16
  independently-locked shards. See DESIGN.md.
- Correctness under concurrency is checked separately, not by this
  benchmark. `MVCCStoreTest.ConcurrentIncrementNoLostUpdates` runs 4 real
  threads doing 200 read-increment-write-commit cycles each against a
  shared counter, with retry-on-conflict, and asserts the final value is
  exactly `threads x increments`.

## Phase 6: SQL Front-End & Tiny Optimizer

**What's measured**: the same logical query issued two ways against an
identical `BTREE`-backed table, where `id` (the primary key) and `tag`
hold identical values. `WHERE id = X` lets the planner pick
`POINT_LOOKUP`/`RANGE_SCAN`; `WHERE tag = X` is logically equivalent but
`tag` isn't indexed, so the planner falls back to `FULL_SCAN`. Swept
across 1,000 / 10,000 / 100,000 rows.

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='PointLookup_.*Predicate|RangeScan_.*Predicate'
```

| Rows | Point, indexed | Point, unindexed | Slowdown |
|---|---|---|---|
| 1,000 | 1.12 us | 51.7 us | 46x |
| 10,000 | 2.13 us | 869 us | 408x |
| 100,000 | 3.86 us | 14,444 us | 3,742x |

| Rows | Range, indexed | Range, unindexed | Slowdown |
|---|---|---|---|
| 1,000 | 14.4 us | 66.8 us | 4.6x |
| 10,000 | 19.9 us | 941 us | 47x |
| 100,000 | 29.6 us | 16,358 us | 553x |

Takeaways:
- Indexed access paths grow slowly (roughly logarithmic) with table size;
  unindexed paths grow roughly linear. `POINT_LOOKUP` costs one B+-tree
  descent regardless of which row; `FULL_SCAN` costs a full pass over
  every row. At 100,000 rows the gap is ~3,700x for a point query and
  ~550x for a bounded range, and both ratios are still growing.
- Both queries in each pair are the same logical request over the same
  table. Only the column named in `WHERE` differs. `PlanQuery`
  (`src/sql/planner.cpp`) turns "does this predicate touch the primary
  key" into which access path to run, and this benchmark confirms that
  decision is worth orders of magnitude, not a marginal optimization.
- The range-scan gap is smaller than the point-lookup gap at every row
  count, expected rather than a discrepancy. `RANGE_SCAN` still pays an
  O(log n) descent to find its start, same as `POINT_LOOKUP`, but both
  indexed and unindexed paths then iterate the same ~100-row window,
  while `POINT_LOOKUP` touches one row and `FULL_SCAN` touches every row.

## Phase 7: Validation, Differential Testing & TPC-Style Workloads

**What's measured**:
- A TPC-C-inspired mixed OLTP transaction (`benchmark/tpcc_bench.cpp`): 1
  customer lookup, 10x (1 stock lookup + 1 stock update + 1 order-line
  insert), 1 order insert. 13 SQL statements per "transaction," against a
  4-warehouse dataset, `BTREE`- and `LSM`-backed.
- A TPC-H-inspired Q1-style filter (`benchmark/tpch_bench.cpp`): `SELECT
  l_extendedprice FROM lineitem WHERE l_shipdate <= X` against a
  300,000-row fact table, swept across selectivity (10% / 50% / 90%).

Neither is compliant with the official TPC-C/TPC-H specs. See DESIGN.md
for what that would require.

**Reproduce**:
```sh
./build/benchmark/dbengine_bench --benchmark_filter='TPCC'
./build/benchmark/dbengine_bench --benchmark_filter='TPCH'
```

| Engine | Time/transaction | Transactions/sec |
|---|---|---|
| B+-tree | 90.9 us | 11,003/s |
| LSM-tree | 66.4 us | 15,287/s |

| Selectivity | Matched rows (of 300,000) | Query time |
|---|---|---|
| 10% | 30,300 | 55.0 ms |
| 50% | 150,300 | 69.2 ms |
| 90% | 270,300 | 80.8 ms |

Takeaways:
- LSM beats B+-tree on the New-Order-style transaction (15,287/s vs
  11,003/s, ~1.4x), consistent with Phase 3's finding that LSM wins
  write-heavy workloads. 11 of 13 statements in this transaction are
  writes.
- The TPC-H-style scan's cost grows far more slowly than its result size.
  A 9x increase in matched rows (30,300 to 270,300) costs only ~1.5x more
  time (55.0ms to 80.8ms). Expected for a full-table scan with no usable
  index: cost tracks table size, not result size.
- Together with Phase 6, these two results cover the planner's access-path
  choice completely for this project's grammar. Phase 6 shows indexed
  access is independent of table size and far cheaper than `FULL_SCAN`
  at the same size. This phase shows that within `FULL_SCAN`, cost is
  independent of result size and dependent only on table size.

## Phase 8: Deployment

No new benchmark for this phase. It's about making the existing engine
and its results reachable (a published image, a results page, a read-only
HTTP front-end), not adding storage-engine behavior to measure. See
DESIGN.md for the race condition (`HttpServer::Run()` vs `Stop()`) its
test suite caught.

This closes the 8-phase plan:
- Phases 2-3: a disk-backed B+-tree and LSM-tree behind one interface.
- Phase 4: ARIES-style crash recovery proven against real `kill -9`s.
- Phase 5: MVCC snapshot isolation, with the anomalies it does and
  doesn't prevent made visible.
- Phase 6: a SQL front-end with a real, tiny cost-based optimizer.
- Phase 7: correctness validated against an independent implementation
  and industry-benchmark-inspired workloads.
- Phase 8: a way to reach any of it without cloning the repo.
