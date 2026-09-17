# Stage 4 report: backup, recovery, migrations, the Punchline schema

Scope from `docs/plan-v1.md` (Stage 4): integrity check, restore, online
backup, log archive, point-in-time recovery, restore-and-verify in CI.
Pulled forward from Stage 5 at your request: the migration framework, with
the Punchline schema as the first migration. CI run `[CI-RUN]`.

A correction first. The Stage 3 report's closing section said Stage 4
was SQL and the change feed. It is not: the accepted plan puts SQL in
v1.1. The report is corrected in place, and the migration framework and
schema are in Stage 4 because its backup and recovery tests are only
meaningful against a real schema.

## Gate status

| # | Requirement | Status |
|---|---|---|
| 1 | Integrity check | **Done.** `archivum check --db` runs `Store::check` (pager, every tree, index ↔ row both ways, constraints at rest, page ownership) and exits 1 on any problem |
| 2 | Online backup | **Done.** `Db::backup` / `archivum backup` copies one snapshot into a standalone database while the writer keeps committing; the writer is never blocked, the next checkpoint waits until the backup's snapshot ends. Test: five backups under a concurrent writer, each restored, checked and holding exactly the rows its counter implies |
| 3 | Restore | **Done.** `archivum restore` copies a backup into place and checks it; refuses when a log sits beside the target unless `--discard-log`. The engine refuses a log that does not continue its file (I14), so a restored backup beside a live log cannot be replayed by accident (test `restored_backup_beside_a_live_log_is_refused`) |
| 4 | Log archive | **Done.** `DbOptions::archive_dir`: every checkpoint copies the committed log to `<archive>/<db id>-<base>.wal`, syncs it and its directory, then empties the log (I15). Commit frames carry the change counter and wall-clock time (I16) |
| 5 | Point-in-time recovery | **Done.** `recover_to_point` / `archivum pitr`: from a backup, through the archived segment that covers the backup's counter and every later one, optionally through the live log, to a change counter or a time; commits verified consecutive; result checked. Tests: five counter targets and two time targets each compared with the dump recorded at that counter; a target beyond the end reported as not reached; archive only, without the live log |
| 6 | Restore-and-verify in CI | **Done.** `backup_test` (library) and `cli_test` (the binary on real files: migrate, check, backup, restore, pitr) run on all four CI jobs |
| + | Migration framework | **Done.** `migrate`: versioned steps, one write transaction each, recorded in `archivum_migrations` with the catalog's schema version; idempotent; a database newer than the binary is `Unsupported`; a diverged history is `Corrupt`; a failing step leaves nothing (I18) |
| + | Punchline schema migration | **Done and green.** Eleven tables (`docs/punchline-schema.md`): employees, role_grants, local_accounts, devices, pay_periods, schedules, audit_log, time_entries, sync_batches, approvals, exceptions. Applied, reapplied as a no-op, exercised through every kind of constraint, and crashed at every VFS operation under every persistence policy |

## The Punchline migration under crash injection

`punchline_migration_survives_a_crash_at_every_step` runs the migration
over the crash shim with a crash injected at operation k, for k = 1, 2,
3, ... until a run completes without the crash landing, under each of
the four persistence policies. After every crash the store is reopened,
checked, and required to be at a step boundary: schema version 0 with no
tables, or version 1 with all of them, never anything between. The
migration is then rerun and the result compared with a reference dump.

| Configuration | Crash points | Result |
|---|---|---|
| Linux Debug, ASan+UBSan, local | 588 | green |
| Linux Release, local | `[REL-CRASH]` | green |
| CI, four jobs | `[CI]` | `[CI]` |

## Figures

| Test | Linux Debug (ASan+UBSan) | Linux Release |
|---|---|---|
| migrate_test | `[T]` | `[T]` |
| backup_test | 1.6 s | `[T]` |
| punchline_migration_test | `[T]` | `[T]` |
| cli_test | 0.7 s | `[T]` |

Whole engine suite, Debug at 1000 iterations: 17 tests green. Release at
10000 iterations: green.

## What the tests found

0. **A pager race, present since Stage 2.** `Db::begin_read` took its
   snapshot (the last committed frame), read the header, and only then
   registered itself as a reader. Between the two a commit and its
   automatic checkpoint could run, see no reader, copy the log into the
   file and empty it. The snapshot's frame numbers then pointed into an
   emptied log: reads fell through to the file, which held newer pages,
   or hit later transactions' frames under the old numbers. The Stage 2
   crash suites never saw it because they are single-threaded; the
   Release run of the new backup-under-concurrent-writes test failed its
   integrity check once in a few hundred runs. Fixed: the reader is
   registered in the same critical section that takes the snapshot.
   Regression test `db_reader_registration_is_atomic_with_its_snapshot`:
   four readers against a writer that checkpoints after every commit,
   6,000 snapshots each checked for a header that matches its counter
   and three pages that agree; without the fix it reports 100 to 170
   inconsistent snapshots per run, with it none.
1. **A backup is taken mid-segment.** The first recovery required a
   segment whose base equalled the backup's counter exactly. Real backups
   land inside a segment. Found by `cli_test` (zero segments applied).
   Fixed: recovery lists the archive, starts from the segment with the
   largest base at or below the backup's counter, and skips the commits
   the backup already holds. This added `Vfs::list` on POSIX, Win32 and
   the test doubles.
2. **Log continuity.** Introducing the base counter check (I14) was
   validated by every Stage 2 crash test, which cross the checkpoint's
   sync-then-reset window at every step: the rule had to admit "file
   ahead of the base by no more than the log's last commit" or a crash
   between the file sync and the log reset would have made the database
   unopenable.
3. **Nothing new below the typed layer** beyond those two rules: the
   Stage 2 and 3 model-based suites ran unchanged against the new
   header and log formats (format version 2; nothing is deployed, so no
   migration of files).

## Design points the next stages inherit

- **The archive is the off-host defence.** The engine writes segments
  locally and durably; the deployment copies them off host. The loss
  window after a host loss is one checkpoint interval plus the copy
  cadence (`docs/durability.md`). `[FILL: backup destination]` and the
  cadence come from instructions v2 Q8.
- **The change counter is the coordinate.** Backups, segments and
  recovery targets all speak in it; the commit time is a convenience for
  operators, the counter is what is verified.
- **Schema rules the engine cannot express** are listed at the end of
  `docs/punchline-schema.md`; each is a Stage 6 application rule with a
  test.
- **One active device per employee** is enforced by enrollment, not the
  schema, so a replaced device keeps its predecessor's history.

## Stage 5 next

Server core: configuration (including `archive_dir`), structured logs,
health with certificate expiry, trusted proxies, device enrollment and
revocation, local accounts and break-glass, roles on `oid` and `tid`,
the audit writer with per-field recordability, the module framework
around the migrations that now exist, and the mandatory off-host archive
check.
