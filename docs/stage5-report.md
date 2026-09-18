# Stage 5 report: server core

Scope from `docs/plan-v1.md` (Stage 5) and the Stage 5 rulings. CI run
[23](https://github.com/l-m-graves/Archivum/actions/runs/35367758201) on five jobs: Linux Debug (ASan+UBSan), Linux Release, Linux
Debug ThreadSanitizer, Windows Debug, Windows Release.

## 1. ThreadSanitizer

### The job

`linux-tsan`: a separate Debug build with `-fsanitize=thread`, and the
triplet `x64-linux-cxx20-tsan` so that every vcpkg port (Drogon, trantor,
OpenSSL, jsoncpp, libsodium) is instrumented too. It runs every test
labelled `concurrency`: the pager's snapshot-reader test (`db_test`), the
store's reader test (`store_test`), the backup-under-writes test
(`backup_test`), the new `concurrency_test`, `core_test`, and the three
server suites. `TSAN_OPTIONS=halt_on_error=1`: a report fails the job.

### What it flagged

Everything it reported, in the order found. Nothing is suppressed: there
is no suppressions file and no `ATTRIBUTE_NO_SANITIZE` anywhere.

1. **A real race in the engine, a violation of I1.** `random_salt()` in
   `wal.cpp` drew from a function-local static `std::mt19937_64`: global
   state shared by every `Db` instance. Two instances on two threads
   (`concurrency_two_instances_on_two_threads`) raced on it inside their
   checkpoints. Single-threaded, the two-instance tests of Stage 2 could
   not see it; the consequence would have been correlated salts, not
   corruption, but "no global state" was the invariant and it was false.
   Fixed: a `std::random_device` per call. `random_db_id()` already did
   that.
2. **35 and 24 reports in the two server suites, all false.** With Drogon
   and trantor built without TSan (the first local run used the plain
   local build of them), TSan cannot see their atomics and reference
   counts: `trantor::EventLoop::wakeup`, `std::shared_ptr` control
   blocks, `operator delete` after an uninstrumented release, and
   promise/future state in `queueInLoop`. Rebuilt with instrumented
   Drogon and trantor, both suites report nothing. That is why the CI job
   instruments every port through its own triplet rather than adding a
   suppressions file: a suppression would have hidden the same classes
   of report in our own code.
3. **One report from CI that the local run did not produce: an at-exit
   race in the test fixture.** The server test fixture is heap-allocated
   and never destroyed (Drogon's singletons are destroyed during exit),
   so its client event-loop thread was still tearing down a connection,
   freeing an `SSL_CTX`, while `OPENSSL_cleanup` ran from `atexit` on the
   main thread. A real race, in test code, only at process exit. Fixed:
   the last test stops the client loop and joins it before returning.
4. **Nothing else.** With those fixed, the engine suites (`db_test`,
   `store_test`, `backup_test`, `concurrency_test`, `core_test`) and the
   three server suites are clean under TSan with instrumented
   dependencies, locally and in CI run [23](https://github.com/l-m-graves/Archivum/actions/runs/35367758201).

One more defect came out of the review the TSan work prompted, not from
TSan itself: the checkpoint wrote an archived segment directly under its
final name, so the off-host shipper, which lists the archive directory
on its own thread, could copy a half-written segment and never revisit
it. Segments are now written to a `.part` name and renamed into place;
the shipper copies the same way.

### Where else a multi-threaded path had only run single-threaded

The reader-registration race existed because the test that would catch
it did not exist. The same question, asked of everything else:

| Path | Before Stage 5 | Now |
|---|---|---|
| Two `Db` instances used from two threads (the two-database design) | single-threaded interleaving only | `concurrency_two_instances_on_two_threads`; found the salt race |
| Schema changes (`create_index`, `drop_table`) while readers hold snapshots; the catalog cache keyed by change counter | readers tested only against row inserts | `concurrency_ddl_under_readers`: four readers assert their snapshot's catalog is self-consistent while a writer creates and drops indexes and tables |
| `migrate` from two threads | never | `concurrency_migrate_from_two_threads`: one applies, the other sees Busy or nothing to do, never a double application |
| Backups and checkpoints with archiving against a writer | backups against a writer only | `concurrency_backup_checkpoint_archive_and_writers`, then recovery from each backup through the archive |
| The archive shipper thread against checkpoints and requests | new in Stage 5 | server test `health_fails_loudly_until_the_off_host_copy_succeeds` runs it live; `concurrency_backup_checkpoint_archive_and_writers` covers the engine side |
| OIDC key cache: concurrent requests, refresh under an unknown kid | concurrent requests in the saturation test; concurrent refresh never | still single-threaded for the refresh path. The cache is one mutex and the refresh is rate-floored, so the worst case is a redundant fetch; a test with N threads presenting an unknown kid at once is on the Stage 6 list |
| `MemVfs` and `FaultVfs` | `MemVfs` used from several threads by `store_test` and `backup_test` with no lock of its own; TSan did not object because the pager's locks happened to order every access to each file, which is luck, not design | `MemVfs` now serialises the path table on one mutex and each file's bytes on the file's own; the concurrency suites are TSan clean with it. `FaultVfs` stays single-threaded by design: the crash shim is a deterministic schedule and every crash test drives it from one thread |
| The client journal | single-threaded by design (one writer, the client) | unchanged; documented in `docs/journal-format.md` |
| Windows | the same code, never under TSan | MSVC has no ThreadSanitizer. The Windows jobs run the concurrency tests, which catch logic races through their assertions but not data races. This is the remaining blind spot and it is structural; the mitigation is that no engine or core code path is platform-specific above the VFS |

## 2. Cross-column checks: module enforcement

Chosen: enforce in the module, and record every module-enforced
invariant in `docs/punchline-schema.md` with a test asserting that the
engine accepts the bad row and the module rejects it. The list has eight
rules (PL-1 to PL-8); PL-1, PL-2 and PL-8 are enforced and tested now,
PL-3 to PL-7 are named with their Stage 6 owner (sync, pairing, approval
lifecycle) and each carries the test obligation. `clock_out > clock_in`
is PL-6: entries are single punches, so the rule is a pairing rule, and
pairing is Stage 6.

Why not the evaluator earlier: it is the front of the v1.1 SQL engine
(7 to 10 weeks) and moving it forward would put the parser and the
expression language ahead of the approval lifecycle. A column-to-column
`CheckDef` in the engine would be a day's work if a rule turns out to
need it, and I will propose it if PL-6 or PL-7 turns out to be awkward
as a module rule.

## 3. The five schema items

| Item | Answer | Where |
|---|---|---|
| Effective-dated supervisor mapping | was absent; now a **table** `supervisor_assignments` (employee, supervisor employee, effective_from, effective_to nullable, assigned_by, audit_id NOT NULL), non-overlapping per employee (PL-2), with an admin route | `docs/punchline-schema.md` |
| Three time values plus tzdb version | **columns** of `time_entries`: `device_time` (µs UTC), `site_zone` (IANA), `local_time` (wall clock as recorded), `tzdb_version`, and `receipt_time` as the server's instant | unchanged from Stage 4 |
| Attestation source | **column** `time_entries.attestation` in device, server, manual; NOT NULL, checked, now indexed with `device_time` | Stage 4 column, Stage 5 index |
| Pay codes | was a free-text column; now a **lookup table** `pay_codes` (regular, overtime, double_time, pto, holiday, seeded) and `time_entries.pay_code` is a foreign key to it | Stage 5 |
| Change feed | **built**, in Stage 5, as one mechanism with the audit writer: `core::Recorder` writes the audit row and the change-feed images inside the business transaction; the presence-and-shape rule is applied in that one class and nowhere else | `docs/audit-and-change-feed.md` |

Two schema moves in the same migration, because nothing is deployed:
`audit_log`, `role_grants` and `local_accounts` now belong to the core
module (they serve every module), and `archivum_migrations` is keyed by
module.

## 4. Stage 5 scope

| Item | Status |
|---|---|
| Configuration with the archive directory; health failing loudly with no off-host destination | **Done.** `database` and `backup` sections required; no destination or cadence, no server. `/healthz` is 503 until the first successful off-host copy and whenever the last one is older than twice the cadence |
| Structured logging and the health endpoint | **Done.** JSON lines with named fields and an alert level; health reports TLS, JWKS, schema, database and archive state |
| OIDC validation against the local test issuer | **Done** (Stage 1), now followed by standing from data |
| Device enrollment and revocation with the per-device credential | **Done.** One device, one credential (32 random bytes, Argon2id verifier, shown once), one employee; revocation keeps the row; a revoked device is 403 and audited; PL-1 |
| Local accounts and break-glass (Q9) | **Done.** Console-only creation, disabled by default, enabled for a bounded time, auto-disabled at expiry in the store, every use an audit row and an alert |
| Roles and dataset grants as data on `oid` with the nullable-oid rule | **Done.** `role_grants` and `dataset_grants` keyed on `(tid, oid)`; an employee may have no identity; an identity with no grant and no employee is 403 and audited; email never consulted |
| Audit writer with every mutating request's audit rows in the same write transaction | **Done.** `core::Recorder` on the request's `Writer` |
| Change feed in the same writer | **Done.** Same class |
| Module framework around the migrations | **Done.** `core::Module` (migrations, record policy) plus routes; migrations keyed by module; Punchline is the built-in |
| Contract test: employee_id rejected, not ignored, on every endpoint | **Done.** Seven endpoints, four body shapes (top level, nested, in an array), 400 naming the key, no audit row written |

## 5. Figures

CI run 23, seconds:

| Test | Linux Debug (ASan+UBSan) | Linux Release | Linux TSan | Windows Debug | Windows Release |
|---|---|---|---|---|---|
| concurrency_test | 2.01 | 0.10 | 4.48 | 1.79 | 0.70 |
| core_test | 6.38 | 0.33 | (not labelled) | 1.75 | 0.46 |
| punchline_rules_test | 0.09 | 0.01 | | 0.05 | 0.01 |
| punchline_migration_test (740 crash points) | 30.36 | 1.88 | | 28.58 | 2.22 |
| server_integration_test | 3.88 | 3.61 | 5.01 | 6.28 | 4.00 |
| server_core_test | 12.79 | 5.50 | 42.78 | 13.14 | 6.32 |
| server_saturation_test | 4.12 | 3.66 | 5.10 | 12.44 | 5.02 |
| whole suite (23 tests; TSan: the 7 labelled) | 119.3 | 36.9 | 64.5 | 106.9 | 50.2 |

`server_core_test` under TSan spends most of its time in the health test
waiting out the shipper cadence, as designed.

Two CI-only failures on the way, both in tests, both fixed in the same
push as the fixture race: `/healthz` is 503 by design until the first
off-host copy succeeds, and the health and saturation tests on the CI
runners asked before the shipper's first pass had run; they now wait for
the first 200.

## 6. Open

- Q8's answers set `backup.destination` and the cadences; the example
  configuration holds placeholders.
- The OIDC concurrent-refresh test (table above).
- The Windows blind spot is structural (no TSan on MSVC); noted, not
  mitigable beyond keeping platform-specific code below the VFS.
