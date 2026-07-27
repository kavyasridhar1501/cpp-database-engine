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
