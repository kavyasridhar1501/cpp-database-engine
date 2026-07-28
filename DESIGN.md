# Design Notes

Architectural decisions and trade-offs, phase by phase. References point to
CMU 15-445 and Hellerstein, Stonebraker & Hamilton, *Architecture of a
Database System* (2007), used here as an architectural reference only.
Code is original, not derived from BusTub.

## Phase 0: Disk Manager

- Fixed 4096-byte page size, compile-time constant (`dbengine::PAGE_SIZE`).
  Matches the common OS page/sector size, so a page read/write is a single
  aligned I/O. Every layer above (buffer pool, node layouts) sizes against
  this constant instead of a runtime parameter.
- `pread`/`pwrite` instead of `seek` + `read`/`write`. Offset is passed per
  call, so reads/writes to different pages are safe to issue concurrently
  from multiple threads with no external locking. Matters starting Phase 1,
  where the buffer pool dispatches concurrent page I/O.
- Page IDs allocate from a monotonic counter, not a free list. No delete
  path exists yet in Phase 0, so nothing needs the space back. A free list
  arrives in later phases once B+-tree merges and LSM compaction produce
  pages that need reclaiming.
- Reading an unwritten page zero-fills rather than erroring. A page ID can
  be reserved before it's written; treating "never written" as zero-filled
  data keeps the read interface to one call and mirrors sparse-file
  semantics most filesystems already give for free.
- No implicit fsync on write. `WritePage` lands in the OS page cache;
  durability needs an explicit `Sync()`. There's no WAL yet in Phase 0, so
  no crash-consistency story to enforce. Phase 4's ARIES write-ahead
  logging rule (log record durable before its data page) is what makes
  `Sync()` ordering matter later.
- One heap file for all pages, not per-table files. Matches BusTub and most
  single-file engines (SQLite included). Table/file mapping doesn't exist
  until the SQL front-end in Phase 6.
- Errors are exceptions (`IOException`), not error codes. I/O failures here
  are unrecoverable precondition violations (bad path, disk full,
  permissions), not expected control flow.

## Phase 1: Buffer Pool Manager

- LRU-K (k=2) is the production eviction policy, not CLOCK. LRU-K's
  backward k-distance directly encodes "accessed once" vs "accessed
  repeatedly": a frame with fewer than k accesses has infinite backward
  distance and gets evicted first, regardless of how recent that one touch
  was. CLOCK approximates LRU cheaply but shares the same blind spot.
  Reference: O'Neil, O'Neil & Weikum, "The LRU-K Page Replacement Algorithm
  For Database Disk Buffering," SIGMOD 1993.
- Plain LRU (`LRUReplacer`) exists only as a benchmark baseline, not as a
  production option. "Beat plain LRU" needs a correct, honest plain-LRU
  implementation to beat, not a strawman. Shares the same `Replacer`
  interface as `LRUKReplacer` so the benchmark can run an identical trace
  through both.
- `Replacer` and `BufferPoolManager` are split with a narrow interface.
  `Replacer` only knows frame IDs and evictable/non-evictable; it has no
  concept of a page or a pin count. `BufferPoolManager` owns that
  translation and all page/disk state. A CLOCK replacer could drop in
  later without touching `BufferPoolManager` at all.
- One mutex over the whole `BufferPoolManager`, not per-page latches. The
  simplest correct option; there's no concurrent workload requirement until
  MVCC in Phase 5. Per-page latching is worth doing once there's an actual
  contention measurement to justify it.
- No free-page reuse yet. `DeletePage` returns a frame to the internal free
  list, but the disk page ID is never returned to `DiskManager`. Same
  deferral as Phase 0. B+-tree merges and LSM compaction in Phase 2/3 will
  be the first real consumers.
- `Evict()` is O(evictable frames), not O(1) or O(log n). Both replacers
  scan their evictable set on every eviction. Visible directly in the
  benchmark: `BM_Zipfian_LRUK` goes from ~63ms at pool size 8 to ~729ms at
  pool size 2048, almost all of it the linear scan repeated ~200,000
  times. Acceptable for now; a priority-queue-backed LRU-K would restore
  O(1) amortized eviction if pool sizes grow.

## Phase 2: B+-Tree (Engine A)

- `StorageEngine` uses fixed-size `int64_t` keys and byte-string values
  capped at `MAX_VALUE_SIZE` (64 bytes), enforced by both engines. A real
  B+-tree with variable-length values needs a slotted page (slot
  directory, free-space compaction). Capping value size means every leaf
  entry is `key + length + fixed slot`, a plain C array works as the node
  layout, and there's no free-space bookkeeping to get wrong. The cap
  applies to the LSM-tree too (Phase 3), even though its write path
  doesn't strictly need one, so the head-to-head comparison stays
  apples-to-apples.
- Node classes (`BPlusTreePage`, `LeafPage`, `InternalPage`) overlay
  directly on a `Page`'s byte buffer via `reinterpret_cast`, no
  serialize/deserialize step. No virtual functions (a vtable pointer would
  corrupt the on-disk layout). The in-memory representation is the wire
  format.
- Node capacity is computed at compile time from `PAGE_SIZE`, with one
  slot of headroom above the logical `max_size_`. Lets `Insert` write
  first and check overflow after, instead of a pre-flight capacity check.
  Default fanout: leaves hold ~54 entries, internal nodes ~254 children,
  so a 10M-key tree is about 3 levels deep.
- `Insert` is upsert, not insert-or-fail. `StorageEngine` is meant to work
  as a real KV store, including what SQL row updates go through in Phase
  6, so an existing key's value gets overwritten in place. Fixed-size
  slots mean an update never has to move surrounding entries.
- One mutex for the whole tree, not latch crabbing. Same simplification as
  the buffer pool, same reason: no concurrent workload requirement until
  Phase 5. A `BPlusTree::Iterator` pins its current leaf but doesn't hold
  the tree mutex between `Next()` calls, so scanning concurrently with a
  mutation is out of scope for now.
- Root page ID persistence lives in `BPlusTreeEngine`, not `BPlusTree`.
  `BPlusTree` just tracks `root_page_id_` in memory. `BPlusTreeEngine`
  reads it from page 0 on construction and writes it back after any
  structural change. Keeps `BPlusTree` a pure data structure with no
  notion of "this is page 0 of a database file."
- Benchmarks insert in shuffled order, not sequential, with a buffer pool
  deliberately smaller than the dataset (2,000 frames for both 1M and 10M
  keys). Sequential insertion is the easy case for a B+-tree (always
  splitting the rightmost leaf) and would flatter it next to Phase 3's LSM-
  tree, whose write path doesn't care about key order. Result: point
  lookup barely moves between 1M and 10M keys; insert throughput drops
  noticeably at 10M, because the fixed pool is a smaller fraction of the
  bigger dataset.

## Phase 3: LSM-Tree (Engine B)

- Skip list and Bloom filter are original implementations, not vendored.
  The constraints allow a vendored header-only version; this project
  writes its own (`src/lsm/skip_list.h`, `src/lsm/bloom_filter.h`) to keep
  the whole codebase auditable without a third-party dependency. Skip list
  follows Pugh (1990). Bloom filter uses double hashing (Kirsch &
  Mitzenmacher, 2006) to derive k hash functions from two 64-bit mixes.
- SSTables reuse the fixed-size-value discipline from Phase 2, and it
  shows up in the numbers. An SSTable data entry has the same layout as a
  B+-tree leaf entry, for the same reason: bulk-loading a sorted page
  becomes a flat array fill. Cost: space-amplification numbers are
  inflated for both engines by padding every value to 64 bytes regardless
  of actual size.
- Each SSTable's Bloom filter and sparse index load fully into memory at
  `Open()`. Only data pages go through a buffer pool, and each SSTable
  gets its own small dedicated pool (8 frames) rather than one shared
  cache, since `BufferPoolManager` is keyed by a single file. A shared
  cache across SSTables is a real simplification relative to production
  engines (RocksDB's block cache is global), noted as a deferred
  improvement.
- Each SSTable is its own file, deleted outright when compacted away.
  Every other phase defers page reuse because nothing yet both frees pages
  and needs the space back; compaction is that consumer, and since an
  SSTable already owns a whole file, deleting it is the simplest correct
  reclaim path. Lifetime uses `shared_ptr<SSTable>`: a superseded table is
  marked obsolete and dropped from the engine's reference, but a
  concurrent reader holding its own `shared_ptr` keeps the file alive
  until it's done.
- Flush is synchronous; compaction is a real background thread. `Put`/
  `Delete` build a new SSTable inline on the caller's thread once the
  memtable crosses its flush threshold, holding the engine mutex only to
  swap in a fresh memtable. Compaction runs on a dedicated thread that
  wakes on a condition variable (or a 100ms poll) and merges a full tier
  without holding the lock, since the tables being merged are immutable.
- Tombstones drop only when compacting the bottommost populated tier. A
  delete can't touch older SSTables in place, so it's recorded as a
  tombstone that must keep shadowing any older value until nothing older
  remains. Same rule RocksDB uses.
- The merge iterator (`LSMMergeIterator`) k-way-merges any set of
  priority-ordered cursors via a min-heap, producing one entry per key
  including tombstones. `Scan()` filters tombstones out; compaction
  consumes the raw stream and applies the tombstone-drop rule above.
  Priority order is memtable, then each tier newest to oldest, matching
  what `Get()` uses, so point lookups and range scans never disagree on
  which source wins a key.
- **Bug found by stress testing**: the background compaction thread could
  be starved by a fast foreground write loop, growing a tier past its
  manifest capacity. A tight write loop with no I/O wait let the thread
  releasing a lock consistently win the race to reacquire it over a thread
  parked on a condition variable, a classic starvation pattern.
  `LSMTreeEngineTest.RandomizedOperationsMatchStdMapOracle` (20,000
  tight-loop ops) crashed about 1 run in 20 with "exceeded manifest
  per-tier capacity."
  - Fix: backpressure, not a bigger capacity number. `kWriteStallTierSize`
    is checked after every flush; if crossed, the writer itself calls
    `CompactOneTierIfNeeded()`, guarded by a `compaction_running_` flag so
    two callers never duplicate the same merge. Same approach RocksDB uses
    for write stalls.
  - This kind of bug doesn't show up in a fixed-seed unit test, only a
    stress-repeated one. Relevant again for Phase 5's MVCC work, which has
    more concurrency surface than this.
- Manifest capacity is a fixed-size array (8 tiers x 16 SSTables/tier), not
  a growable log. Enough headroom for this project's tests and benchmarks.
  A real scale limit, flagged rather than fixed since nothing in scope
  exercises it. Production systems use a growable append-only manifest
  (RocksDB's `MANIFEST` file).
- No WAL yet for the LSM-tree either, same phased deferral as the B+-tree.
  A crash loses whatever's in the active memtable since the last flush.
  The destructor does flush on a clean shutdown, so close-then-reopen
  behaves correctly; it's specifically crash durability that waits for
  Phase 4.

## Phase 4: Write-Ahead Log & ARIES-Style Recovery

- Logging is logical, not physical. An UPDATE record says "Put(key,
  new_value)" or "Delete(key)" happened, carrying the prior value undo
  would need. Two reasons this is the right call, not a shortcut:
  - Records stay small and fixed-size (capped by `MAX_VALUE_SIZE`), so the
    WAL reuses the same "pack fixed-size records into pages" pattern used
    everywhere else in this codebase.
  - The ARIES paper itself prescribes logical undo for tree-structured
    indexes specifically: a physical "restore these exact bytes" stops
    making sense once a subsequent split or merge has changed the page's
    layout.
  - Trade-off: replay must be deterministic (true here, since split/merge
    behavior is a pure function of `max_size_` and insertion history), and
    there's no per-page LSN or dirty-page table to maintain.
- No per-page LSNs, no dirty-page table. Checkpoints are "sharp" instead.
  `DoCheckpoint()` flushes every dirty page before recording anything, a
  synchronous pause-the-world checkpoint. Trades checkpoint cost
  (proportional to dirty pages held) for a recovery algorithm that skips a
  whole bookkeeping structure. Worth revisiting only if a benchmark shows
  checkpoint pauses hurting foreground latency; none has yet.
- **Bug found by the recovery-time benchmark**: recovery time was bounded
  by total log size, not log-since-checkpoint, silently defeating the
  point of checkpointing.
  - `RecoverOnStartup()` located the last checkpoint by scanning the
    entire log forward with `LogManager::ReadAll()`, then trimmed the
    actual redo work correctly from there. The trim was right; the scan
    to find the checkpoint wasn't bounded, and dominated recovery time.
  - The benchmark built specifically to prove "checkpoints bound recovery
    time" instead showed recovery time still scaling linearly with total
    log size, checkpoints or not.
  - Fix: persist the last checkpoint's `CHECKPOINT_BEGIN` LSN in the
    engine's metadata page. Recovery reads that one field and jumps
    straight to the checkpoint instead of scanning for it.
- Undo is per-transaction and independent, not ARIES's globally
  LSN-interleaved order. Real ARIES processes all losers' undo actions in
  strict descending LSN order globally, because physical undo needs to
  respect page-latching dependencies between transactions. This project's
  undo is logical and key-scoped, so undoing each loser transaction fully,
  one at a time, is still correct. Every compensating action writes a CLR
  carrying an `undo_next_lsn`, so an undo interrupted by a second crash
  resumes correctly instead of redoing the compensation.
- The B+-tree gets the full ARIES treatment; the LSM-tree gets a smaller,
  asymmetric one, on purpose.
  - `WALBPlusTreeEngine` adds explicit multi-operation transactions
    (`Begin`/`Put`/`Delete`/`Get`/`Commit`/`Abort`) specifically so `Abort`
    (and recovery's Undo phase) has something real to demonstrate.
  - `LSMTreeEngine`'s optional WAL (default off) is redo-only: log before
    applying to the memtable, replay on restart. No transaction concept,
    no undo, because an SSTable is atomic by construction and the memtable
    is pure in-memory state a crash erases regardless.
- **A correctness bug avoided by design, not fixed after the fact**:
  truncating a single shared memtable-WAL file races against concurrent
  writers. The first design (one WAL file per memtable, truncated on
  flush) has a real data-loss window between swapping in a fresh memtable
  and the flush finishing. Fix: a per-generation WAL file
  (`db_path + ".memwal" + generation`), rotated atomically with the
  memtable swap, so a flush only ever deletes the generation it just
  durably flushed.
- The crash-injection harness uses a real `fork`/`SIGKILL`, not an
  in-process simulation. A C++ object going out of scope in the same
  process always runs its destructor, so an in-process "simulated crash"
  can't prove the WAL did anything (the graceful path saves data either
  way). The harness forks a real worker, lets it run 1-50ms, sends
  `SIGKILL`.
  - Building the oracle for this surfaced its own bug: logging "commit
    confirmed" to a side file *after* the engine call returns races the
    kill, producing spurious failures that looked like engine bugs. Fix:
    bracket every attempt with fsynced 'A' (before) and 'C' (after)
    markers; only a trailing unmatched 'A' is genuinely ambiguous.

## Phase 5: MVCC Concurrency

- `MVCCStore` is deliberately not a `StorageEngine`. The `Get`/`Put`/
  `Delete`/`Scan` interface has no concept of a transaction boundary or
  isolation level, and demonstrating those is the point of this phase.
  `MVCCStore` sits beside the disk engines as a separate, in-memory
  component with its own API (`Begin`/`Read`/`Write`/`Delete`/`Commit`/
  `Abort`). Combining it with disk-backed WAL durability would be a
  separate project, not a natural extension.
- Snapshot isolation does not prevent write skew, and this project shows
  that honestly instead of papering over it.
  - It's an established result (Berenson et al., "A Critique of ANSI SQL
    Isolation Levels," 1995; Fekete et al., "Making Snapshot Isolation
    Serializable," 2005) that plain snapshot isolation closes dirty reads
    and non-repeatable reads but not write skew, because it only checks
    write-write conflicts.
  - Rather than contradict that theory or build full Cahill-et-al.
    Serializable Snapshot Isolation (a much larger undertaking), this
    project implements four isolation levels with an explicit table of
    what each one closes:

    | Isolation level | Dirty read | Non-repeatable read | Write skew |
    |---|---|---|---|
    | `READ_UNCOMMITTED` | visible | visible | visible |
    | `READ_COMMITTED` | prevented | visible | visible |
    | `SNAPSHOT` | prevented | prevented | visible (textbook SI) |
    | `SERIALIZABLE_SNAPSHOT` | prevented | prevented | prevented |

  - `SERIALIZABLE_SNAPSHOT` is a simplified, conservative SSI-lite: at
    commit time it also checks the read set for conflicting writes, which
    catches the classic two-key write-skew pattern without a full
    rw-antidependency graph. It rejects some transactions a full SSI
    implementation would allow. See `WriteSkew*` tests in
    `mvcc_store_test.cpp`.
- Version chains, not a single mutable slot. Each key maps to a
  `VersionChain`, a `std::list<Version>` behind its own mutex. `list` is
  chosen specifically so a transaction can hold a stable iterator into it,
  giving O(1) commit and abort instead of a linear re-search. Each
  `Version` carries a `seq` (write-time order, what `READ_UNCOMMITTED`
  uses) and a `commit_ts` (commit-time order, what the other levels use).
  Read-your-own-writes is unconditional: `Read()` always checks the
  caller's own pending version first, regardless of isolation level.
- Commit-time conflict detection is first-committer-wins, with chains
  locked in sorted key order to avoid deadlock. `Commit()` collects every
  key to check (write set always, read set too under
  `SERIALIZABLE_SNAPSHOT`), sorts and dedups them, then locks each chain in
  that order. Two transactions committing concurrently over overlapping
  keys always acquire locks in the same relative order, ruling out
  classic A-waits-for-B-waits-for-A deadlock with no detector needed.
- Garbage collection tracks every active transaction's `start_ts`, not
  just the oldest one.
  - The first version computed a single minimum `start_ts` and reclaimed
    anything older. Safe, but needlessly conservative: it kept every
    version between the oldest reader's boundary and the newest, even ones
    no active reader could see.
  - The final version computes the full set of active `start_ts` values
    and reclaims a committed version exactly when no active transaction's
    `start_ts` falls in the half-open interval where that version is the
    answer. See `GarbageCollectionRespectsActiveSnapshot`.
- **Bug found by the throughput benchmark**: a single global mutex
  guarding the whole transaction table undid the point of fine-grained
  MVCC locking.
  - Every autocommit transaction does one `Begin` (insert) and one
    `Commit` (erase) against the transaction table. With one mutex, that
    table was contended on nearly every operation regardless of which key
    it touched, so the per-key `VersionChain` locking never mattered.
  - Measured effect: throughput fell from 1 to 4 threads (450k/s to
    168k/s) instead of holding steady, the opposite of what fine-grained
    locking should do.
  - Fix: shard the transaction table into 16 independently-locked shards
    keyed by `txn_id % 16`, the same technique the buffer pool and LSM-tree
    already use. Throughput holds flat from 1 to 4 threads after the fix.
- Concurrency-safety contract: one thread drives a given `txn_id` at a
  time. `Begin`/`Read`/`Write`/`Delete`/`Commit`/`Abort` are never called
  concurrently for the same `txn_id` from two threads, a standard
  one-thread-per-session model. This is what lets a `Transaction`'s own
  fields be touched without a per-transaction lock once looked up under
  its shard's mutex.

## Phase 6: SQL Front-End & Tiny Optimizer

- Every table shares one `StorageEngine` instance instead of getting its
  own file. `Database` opens a single engine; each table's rows live in a
  slice of its `int64_t` keyspace: the top 16 bits hold a table ID, the
  rest hold the row's primary key. Ascending key order groups each
  table's rows contiguously, which is what lets `Executor` start a scan at
  a table's first key and stop the moment the table ID changes, with no
  engine-level notion of a table boundary. Real limit: at most 2^16
  tables, 2^48 rows each, both generous here but not production-grade.
- The primary key is the only column the planner can index, because
  `KeyType` is a single `int64_t` across the whole project. `CREATE TABLE`
  requires the first column to be `INTEGER` and treats it as the row's
  storage key. With exactly one indexed column, access-path selection
  collapses to three cases:
  - Equality on the primary key: `POINT_LOOKUP`.
  - A bound (`<`, `<=`, `>`, `>=`) on the primary key: `RANGE_SCAN`.
  - Neither: `FULL_SCAN`.
  Every comparison not implied by the chosen path travels along as a
  residual filter applied after fetching, so a plan is always correct even
  when it's not maximally selective.
- No secondary indexes, so planning never considers more than one access
  path per table. That's the specific scope cut that keeps this a "tiny"
  optimizer. A real cost-based optimizer chooses among multiple candidate
  indexes by estimated selectivity, decides join order across tables, and
  uses cardinality estimates. None of that has anywhere to attach without
  secondary indexes or multi-table queries (see Deferred).
- Grammar is a small subset: `CREATE TABLE` / `INSERT` / `SELECT` /
  `DELETE`, `WHERE` limited to an AND-conjunction of comparisons. No
  `UPDATE`, no `OR`, no joins, no subqueries, no aggregates.
  - `UPDATE` is absent by design: every `Put` is already an upsert, so
    re-inserting an existing primary key overwrites the row.
    `DatabaseTest.UpdateSemanticsViaReinsert` locks that behavior in.
  - AND-only `WHERE` keeps bound-intersection a simple min/max fold. `OR`
    between primary-key bounds would need a union of ranges, a reasonable
    next step but real added complexity for a phase about access-path
    choice, not expression evaluation.
- Row encoding is fixed and non-nullable, sized to fit `MAX_VALUE_SIZE`
  (64 bytes), the same cap both storage engines already enforce. The
  primary key is never part of the payload since it's already the storage
  key, buying back 8 bytes. `INTEGER` columns encode as 8 raw bytes,
  `TEXT` as a 2-byte length prefix plus bytes. `Database::ExecuteInsert`
  raises a `SqlException` (not a generic `IOException`) the moment a row
  would exceed the cap.
- The catalog is in-memory only and doesn't persist across a `Database`
  restart on the same file. Intentional scope cut, not an oversight: the
  row data itself is durable (the underlying engine is disk-backed), but
  reinterpreting it correctly again requires re-issuing the same `CREATE
  TABLE` statements in the same order. Every test and the CLI's `sql`
  command only ever use one `Database` per process lifetime, so this
  doesn't block correctness demonstration.
- The CLI's `sql` command uses a separate file (`db_path + ".sql"`), never
  the file the raw `alloc`/`write`/`read` commands touch. Those write
  directly through `DiskManager` with no notion of the SQL layer's key
  prefixing; sharing a file would let the two corrupt each other silently.

## Phase 7: Validation, Differential Testing & TPC-Style Workloads

- Linking a real SQLite for differential testing doesn't violate "no
  external database libraries," because that rule is about what ships in
  the engine, not what validates it. SQLite is pulled in via
  `find_package(SQLite3)` only inside `if(DBENGINE_BUILD_TESTS)`, and only
  the test binary links it, the same role GoogleTest and Google Benchmark
  already play. The Docker image (built with tests off) has zero SQLite
  dependency, confirmed directly with `ldd`.
- No translation layer is needed for the SQL text itself, since this
  project's grammar is a strict syntactic subset of real SQL. One semantic
  bridge is needed though: primary-key uniqueness.
  - This project's grammar treats column 0 as an implicit, always-enforced
    primary key. Bare `id INTEGER` in standard SQL is just a plain column
    with no uniqueness at all.
  - **What this looked like as a bug**: the first version of the
    differential test sent identical text to both sides. After a
    randomized insert/delete sequence, SQLite reported far more rows than
    this engine for the same key, e.g. three rows with `id = 114` where
    the engine held exactly one. Not a bug: SQLite did exactly what was
    asked, since nothing told it `id` had to be unique. This is the value
    of differential testing against an independent implementation: it
    surfaces assumptions a test author could otherwise bake into both
    sides without noticing.
  - Fix: rewrite only `CREATE TABLE` and `INSERT` before sending them to
    SQLite. `INTEGER` on the first column becomes `INTEGER PRIMARY KEY`;
    `INSERT` becomes `INSERT OR REPLACE`. `SELECT` and `DELETE` are sent
    unmodified.
- The randomized differential test avoids two of this project's own
  documented scope limits rather than treating them as SQLite
  disagreements: generated primary keys stay non-negative, and generated
  `TEXT` payloads stay under `MAX_VALUE_SIZE`. Both are already covered by
  dedicated Phase 6 tests.
- "TPC-C-style" and "TPC-H-style" mean inspired by, not compliant with,
  the official specs. Real TPC-C has five transaction types with audited
  response-time percentiles; TPC-H has 22 analytical queries, most with
  joins and aggregates. This project's SQL layer has none of `UPDATE`,
  joins, `GROUP BY`, or aggregates, so literal compliance isn't achievable
  without building substantially more of a SQL engine.
  - TPC-C-inspired (`benchmark/tpcc_bench.cpp`): warehouse/customer/stock/
    orders/order_line tables, composite keys flattened to a single
    `INTEGER` by arithmetic, a simplified New-Order transaction. Not
    wrapped in a real multi-statement transaction, since the SQL layer
    doesn't expose `Begin`/`Commit` yet.
  - TPC-H-inspired (`benchmark/tpch_bench.cpp`): a single 300,000-row
    fact table and a Q1-style filter swept across selectivity.
    `l_shipdate` is scattered relative to the primary key so no
    insertion-order trick beats a genuine `FULL_SCAN`.

## Phase 8: Deployment

- Three deliverables, each a thin layer over work already done. This
  phase adds no new storage-engine ideas, only reachability.
  - A GHCR image (`.github/workflows/publish.yml`) builds the same
    `Dockerfile` the CI workflow already validates and publishes it using
    the repo's automatic `GITHUB_TOKEN`.
  - A static results page (`docs/index.html`) is hand-written HTML/CSS
    transcribing BENCHMARKS.md's headline numbers. No generator, no
    client-side JS, no external CDN.
  - A read-only HTTP API (`src/http/`) is a thin front-end over
    `Database::Execute`, restricted to `SELECT`, reusing the Phase 6
    parser/planner/executor unchanged.
- The HTTP API is raw POSIX sockets, not an external web framework, for
  the same reason the SQL layer has its own lexer instead of a
  parser-generator: a demo-scale server (two routes, GET-only, no
  keep-alive) is well within what's reasonable to hand-write. `HttpServer`
  parses just enough of an HTTP/1.1 request to route `GET /health` and
  `GET /query?sql=...`, with a small hand-written JSON serializer for
  `QueryResult`.
- Read-only is enforced by parsing every request twice: once by
  `HttpServer` to check the statement type, once by `Database::Execute` to
  run it. Anything that isn't a `SelectStmt` gets a 400 before it reaches
  the engine at all. A double parse costs microseconds against network I/O
  that costs milliseconds.
- Single-threaded, one connection at a time, by design. `Database` was
  never built for concurrent access (no locking in `Catalog`,
  `EncodeRow`/`DecodeRow`, or the executor), and `MVCCStore` (Phase 5's
  answer to concurrency) is a separate component the SQL layer doesn't sit
  on. Serving one request at a time sidesteps that entirely, at the cost
  of running only one query at a time. Acceptable for a demo API, not a
  claim about production read replicas.
- Catalog non-persistence (documented in Phase 6) becomes directly
  visible here: a fresh `dbengine_httpd` process has no way to know a
  table exists even when the row bytes are on disk. Fix: `dbengine_httpd`
  takes an optional schema-file argument, run once via `Database::Execute`
  before the listener starts. Nothing in it runs in response to a request,
  so the API's read-only property holds. Found by testing the deployed
  shape end to end: a fresh process pointed at data a CLI session had
  already populated returned "no such table" for every query until this
  was added.
- **Bug found by the HTTP server's own test suite, not manual testing**:
  `HttpServer::Run()` unconditionally set its running-flag to `true` as
  its first statement, racing `Stop()` if `Stop()` executed before the
  spawned thread actually started `Run()`.
  - Manual testing (start, `curl`, `SIGTERM`) never hit this, since human-
    scale delays always closed the race window.
  - A fast test with no delay between starting and stopping the server
    hit it reliably: `Stop()` would set the flag false, then `Run()` would
    start and silently overwrite it back to true, hanging the accept loop
    forever.
  - Fix: `running_` starts `true` as the member's default initializer and
    `Run()` never writes `true` to it again. `Stop()` is a one-way
    transition with no race window regardless of thread scheduling order.

## Deferred

- Free-page reuse in `BPlusTree`'s on-disk page IDs specifically.
- Group commit / log-buffer batching for the WAL, to amortize fsync cost
  across concurrent commits.
- Per-page latching in `BufferPoolManager`, latch crabbing in `BPlusTree`,
  finer-grained locking in `LSMTreeEngine`/`WALBPlusTreeEngine`.
  `MVCCStore`'s per-key locking is this project's one example of what that
  looks like in practice.
- Full Cahill-et-al. Serializable Snapshot Isolation, in place of Phase
  5's simplified `SERIALIZABLE_SNAPSHOT`.
- MVCC version chains persisted to disk / integrated with the WAL.
- O(1)/O(log n) eviction for the replacers, currently a linear scan.
- A shared, global buffer pool cache across an LSM-tree's SSTables,
  instead of one small pool per file.
- A growable manifest log instead of a fixed-capacity manifest page, and a
  growable checkpoint bracket for the WAL.
- Slotted pages / variable-length values for the B+-tree.
- Fuzzy (non-blocking) checkpoints for the WAL.
- A persisted catalog, so SQL tables survive a `Database` restart.
- Secondary indexes, `UPDATE`, `OR`/parenthesized `WHERE`, joins,
  aggregates, and nullable columns in the SQL layer. Secondary indexes
  specifically are what would move the optimizer from "tiny" to real.
- One `StorageEngine`/file per SQL table, instead of the shared table-ID
  keyspace every table currently lives in.
- Transactional SQL statements: wiring the WAL's or MVCC's `Begin`/
  `Commit`/`Abort` into the SQL front-end for real multi-statement
  atomicity and isolation.
- Literal TPC-C/TPC-H compliance, and the joins/`GROUP BY`/aggregates that
  would require.
- Multi-threaded request handling for `HttpServer`, which needs
  `Database`/`Catalog` to grow real concurrency support first.
- A real fix for `dbengine_httpd`'s schema-file workaround: a persisted
  catalog would make the startup file unnecessary.
- HTTPS/TLS, authentication, and rate-limiting for the HTTP API, which is
  explicitly demo-scale today.

## References

- CMU 15-445/645, *Database Systems* (Andy Pavlo). Course structure and
  BusTub's project sequence, used as an architectural reference only.
- Hellerstein, Stonebraker & Hamilton, *Architecture of a Database
  System*, Foundations and Trends in Databases (2007).
- O'Neil, O'Neil & Weikum, "The LRU-K Page Replacement Algorithm For
  Database Disk Buffering," SIGMOD 1993.
- Pugh, "Skip Lists: A Probabilistic Alternative to Balanced Trees,"
  Communications of the ACM (1990).
- Kirsch & Mitzenmacher, "Less Hashing, Same Performance: Building a
  Better Bloom Filter," ESA 2006.
- Mohan et al., "ARIES: A Transaction Recovery Method Supporting
  Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead
  Logging," ACM TODS (1992).
- Berenson et al., "A Critique of ANSI SQL Isolation Levels," SIGMOD 1995.
- Fekete et al., "Making Snapshot Isolation Serializable," ACM TODS (2005).
- Cahill, Rohm & Fekete, "Serializable Isolation for Snapshot Databases,"
  SIGMOD 2008.
