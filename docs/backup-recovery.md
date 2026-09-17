# Backup, restore, log archive and point-in-time recovery

For operators. The engine side is `docs/engine-design.md`, invariants
I14 to I18. Every command is the one binary:

```
archivum check   --db <path>
archivum backup  --db <path> --to <file>
archivum restore --from <file> --to <path> [--discard-log]
archivum pitr    --backup <file> --archive <dir> --to <path>
                 [--live-log <file>] [--change-counter N | --time-us T]
archivum migrate --db <path>
```

## Files

| File | What it is |
|---|---|
| `<db>` | the database: pages, written only by checkpoints |
| `<db>.wal` | the live log: every commit since the last checkpoint |
| `<archive>/<db id>-<base>.wal` | an archived log segment: the committed log at the checkpoint that emptied it. `base` is the change counter of the database the segment continues from, zero-padded so names sort |

The change counter increases by one per commit and is printed by every
command; it is the coordinate of a database state. Each commit also
records its wall-clock time (µs since the Unix epoch, UTC).

## Backup

`archivum backup` copies one committed snapshot into a new file. It runs
as a reader: the server keeps committing while it runs, and the backup
holds a checkpoint off until it finishes (the checkpoint reports Busy and
retries after the next commit). The result is a complete database with no
log; it opens on its own, `archivum check` passes on it, and it is the
base for point-in-time recovery. The command prints the change counter
the backup is at.

Take the backup on the host, then copy it off host. The engine does not
do the off-host copy; the deployment does, and the `[FILL: backup
destination]` from instructions v2 Q8 names where.

## Log archive

Set `archive_dir` on the database (server configuration, Stage 5). Every
checkpoint then copies the log it is about to empty into the archive and
syncs it before emptying. Copy the archive directory off host at the
cadence that matches the acceptable loss window: with an honest disk the
loss window after a host loss is at most one checkpoint interval plus the
copy cadence. Segments are never rewritten except after a crash between
the copy and the reset, when the next checkpoint writes the same name
with a superset of its commits; keep the newer file.

## Restore

`archivum restore --from <backup> --to <path>` copies the backup into
place and runs `check` on it. It refuses if `<path>` or `<path>.wal`
exists: a restored backup beside a live log is the one situation the
engine cannot tell from a crash, and it would replay the log onto the
wrong base. The engine guards this too (a log whose base does not match
its database is refused at open), so `--discard-log` is the explicit way
to say the old log is not wanted.

## Point-in-time recovery

`archivum pitr` starts from a backup and replays archived segments, and
optionally the live log, up to a target:

- `--change-counter N` stops at exactly N and fails (exit 3) if the
  segments do not reach it;
- `--time-us T` applies every commit at or before T;
- neither applies everything available.

The backup is usually mid-segment; recovery finds the segment whose base
is at or below the backup's counter, skips the commits the backup already
holds, applies the rest, and continues with the segment that starts where
the last one ended. The result is checked before the command returns.
Commits are verified to be consecutive; a gap or a segment for another
database is an error, never a silent skip.

## Verification in CI

- `tests/unit/backup_test.cpp`: backups taken while a writer commits,
  each restored and checked and holding exactly the rows its counter
  implies; a restored backup beside a live log refused; recovery to
  several counters and times through archived segments and the live log,
  each result compared with the dump recorded at that counter.
- `tests/cli/cli_test.cpp`: the commands above on real files in the
  build directory, on Linux and Windows.
- `tests/crash/punchline_migration_test.cpp`: the first migration
  crashed at every VFS operation under every persistence policy, rerun to
  completion each time.

## Monthly restore procedure (Stage 11 handbook)

Restore last month's backup to a scratch path with `archivum restore`,
run `archivum pitr` from it to the current counter with the archive, and
compare row counts with production. Both commands end with `check`.
