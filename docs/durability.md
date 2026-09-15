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

## Engine (Stages 2 to 4, to be written)

The engine will make the same two-tier promise per transaction: durable with
an honest fsync, consistent regardless. The defence against a lying fsync is
operational, not algorithmic: log archiving to an off-host destination at a
fixed cadence, mandatory for a healthy deployment (instructions v2, Q8).
