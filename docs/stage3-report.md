# Stage 3 report: b-trees, records, catalog, constraints, typed API

Scope from `docs/plan-v1.md` (Stage 3): b-trees, records and types,
catalog, constraints, typed API, snapshot readers; model-based tests,
invariant checker, dump and reload round trip; the timeout-versus-
checkpoint test. CI run: 14 (commit 27d2ddb), in progress at the time of
writing; the CI rows below are filled in when it completes.

## Gate status

| # | Requirement | Status |
|---|---|---|
| 1 | B-trees over the pager | **Done.** `btree.h/.cpp`: slotted nodes, overflow chains, fixed root, doubly linked leaves, empty-leaf unlink and interior collapse, structural checker. `docs/btree-format.md` |
| 2 | Records and types | **Done.** integer, decimal (int64 + scale, never floating point), text (UTF-8 validated), blob, timestamp (int64 µs UTC), boolean, uuid; order-preserving composite key encoding; compact row encoding. `docs/store-format.md` |
| 3 | Catalog in the database file, versioned, migrations as one write transaction | **Done.** Catalog b-tree at page 1; schema version entry; table definitions as rows; DDL and DML in one `Writer`; a rolled-back migration leaves nothing (test `store_migration_is_atomic_and_reopens`) |
| 4 | Constraints in the engine: PK, unique, not-null, FK restrict, check | **Done.** Enforced in `Writer` before any page is written, so a violation leaves the transaction unchanged (I12). Check supports `column op constant` and `column IN (list)`; richer predicates wait for the SQL layer's expression evaluator |
| 5 | Typed programmatic API: get, range scan by index, insert, update, delete, transaction | **Done.** `Reader::get/scan/scan_all/count`, `Writer::insert/update/remove` and the DDL calls; scans by primary key or any index, bounded by a key prefix on either side, inclusive or exclusive, forward or reverse |
| 6 | Secondary indexes with range scans | **Done.** Index key = indexed columns + primary key; unique indexes with NULLs distinct; built from existing rows on creation |
| 7 | Model-based tests at the typed level | **Done.** `tests/crash/store_model_test.cpp`: see below |
| 8 | Invariant checker | **Done.** `Store::check`: pager checks, every tree's structure, index ↔ row both ways, unique and primary keys, foreign keys at rest, NOT NULL and checks at rest, page ownership partition (I8 to I11) |
| 9 | Dump and reload round trip | **Done.** `Store::dump` / `Store::restore`; restore is one transaction, creates tables parents first, inserts without per-row foreign key checks and verifies every table's foreign keys before commit (self-references restore). Test `store_dump_and_restore_round_trip` compares definitions and rows byte for byte and runs the checker on the copy |
| 10 | Multi-threaded snapshot readers; a query running to its timeout while a checkpoint is pending | **Done.** `store_snapshot_readers_and_pending_checkpoint`: four reader threads scanning while a writer commits 200 transactions and requests a checkpoint after each; a long query holds its snapshot throughout. Every reader sees a consistent snapshot (row count equals the highest id); the checkpoint reports `Busy` while the long query lives and succeeds once it ends; the long query still sees its original, empty, table |

## The model-based test

`store_model_test` drives a store and a naive model (`std::map` of rows
per table plus the definitions) with the same operations and compares
them after every commit. The schema is fixed and deliberately awkward:

- `parents`: integer key; every column type; two unique indexes (one the
  target of a foreign key); a composite non-unique index; a check on a
  nullable decimal.
- `children`: composite key `(parent_id, seq)` whose prefix supports a
  foreign key to `parents.id`; a second foreign key to the unique index
  `parents.u`; an `IN` check.
- `tags`: text key; a two-column foreign key to `children`'s composite
  key; `≠ ''` and `≤` checks.

Each transaction applies 1 to 15 random operations: inserts (45%),
updates of non-key columns (25%), deletes (15%), index creation on random
columns, unique one time in three (7%), index removal (5%), schema
version bumps (3%). Values are drawn from small domains so that duplicate
keys, duplicate unique values, missing parents, referenced parents, NULLs
in NOT NULL columns, check failures and over-long keys all happen often;
the model computes its own verdict for every operation and any
disagreement with the store fails the run. One transaction in six is
rolled back after the writer's own view is compared with the model. One
commit in four is followed by a checkpoint. After each commit every
table is compared by primary key, every index in index order, three
random bounded scans (random prefix length, inclusive or exclusive,
forward or reverse) and three point lookups; the full checker runs every
fifth commit. A crash is injected at a random step in the last third of
each of four rounds under a random persistence policy; the store is
reopened and must match the committed model, an in-flight commit being
accepted as either wholly present or absent, and the checker runs on
the recovered file.

Runs:

| Configuration | Iterations | Result |
|---|---|---|
| Linux Debug, ASan+UBSan, local, seeds 1000 to 1029 and 2000 to 2029 | 60 | green |
| Linux Release, local, seeds 5000 to 6999 | 2,000 | green |
| CI Linux Debug (ASan+UBSan), `ARCHIVUM_CRASH_ITERS=1000` | 20 | `[CI]` |
| CI Linux Release, `ARCHIVUM_CRASH_ITERS=10000` | 200 | `[CI]` |
| CI Windows Debug / Release (MSVC 19.44) | 20 / 200 | `[CI]` |

An iteration is four rounds of up to 25 transactions with a crash per
round. `btree_model_test` (the byte-string tree against `std::map`) ran
60 sanitized iterations locally and runs at the same CI counts. The base
seed is printed by every run and `ARCHIVUM_SEED` reproduces it.

## What the tests found

1. **Foreign keys supported by the primary key.** The first cut required
   a secondary index on the child's foreign key columns even when the
   child's primary key led with them (`children(parent_id, seq)`). Fixed:
   a primary key prefix supports the parent-side lookup exactly as an
   index does, and `drop_index` accounts for it.
2. **Dropping a redundant unique index.** `drop_index` refused to drop
   any unique index that was the target of a foreign key, even when
   another unique index on the same columns remained. Found by the model
   test's random index DDL at seed 5014. Fixed: refused only when no
   other unique index (or the primary key) on those columns remains.
3. **Key length is a real limit.** A random index on the blob column
   pushed the index key past the b-tree's limit (232 bytes at the test
   page size, 1002 at 4096). The store refuses this as `InvalidArgument`
   at insert, update and index creation; the model was taught the limit.
   Consequence for Stage 4: an index on a long text column needs the SQL
   layer to refuse it or truncate deliberately; the engine will not
   silently do either.
4. **Harness gaps.** A crash landing inside the reader-side comparison
   of an open write transaction, or at `begin_write`, was not followed by
   recovery; both paths now recover and compare like any other crash.
5. **Transitive includes.** CI's GCC 12 rejected `types.cpp` for
   `std::to_integer` without `<cstddef>`; the local build reached it
   through another header. Every new file now names the headers it uses.

No engine bug below the typed layer surfaced: the pager and log were
unchanged by Stage 3 apart from `CheckReport::free_pages` (the free list
in walk order, for the ownership cross-check) and `WriteTxn::change_counter`
(for the catalog cache).

## Design points the next stages inherit

- **Update never changes the primary key.** The SQL layer implements
  `UPDATE ... SET pk = ...` as delete plus insert inside the transaction;
  foreign keys then apply as for a delete.
- **A row cannot reference itself at insert.** RESTRICT is checked
  against the committed-plus-transaction state at the moment of the
  operation, not at commit; insert then update is the pattern.
- **Checks are `column op constant` and `column IN`.** Enough for the
  Punchline schema (kinds, states, non-negative amounts); anything else
  is an application check until Stage 4's expression evaluator exists,
  at which point `CheckDef` grows a general predicate.
- **Foreign keys require an index on the child.** `create_table` refuses
  a foreign key the child's primary key or an index cannot support, so
  the parent-side check is always a prefix lookup and never a table scan.
- **Catalog per snapshot.** Readers carry the catalog of their change
  counter; the store caches one catalog per counter, so the common case
  costs nothing and a schema change costs one catalog read per reader
  that starts after it.

## Stage 4 next

Read-only SQL over the typed API (parser, planner, expression evaluator,
the visual builder's backend), the change feed, and the Punchline schema
as the first migration. Nothing in Stage 4 touches pages.
