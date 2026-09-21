# Engine design

Stage 2: pager, log, transactions (`engine/include/archivum/engine/db.h`,
`docs/page-format.md`, `docs/durability.md`). Stage 3: b-trees, records,
catalog, constraints and the typed API (`btree.h`, `record.h`, `types.h`,
`store.h`; `docs/btree-format.md`, `docs/store-format.md`).

## Model

The SQLite WAL model, in process. Every write goes to the log as a full
page image. The data file is written only by checkpoint. One writer at a
time; readers take a snapshot of the committed state and are never blocked
by the writer, and never block it. Because this process is the only one
that opens the files, the log index lives in memory and is rebuilt from the
log at open.

## Named invariants

**I1. `Db` is an instance; there is no global state.** A `Db` owns its
file handle, log, index, page cache, reader count, writer lock, and
checkpoint counter. Two instances on two files share nothing. The two-
database design (operational and analytical stores, pipeline document
section 2) is this invariant. Tested by `db_two_instances_are_independent`
(a writer on one does not block a writer on the other; a reader on one
does not block a checkpoint on the other) and by
`db_two_instances_crash_at_every_step`, which interleaves transactions on
two instances over one crash shim, crashes at every operation under every
persistence policy, and checks that each instance recovers to exactly its
own committed state.

The Stage 5 ThreadSanitizer run found one violation of I1: the log's
salt generator was a function-local static shared by every instance, and
two instances on two threads raced on it in their checkpoints. The Stage 5
fix, a per-call `std::random_device`, could throw inside a checkpoint when
the entropy source is unavailable, which the Stage 6 ruling judged the
worse failure. Salts and database ids now come from libsodium's
`randombytes_buf` (`archivum/entropy.h`): thread-safe, no state of ours,
and unable to fail once `sodium_init()` has succeeded. `init_entropy()`
runs at process start (the binary refuses to start otherwise) and again
in `Db::open`, so a caller that skipped it fails at open, never inside a
checkpoint. Two instances on two threads produce distinct salt pairs on
every concurrent checkpoint
(`concurrency_checkpoints_on_two_threads_produce_distinct_salts`).

**I2. A transaction never spans instances.** `WriteTxn` is created by one
`Db` and holds only that instance's writer lock; no API takes two
databases. Consistency between the stores is the change feed's job, and
is eventual by design.

**I3. Committed means synced.** `WriteTxn::commit()` returns ok only after
the log write and the log sync succeed. Until then nothing is visible to
readers and nothing is in the index.

**I4. Readers see one snapshot.** A `ReadTxn` records the last committed
frame at begin and resolves every page against it: the latest frame at or
before the snapshot, else the data file. A checkpoint cannot run while any
reader exists (`Busy`), so the data file never changes under a snapshot.
The reader is registered in the same critical section that takes its
snapshot; registering afterwards left a window for a checkpoint (found in
Stage 4 by the backup test, regression-tested by
`db_reader_registration_is_atomic_with_its_snapshot`).

**I5. Nothing corrupt is served.** Every page read from the data file or
the log is verified against its trailer, and every log frame against the
chained checksum; a failure is `Corrupt`, never data.

**I6. Fail closed after log I/O errors.** After any I/O error while
appending, discarding, or resetting the log, the instance refuses further
writes until reopened. Reopen re-reads the truth from disk. This mirrors
the journal's policy and the reason is the same: after a failed fsync the
OS may have dropped dirty pages.

**I7. Open is a durability barrier.** `Db::open` syncs the data file and
its directory after validating the header, and the log does the same after
recovery. Found by the randomized crash test: a failed sync during
creation, followed by a retry that trusted the cached header, let commits
be acknowledged on top of a file that was not on disk; a crash then lost
the file and open discarded the log as belonging to nothing.

## Transaction lifecycle

```
begin_write  lock writer; snapshot = last commit; header = committed page 0
write_page   copy into the private dirty set
allocate     pop free list (page content must decode as a free page) or grow
free_page    write a free-page record linking to the old head
commit       seal dirty pages; page 0 with the new header is the commit
             frame; one write, one sync; publish; maybe checkpoint
rollback     drop the dirty set
```

An in-flight commit at crash time, or a commit that returned an I/O error,
is in doubt: after recovery it is either fully present or fully absent,
never partial. Callers that must know use idempotent operations and check;
Punchline's client-generated UUIDs are that mechanism.

## Checkpoint

Holds the writer lock, requires zero readers. Copies the latest committed
frame of every page into the data file, truncates the file to the
committed page count, syncs the file, then resets the log. A crash at any
point is safe: before the file sync the log still has everything; after it
the file has everything; the reset happens last. Page 0 in the data file
may be torn by a crash mid-checkpoint; `Db::open` recovers it from the log,
which is identified by its own header.

Automatic checkpoints run after a commit when the log holds at least
`checkpoint_threshold_frames` frames and no reader is active.

## Testing

- `tests/unit/db_test.cpp`: API behaviour, snapshot isolation, free list
  reuse, corruption detection, torn log tail, foreign log, auto
  checkpoint, two instances.
- `tests/crash/db_crash_test.cpp`: model-based (a naive map of page
  contents, the free set, and the page count), crash at every operation of
  a fixed workload under every persistence policy, the lying-fsync tier,
  the two-instance test, and seeded randomized runs with random faults of
  every kind.

## Stage 3: the typed layer

```
Store        one Db; the catalog b-tree at page 1; a catalog cache per change counter
Reader       a ReadTxn plus the catalog of its snapshot: get, scan, count
Writer       the WriteTxn plus a private copy of the catalog: insert, update,
             remove, create/drop table and index, schema version; commit
BTree        ordered byte-string map over PageReader/PageWriter (a ReadTxn or WriteTxn)
```

Additional invariants, all checked by `Store::check`:

**I8. Every row's key is its primary key.** The key of a table entry
decodes to the primary key columns of the row stored under it.

**I9. Indexes and rows agree both ways.** For every row and every index
of its table the index entry computed from the row exists; for every
index entry the row it names exists and yields exactly that entry; entry
counts equal row counts; a unique index has no two entries with the same
non-NULL prefix.

**I10. Constraints hold at rest.** Every stored row satisfies its
column types, NOT NULL and checks, and every non-NULL foreign key finds
its parent.

**I11. Pages are owned exactly once.** The catalog tree, every table
tree, every index tree (overflow pages included) and the free list
partition pages 1 to N−1; no page is owned twice and none by nobody.

**I12. A constraint violation changes nothing.** `Writer` validates the
row, looks up the primary key, unique indexes and parents, and only then
writes. Only an I/O error can leave a transaction half applied, and such
a transaction must be rolled back (I6 makes the log refuse further
writes anyway).

**I13. The catalog is part of the snapshot.** A reader's catalog is the
one committed at its change counter; a writer's schema changes are
visible to itself and to nobody else until commit.

## Testing (Stage 3)

- `tests/unit/btree_test.cpp`, `tests/crash/btree_model_test.cpp`:
  the tree against `std::map<Bytes, Bytes>` with random puts, replaces,
  erases, point lookups, forward and backward scans, crashes at random
  points with in-doubt commits accepted either way, and the structural
  checker after every operation.
- `tests/unit/record_test.cpp`: key order across the sign boundary,
  prefixes, embedded NULs, composites; row round trip and truncation.
- `tests/unit/store_test.cpp`: schema and rows with every constraint
  exercised; a migration rolled back and committed; dump and restore
  round trip; four snapshot readers against a committing writer while a
  long query holds its snapshot and the checkpoint reports Busy until it
  ends.
- `tests/crash/store_model_test.cpp`: the typed model-based test. Three
  tables (self-consistent schema with composite keys, a unique-index
  foreign key target, a two-column foreign key, checks of every kind and
  every column type) against a naive model of rows per table. Random
  inserts, updates and deletes with deliberate violations whose verdict
  the model computes independently, random index creation and removal,
  rollbacks, checkpoints, and crashes at random points. After every commit
  the tables are compared by primary key, every index in index order,
  random bounded scans in both directions and point lookups; the full
  invariant checker runs every few commits and after every recovery.

## Stage 4: backup, log archive, point-in-time recovery, migrations

`docs/backup-recovery.md` has the operator's view. The engine additions:

**I14. The log continues its file, or is refused.** The log header
carries the change counter of the file it continues from (its base). At
open the base must equal the file's counter, or the file may be ahead of
the base by no more than the log's last commit (a crash between a
checkpoint's file sync and its log reset). Anything else, such as a
restored backup beside a live log, is refused as `Corrupt` rather than
replayed. Tested by `restored_backup_beside_a_live_log_is_refused` and by
every crash test of Stage 2, which now cross this rule at every step.

**I15. The archive is durable before the log is emptied.** With
`DbOptions::archive_dir` set, a checkpoint copies the committed log to
`<archive>/<db id>-<base>.wal`, syncs it and its directory, and only then
resets the log. A crash after the copy and before the reset re-archives
the same base on the next checkpoint with a superset, which recovery
accepts.

**I16. Every commit names itself.** The page-0 image of a commit carries
the change counter and the commit's wall-clock time, so a segment's
commits are addressable by counter or by time without any side index,
and recovery can verify that the commits it applies are consecutive.

**I17. A backup is a database.** `Db::backup` copies every page of one
snapshot into a new file, so the copy opens on its own, passes
`Store::check`, and is the base for recovery. It runs as a reader and
never blocks the writer.

**I18. A migration step is one transaction.** `migrate` applies each
pending step in its own `Writer` with its row in `archivum_migrations`
and the new schema version; a crash leaves the store at a step boundary
and the next run continues. `punchline_migration_survives_a_crash_at_every_step`
crashes the Punchline schema migration at every VFS operation under
every persistence policy.

Not in the engine: the operational SQL layer and the change feed, which
are v1.1 (`docs/plan-v1.md`).
