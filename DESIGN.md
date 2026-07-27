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

## Phase 2 — B+-Tree (Engine A)

**`StorageEngine` uses fixed-size `int64_t` keys and byte-string values
capped at `MAX_VALUE_SIZE` (64 bytes), enforced by both engines.** This is
the central simplification of Phase 2. A real B+-tree leaf holding
variable-length values needs a slotted page (slot directory, free-space
compaction on delete/update) — legitimate, but a second full subsystem on
top of the tree logic itself. Capping values to a fixed size means every
leaf entry is `sizeof(key) + sizeof(length) + MAX_VALUE_SIZE`, a plain C
array works as the node layout, and there's no free-space management to get
subtly wrong under interleaved insert/delete. The cap is enforced on the
LSM-tree (Phase 3) too, even though its memtable/SSTable path doesn't
strictly need one — so the head-to-head comparison stays apples-to-apples
rather than one engine incidentally supporting bigger values than the
other. This is the same style of trade-off CMU 15-445 / BusTub's project 2
makes (fixed-size `GenericKey`/`RID`), generalized slightly from "a
pointer" to "a small inline payload." The natural pattern for values that
don't fit — store a fixed-size row id/offset here and put the actual
payload in a separate heap page — is exactly how a real non-clustered index
works, and is the option this leaves open for Phase 6 (SQL rows) rather
than closing off.

**Node classes are overlaid directly on a `Page`'s byte buffer via
`reinterpret_cast`, not serialized/deserialized on access.** `BPlusTreePage`
(and its `LeafPage`/`InternalPage` subclasses) have no virtual functions —
a vtable pointer would corrupt the byte layout the moment the page is
written to disk and reread. Every accessor reads/writes the raw bytes in
place. This is the standard technique for page-based storage engines
(BusTub does the same) and means there's no separate "wire format" to keep
in sync with an in-memory representation — the in-memory representation
*is* the wire format.

**Node capacity is computed at compile time from `PAGE_SIZE`, with one
slot of headroom reserved above the logical `max_size_`.** `LeafPage`/
`InternalPage` each declare a fixed C array sized to exactly fill one page
(`kMaxSize`), but `max_size_` (the logical cap `BPlusTree` enforces before
triggering a split) defaults to `kMaxSize - 1`. That spare slot is what
lets `Insert` write the node's entries first and check "did this overflow
max_size_?" *after*, rather than needing a separate pre-flight capacity
check before every insert. Default fanout this produces: leaves hold ~54
entries (8-byte key + 2-byte length + 64-byte value, per PAGE_SIZE=4096),
internal nodes ~254 children (8-byte key + 8-byte page id) — so a tree
holding 10M keys is only about 3 levels deep.

**Put/Insert is upsert, not insert-or-fail.** A B+-tree used purely as a
unique index (BusTub's project 2 framing) typically rejects a duplicate
key. `StorageEngine` is meant to be used as an actual KV store — including,
eventually, as the thing SQL row updates go through in Phase 6 — so
`BPlusTree::Insert` overwrites an existing key's value in place and reports
whether the key was new. Because leaf values are fixed-size slots, an
update never needs to move surrounding entries.

**One mutex for the whole tree, not latch crabbing.** Real B+-tree
implementations take latches on individual nodes and release ancestors
early once a subtree is known to be split/merge-safe ("latch crabbing"),
so concurrent readers and writers on different subtrees don't block each
other. Phase 2 instead takes a single `std::mutex` around every
`Insert`/`Remove`/`GetValue`/`Begin` call — the same simplification made for
`BufferPoolManager` in Phase 1, for the same reason (this project has no
concurrent workload requirement yet; that arrives with MVCC in Phase 5, and
latch crabbing is worth doing once there's a concurrent benchmark to
validate it against). One direct consequence: a `BPlusTree::Iterator`
holds a pin on its current leaf but does *not* hold the tree mutex between
`Next()` calls, so a scan running concurrently with a mutation is out of
scope for now.

**The metadata page (root page id persistence) lives in `BPlusTreeEngine`,
not `BPlusTree`.** `BPlusTree` just tracks `root_page_id_` in memory and
exposes `GetRootPageId()`; it has no idea a database file has a page 0
reserved for bookkeeping. `BPlusTreeEngine` owns that policy — it reads the
root id from page 0 on construction and writes it back after any
`Put`/`Delete` that changed it. This keeps `BPlusTree` a pure data
structure over a `BufferPoolManager`, testable without any notion of "this
is page 0 of a database file," which is exactly how the Phase 2 unit tests
use it directly.

**Benchmark methodology: random-order inserts, and a buffer pool
deliberately much smaller than the dataset.** Point-lookup/range-scan/
insert-throughput are measured with keys inserted in *shuffled*, not
sequential, order — sequential insertion is the easy case for a B+-tree
(always splitting the rightmost leaf) and would flatter it relative to
Phase 3's LSM-tree, whose write path doesn't care about key order at all.
The buffer pool is fixed at 2,000 frames (8MB) for both the 1M-key (~76MB)
and 10M-key (~760MB) datasets — a deliberately small, fixed fraction of
each, modeling an index that doesn't fit in RAM rather than one that does.
See BENCHMARKS.md for what this exposes: point-lookup latency barely moves
between 1M and 10M keys (as expected — O(log N) descent, and tree height
barely changes), but insert throughput drops noticeably at 10M, because the
same fixed-size pool is a much smaller *fraction* of the larger dataset.
That's a real cost of a fixed buffer pool against a growing B+-tree, and a
useful baseline for Phase 3, where an LSM-tree's memtable-then-flush write
path is expected to degrade far less with scale.

## Deferred to later phases

- Free-page reuse — Phase 3 (LSM) once compaction needs it; the B+-tree's
  `DeletePage` already frees the buffer pool frame but page ids on disk are
  still never recycled (same deferral as Phase 0/1, now also true of
  `BPlusTree::Remove`'s merges).
- Group commit / log-buffer batching for the WAL — Phase 4.
- Per-page latching in `BufferPoolManager`, and latch crabbing in
  `BPlusTree` (both currently one coarse mutex) — revisit under MVCC
  (Phase 5) if contention measurements justify it.
- O(1)/O(log n) eviction for `LRUKReplacer`/`LRUReplacer` (currently linear
  scan) — revisit if buffer pool sizes grow enough to make it matter; the
  Phase 2 insert-throughput benchmark is partly bottlenecked on this (each
  eviction rescans up to 2,000 evictable frames).
- Slotted pages / variable-length values for the B+-tree, if a future phase
  needs values larger than `MAX_VALUE_SIZE` without a separate heap page.
