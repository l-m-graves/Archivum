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
| **Day assignment from the instant and the site zone, never from the device's clock** (Stage 6 rulings, item 1): the tz database embedded in the binary, the server computing local time from `device_time` and `site_zone`, the device's wall clock kept and compared with an exception on disagreement, `tzdb_version` naming the embedded database, DST transition tests including the ambiguous hour | option B approved and built up to the data: reader, generator, build-time zdump assertion and tests are in (section 9); the IANA release has not arrived, so the sync path is not yet switched. No payroll export may run on real data before it |
| **Rollback rehearsed with the reverse export** (item 3c): `archivum rollback-export` run against real pilot data, replayed into the old server, the old server serving it | the export, its tests and the replay into the FastAPI backend are in (section 9); the run against real pilot data is the pilot's |
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

## 8. Rulings on this report (2026-09-22), applied

### Item 1, day assignment: the proposal, not yet built

Agreed that the current design records the error and never catches it.
Two ways to embed the IANA database, with what could and could not be
verified from this environment (the egress proxy returns nothing for
`data.iana.org` and `learn.microsoft.com`, and 403 for
`github.com/eggert/tz`):

| | A: Howard Hinnant's `date` library, vcpkg port | B: vendored IANA tzdata in compiled (TZif) form, own reader |
|---|---|---|
| What it is | `date` 3.0.5 at the pinned vcpkg baseline; the port manifest (`ports/date/vcpkg.json` at commit 1577f17) says `"license": "MIT"`. I read the manifest, not the library's LICENSE file, which I could not fetch | The IANA release tarball (public domain; the Ubuntu package's copyright file for tzdata 2025b, `/usr/share/doc/tzdata/copyright`, states "This database is in the public domain"), compiled with `zic` into RFC 8536 TZif files, embedded as byte arrays, read by ~400 lines of ours (header, 64-bit transitions, local time types, and the footer TZ rule for instants past the last transition) |
| Still needs tzdata? | Yes: its `tz.cpp` parses the tzdata text files from a directory at runtime, or the OS database on Linux (`USE_OS_TZDB`, which the self-sufficiency rule forbids), or downloads them (the `remote-api` feature, curl). On Windows there is no OS database. So A vendors tzdata anyway and adds a third-party parser on top | Yes, that is the whole of it |
| Self-sufficiency | A library, statically linked: allowed. But its runtime file access would need patching to read embedded data | Data in the binary; no runtime files, no download |
| Verification we can do here | None beyond the manifest | `zic` and `zdump` are on the build host: every embedded zone can be cross-checked against `zdump -v` output at generation time, and the tests pin the 2026 transitions of the site zone (`zdump`: `America/Los_Angeles` 2026-03-08 09:59:59 UT = 01:59:59 PST, 10:00:00 UT = 03:00:00 PDT; 2026-11-01 08:59:59 UT = 01:59:59 PDT, 09:00:00 UT = 01:00:00 PST) |

**Recommendation: B.** The reader is small, the format is an RFC, the
data is public domain, the build needs nothing at runtime, and every
zone can be checked against `zdump` when it is generated. A buys a
parser we would still have to feed and patch.

What B needs from you, under the evidence rule: the release tarball
(`tzdata<release>.tar.gz`, from `https://data.iana.org/time-zones/releases/`)
attached to the repository, or egress to that host, so the vendored copy
is the IANA file with its published SHA-256, not a distribution's copy.
Until then the only copy here is Ubuntu's package `tzdata
2025b-0ubuntu0.24.04.1` (`tzdata.zi` SHA-256
`a7b113737e97a9b58f9f040a1b1e7e3af6e800649b1c381b601a9c69d425dc70`,
598 zone and link lines, `# version 2025b`), which is the same data by
the package's own statement but not the upstream artifact.

Design once approved: `punchline::localtime` gains `local_of(instant,
zone)` and `instants_of(local, zone)` (zero, one or two instants: the
gap and the fold); day assignment, pairing, schedules and the checks
take the server's local time; the device's `local_time` is kept and
compared, and a difference beyond `local_clock_tolerance_seconds`
(configuration) opens `local_clock_mismatch` naming the device;
`tzdb_version` is stamped with the embedded release; tests at both 2026
transitions for the site zone including the ambiguous hour, asserting the
instant resolves it.

### Item 2, Windows rename durability: done

- `docs/durability.md` has a "Renames and directories" section: POSIX
  rename plus directory fsync; Windows `MoveFileExW` with
  `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`, which the code
  has used since Stage 0 for every rename, local archive and destination
  alike (`engine/src/vfs_win32.cpp`). The Win32 `sync_directory` comment
  and `docs/server-core.md` no longer say "no-op by design"; they say
  the write-through flag is the mechanism and the directory call has
  nothing left to do.
- The Microsoft reference page is cited by URL with its wording quoted
  from memory and marked **unverified**: I could not fetch it. Someone
  who can read it must confirm the quotation before the handbook cites
  it.
- The SMB share: the same section states what the shipper proves (the
  complete bytes readable under the final name after the rename, from
  this client, at that moment) and what it cannot (the server's own
  commitment to stable storage, which no client call can observe), and
  what to do about it (a journaling file server, and the monthly restore
  run from the destination).
- Verification now runs **after** the final rename, on the file under
  its final name, for segments and backups alike; a mismatch removes the
  file so the next pass copies it again. Found while testing it: a
  fresh shipper at an unchanged change counter would have rewritten an
  existing, verified backup under the same name; a backup whose final
  name already exists is now counted and left alone. Test:
  `server_archive_test`, with a VFS whose renames truncate or flip a byte
  in the destination.

### Item 3, the contract differences against the real client

**Unknown keys.** The deployed TimeClock client builds one request
shape, in one function (`src/sync.cpp`, `BuildPayload`), used for every
batch including retries; error and retry paths change scheduling, not
the request. It sends, and only ever sends: top level `client_version`,
`device_id`, `submitted_at`, `entries`; per entry `uuid`,
`employee_id`, `employee_name`, `company`, `cost_center`, `clock_in`,
`clock_out`, `minutes`, `note`; header `Authorization: Bearer <shared
token>` to `POST /api/v1/timesheets`.

That is where the design assumption breaks, and it needs saying plainly:
**the deployed client cannot talk to Archivum "unchanged apart from
auth"**. Its shape is a completed shift with a self-asserted employee
id; Archivum's is punches under a device credential with the employee
derived, and `employee_id` in every entry is refused by ruling. Three of
its four top-level keys and eight of its nine entry keys are not in
Archivum's allow-list, and the one that must not be there always is. So
the allow-list cannot cover this client, and the parallel run cannot use
it. What the parallel run needs is the rewritten client's sync layer,
which the Punchline repository does not yet have (`client/` holds the
journal and the verification layer only). Its keys are the contract in
`docs/punchline-module.md`, and a test in `server_punchline_test` now
sends every one of them in one batch (`batch_uuid`, `journal_id`,
`client_time_us`, every entry key including `pay_code` and `note`, and
`reports`) and is accepted. The assertion that the rewritten client
sends nothing outside that list belongs beside that client when it is
written, and I will hold the contract file to it.

**Reissued sequence numbers.** Refusal is per entry: `apply_batch`
rejects the entry with its uuid and a reason and accepts the rest, and
only a body that is not attributable to an entry (`entries` not an
array, an entry not an object, more than `max_batch_entries`) refuses
the request. A resend gets the same per-entry refusal every time.

What the current client does with a refusal (`src/sync.cpp`,
`src/db.cpp`): `MarkFailed` sets `next_attempt_at` an hour ahead and
increments `attempts`; `ClaimPending` has no attempt cap; the UI shows
"N entries were rejected, will retry". So a refused entry is never
dropped, but it is retried hourly forever and surfaces nowhere but the
device's status line. The same is true against the prototype server
today. Archivum now closes the server half of that: any batch that
refuses an entry opens `entry_rejected` for the device with the refused
uuids and reasons, replaced when the list changes and never duplicated
by the hourly resend; payroll and admin see it. The client half, a
retry cap that reports `retry_exhausted` through the batch's `reports`,
is the rewritten client's work and the contract already carries the
report.

**Scenarios to endpoints.** The client uses two endpoints; the coverage
is by endpoint, not by count:

| Endpoint the client calls | Scenarios (`tests/contract/scenarios.json`) | Other tests |
|---|---|---|
| Prototype: `POST /api/v1/timesheets` (the only endpoint it calls) | all eight, via the pytest runner | the prototype's own suite |
| Archivum: `POST /api/v1/device/sync` | all eight, via `server_contract_test` | `server_punchline_test` (idempotency, per-entry outcomes, every key, tamper signal, late punches), `punchline_sync_test` (the module underneath) |
| Archivum: `POST /api/v1/device/heartbeat` | none: the prototype has no equivalent | `server_core_test` (credential, revocation, the employee_id contract), `server_punchline_test` (freshness restored, `device_stale` resolved) |

### Item 4, late punches after approval: confirmed and tightened

A late `out` was already flagged `unpaired_punch`; a late `in` that
paired normally was not flagged at all and showed up only as a 409 when
payroll tried to release. Now any punch accepted into a period where
the employee's entries are already past `recorded` opens `late_punch`
(employee, entry, period), visible in the supervisor's queue, and the
re-approval of that employee's period closes it. Tested at the module
level and over HTTP (the supervisor lists it with the period id; it is
closed after the override approval).

### Item 5, the format is frozen from the pilot

`docs/page-format.md` now carries the rule: any later change to the
file, log, segment, backup or catalog layout bumps the version and ships
with an upgrade path, a downgrade or rollback story, and a test that
opens a fixture written by the previous version. A change without all
three is a defect.

### Item 6

The day-assignment work is in the gates table above. Q8 and section 11
answers blocking at the pilot, noted.

## 9. Second rulings on this report (2026-09-22), applied

### The IANA release did not arrive

The message said the tarball and its detached signature were attached.
Nothing reached this environment: the upload directory holds only the
earlier documents, the attachment mount is empty, and the branch carries
no such file. Per the ruling I have not substituted a distribution's
copy. Everything that does not depend on the data is built and tested;
the two steps that do are listed at the end of this section. When the
tarball is committed to `third_party/tzdata/` (the README there says
exactly what and where), the remaining work is one commit.

### Item 1, option B: built up to the data

| Piece | Where | Proof |
|---|---|---|
| TZif reader: versions 1 to 4, the 64-bit block, the footer POSIX rule (Mm.w.d, Jn, n; quoted names; negative and over-24-hour times), `local_of`, `instants_of` (none in the gap, two in the fold), `transitions` | `modules/punchline/{include/archivum/punchline/tzif.h,src/tzif.cpp}` | `tzif_test`: five tests on fixtures built in the test, including the 2026 Pacific transitions at 2026-03-08 10:00:00 UT and 2026-11-01 09:00:00 UT (definitional under the rule and equal to what zdump printed for the host's data), the 02:30 gap resolving to no instant, the 01:30 fold resolving to two instants an hour apart with the instant deciding, a southern-hemisphere rule spanning the new year, Jn skipping Feb 29, and corrupt input refused |
| Generator: SHA-256 gate, `version` and `tzdata.zi` required, `zic -b slim`, every zone embedded as a byte array, zdump-pinned transitions for the pinned zones, deterministic | `tools/tzdata/generate.py`, `tools/tzdata/pinned-zones.txt` | `tzdata_generate_test` (Linux): regenerates a synthetic two-zone release checked in under `tests/tzdata/` and diffs byte for byte; the wrong SHA-256 is refused |
| Build-time assertion: `tzcheck` runs after every build on every platform and fails it if the embedded bytes disagree with any pinned zdump transition; reports the placeholder loudly | `tools/tzdata/tzcheck.cpp`, `modules/punchline/CMakeLists.txt` | `tzcheck_synthetic_test`: the tool against zic's output for the synthetic release with 22 zdump-pinned transitions, on Linux and Windows |
| Pinned zones | `tools/tzdata/pinned-zones.txt` | `America/Los_Angeles`, `America/New_York`, `UTC`, `Europe/London`, `Australia/Sydney` over 2020 to 2031 until the section 11 "site time zones" answer replaces the list with every deployed zone |

Not switched yet, because it needs the database to test: the sync path
still takes the device's wall clock for day assignment. The switch is:
`apply_batch` and `record_manual_entry` compute local time from
`device_time` and the employee's `site_zone` through `tz::zone`, compare
it with the device's `local_time`, open `local_clock_mismatch` beyond
`local_clock_tolerance_seconds`, and stamp `tzdb_version` from
`tz::database_version()`; the schedule and pairing checks take the
server's local time; `ARCHIVUM_TZDATA_ALLOW_EMPTY` goes OFF so a build
without the database fails. Tests at both transitions of every pinned
site zone including the fold, through sync. One commit, with the data.

### Item 2: rewritten

`docs/durability.md` now says what is documented: `MOVEFILE_WRITE_THROUGH`
flushes a cross-volume (copy-and-delete) move before returning; a
same-volume rename is atomic with no documented durability guarantee,
and NTFS metadata journaling is stated as the reason it survives in
practice, not as a promise. The durability argument for the archive is
the idempotent, verifying re-ship: a rename lost to a crash leaves the
segment absent and the next pass copies and verifies it again, on any
file system including SMB. The unverified marker is gone; the code
comments and `docs/server-core.md` say the same.

### Item 3

- **3a**: Stage 6-C is in `docs/plan-v1.md` with its scope and an
  estimate of 3 to 5 weeks, broken down; the v1 total is 38 to 58 weeks.
- **3b, confirmed and tested**: `employees` now carries `company` and
  `cost_center` (server-owned, admin-set); the sync endpoint refuses
  `employee_name`, `company`, `cost_center` and `minutes` per entry by
  name, the rest of the batch standing, and the refusal is queued as
  `entry_rejected`. Minutes are computed from the punches at pairing;
  nothing a client sends reaches a pay figure.
- **3c, built**: `archivum rollback-export --db --period --to
  [--released-only]` writes the old server's ingest batches (section
  "Rollback" of `docs/punchline-module.md`). Proven by the module test,
  the CLI round trip, and `tests/contract/test_rollback.py`, which
  replays the CLI's own output (checked in as the fixture) into the
  FastAPI backend and asserts every entry is served back with the same
  values and that a second replay stores nothing twice: 9 of 9 in the
  contract directory against the checked-out Punchline repository. Added
  to the v1 gates with the phased-by-device parallel run and no
  dual-write.

### What remains for item 1, in order

1. The IANA release tarball and `.asc` in `third_party/tzdata/`, with the
   release name, SHA-256 and the signature verification result recorded
   in `docs/toolchain.md` (I will record what `gpg --verify` prints once
   the files and IANA's public key are here; the key is not in this
   environment either).
2. `generate.py` run, `ARCHIVUM_TZDATA_ALLOW_EMPTY` OFF, the sync path
   switched, the site-zone transition tests through sync, and the gate
   line in section 6 updated. Then Stage 8-P.

## 10. Third rulings (2026-09-27), applied; Stage 6-C delivered

### The tarball, still

The message said the release tarballs (`tzdata2026d`, `tzcode2026d`,
each with its `.asc`) would be committed with the next message. Nothing
under `third_party/tzdata/` has arrived in this environment as of this
section. I have not substituted a distribution copy or fetched anything:
`data.iana.org` and the tz mirror on GitHub are unreachable from here,
and the ruling says not to. The pins are scoped to 2026d
(`tools/tzdata/pinned-zones.txt`), so the generator will accept exactly
that release and refuse any other without `--new-release`. When the four
files land, the run is the one in `third_party/tzdata/README.md`, and it
records both SHA-256 values, the key fingerprint from the announcement,
and `gpg --verify`'s answer in `docs/toolchain.md`.

### Item 1, release-scoped pins: done

- `pinned-zones.txt` carries `release 2026d`; the generated pin file
  carries `# release <name>`; `tzcheck` fails the build when the embedded
  database's version is not the pins' release, before it reads a
  transition. Proven by `tzcheck_synthetic_wrong_release_test` (pins
  scoped to 2000b against the embedded synthetic 2000a: refused by name).
- Changing the release is explicit: the generator refuses another
  release's tarball unless run with `--new-release`, then rewrites the
  release line, regenerates the pins and prints the transition diff
  between old and new pins; the commit carries the diff for review.
  Pins that would change without a release change are refused as a tool
  difference or a non-determinism. `tzdata_generate_test` proves the
  refusal, the rewrite, and the drift refusal.
- `zic` and `zdump` come from the tzcode tarball of the same release:
  `--tzcode` extracts it, checks its `version` against the data
  tarball's, runs `make zic zdump`, and requires the built `zic` to
  report the release. The host's tools are accepted only with
  `--host-tools`, which the synthetic test uses, and the output records
  which was used (without the host tool's version string, so
  regeneration stays byte-identical across hosts). The tzcode path is
  proven against a synthetic tzcode tarball whose Makefile builds
  wrappers that report the release; a tzcode of another release is
  refused. The real `make` on the real tzcode runs when the tarball is
  here.
- Signatures: `--signature` and `--fingerprint` run `gpg --verify` and
  require `VALIDSIG` by exactly the announced key; "valid under some key"
  is refused. Proven with a throwaway key in a temporary keyring: the
  right fingerprint passes, another is refused by name.
- The statement that tzdb models legal intent and sometimes lags it
  (Alberta's permanent −06 effective June 2026, modelled at 1 November
  2026 like BC in 2026b), and why `tzdb_version` is load-bearing, is in
  `docs/punchline-module.md` under "Local time".

### Item 3, rollback limitation: recorded

`docs/handbook.md` is the seed of the IT handbook (Stage 11 completes
it), and its cutover-and-rollback section states it: the old store has
no schema for approval state, exception records or device attestation;
rollback preserves hours, not the approval trail; the trail stays in
Archivum's store, archived logs and backups, which a rollback plan
retains and names. The same statement is in the module doc's "Rollback"
and in the v1 gate.

### Item 2, Stage 6-C: delivered on the branch `claude/stage-6c-sync-layer` of the Punchline repository

Everything the plan row lists is built and tested; the Punchline
repository's `docs/client-sync.md` is the design document.

| Scope item | Where | Proof |
|---|---|---|
| Per-device credential in place of the shared token | `client/sync/credential.h`: `Device <uuid>:<secret>` as Archivum issues it, in the DPAPI store on Windows; enrollment saves, heartbeats, and clears on refusal | `credential_*` tests; end-to-end steps 1 and 5 (wrong credential refused and nothing kept; revoked device refused, punch kept on the device) |
| Employee identity dropped from the payload | `punch.h`: the journal payload and the wire entry have no field for it | `payload_punch_round_trips_...` scans the encoding for "employee"; the server model answers 400 to an `employee_id` anywhere and the crash suite asserts it never happened; end-to-end step 6 greps the server log |
| The journal driving the batch shape, `(journal id, sequence)` idempotency, acknowledgement-driven compaction | `sync_client.h`: batches from pending records, `journal_sequence` from the record, acknowledgement of accepted and rejected entries, resend of unmentioned ones, `Compact` after `compact_after_acks` | `sync_*` tests; the crash suite's sequence-conflict and drain assertions; end-to-end step 4 (the same punch resent from a restored journal: accepted again, no second row) |
| Three time values and the site zone from the heartbeat | `clock.h` (`device_time_us`, `local_time` as the OS shows it), `site_zone` cached from the heartbeat and refreshed | `sync_heartbeat_updates_the_enrollment`; end-to-end step 2 |
| Device-attested marking left to the server | nothing in the client claims it | timesheet read-back in end-to-end step 3: `attestation: device` |
| Bounded retry with backoff, cap reporting `retry_exhausted` and `journal_recovery` | `sync_client.cpp`: offline never burns an attempt; 5xx, garbage and empty answers do; the cap writes the report into the journal and it rides the next batch | `sync_offline_backs_off...`, `sync_retry_cap_reports...`, `sync_garbage_answers...`, `sync_recovery_report_rides...`; the crash suite asserts every exhaustion and every recovery with loss reached the server |
| Enrollment screen taking the one-time credential into the DPAPI store | `client/app/main.cpp`: enrollment window (server URL, credential, PIN twice), punch window | compiled by the Windows CI job; not runnable here |
| Crash suite over journal plus sync | `client/sync/tests/test_crash_sync.cpp` with `testing/fake_server.h` | eight seeds under ctest, 20,000 operations under one seed in CI; a 20,000-operation local run: 8,922 punches, 2,458 crashes, 440 lost answers, 359 server errors, 582 rejections, everything delivered exactly as the model says |
| Contract suite from the real client against Archivum | `tests/e2e/client_e2e.sh` here, `tests/e2e/e2e_server.cpp`, `punchline-cli` there | passes locally (transcript below); a token-gated step of the Linux release CI job, like `contract-fastapi` |

The end-to-end run, as it prints:

```
e2e: 1. wrong credential refused, nothing kept
e2e: 2. enrolled; heartbeat named E0001 in America/Los_Angeles
timesheet: 2 device-attested entries, 1 closed shift, journal sequences 1 and 2
e2e: 3. punches delivered and on the timesheet
e2e: 4. the same punch resent from the restored journal: accepted again, 3 rows not 4
e2e: 4b. the heartbeat says the employee is clocked in by the server's fold
e2e: 5. revoked device refused; the punch stays on the device
e2e: 6. 3 batches logged, no employee id asserted
client_e2e: PASS
```

**What the run found.** The first real batch from the rewritten client
was refused by Archivum: "tzdb_version is required". The sync path
required a release name from the device, while the ruling says the
release is the server's and a client's opinion of it is ignored. Fixed
on the server: `site_zone` and `tzdb_version` were made optional from a
device, and an entry without them took the employee record's zone and the
embedded release. That fix was wrong in one respect and is corrected in
section 12: it still stored a device's value when one was sent. The contract doc is corrected. This is the kind of disagreement the
end-to-end gate exists to find, and it was not visible from either
side's own tests.

**One server addition**: the heartbeat answers `clocked_in`, whether the
employee has an open shift by the server's fold, so the punch screen
shows the right button after a restart with nothing pending on the
device. The device's own last punch wins while anything is pending.

**Not in 6-C, by the plan**: the server's comparison of the device's
`local_time` with its own computation (`local_clock_mismatch`) lands with
the switch commit; the old client's SQLite queue is not migrated
(cutover by device, old queue drained into the old server first, no
dual-write).

**Estimate against actual.** The row said 3 to 5 weeks; the work here is
one session, because the journal and the verifier were already built and
tested, the contract was already pinned by the scenario file, and the
Win32 screens are a straight rewrite of the prototype's. The Windows
build of the screens and of WinHTTP is verified by CI, not by hand, and
that is the part that could still cost time.

**Branch and merge.** The client is on `claude/stage-6c-sync-layer` of
the Punchline repository (I did not push to `main` there; the branch
rule in this session names only Archivum). Archivum's CI step checks
out that branch by name (`PUNCHLINE_CLIENT_REF`) and moves to `main`
when it is merged.

## 11. Stage 8-P, the plan (draft, to be confirmed once item 1's switch has landed)

Per your item 5, the Stage 8-P report is due with the switch commit. The
plan is drafted here so that nothing waits on the tarball but the
switch itself.

**Scope (plan row 8-P, 4 to 6 weeks)**: the Punchline web views, served
by Archivum itself (no separate front-end host; static files under the
server's own TLS, Entra sign-in through the existing OIDC bearer):

1. **Employee self-service** (`/api/v1/me`, `/timesheets/{period}/me`,
   `/me/flag`): my punches and shifts by day, my approver, open items,
   the flag-to-approver form. 0.5 week.
2. **Supervisor queue** (`/supervisor/periods/{id}`, approve, the
   exception list scoped to my reports, resolve and dismiss, manual entry
   with reason): the daily screen. 1 to 1.5 weeks.
3. **Exception queue** across kinds with the filters the module already
   answers (`?kind=`), device items to payroll and admin. 0.5 week.
4. **Payroll console** (release, lock, the hours-by-employee-by-pay-code
   view, CSV export, the period audit report). 1 week.
5. **Admin**: employees with company and cost centre, device enrollment
   showing the credential once, revocation with reason, supervisor
   assignments with the half-open intervals, schedules, pay periods,
   the segregation-of-duties report. 1 week.
6. **Cross-cutting**: one page shell, sign-in state, role-driven
   navigation from the roles the token carries, request ids surfaced on
   every error, a browser test run (Playwright in CI against the test
   fixture, the same `ServerFixture` the e2e server uses) for the
   approve and release flows. 0.5 to 1 week.

**Rulings I will ask for at the start of 8-P**, so they are on the table
now: whether the views are served by Archivum (my proposal) or hosted
elsewhere; whether a supervisor may approve from the exception queue
directly or only from the period screen (my proposal: only from the
period screen, so the approval trail has one entry point); and the CSV
export's exact column set for the payroll system, which section 11's
answer on pay codes decides.

## 12. Fourth rulings (2026-09-30): zone authority, gates that cannot skip

### The site zone and the release stamp: what the code did, and what it does now

You asked for what the code does, not what it should. Before this
change, on the day-assignment path and the stamps:

| Question | The code before this change |
|---|---|
| Where did the day assignment's local day come from? | The `local_time` string the **device** sent. The site zone was not consulted at all: `period_for_day`, the schedule checks and the pairing's local day all read it. This is the device-clock problem itself and is the switch commit's job; it is not fixed by this change |
| Which `site_zone` was stored on a synced entry? | **The device's, when it sent one**; the employee record's only when the field was absent. Last session's fix did exactly the thing you describe: the device became a source of truth whenever it spoke |
| Which `tzdb_version` was stored on a synced entry? | **The device's, when it sent one**; the embedded database's only when absent |
| Manual entries and pay periods | Both required `site_zone` and `tzdb_version` from the **request body** (a supervisor's or payroll's, not a device's) and stored them as given; the period's `tzdb_version` likewise |
| Was a device's zone ever compared? | No. `local_clock_mismatch` did not exist in the schema at all, only in the design text |

What it does now (commit follows this section):

- **Synced entry**: `site_zone` is the employee record's and `tzdb_version`
  is `release_stamp()` (the embedded release, or the literal `unavailable`
  when none is embedded), always. A device-supplied `tzdb_version` is
  discarded. A device-supplied `site_zone` is compared with the employee
  record's: a difference opens `local_clock_mismatch` naming both values
  and stores nothing from the device; a later batch whose claim agrees
  closes it. Both keys are still accepted on the wire, as tolerance, and
  have no other effect. `local_clock_mismatch` is now a known exception
  kind.
- **Manual entry**: `site_zone` and `tzdb_version` are no longer request
  fields (400 by name); the zone is the employee record's and the release
  is the embedded one.
- **Pay period**: `tzdb_version` is no longer a request field; the
  period's `site_zone` is still an administrator's setting (it is the
  site's own record, not a device's claim), and the release is stamped.
- Tests: `sync_stamps_zone_and_release_from_the_server_never_from_the_device`
  (no claim; a disagreeing zone with a made-up release, both discarded and
  the mismatch raised once; an agreeing claim closes it), and the route
  refusals in `server_punchline_test`. The end-to-end run, whose client
  sends its heartbeat's zone, still passes and raises nothing.
- **Still the device's, until the switch commit**: the local day. So the
  stamp is now trustworthy and the day assignment under it is not. The
  switch changes where the local day comes from and nothing else.
- An unset employee zone cannot occur: `site_zone` is required and
  non-empty on every employee (schema check and route). There is no
  fallback to a device value anywhere.

### Gates that do not skip

- Both token-gated steps (contract-fastapi, and the end-to-end step in
  linux-release) now **fail** when `PUNCHLINE_REPO_TOKEN` is empty, with an
  error annotation saying the gate was not run. Whether the secret is
  present on the repository is something I cannot read from here; the next
  run answers it, and a failure on "Gate not run" means it is not there
  yet.
- The contract pytest modules now raise, rather than skip, when
  `PUNCHLINE_REPO` is unset and `CI` is set. A local run without it still
  skips.
- The v1 gate list in `docs/plan-v1.md` now has a table saying, for each
  gate, the repository, workflow and job that proves it and whether it has
  run. The two token-gated gates are recorded **unverified in CI**, not
  passing.

### Every place CI can be green without having run the check

Archivum, `ci.yml`:

| Where | Condition | What is not run while green |
|---|---|---|
| contract-fastapi, end-to-end step | token empty | **Fixed**: fails. Was: exit 0 |
| End-to-end step | `if: matrix.preset == 'linux-release'` | Absent from linux-debug, linux-tsan and both Windows jobs, by design: one Linux run is the gate |
| `tzcheck` after every build | `ARCHIVUM_TZDATA_ALLOW_EMPTY` defaults ON (`modules/punchline/CMakeLists.txt`) | With no database embedded it prints that none is embedded and exits 0, in every job. The embedded-release check and every pinned transition are skipped until the switch commit turns this OFF |
| `tzdata_generate_test` | `if(NOT WIN32)` | Not run on windows-debug or windows-release. On Linux it needs `sh`, `python3`, `gpg`, `make`; a missing `gpg` fails it (it named the failure in run 35) |
| linux-tsan | test preset filter `label: concurrency` | Only `db_test`, `store_test`, `backup_test`, `concurrency_test` and the server tests run under ThreadSanitizer; `cli_test`, the crash tests, the tz tests and the rules and sync unit tests do not |
| Crash and model tests | `ARCHIVUM_CRASH_ITERS`: 1000 debug, 200 tsan, 10000 release | The large counts run only in linux-release. Windows uses its own default |
| Contract pytest modules | `PUNCHLINE_REPO` unset | **Fixed in CI**: raises when `CI` is set. A local run skips |
| vcpkg binary cache save | `if: always() && cache-hit != 'true'` | Skipped on a cache hit; not a check |

Punchline, `client.yml` (the only workflow in that repository):

| Where | Condition | What is not run while green |
|---|---|---|
| Workflow trigger | `paths: client/**` and the workflow file | Nothing runs for a change to `backend/` or to the prototype in the root. **No workflow in the Punchline repository runs the FastAPI backend's own tests (`backend/tests`) or builds the prototype.** The backend is exercised only by Archivum's contract-fastapi job |
| windows-msvc | `build: --config Debug`, `test: -C Debug` | No Release build of the client on Windows and no sanitizers there; Release runs on Linux only |
| `WinHttpTransport`, the Win32 app (`client/app`) | Windows-only sources | **Compiled by windows-msvc, never exercised by any test.** The only transport any test runs is the fake server model and, in the end-to-end step, the OpenSSL one |
| DPAPI store | `#ifdef _WIN32` in `test_verify.cpp` | Tested on Windows only, and only on its own; the enrollment record's round trip is tested on `MemorySecretStore`, so enrollment under DPAPI is untested anywhere |
| `punchline_verify`, `punchline_sync`, `punchline-cli` targets | `return()` when libsodium is absent (CMake) | CI sets `PUNCHLINE_REQUIRE_SODIUM=ON`, which makes absence a configure error, not a skip. A local build without libsodium skips them silently |
| OpenSSL transport | `find_package(OpenSSL QUIET)` | Missing OpenSSL on Linux breaks the CLI's compile rather than skipping it; CI installs `libssl-dev` |
| Longer crash runs | one seed per run, 20,000 operations | The eight seeds under ctest run fewer operations each; only one seed runs long |

The two things in this list I would act on beyond the fixes above, each
yours to decide: the Punchline repository needs a workflow that runs
`backend/tests` (I have not added one), and the tzcheck allow-empty default
should flip to OFF in CI the moment the database is embedded, which the
switch commit does.

