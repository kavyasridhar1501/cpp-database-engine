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

## Deferred to later phases

- Buffer pool (pinning, eviction policy) — Phase 1.
- Free-page reuse — Phase 2 (B+-tree) / Phase 3 (LSM), once deletes exist.
- Group commit / log-buffer batching for the WAL — Phase 4.
- Concurrent multi-writer safety beyond what `pread`/`pwrite` gives for free —
  revisit once the buffer pool introduces shared mutable frame state.
