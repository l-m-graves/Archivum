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

## Engine tooling (Stage 4)

Backup, restore, log archive and point-in-time recovery
(`docs/backup-recovery.md`) inherit the guarantees above. A backup is a
consistent copy of one committed snapshot; an archived log segment is the
committed log as of one checkpoint, synced before the live log is
emptied. Under an honest fsync, a backup plus the archived segments plus
the live log reproduce every acknowledged commit. Under a lying fsync the
archive is the defence the second tier depends on: copied off host at a
cadence set by the acceptable loss window (instructions v2, Q8), it makes
a commit that the local disk drops recoverable from the copy that was
made while it was still there. What the archive cannot do is recover a
commit the disk dropped before the checkpoint that would have archived
it, so the loss window is at most one checkpoint interval plus the copy
cadence.

## Renames and directories (Stage 6)

Every file the engine or the server puts in place under its final name
(an archived log segment, an off-host copy, a backup, a restored database)
is written under a temporary name, synced, and renamed. What the rename
itself guarantees differs by platform, and the guarantees are narrower
than "the rename is durable":

- **POSIX**: `rename(2)` followed by `fsync` of the parent directory
  (`Vfs::sync_directory`). Without the directory sync the rename is atomic
  but not durable: a crash can bring back the old directory entry. With
  it, the new name is durable.
- **Windows**: `MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING |
  MOVEFILE_WRITE_THROUGH)`, for every rename, local and at the
  destination (`engine/src/vfs_win32.cpp`). Microsoft's reference page for
  `MoveFileExW` (`learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw`,
  read and confirmed 2026-09-22) documents `MOVEFILE_WRITE_THROUGH` as
  guaranteeing that a move performed as a copy-and-delete operation is
  flushed to disk before the function returns, the flush occurring at the
  end of the copy. That is the **cross-volume** case. For a
  **same-volume** rename, which is what the local archive does and what
  the shipper does when the temporary and final names sit in the same
  destination directory, the page documents no flush guarantee: the
  rename is atomic (the new name appears or it does not) and that is the
  whole of the documented contract. NTFS journals its metadata, which is
  why a same-volume rename survives a crash in practice; that is an
  observation about NTFS, not a promise Archivum relies on, and there is
  no directory fsync on Win32 to add one.

**The durability argument for the archive is therefore not the rename.**
It is that the shipper is idempotent and verifies:

- a checkpoint's archive write is redone by the next checkpoint if its
  rename was lost (the segment is named by its base change counter and
  rewritten with a superset of its commits; `docs/backup-recovery.md`);
- the shipper copies every segment the destination lacks, so a rename
  lost to a crash at the destination means the segment is simply absent
  on the next pass and is copied and verified again;
- nothing counts as shipped, and the health check is not satisfied,
  until the file has been read back under its final name and matched
  against the source.

A lost rename costs one pass of the cadence, never a segment. That
argument holds on every platform and every file system, including the
ones below, and it is the one the handbook should cite.

### The off-host destination on a network share

The destination is likely an SMB share. What Archivum can and cannot
promise there:

- **What it does**: the copy is written to a `.part` name and closed with
  `FlushFileBuffers`, renamed with `MOVEFILE_WRITE_THROUGH`, and then read
  back under its final name and compared with the source (size and
  CRC32C; a backup page by page). Only a copy that verifies after the
  rename counts as shipped and satisfies the health check.
- **What that proves**: the file server presented the complete, correct
  bytes under the final name after the rename, to this client, at that
  moment.
- **What it cannot prove**: that the file server has committed those
  bytes and that directory entry to stable storage. Whether a
  write-through move or a flush over SMB reaches the server's disk before
  the call returns depends on the server's implementation and its own
  storage; the client has no way to observe it, and a read-back can be
  served from the server's cache. Archivum makes no claim about the
  server's durability and the handbook must not either. The idempotent
  re-ship above is what covers a copy the server later loses: on the
  next pass the segment is absent, and is copied and verified again.
- **What to do about it**: put the share on a server whose file system
  journals metadata and whose storage honours flushes, keep the shipped
  copies out of the server's reach for deletion, and run the monthly
  restore procedure (`docs/backup-recovery.md`) from the destination, not
  from the host: that is the only end-to-end proof that the off-host copy
  is what it claims to be.
