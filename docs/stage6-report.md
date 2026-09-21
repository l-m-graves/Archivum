# Stage 6 report: the Punchline module

Scope from `docs/plan-v1.md` (Stage 6) and the Stage 6 rulings. CI run
[29](https://github.com/l-m-graves/Archivum/actions/runs/35633803162) on six jobs: Linux Debug (ASan+UBSan), Linux Release, Linux
Debug ThreadSanitizer, Windows Debug, Windows Release, and the contract
suite against the FastAPI prototype (which passes visibly-skipped until a
token for the Punchline repository is configured; it ran here against the
checked-out repository, below).

## 1. The rulings, applied

| Ruling | Done |
|---|---|
| 1. WAL salt from libsodium, `sodium_init` at process start, fail startup not checkpoint, a distinct-salts test | `archivum/entropy.h`: `init_entropy()` (idempotent `sodium_init`, Io on failure) runs first thing in `main` and again in `Db::open`, so a caller that skipped it fails at open; `random_bytes` is `randombytes_buf` for salts and database ids. `concurrency_checkpoints_on_two_threads_produce_distinct_salts`: two instances, two threads, 120 concurrent checkpoints, 120 distinct salt pairs, read back from each log's header. The engine links libsodium; the core's own initialisation now calls the engine's |
| 2a. Parent directory synced after the rename, locally and at the destination | Confirmed present on both sides and left as it was: the checkpoint writes `<seg>.part` (file and its directory synced by `copy_committed_to`), renames, then `sync_directory(archive_dir)`; the shipper copies to `.part` (`copy_file` syncs file and directory), renames, then `sync_directory(destination)`; the backup likewise. On Windows `sync_directory` is a no-op by design (NTFS journals metadata; the rename is `MOVEFILE_WRITE_THROUGH`), now stated in the shipper's header and `docs/server-core.md` |
| 2b. Every copy verified before it counts | Was missing; added. A segment's copy is read back from the destination and compared with the source by size and CRC32C (`engine::file_digest`); the backup, written straight to the destination, is checked page by page against the page checksums (`engine::verify_backup_file`). A mismatch removes the `.part`, fails the pass (health goes 503 on the cadence rule), alerts `archive.copy_verification_failed` or `archive.backup_verification_failed`, and counts in `/healthz` as `verification_failures`. `backup_and_segment_verification_helpers` proves the helpers catch a truncation and a flipped byte and name the page |
| 3. Same-row two-column comparisons into the engine | `CheckDef.other_column`: `column op other_column` with matching types, NULL on either side passing, persisted in the catalog (format version 3; nothing was deployed under 2). Moved: `supervisor_assignments.effective_to > effective_from`, `schedules.end_minute > start_minute` and its day range, `pay_periods.end_day ≥ start_day` and `approve_by ≥ submit_by`, `shifts.out_time > in_time`. PL-2's "range is ordered" clause is therefore the engine's now; `store_column_to_column_checks` covers definition errors, NULL semantics, updates and the catalog round trip |
| 4. Non-overlap module-enforced and listed; half-open intervals | PL-2 was already listed with its accept/reject test; the overlap check was already half-open. Made explicit: the doc says `[effective_from, effective_to)`, `supervisor_at` picks the later assignment at the boundary instant, and the test asserts the boundary (199 → supervisor 2, 200 → supervisor 3) and the open end |
| 5. Employee-id rejections recorded | Still 400, still no business audit row. Now every rejection is logged at warning as `request.employee_id_asserted` with the route, the key path (`entries.employee_id`), and the caller; from a device the count is kept on the device row and at `employee_id_rejection_threshold` (default 5) an `employee_id_asserted` exception opens, audited `device.tamper_signal` with the device as actor, and alerted. Tested: three rejected syncs from one device open the item; a principal's rejection is logged with the caller and not counted |
| 6. Windows-only threaded surface | Section 5 |

## 2. Stage 6 scope

| Item | Status |
|---|---|
| Sync endpoint with idempotent batches, keyed on entry uuid and journal sequence | **Done.** One transaction under one audit row; accepted-again on a stored uuid; `(journal_id, journal_sequence)` unique with a recreated journal's new id; a reissued sequence under another uuid rejected by name; per-entry outcomes; replayed batches answered again; 413 over the limit. `docs/punchline-module.md`, "Sync" |
| Punch pairing | **Done.** The fold in `repair_pairing`: in opens, the next out of the same device closes, everything else is flagged; reruns from the earliest new punch with a rebuild range closed under overlap; never rebuilds an approved shift, so a late sync never fails and never alters an approval |
| Approval lifecycle, every transition audited, payroll only by an authenticated transition | **Done.** `transition` and `lock_period`: one `approvals` row per transition under the request's audit row, PL-7 standing at the time of the action, no skipping an entry that is behind, locking requires everything released. The payroll export reads released and locked shifts only and counts what it withheld; the audit report lists every transition with actor and time |
| Device-attested marking resolved through approval | **Done.** Entries stay `attestation: device` (distinguishable in every query); the per-period count opens `device_attested_count` at the threshold and approval by the named supervisor resolves it and stamps `approval_audit_id` on the entries |
| Exception queue, including freshness monitoring on device sync | **Done.** Thirteen kinds, idempotent while open, always through the Recorder of the action; scoped listing; resolve and dismiss audited; the monitor opens `device_stale` and the next sync or heartbeat resolves it |
| Escalation after cutoff | **Done.** The monitor opens `past_cutoff` per employee and period after `submit_by` (still recorded) and `approve_by` (still submitted) with an alert; payroll may approve with a documented override, which the approvals row and the audit report show as an override |
| PL-3 through PL-7 with their tests | **Done.** `punchline_rules_test` has one test per rule asserting the engine accepts and the module refuses; `punchline_sync_test` and `server_punchline_test` exercise them through sync, corrections and the lifecycle |
| The OIDC concurrent key-refresh test | **Done, and it found a design gap.** The Stage 5 note said the worst case of the rate floor was a redundant fetch; it was not: N requests presenting a rotated kid at once got one refresh and N-1 refusals until the floor elapsed. Refreshes are now single-flight (one fetch, the rest wait for it and re-verify); `concurrent_unknown_kid_requests_share_one_refresh` sends twelve threads and asserts twelve accepted, one refresh, nothing suppressed, under ThreadSanitizer in CI |
| Contract suite against FastAPI | **Done.** Both repositories were available (the Punchline repository holds the TimeClock prototype at its root, the FastAPI backend under `backend/`, and the rewritten client under `client/`). `tests/contract/scenarios.json` runs against the prototype with pytest and against Archivum from `server_contract_test`; eight scenarios pass on both, with `employee_id` in the body recorded as the one deliberate difference. Section 3 |
| Corrections, self-service with approver visibility and flag, supervisor queue, period audit report, segregation-of-duties report, CSV export | **Done** (from the fold-in list and section 7 of punchline-updates.md); the audit report was built now rather than as a reporting afterthought |
| Not built | Return-to-employee and retroactive adjustments against a locked period, pay-code computation and rounding, notifications: Stage 8-P with the payroll console. Rate limiting by client address. The client journal integration is Punchline-repository work against this contract |

## 3. The contract suite

The prototype's contract (`POST /api/v1/timesheets`, completed shifts
with a self-asserted `employee_id`, `company` and `cost_center` under a
shared bearer token) and Archivum's (`POST /api/v1/device/sync`,
punches under a per-device credential, the employee derived) cannot be
byte-identical, as punchline-updates.md section 10 says. What can be
identical is proven by one file both runners read: a valid batch
accepted; replay idempotent with nothing duplicated; one bad entry does
not sink the batch and is named; a duplicate uuid in one batch stores
one row; a shift over the limit is named, not stored silently (the
prototype rejects with a reason, Archivum accepts and opens `long_shift`
for the supervisor, and the file records both as acceptable); a
malformed uuid rejected by name with the uuid echoed as sent; an
unauthenticated batch 401; and `employee_id` in the body accepted by the
prototype and 400 from Archivum, recorded so the difference is proven.

Run against the prototype from the checked-out Punchline repository
(commit f294956, `backend/` at its `requirements-dev.txt`): 8 passed in
0.84 s. Run against Archivum in `server_contract_test`: 8 scenarios, 0
failures, in every CI job below.

Two things the suite surfaced in the prototype's contract that the
handbook should carry: the prototype ignores unknown keys
(`extra="ignore"`), Archivum refuses them; and the prototype's
per-device idempotency is by shift uuid only, so a client whose journal
reissues a sequence number under a new uuid duplicates a shift there and
is refused by name here.

## 4. Figures

CI run [29](https://github.com/l-m-graves/Archivum/actions/runs/35633803162), seconds (the crash-point count is
the local Debug run's; the crash schedule is seeded, so CI walks the same
852):

| Test | Linux Debug (ASan+UBSan) | Linux Release | Linux TSan | Windows Debug | Windows Release |
|---|---|---|---|---|---|
| concurrency_test (5 tests, incl. the salts) | 2.03 | 0.12 | 4.68 | 1.52 | 0.52 |
| punchline_rules_test (8) | 0.29 | 0.02 | | 0.14 | 0.03 |
| punchline_sync_test (4) | 0.52 | 0.04 | | 0.26 | 0.05 |
| punchline_migration_test (852 crash points) | 40.60 | 2.50 | | 38.75 | 2.48 |
| server_integration_test (12, incl. the concurrent refresh) | 5.39 | 4.91 | 6.60 | 8.84 | 5.52 |
| server_core_test (7; 18-endpoint contract) | 15.73 | 8.14 | 57.03 | 20.85 | 8.90 |
| server_punchline_test (9) | 15.85 | 4.94 | 74.52 | 16.17 | 5.83 |
| server_contract_test (8 scenarios) | 14.36 | 1.56 | 83.36 | 10.03 | 2.39 |
| whole suite (26 tests; TSan: the 9 labelled) | 165.9 | 47.8 | 239.5 | 149.6 | 56.3 |

## 5. The Windows-only threaded surface

Everything `#ifdef _WIN32` or Windows-only in the tree, and which threads
reach it:

| Code | Size | Threads that reach it | Shared state | Race detection |
|---|---|---|---|---|
| `engine/src/vfs_win32.cpp` (the Win32 VFS: `CreateFileW`, `ReadFile`/`WriteFile` at explicit `OVERLAPPED` offsets, `FlushFileBuffers`, `SetFilePointerEx`+`SetEndOfFile` for truncate, `MoveFileExW`, `DeleteFileW`, `FindFirstFileW`) | 235 lines | every Drogon IO thread (requests), the archive shipper, the Punchline monitor, checkpoints on whichever thread commits | none of ours: one `HANDLE` per `File`, no statics, no caches. Reads and writes carry their offset (no shared file position); truncate's `SetFilePointerEx` does move the handle's position, but every `File` is used under the `Db`'s own locks, as on POSIX | none on Windows. The same code paths run under the Linux VFS with TSan; the Win32 file is 1.4× the POSIX file's size and structurally parallel |
| `server/src/log.cpp`: `gmtime_s` | 1 line | every thread that logs | none (reentrant, the caller's `tm`) | covered by inspection: the POSIX branch is `gmtime_r`, the same contract |
| `server/src/app.cpp`: `_mkgmtime` | 1 line | the thread that reads the certificate at startup | none | single-threaded |
| `tests/server/server_fixture.h`: `_getpid`; `tests/cli/cli_test.cpp`: `std::system` quoting | test code | test main thread | none | not shipped |
| Third party under `_WIN32`: trantor's event loop (wepoll), Drogon, OpenSSL, libsodium | not ours | IO threads | theirs | not ours to instrument; the Linux TSan job instruments the same libraries' POSIX builds |

So the Windows-only threaded surface Archivum owns is one file of 235
lines with no shared state, plus two one-line reentrant calls. That is
as small as it can be while the VFS is a platform boundary, and nothing
above it is platform-specific. What the Windows jobs do check is the
logic: the same concurrency tests run there and catch ordering and
consistency failures through their assertions, not data races.

## 6. The v1 release boundary

The proposal from the v3 rulings (section 5) is `docs/plan-v1.md`
section 1; this restates it against what now exists.

**v1 is Punchline in production**, with the read-only SQL engine, the
query builder and the SQL editor in v1.1 (the pushback in plan-v1: no
Punchline production need for ad hoc SQL, seven to ten weeks saved, and
the analyst role and dataset permissions out of the v1 audit scope). The
two items the ruling kept in v1 despite the line are in: the
two-database pager design (Stage 2) and freshness monitoring on device
sync (this stage).

**What is in v1 and done**: Stages 0 to 6, the engine, the server core
and the Punchline module with the approval lifecycle. **What remains for
v1**: Stage 8-P, the Punchline web views (employee self-service,
supervisor queue, exception queue, payroll console with the actions this
stage did not build: return-to-employee, retroactive adjustment against a
locked period, pay-code computation and rounding, notification), the
pilot and the cutover; Stage 11, the Windows service installer, the
Linux container, the IT handbook with the accepted revocation-latency
bound, upgrade and rollback, the monthly restore procedure. Figures in
plan-v1 stand: 4 to 6 weeks and 2 to 3 weeks.

**The v1 gates**, with where each stands:

| Gate | Now |
|---|---|
| Crash-injection, model-based, randomized and concurrency suites green on both platforms and on the exact Windows build deployed | green on every push (six jobs); "the exact Windows build" is Stage 11's installer |
| Backups and archived logs shipping off host on a schedule; restore-and-verify in CI and against real pilot data | shipping, verified per copy, health failing loudly; the CI restore-and-verify exists; pilot data is the pilot's |
| Contract suite identical between the FastAPI server and Archivum, idempotent replay verified, every endpoint rejecting an employee id in the body | this stage, section 3 |
| No shared token; every device enrolled individually with revocation tested end to end | Stage 5, tested |
| Audit trail verified for every mutating endpoint and every lifecycle transition; the period audit report producible | this stage: every mutating route writes through the Recorder, every transition an approvals row, the report is `GET /api/v1/payroll/periods/{id}/audit` |
| Segregation-of-duties report shows nobody holding both payroll and supervisor | `GET /api/v1/admin/roles/conflicts` |
| Certificate expiry in health; off-host archive configured or health fails; break-glass exercised | Stages 1 and 5 |
| Rollback rehearsed; integrity check scheduled; IT handbook complete | Stage 11 |

**v1 closes** when the FastAPI cutover is complete and the old store is
retired at the end of the rollback window. The Q8 answers (destination,
cadences) and the section 11 answers (calendar, cutoffs, thresholds,
zones) are configuration in place with placeholders; none is structure.

## 7. Open

- The Q8 and section 11 answers, as above.
- A check that `device_time` and `local_time` agree under `site_zone`
  needs a tz database on the server (v1.1; the values are stored for it).
- The prototype ignores unknown keys and dedupes by uuid only (section
  3): two behaviours the pilot devices will meet as a difference.
- The Windows blind spot is structural and now measured (section 5).
