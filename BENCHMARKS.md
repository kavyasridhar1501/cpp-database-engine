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
