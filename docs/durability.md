# Durability model

This document states exactly what "committed" promises. It grows with each
stage; Stage 0 covers the append-only journal, which is the write-ahead
log's model in miniature.

## Vocabulary

- **Acknowledged**: the call returned ok to the application.
- **Durable**: present after power loss and reboot.
- **Consistent**: what recovery yields is a prefix of what was appended, in
  order, with no torn or corrupted record, and the file is usable.

## Journal (Stage 0)

With an honest fsync (the OS and device persist what `FlushFileBuffers` /
`fsync` claim):

- Every acknowledged record is durable.
- Recovery is consistent.
- A record whose append had not returned may or may not be present. If it is
  present it is complete and correct.

With a lying fsync (the device acknowledges a flush it did not perform):

- Durability cannot be promised by any software. Acknowledged records may be
  lost.
- Recovery is still consistent: a prefix, never a torn or mixed state, and
  the journal remains usable.

Both claims are tested at every crash point under every persistence policy
in `tests/crash/journal_crash_test.cpp`, and the harness is itself tested to
catch a journal that skips fsync.

## Engine pager and log (Stage 2)

Per write transaction, with an honest fsync:

- Every acknowledged commit is durable and visible after recovery, whole.
- No transaction that was rolled back, or whose commit was never attempted,
  is visible.
- A commit that was in flight at the crash, or that returned an I/O error,
  is in doubt: after recovery it is either fully present or fully absent,
  never partial. Callers that need certainty retry idempotently.
- Recovery is consistent: the data file plus the replayed log equal exactly
  the state after some prefix of the acknowledged commits, followed at most
  by one in-doubt commit.
- A page is never served unless its checksum verifies; a log frame is never
  replayed unless its chained checksum verifies.
- Two instances on two files recover independently of each other.

With a lying fsync:

- Acknowledged commits may be lost.
- A crash during checkpoint may leave the data file holding a mix of page
  versions, because write ordering is exactly what the lying fsync fails to
  provide. Every page still verifies individually, `Db::check()` reports
  what it can detect (free-list inconsistencies, unreadable pages), and a
  torn header that the log cannot repair is refused with `Corrupt` rather
  than served. The remedy is restore from backup.
- The defence is operational, not algorithmic: log archiving to an off-host
  destination at a fixed cadence, mandatory for a healthy deployment
  (instructions v2, Q8).

Both tiers are tested at every crash point of a fixed workload under four
persistence policies, and with seeded random faults, in
`tests/crash/db_crash_test.cpp`.

## Engine tooling (Stage 4, to be written)

Backup, restore, log archive, and point-in-time recovery inherit the
guarantees above and add the off-host copy that the lying-fsync tier
depends on.
