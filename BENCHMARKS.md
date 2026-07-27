# Benchmarks

Running log of benchmark results, phase by phase. All numbers are from
Google Benchmark (`benchmark/`) and are indicative, not authoritative —
re-run `reproduce` on your own hardware before citing a number.

## How to reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/benchmark/dbengine_bench
```

Add `--benchmark_filter=<regex>` to run a subset, `--benchmark_repetitions=N`
for statistical stability, or `--benchmark_out=results.json
--benchmark_out_format=json` to save machine-readable output.

## Phase 0 — Disk Manager

**What's measured:** `DiskManager::ReadPage` / `WritePage` latency and
throughput over a single 16 MiB heap file (4096 pages x 4096 bytes), pages
pre-allocated (and, for read benchmarks, pre-written) before timing starts so
first-touch effects don't pollute the numbers. "Sequential" walks page ids
`0..N-1` in order; "random" walks a fixed Fisher-Yates shuffle of the same
page ids. Both hit the OS page cache (no `O_DIRECT`, no cache-drop between
runs), so this is measuring the `pread`/`pwrite` + page-cache path, not raw
disk hardware — that's the right thing to measure here, since everything
above `DiskManager` (buffer pool, engines) will also see the OS cache.

**Environment this run:** 4-core Intel Xeon @ 2.10GHz, containerized sandbox
(cloud dev environment, not bare metal) — throughput numbers will vary
significantly on real hardware/NVMe and are recorded here for trend-tracking
across phases, not as an absolute performance claim.

**Results** (`--benchmark_min_time=0.5s`, Release build, GCC 13):

| Benchmark           | Time/op  | Throughput   |
|---------------------|----------|--------------|
| `BM_SequentialWrite` | 565 ns  | 6.75 GiB/s   |
| `BM_RandomWrite`     | 605 ns  | 6.30 GiB/s   |
| `BM_SequentialRead`  | 458 ns  | 8.33 GiB/s   |
| `BM_RandomRead`      | 516 ns  | 7.39 GiB/s   |

**Takeaways:**
- Sequential is faster than random for both reads and writes, as expected,
  but the gap is small (~7-12%) because this working set (16 MiB) comfortably
  fits in the page cache — there's no real seek cost to amortize since
  nothing touches physical disk layout at this phase. The gap should widen
  once Phase 1's buffer pool is tested against a working set that doesn't fit
  in RAM, and again once real disks (vs. this sandbox's block storage) are in
  the loop.
- Reads are consistently faster than writes, consistent with `pwrite` going
  through the page cache's dirty-page bookkeeping while `pread` on a
  recently-written (cached) page is closer to a memcpy.
- These numbers are the Phase 0 baseline for the buffer-pool hit-rate
  benchmark in Phase 1 (cache hit-rate curve vs. pool size) and the point
  lookup / insert throughput numbers in Phase 2 — both will be judged against
  "how much better than raw page I/O" rather than in isolation.

## Phase 1 — Buffer Pool Manager (LRU-K vs. plain LRU)

**What's measured:** cache hit rate as a function of buffer pool size (in
frames), for `BufferPoolManager` configured with the production replacer
(`LRUKReplacer`, k=2) versus the plain-LRU baseline (`LRUReplacer`), replaying
the *same* fixed access trace through both. Each data point is a single
deterministic replay (`->Iterations(1)`) against a fresh `DiskManager` file
and fresh `BufferPoolManager` — no state carries over between pool sizes or
between replacers.

Two traces:
- **`Zipfian`**: 200,000 accesses over 10,000 pages, Zipfian skew 0.99 (the
  standard YCSB default) — a small set of pages absorbs most of the traffic,
  matching real skewed key-popularity.
- **`ZipfianWithScan`**: 100,000 Zipfian accesses over a 2,000-page hot set,
  with a 300-page cold sequential scan (over a disjoint 2,000-page cold
  range) injected every 2,000 accesses — the standard workload for exposing
  a replacement policy's vulnerability to "sequential flooding" (O'Neil,
  O'Neil & Weikum, SIGMOD 1993).

**Reproduce:**
```sh
./build/benchmark/dbengine_bench --benchmark_filter='Zipfian'
```

**Results** (4-core sandbox, same caveats as Phase 0 — trend data, not a
hardware claim):

| Pool size | LRU-K hit rate | LRU hit rate | LRU-K advantage |
|-----------|---------------:|-------------:|-----------------:|
| 8         | 24.0%          | 10.4%        | +13.6 pts |
| 16        | 31.4%          | 16.8%        | +14.6 pts |
| 32        | 38.8%          | 24.5%        | +14.3 pts |
| 64        | 46.2%          | 32.5%        | +13.7 pts |
| 128       | 52.6%          | 40.6%        | +12.0 pts |
| 256       | 58.5%          | 49.0%        | +9.5 pts |
| 512       | 65.2%          | 57.6%        | +7.6 pts |
| 1024      | 72.3%          | 66.5%        | +5.8 pts |
| 2048      | 79.6%          | 75.9%        | +3.8 pts |

*(pure Zipfian trace; `BM_Zipfian_LRUK` vs `BM_Zipfian_LRU`)*

| Pool size | LRU-K hit rate | LRU hit rate | LRU-K advantage |
|-----------|---------------:|-------------:|-----------------:|
| 8         | 25.1%          | 12.6%        | +12.5 pts |
| 16        | 33.2%          | 20.0%        | +13.2 pts |
| 32        | 40.9%          | 28.2%        | +12.7 pts |
| 64        | 47.8%          | 36.6%        | +11.2 pts |
| 128       | 54.8%          | 45.0%        | +9.8 pts |
| 256       | 61.7%          | 52.8%        | +8.9 pts |
| 512       | 69.0%          | 60.5%        | +8.5 pts |
| 1024      | 76.8%          | 70.5%        | +6.3 pts |

*(Zipfian + periodic scan-pollution trace; `BM_ZipfianWithScan_LRUK` vs
`BM_ZipfianWithScan_LRU`)*

**Takeaways:**
- LRU-K beats plain LRU at *every* pool size tested, on both traces — the
  deliverable's "beat plain LRU on the same trace" bar is met without
  qualification.
- The advantage is largest at small pool sizes (roughly +13-15 points at
  8-64 frames) and narrows as the pool gets large enough to hold most of the
  hot set anyway (both policies converge toward the trace's inherent hit
  ceiling). This matches the theory: LRU-K's edge comes from not letting a
  single recent touch buy a page a spot in a scarce cache, and that edge
  matters most exactly when the cache is scarce.
- The scan-pollution trace doesn't show a *larger* gap than pure Zipfian
  here — both traces show LRU-K winning by a similar margin — which is a bit
  softer than the "LRU-K crushes LRU under scans" story sometimes told for
  this workload. At these scan parameters (300-page scan every 2,000
  accesses, hot set of only 2,000 pages) the scan isn't large or frequent
  enough relative to the hot set to fully flush it under LRU either. A
  bigger scan-to-hot-set ratio would likely widen the gap further; the
  honest result here is that LRU-K wins clearly on both a plain skewed
  workload and a scan-polluted one, without needing the scan to make its
  case.
- `BM_Zipfian_LRUK` runtime grows noticeably faster than `BM_Zipfian_LRU`'s
  as pool size increases (63ms → 729ms from pool size 8 to 2048, vs. LRU's
  roughly flat ~50-65ms). Both replacers scan their evictable set on every
  eviction (see DESIGN.md), but LRU-K's per-frame work (deque bookkeeping,
  the +inf/finite comparison) is heavier per element — a known, documented
  scaling limitation, not a correctness issue, and irrelevant to the hit-rate
  result above.

## Phase 2 — B+-Tree (Engine A)

**What's measured:** point-lookup latency, range-scan throughput, and
insert throughput at 1M and 10M keys, via `BPlusTreeEngine` (the
`StorageEngine` adapter). Keys are inserted in **shuffled**, not sequential,
order for all three benchmarks — see DESIGN.md for why. The buffer pool is
fixed at 2,000 frames (8MB) for both scales: ~10.5% of the 1M-key dataset
(~76MB) but only ~1.05% of the 10M-key one (~760MB), i.e. an index that
doesn't fit in RAM at either scale, and proportionally less of it fits as N
grows.

**Reproduce:**
```sh
./build/benchmark/dbengine_bench --benchmark_filter='PointLookup|RangeScan|InsertThroughput'
```
(The 10M-key insert-throughput run alone takes several minutes — filter it
out with e.g. `--benchmark_filter='PointLookup|RangeScan'` for a quick
check.)

**Results** (4-core sandbox, same caveats as Phase 0/1 — trend data, not a
hardware claim):

| Benchmark | 1M keys | 10M keys |
|---|---:|---:|
| Point lookup (random key) | 23.5 µs/op (42.5k ops/s) | 25.8 µs/op (38.8k ops/s) |
| Range scan (1,000-key window, random start) | 546 µs/scan (1.83M keys/s) | 599 µs/scan (1.67M keys/s) |
| Insert throughput (fresh tree, shuffled keys) | 18.67 s total (53.6k ops/s) | 276.2 s total (36.2k ops/s) |

**Takeaways:**
- **Point lookup barely changes across a 10x increase in data** (23.5µs →
  25.8µs, +10%) — this is the B+-tree's core promise showing up directly in
  the numbers. A lookup costs one page fetch per tree level, tree height
  grows logarithmically (both 1M and 10M keys fit in a 3-level tree at this
  fanout), so the dominant cost per lookup — descending from root to leaf
  under a mostly-cold buffer pool — is nearly flat regardless of N.
- **Range scan is similarly flat** (1.83M → 1.67M keys/s) since it's
  dominated by walking the leaf chain, a cost per key that doesn't depend
  on tree depth at all.
- **Insert throughput drops substantially at scale** (53.6k → 36.2k ops/s,
  -32%) — unlike the two read paths, this one is *not* flat, and that's the
  most interesting result in this section. The B+-tree's own algorithmic
  cost per insert is still O(log N) (same shallow-tree argument as lookups),
  so the slowdown isn't the tree getting structurally harder to navigate —
  it's that the fixed 2,000-frame buffer pool is a shrinking fraction of a
  growing dataset, so a larger share of each insert's page touches (finding
  the leaf, touching siblings during a split, updating parent pointers)
  miss the pool and pay for real page I/O plus an LRU-K eviction (itself an
  O(pool size) linear scan — see DESIGN.md). This is a genuine cost of
  pairing a fixed-size buffer pool with an ever-growing B+-tree, not a bug,
  and it's the baseline Phase 3's LSM-tree needs to beat: an LSM's write
  path (append to an in-memory memtable, flush when full) is expected to
  degrade much less with N, since it doesn't need to touch the on-disk
  structure at all for most writes. That comparison is the Phase 3
  deliverable.

## Phase 3 — Head-to-Head: B+-Tree vs. LSM-Tree

**What's measured:** both engines, populated with the same 100,000 keys
(shuffled insertion order, same methodology as Phase 2) and the same
buffer-pool budget (2,000 frames for the B+-tree; the LSM-tree's SSTables
each get their own small 8-frame pool — see DESIGN.md), then driven through
a Zipfian-distributed (skew 0.99) mixed read/write workload swept across
write fractions from 5% to 90%, plus a dedicated range-scan workload
(500 scans of a 100-key window from random start points). Reported per
point: throughput, average latency, disk reads per operation ("read
amplification"), and total on-disk bytes versus the logical dataset size
("space amplification").

**Reproduce:**
```sh
./build/benchmark/dbengine_bench --benchmark_filter='MixedWorkload|RangeScan_(BPlusTree|LSM)'
```

**Results** (4-core sandbox, same caveats as earlier phases):

| Write % | B+-tree ops/s | LSM ops/s | B+-tree reads/op | LSM reads/op |
|--------:|--------------:|----------:|-----------------:|-------------:|
| 5       | 541k          | 1.19M     | 0.106            | 0.401        |
| 10      | 538k          | 1.27M     | 0.106            | 0.370        |
| 30      | 560k          | 1.50M     | 0.106            | 0.263        |
| 50      | 550k          | 1.58M     | 0.106            | 0.198        |
| 70      | 550k          | 1.85M     | 0.106            | 0.119        |
| 90      | 530k          | 1.97M     | 0.106            | 0.042        |

| Write % | B+-tree space amp | LSM space amp |
|--------:|-------------------:|---------------:|
| 5–30    | 7.56×               | 4.84×           |
| 50–70   | 7.56×               | 5.34×           |
| 90      | 7.56×               | 5.64×           |

Range scan (100-key window, random start): B+-tree 51.8k scans/s (1.238
disk reads/scan), LSM 88.2k scans/s (3.852 disk reads/scan).

**Takeaways:**
- **No throughput crossover in this range — the LSM-tree wins outright at
  every write fraction tested**, and the gap *widens* as writes increase
  (1.19M → 1.97M ops/s for the LSM-tree, essentially flat at ~530-560k for
  the B+-tree). This is the direct, expected consequence of the two
  write paths: a B+-tree write is a tree descent plus (at this buffer-pool
  size) real page I/O, roughly constant cost regardless of what fraction of
  the workload is writes; an LSM-tree write is an in-memory skip-list
  insert that's O(1) amortized against disk (cost is paid later, in bulk,
  at flush/compaction time). At no point in the 5-90% range does adding
  more writes hurt the LSM-tree's throughput — if anything it helps, since
  fewer of the ops are the (relatively) more expensive multi-SSTable reads.
- **The classic B+-tree/LSM-tree crossover shows up clearly in read
  amplification instead, and it *does* cross within the tested range.**
  The B+-tree's disk-reads-per-op is flat at ~0.106 regardless of workload
  mix (a lookup always costs the same tree descent). The LSM-tree's is
  workload-dependent: 0.401 reads/op at 5% writes (mostly reads, each one
  potentially checking several SSTables past the memtable) down to 0.042 at
  90% writes (mostly writes, which are memtable-only; the few reads that do
  happen more often hit the memtable or the first SSTable checked). The two
  lines cross somewhere around 75-85% writes — below that, the LSM-tree
  reads more pages per operation than the B+-tree does; above it, fewer.
  This is the textbook LSM trade-off (cheap writes, potentially-amplified
  reads) made directly measurable, and it's *why* Bloom filters matter: at
  5% writes the LSM-tree is still only touching 0.4 pages/op on average
  across however many SSTables exist, because a Bloom filter miss costs
  nothing (a bit-array check, no disk I/O) — without it, this number would
  be far higher and the crossover point would shift well to the right.
- **Space amplification is higher for the B+-tree (7.56×) than the
  LSM-tree (4.84-5.64×) at every write fraction, and for a specific,
  identifiable reason, not because one engine is "worse."** Both engines
  pay the same fixed-size-value tax documented in DESIGN.md (every ~8-byte
  test value padded to a 64-byte slot) — that alone accounts for roughly
  4-5× of both numbers. The B+-tree's *additional* overhead beyond that
  comes from page fill factor: random-order inserts and B-tree splits
  leave leaf pages roughly 65-70% full on average (a well-known property of
  B-trees under random insertion), whereas the LSM-tree's SSTable builder
  bulk-packs sorted data into pages at essentially 100% fill (only the last
  page of a run is ever partially empty). That fill-factor gap is a real,
  structural LSM advantage — bulk-sorted writes pack tighter than
  incremental random-order ones — independent of the fixed-value-size tax
  both engines share.
- **LSM space amplification grows with write fraction** (4.84× at 5% writes
  → 5.64× at 90%), which is exactly the expected LSM behavior: more writes
  mean more not-yet-compacted data sitting in tier 0 at any given moment
  (updates to existing keys leave stale copies behind until compaction
  removes them), so space amplification is a function of write load, not a
  fixed constant — unlike the B+-tree, where it's roughly workload-
  independent (updates overwrite in place; the tree's shape barely changes).
- **Range scan: the LSM-tree is faster in raw throughput (88.2k vs. 51.8k
  scans/s) despite reading more disk pages per scan (3.852 vs. 1.238).**
  This looks counter-intuitive but has a specific cause: a single LSM scan
  fans out across the memtable and multiple SSTables simultaneously (the
  merge iterator holds one cursor per source), so "disk reads per scan"
  counts touches across several small, page-cache-friendly per-SSTable
  pools rather than one larger shared pool — more total page touches, but
  each one cheaper on average given the sandbox's warm OS page cache. This
  is more a statement about this benchmark's scale (100k keys, few tiers)
  and the sandbox environment than a general claim that LSM range scans are
  always faster; the more SSTables accumulate before compaction catches
  up, the more cursors a scan fans out across, and that trend would
  eventually reverse this result at larger scale or under heavier write
  load — a good candidate for a deeper follow-up if this project continues
  past its planned phases.
