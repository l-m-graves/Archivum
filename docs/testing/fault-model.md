# Crash-injection fault model

`FaultVfs` (`engine/testing/include/archivum/testing/fault_vfs.h`) sits
between the engine and the "disk" in tests. This document says exactly what
it simulates, so that a passing test means something specific.

## State

Two in-memory filesystems and a list:

| Name | Meaning |
|---|---|
| `cache` | What the running process sees. Like the OS page cache. |
| `durable` | What survives a power loss. |
| `pending` | Every mutation applied to `cache` that has not yet reached `durable`, in order. |

Mutations are `Write(path, offset, bytes)`, `Truncate(path, size)`,
`Create(path)`, `Remove(path)`, `Rename(from, to)`.

## Operations

- **write / truncate / create / remove / rename** apply to `cache` and append
  to `pending`.
- **File::sync()** moves that file's pending `Create`, `Write`, and
  `Truncate` operations to `durable`, in order, and removes them from
  `pending`. Other files' operations and all `Remove`/`Rename` operations
  stay pending. This matches NTFS and ext4, where `FlushFileBuffers` /
  `fsync` of a newly created file also makes its directory entry durable.
- **Vfs::sync_directory()** moves pending `Create`, `Remove`, and `Rename`
  operations to `durable`. A `Create` persisted this way is an empty file;
  its data still needs `File::sync()`.
- With `FaultConfig::drop_all_syncs`, every sync reports success and moves
  nothing. This is the "fsync that lies".

## Crash

`crash()` simulates power loss. A subset of `pending` reaches `durable`
according to `CrashPolicy::persist`:

| Policy | What lands |
|---|---|
| `None` | Nothing. |
| `Prefix` | A random-length prefix of `pending`, in order. Models a write cache flushed in order and cut off. |
| `Subset` | An arbitrary subset, applied in a random order. Models write reordering: any unsynced write may land or not, independently, and overlapping writes may land in either order. |
| `All` | Everything. Models a crash right after the cache drained. |

With `torn_writes`, the last persisted write (`Prefix`) or any persisted
write (`Subset`, probability one in four) is cut short at a random byte, or
at a multiple of `sector_bytes` when that is set. The old bytes remain in
the unwritten tail, as on a real device.

A `Write` whose `Create` did not land is dropped: the file never existed.

After `crash()` every open handle returns `ErrorCode::Crashed` forever, and
so does the Vfs until `recover()`. `recover()` copies `durable` into `cache`
and empties `pending`. That is the state a process sees after reboot.

## Injected faults

A `FaultInjector` is consulted before every operation, with the operation's
sequence number, kind, path, offset, and length:

| Decision | Effect |
|---|---|
| `FailWrite` | Nothing is written; `IoError` is returned. |
| `PartialWrite(n)` | The first `n` bytes are written and pending; `IoError` is returned. |
| `DropSync` | The sync reports success and persists nothing. |
| `FailSync` | The sync returns `IoError` and persists nothing. |
| `Crash` | The operation is in flight when power is lost. For a write, the write is pending and the crash policy decides whether any of it lands. For a sync, nothing is persisted first. |

`CrashAtStep(k)` crashes on operation `k`. `RandomInjector(seed, rates)`
draws one uniform value per operation from its own generator, so its
decisions are a function of the seed and the operation sequence alone.

## Determinism

Every random choice inside `FaultVfs` comes from one `std::mt19937_64`
seeded by `FaultConfig::seed`. A test failure prints the seed, the step, and
the policy; that triple reproduces the run.

## How the journal tests use it

`tests/crash/journal_crash_test.cpp`:

1. **Crash at every step.** Count the operations of a fixed workload, then
   for each step, each of the four policies, three seeds, and both tear
   granularities, crash there, recover, and check: every acknowledged
   record is present, every recovered record equals what was appended, no
   record beyond what was attempted exists, and the journal accepts new
   appends.
2. **Crash twice**, the second time during recovery-and-continue.
3. **Lying fsync.** Same crash points with `drop_all_syncs`; durability is
   not required, consistency and usability are.
4. **Teeth.** A journal built with `unsafe_skip_sync` must be caught losing
   an acknowledged record at some crash point. If this test ever passes
   without violations, the harness is broken.
5. **Randomized.** Random policy, random tear granularity, random faults of
   every kind at low rates, one to four crash rounds per run, 300 iterations
   locally and 1,000 in CI. `ARCHIVUM_SEED` reproduces a run;
   `ARCHIVUM_CRASH_ITERS` sets the count.

## What the model does not cover

- Metadata corruption of the filesystem itself.
- Silent bit rot on a device that acknowledged correctly. Page and record
  checksums are the engine's defence; the shim can be extended to flip
  bits in `durable` when that is tested.
- Concurrency between processes: only one process opens Archivum's files.
- Real timing. The shim is synchronous; ordering is what it models.
