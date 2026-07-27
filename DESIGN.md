# Design Notes

Running log of architectural decisions and trade-offs, phase by phase.
Citations point to the CMU 15-445 course materials and Hellerstein, Stonebraker
& Hamilton, *Architecture of a Database System* (2007), which this project
uses as its architectural reference (code is original, not derived from
BusTub).

## Phase 0 — Disk Manager

**Fixed page size (4096 bytes).** Matches the common OS page/sector size, so a
page-granular read or write is (on typical filesystems) a single aligned I/O.
4096 is a compile-time constant (`dbengine::PAGE_SIZE` in `src/common/config.h`)
rather than configurable at runtime: every layer above the disk manager
(buffer pool frames, node layouts in later phases) is sized against it, and
BusTub/15-445 make the same simplifying choice for the same reason — it lets
higher layers reason about "one page" as a fixed-size, self-contained unit
without a runtime parameter threading through every interface.

**pread/pwrite over seek+read/write.** `DiskManager` never calls `lseek`;
every access is `pread(fd, buf, PAGE_SIZE, page_id * PAGE_SIZE)` /
`pwrite(...)`. This makes reads and writes to *different* pages safe to issue
concurrently from multiple threads without external locking, since the offset
is passed per-call instead of mutated shared file-descriptor state. That
matters starting in Phase 1, where the buffer pool manager will dispatch
concurrent page I/O.

**Page-id allocation is a monotonic counter, not a free list.** Phase 0 has no
delete path, so `AllocatePage()` is `next_page_id_.fetch_add(1)` and pages are
never reused. A free list (recycling pages freed by B+-tree merges / LSM
compaction) is deferred to Phase 2+, once there's something that actually
frees pages. Introducing it now would be speculative.

**Reading an unwritten/past-EOF page zero-fills rather than erroring.** A page
id can be allocated (reserved) before it's ever written — the buffer pool will
rely on this when it allocates a page for a brand-new B+-tree node and
initializes it in memory before the first flush. Treating "never written" as
zero-filled data (rather than a distinct error state) keeps the interface to
one call (`ReadPage`) instead of needing an existence check first, and it
mirrors sparse-file semantics that most filesystems already give you for free
via `pread` short-reads.

**No implicit fsync on every write.** `WritePage` uses `pwrite`, which lands
in the OS page cache; durability requires an explicit `Sync()` call. This is
deliberate: Phase 0 has no WAL yet, so there is no correctness story around
crash consistency to enforce. Once Phase 4 implements the ARIES *write-ahead
logging rule* (log record for an update must be durable before the
corresponding data page is written back — Hellerstein et al. §5, 15-445
Lecture on Logging & Recovery), `Sync()` ordering between the WAL file and the
heap file becomes the enforcement point.

**Single heap file, not per-table files.** All pages, regardless of what
they'll eventually hold, live in one file addressed by page id. This matches
BusTub/15-445's disk manager (and most real single-file engines, e.g.
SQLite) and avoids introducing a table/file-mapping concept before there are
tables (that arrives with the SQL front-end in Phase 6).

**Error handling: exceptions, not error codes.** `IOException` is thrown on
any short read/write or failed `open`/`fstat`/`fsync`. At this layer, I/O
failures are unrecoverable precondition violations (bad path, disk full,
permissions) rather than expected control flow, so an exception that
propagates to the CLI's top-level catch is simpler than threading
`std::expected`-style results through every call site this early. This may be
revisited once transactions need to distinguish "abort this transaction" from
"the process should crash."

## Phase 1 — Buffer Pool Manager

**LRU-K (k=2) as the production eviction policy, not CLOCK.** The brief
allowed either. LRU-K was chosen because it directly encodes the property
that matters most for a database buffer pool: distinguishing "accessed
once" from "accessed repeatedly." CLOCK approximates LRU cheaply but shares
LRU's fundamental blind spot — a page touched exactly once looks the same
to the policy as a page in a small hot working set, as long as both were
touched recently. LRU-K's *backward k-distance* (how long ago the k-th most
recent access happened) makes that distinction explicit: a frame with fewer
than k accesses has "infinite" backward distance and is evicted first,
regardless of how recent that single access was. See O'Neil, O'Neil &
Weikum, "The LRU-K Page Replacement Algorithm For Database Disk Buffering"
(SIGMOD 1993); this is also the policy BusTub's later-semester projects
converged on for the same reason.

**Plain LRU is implemented too, but only as a benchmark baseline
(`LRUReplacer`), not something `BufferPoolManager` is meant to run in
production.** It exists because "beat plain LRU" needs an honest, fully
correct plain-LRU implementation to beat — not a strawman. It shares the
`Replacer` interface with `LRUKReplacer` so both can be dropped into the same
`BufferPoolManager` unmodified, which is what the Phase 1 benchmark exploits
to run the identical trace through both.

**Replacer/BufferPoolManager split, with a narrow interface between them.**
`Replacer` knows only about frame ids and an evictable/non-evictable flag; it
has no idea what a "page" or a "pin count" is. `BufferPoolManager` owns that
translation (pin count hits zero → `SetEvictable(frame, true)`) and owns all
actual page/disk state (the page table, the frame array, dirty flags, and
writing a victim back before reuse). This mirrors the BusTub/15-445 project
structure and, more importantly, means a CLOCK replacer could be dropped in
later without touching `BufferPoolManager` at all — useful if a future phase
wants to compare policies again under concurrent load.

**Single mutex over the whole `BufferPoolManager`, not per-page latches.**
Every public method takes one `std::mutex`. This is the simplest correct
thing and is fine for now — Phase 1 has no concurrent workload requirement
yet. Per-page (or per-bucket) latching would let concurrent `FetchPage`
calls for *different* pages proceed in parallel, but introducing that now
would be optimizing for a scenario (high-thread-count concurrent access)
this project doesn't exercise until MVCC (Phase 5). Revisit then, guided by
actual contention measurements rather than guesswork.

**No free-page reuse yet, still.** `DeletePage` returns a frame to
`BufferPoolManager`'s internal free list, but the underlying disk page id
itself is never returned to `DiskManager` (which still only ever grows via
`AllocatePage`). This is the same deferral noted in Phase 0 — there's still
nothing that both frees pages *and* needs the space back (B+-tree
merges/LSM compaction in Phase 2/3 will be the first such consumer).

**`Evict()` is O(evictable frames), not O(1) or O(log n).** Both
`LRUKReplacer` and `LRUReplacer` scan their evictable set on every eviction.
This is visible in the benchmark numbers: `BM_Zipfian_LRUK` slows from ~63ms
at pool size 8 to ~729ms at pool size 2048, almost all of it the linear
eviction scan repeated ~200,000 times over a growing evictable set. It's an
acceptable simplification for now (correctness and a clean interface over
micro-optimizing a policy that may still change), but if buffer pools grow
much larger in later phases this is the first thing to revisit — a
priority-queue-backed LRU-K or an approximate CLOCK sweep would restore
O(1) amortized eviction.

## Deferred to later phases

- Free-page reuse — Phase 2 (B+-tree) / Phase 3 (LSM), once deletes exist.
- Group commit / log-buffer batching for the WAL — Phase 4.
- Per-page latching in `BufferPoolManager` (currently one global mutex) —
  revisit under MVCC (Phase 5) if contention measurements justify it.
- O(1)/O(log n) eviction for `LRUKReplacer`/`LRUReplacer` (currently linear
  scan) — revisit if buffer pool sizes grow enough to make it matter.
