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

## Phase 3 — LSM-Tree (Engine B)

**Skip list and Bloom filter are original implementations, not vendored
code.** The hard constraints allow a vendored header-only skip list or
Bloom filter; this project writes its own instead (`src/lsm/skip_list.h`,
`src/lsm/bloom_filter.h`), consistent with "write our own code" and keeping
the whole codebase auditable without a third-party dependency to reason
about. The skip list follows Pugh (1990); the Bloom filter uses double
hashing (Kirsch & Mitzenmacher, 2006) to derive k hash functions from two
64-bit mixes instead of k independent hash functions.

**SSTables reuse Phase 2's fixed-size-value discipline, and it shows up
directly in the benchmark numbers.** An SSTable's data-page entries have
the same layout as a B+-tree leaf entry (key + length + a `MAX_VALUE_SIZE`
slot) for the same reason: fixed-size entries mean bulk-loading a sorted
page is a flat array fill, no slotted-page free-space bookkeeping. The
cost is real and visible: BENCHMARKS.md's space-amplification numbers are
inflated for *both* engines by padding every value out to 64 bytes
regardless of how many bytes it actually uses (our benchmark's values are
~8 bytes). This is the same trade-off flagged in Phase 2, now paid a
second time by the LSM-tree — a deliberate, documented consequence of
keeping the two engines' storage format comparable rather than letting one
quietly support more efficient variable-length values than the other.

**Per-SSTable Bloom filter and sparse index are loaded fully into memory
at `Open()`; only data pages go through a (small, per-SSTable) buffer
pool.** An SSTable's index (one entry per data page) and Bloom filter are
small and consulted on every lookup, so there's no reason to page them
through a cache — they're parsed once into `std::vector`/`BloomFilter`
objects and kept resident for the SSTable's lifetime. Data pages, which
could be large, go through an actual `BufferPoolManager` so repeated reads
of hot pages are still cached. Each SSTable gets its **own** small
dedicated buffer pool (8 frames) rather than sharing one global cache
across all SSTables, because `BufferPoolManager` is keyed by a single
`DiskManager`/file (see Phase 1) and extending it to a shared cache across
multiple files would mean re-keying frames by `(file, page_id)` — a real
change to a component two phases old, out of scope here. This is a
genuine simplification relative to production LSM engines (RocksDB's block
cache is global, not per-SSTable); noted as a deferred improvement.

**Each SSTable is its own file, deleted outright when compacted away —
unlike every other "no free-page reuse yet" deferral in this project.**
Phase 0-2 all defer *disk page* reuse because nothing yet both frees pages
and needs the space back. Compaction is exactly that consumer, and since
an SSTable already owns a whole file, the natural (and simplest correct)
way to reclaim its space is to delete the file, not to recycle pages
within a shared heap file. Lifetime is managed via `shared_ptr<SSTable>`:
a compaction that supersedes a table calls `MarkObsolete()` and drops the
engine's reference, but a concurrent reader that grabbed its own
`shared_ptr` before the swap keeps the file alive (and only deletes it in
the destructor, and only if it was marked obsolete) until it's done. This
is what makes background compaction safe to run concurrently with reads
without them ever seeing a half-deleted table.

**Flush is synchronous; compaction is genuinely asynchronous (a real
background thread), per the Phase 3 brief.** `Put`/`Delete` write into the
active memtable and, if it crosses `memtable_flush_threshold_bytes`,
build a brand-new SSTable *inline* on the caller's thread (holding the
engine's mutex only briefly — to swap in a fresh empty memtable — not for
the disk I/O itself, which proceeds against the old, now-unreachable
memtable without blocking other operations). Compaction is different: a
single dedicated thread wakes on a condition variable (or a 100ms poll,
as a fallback) whenever a tier reaches `tier_compaction_threshold`
SSTables, merges that tier's tables (a k-way merge over their sorted
entries — see below), and promotes the result to the next tier. The
tables being merged are immutable, so the merge itself runs *without*
holding the engine's lock; the lock is only retaken briefly to install the
result and retire the old tables. This mirrors why LSM-trees are
compaction-friendly in the first place: nothing being read is ever being
mutated.

**Tombstones are dropped only when compacting the bottommost populated
tier.** A delete can't touch older, already-written SSTables in place, so
it's recorded as a tombstone entry that must keep shadowing any older
value for that key until nothing older remains that it could still be
shadowing. Compaction checks "does any tier below this one currently hold
data?" before deciding a tombstone is safe to drop entirely; if there's
anything below, the tombstone is written forward into the merged output
instead. This is the same rule RocksDB uses (a compaction can drop a
tombstone once it reaches, or is known to be at, the bottom of the LSM).

**The merge iterator is one algorithm serving two callers, deliberately
kept separate from tombstone policy.** `LSMMergeIterator` k-way-merges any
set of priority-ordered cursors (a min-heap on (key, source priority)),
producing one entry per distinct key — *including* tombstones — with the
highest-priority source winning ties and every shadowed lower-priority
entry silently discarded. `Scan()` wraps this with a filter that hides
tombstones from callers (a `StorageIterator` should never surface a
deleted key); compaction consumes the raw stream and decides per the rule
above. Priority order is [memtable, tier 0 newest→oldest, tier 1
newest→oldest, ...] for `Scan()`, and [tier newest→oldest] for a single
tier's compaction merge — the same precedence `Get()` uses, so a point
lookup and a range scan can never disagree about which source wins a key.

**A real bug this surfaced: the background compaction thread can be
starved by a very fast foreground write loop, growing a tier past its
manifest capacity.** Under a tight, uncontended write loop (no I/O wait
between calls), a non-fair mutex can consistently let the thread that just
released a lock win the race to reacquire it over a thread parked on a
condition variable — a classic convoy/starvation pattern. This showed up
concretely: `LSMTreeEngineTest.RandomizedOperationsMatchStdMapOracle`
(20,000 tight-loop ops) crashed roughly 1 run in 20 with "exceeded manifest
per-tier SSTable capacity" because the compaction thread never got
scheduled in time. The fix is backpressure, not a bigger capacity number:
`kWriteStallTierSize` (comfortably below the hard manifest cap) is checked
after every flush, and if crossed, the *writer* calls
`CompactOneTierIfNeeded()` itself — the same method the background thread
calls, guarded by a single `compaction_running_` flag so two callers never
duplicate the same merge. This mirrors how production LSM engines handle
the same problem (RocksDB's write stalls): if compaction can't keep up,
the writer causing the backlog pays for it directly, which bounds tier
growth regardless of how the OS happens to schedule the background thread.
This is exactly the kind of bug a fixed-seed unit test won't reliably catch
and a stress-repeated one will — worth remembering for Phase 5's MVCC work,
which will have considerably more concurrency surface than this.

**Manifest capacity is a fixed-size array (8 tiers × 16 SSTables/tier),
not a growable log.** The manifest is one page: `next_sstable_id` plus,
per tier, a count and an array of SSTable ids. This is enough headroom for
any workload this project's tests/benchmarks generate (size-tiered
compaction keeps steady-state occupancy per tier below the trigger
threshold, itself well below the array capacity — see the backpressure
mechanism above), but it is a real scale limit, not a real design: a
production system would use a growable, append-only manifest log (exactly
what RocksDB's `MANIFEST` file is). Flagged rather than fixed, since nothing
in this project's scope exercises it.

**No WAL yet for the LSM-tree either — same phased deferral as the
B+-tree.** A crash mid-way loses whatever's in the active memtable since
the last flush, which is the well-known reason real LSM engines pair a
memtable with a parallel WAL. `LSMTreeEngine`'s destructor *does* flush a
non-empty memtable on a clean shutdown (stopping the compaction thread
first), so "close the engine, reopen it" behaves the same as the B+-tree
engine for graceful shutdown — it's specifically crash durability that's
deferred to Phase 4, uniformly across both engines.

## Phase 4 — Write-Ahead Log & ARIES-Style Recovery

**Logging is logical, not physical.** A production ARIES implementation
usually logs physical (or physiological) before/after byte images of a
page. This project logs at the level of the operation instead: an UPDATE
record says "Put(key, new_value)" or "Delete(key)" happened, carrying
whatever prior value undo would need to restore. Two things make this the
right call here rather than a shortcut: every record becomes small and
fixed-size (capped by `MAX_VALUE_SIZE`, the same constant that already
bounds a B+-tree leaf entry and an SSTable entry — see Phase 2/3), so the
WAL can reuse the exact "pack fixed-size records into pages" pattern
already used everywhere else in this codebase, with no slotted-page
framing to invent. More importantly, it's what the ARIES paper itself
prescribes for tree-structured indexes specifically: a physical undo of "restore
these exact bytes to this exact page" stops making sense once a
subsequent split or merge has changed that page's layout, so B-tree
operations need logical undo regardless. The trade-off this buys: replay
must be deterministic (the same sequence of logical operations from the
same starting state reconstructs the same tree — true here, since
split/merge behavior is a pure function of `max_size_` and insertion
history) and there's no per-page LSN stamping or dirty-page table, which
is what makes the next few decisions possible.

**No per-page LSNs, no dirty-page table — checkpoints are "sharp"
instead.** Physical ARIES needs both because REDO must know, per page,
exactly how far back it needs to start (the page's own LSN) and a
"fuzzy" checkpoint (cheap, doesn't block writers) still needs the
dirty-page table's recLSNs to bound REDO's start point correctly. This
project doesn't track either. Instead, `DoCheckpoint()` calls
`BufferPoolManager::FlushAllPages()` before recording anything — a
synchronous, pause-the-world checkpoint that guarantees every committed
change is durably on disk at that instant, except for transactions still
active right then (which get recorded, by id and last LSN, in the
checkpoint itself). That's a real cost (checkpointing gets slower as the
buffer pool holds more dirty pages) traded for a recovery algorithm that
doesn't need a second bookkeeping structure threaded through every page
write. Worth revisiting if a later benchmark shows checkpoint pauses
actually hurting foreground latency — nothing here does yet.

**A bug the checkpoint pointer fixed: recovery time was bounded by total
log size, not log-since-checkpoint — silently defeating the point of
checkpointing.** The first working version of `RecoverOnStartup()` located
the last checkpoint by scanning the *entire* log forward with
`LogManager::ReadAll()`, and only then trimmed the actual Analysis/Redo
work to start from that checkpoint. The trim was correct; the scan to
find it wasn't bounded, and dominated recovery time for any log large
enough to matter. The benchmark built specifically to show "checkpoints
bound recovery time" (see BENCHMARKS.md) instead showed recovery time
still scaling *linearly with total log size even with frequent
checkpoints* — the fix was persisting the LSN of the most recent complete
checkpoint's `CHECKPOINT_BEGIN` record in the engine's own metadata page
(the same page that already holds the B+-tree's root page id), so
recovery reads that one field, jumps straight to the checkpoint's small
bracket via `LogManager::ReadRecord()`, and only then calls
`ReadFrom(redo_start_lsn)` for the actual working set. This is the same
category of "measurability caught a real gap" story as Phase 3's write-
starvation bug: the benchmark wasn't just reporting a number, it falsified
the design's central claim before the fix, which is exactly what it was
built to do.

**Undo is per-transaction and independent, not ARIES's globally
LSN-interleaved order.** Real ARIES processes all losers' pending undo
actions in strict descending LSN order, globally across transactions, because
physical undo needs to respect page-latching dependencies between them. This
project's undo is logical and key-scoped — one transaction's compensations
never touch another transaction's bookkeeping — so undoing each loser
transaction fully, one at a time, is still correct without that interleaving.
Every compensating action writes a CLR (Compensation Log Record) carrying an
`undo_next_lsn`, so an undo interrupted by a second crash resumes correctly:
recovery's backward walk treats a CLR it encounters as "already done, skip to
its `undo_next_lsn`" rather than redoing the compensation. The crash-injection
harness's random kill points land mid-abort often enough across hundreds of
runs to exercise this path empirically, not just in the two-crash unit test
written for it directly.

**The B+-tree gets the full ARIES treatment; the LSM-tree gets a smaller,
deliberately asymmetric one.** `WALBPlusTreeEngine` adds explicit
multi-operation transactions (`Begin`/`Put`/`Delete`/`Get`/`Commit`/`Abort`)
specifically so `Abort` — and therefore recovery's Undo phase — has
something real to demonstrate. `LSMTreeEngine`'s optional WAL (default
off, preserving Phase 3's behavior for existing callers) is redo-only: log
a Put/Delete before applying it to the memtable, replay on restart. There's
no transaction concept, no undo, because there's nothing to undo — an
SSTable is built atomically or not at all, and the memtable itself is pure
in-memory state that a crash simply erases regardless of what a WAL says
about it. The asymmetry mirrors what these two structures actually need
protection from: the B+-tree mutates a shared on-disk structure in place
(splits, merges, page overwrites) where "committed" vs. "not" is a real
distinction worth transactions for; the LSM-tree's on-disk artifacts
(SSTables) are already immutable and atomic by construction, and the only
gap Phase 3 left was "durability for whatever's still sitting in the
memtable" — which a plain append-and-replay log closes completely on its
own.

**A second correctness bug the memtable WAL's design specifically avoids:
truncating a single shared log file races against concurrent writers.**
The first design considered was one WAL file for the active memtable,
truncated and restarted every time a flush completed. That has a real data-
loss window: between swapping in a fresh empty memtable and that flush's
SSTable build finishing, a *different* write landing in the new memtable
would log to the *same* file, and then get silently discarded when the
flush's truncation ran. The fix (implemented, not just noted) is a
per-generation WAL file — `db_path + ".memwal" + generation` — rotated
atomically with the memtable swap, so a flush only ever deletes the file
for the generation it just durably flushed, never the one concurrent
writes are now using. Recovery replays every generation file it finds
(there's normally at most one; a crash mid-flush can leave two) in
ascending order into a fresh memtable, immediately re-logging that
replayed data into a new generation so it stays protected until the next
flush.

**The crash-injection harness uses a real `fork`/`SIGKILL`, not an
in-process simulation, and that distinction mattered while building the
test oracle.** A C++ object going out of scope inside the same process
always runs its destructor — for `LSMTreeEngine` that means a graceful
final flush regardless of whether WAL is enabled, which would make an
in-process "simulated crash" incapable of ever proving the WAL did
anything (the graceful path saves the data either way). The harness
instead forks a real worker process, lets it run unpaced for a random
1-50ms, and sends `SIGKILL` — nothing runs on the way out. Building the
oracle for this surfaced its own subtlety: a worker that logs "commit
confirmed" to a side file *after* the engine call returns has a race (the
process can be killed between the engine call succeeding and that log
line reaching disk), which showed up immediately as spurious failures —
"key present in the DB but not confirmed in the side log" — that looked
like an engine bug but was the test oracle's own gap. The fix is
bracketing every attempt with an 'A' (before) and 'C' (after) line, both
individually fsynced: at most one *trailing* bracket in the file can be
left dangling (an 'A' with no matching 'C'), and only that one key's
outcome is genuinely ambiguous — every other key's last 'C' value must
match the DB exactly. See `test/crash/crash_recovery_test.cpp`.

## Deferred to later phases

- Free-page reuse in `BPlusTree`'s on-disk page ids specifically (SSTables
  solve their version of this by deleting whole files — see above).
- Group commit / log-buffer batching for the WAL — logging currently fsyncs
  once per commit (see BENCHMARKS.md for the throughput cost this carries);
  batching multiple transactions' commits into one fsync is the standard
  next step if that cost needs to come down.
- Per-page latching in `BufferPoolManager`, latch crabbing in `BPlusTree`,
  and finer-grained locking in `LSMTreeEngine`/`WALBPlusTreeEngine` (all
  currently one coarse mutex per structure) — revisit under MVCC (Phase 5)
  if contention measurements justify it.
- O(1)/O(log n) eviction for `LRUKReplacer`/`LRUReplacer` (currently linear
  scan) — revisit if buffer pool sizes grow enough to make it matter; the
  Phase 2 insert-throughput benchmark is partly bottlenecked on this (each
  eviction rescans up to 2,000 evictable frames).
- A shared, global buffer pool cache across all of an LSM-tree's SSTables,
  instead of one small pool per SSTable file.
- A growable manifest log instead of a fixed-capacity manifest page (LSM
  tier manifest) / fixed-capacity checkpoint bracket (WAL).
- Slotted pages / variable-length values for the B+-tree, if a future phase
  needs values larger than `MAX_VALUE_SIZE` without a separate heap page.
- Fuzzy (non-blocking) checkpoints for the WAL, if sharp checkpoints'
  pause-the-world cost ever shows up as a real foreground-latency problem.
