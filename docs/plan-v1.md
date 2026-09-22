# Plan v1: release boundary, Punchline fold-in, revised figures

Responds to `archivum-rulings-v3.md` sections 0, 5, and 6, and folds in
`punchline-updates.md`. Supersedes the stage figures in `estimate-v2.md`
where they differ; `estimate-v3-pipeline.md` stands with the rulings
applied.

## 0. Stage 0 language standard: C++20, confirmed with evidence

- `CMakeLists.txt` lines 8 to 10: `CMAKE_CXX_STANDARD 20`,
  `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`. The
  standard is set explicitly in the project, not inherited from a compiler
  default, and applies to every preset in `CMakePresets.json`.
- Evidence from the build: the GCC command line for `journal.cpp` in
  `build/linux-debug` carries `-std=c++20`. CMake emits `/std:c++20` for
  MSVC from the same setting; the first CI run compiled with MSVC 19.44 and
  reported no standard-related diagnostics.
- The subset is in use, not just enabled: `std::span` is the type of every
  buffer crossing the VFS boundary (`vfs.h`, `crc32c.h`, `journal.h`) and
  through the journal, in-memory VFS, and crash shim implementations. The
  CRC32C table is a `constexpr` computation. Nothing from the "do not use"
  list (modules, ranges, `std::format`) appears.

No migration is needed.

## 1. The v1 cut line

**v1 is Punchline in production**, as you proposed, with one pushback.

### Pushback: ad hoc SQL moves to v1.1

Your starting position puts the read-only SQL engine and the GUI in v1. I
would keep the GUI shell (sign-in, roles, the Punchline views) in v1 and
move the read-only SQL engine, the visual query builder, and the SQL editor
to v1.1. Reasons:

- Nothing in Punchline production needs ad hoc SQL. Payroll needs fixed
  reports (hours by pay code, the period audit report, the exception
  queue), which are typed-API queries with CSV export.
- It removes 7 to 10 weeks from the path to production (Stage 7, and the
  builder half of Stage 8).
- It removes the analyst role, the dataset permission model, and the
  query-limit machinery from the v1 attack surface and the v1 audit scope.
- The SQL engine is also the base of the analytical SQL in v1.1, so it is
  built once, at the point where its first two consumers (analysts and
  transforms) both exist.

What v1 loses: an analyst cannot query Punchline data ad hoc until v1.1.
If that matters to someone in the first months, say so and the two pieces
return to v1 at the cost above.

### v1 stages

| Stage | Content | Weeks |
|---|---|---|
| 0 | CI, VFS, crash shim, journal. **Done.** | 1 to 2 |
| 1 | Drogon spike against the four-point gate; vcpkg with pinned baseline; `std::format` and coroutine checks | 2 to 3 |
| 2 | Pager, checksums, free list, WAL, recovery, single-writer transactions; **two-database `Db` design with the two-instance crash test** | 4 to 6 |
| 3 | B-trees, records and types, catalog, constraints, typed API, snapshot readers; model-based tests, invariant checker, dump and reload round trip; timeout-versus-checkpoint test | 8 to 13 |
| 4 | Integrity check, restore, online backup, log archive, point-in-time recovery, restore-and-verify in CI. (Parquet moves to v1.1) | 2 to 3 |
| 5 | Server core: configuration, structured logs, health with certificate expiry, TLS policy, trusted proxies, OIDC with JWKS caching, device enrollment and revocation, local accounts, break-glass, roles on `oid` and `tid`, audit writer with per-field recordability and the change-feed hook, module framework, migrations, mandatory off-host archive check | 4 to 5 |
| 6 | Punchline module, see section 2 | 8 to 12 |
| 6-C | The Punchline client's sync layer, rewritten (Stage 6 rulings, item 3a): the per-device credential in place of the shared token; employee identity dropped from the payload (the server derives it; a client that asserts it is refused and counted); the append-only local journal already in the Punchline repository driving the batch shape, with `(journal id, sequence)` idempotency and acknowledgement-driven compaction; the three time values and the site zone from the heartbeat; device-attested marking left to the server; bounded retry with backoff and a retry cap that reports `retry_exhausted` and `journal_recovery` through the batch; the enrollment screen taking the one-time credential into the DPAPI store; the crash suite over the journal-plus-sync path; and the contract suite run from the real client against Archivum. Estimate: 3 to 5 weeks. The prototype's request builder is one function, but the rest is new: WinHTTP client against the new contract (1 to 2), journal integration and acknowledgement (0.5 to 1), enrollment and credential handling (0.5), retry policy and reports (0.5), tests including the crash suite and the end-to-end run (0.5 to 1). Lands before or after 8-P by your decision | 3 to 5 |
| 8-P | Punchline web views: employee self-service with approver visibility and flag, supervisor queue, exception queue, payroll console, period audit report, segregation-of-duties report, admin enrollment and mapping, CSV export | 4 to 6 |
| 11 | Windows service installer, Linux container, IT handbook, upgrade and rollback, monthly restore procedure | 2 to 3 |

**v1 total: 38 to 58 weeks with Stage 6-C, of which Stages 0 to 6 are done.**

### v1 gates

v1 ships when all of these hold. They are the pilot gates from
`punchline-updates.md` section 10 plus release gates:

- Crash-injection, model-based, randomized, and concurrency suites green
  on both platforms and on the exact Windows build being deployed.
- Backups and archived logs shipping off host on a schedule; the
  restore-and-verify script passing in CI and against a real backup of
  pilot data.
- Contract suite identical between the FastAPI server and Archivum, with
  idempotent replay verified, and every endpoint rejecting an employee ID
  in the request body. Since Stage 6-C is a stage: the suite also run
  from the rewritten client itself against Archivum.
- Day assignment computed by the server from the instant and the site
  zone against the embedded IANA database, never from the device's clock;
  the device's wall clock compared and a disagreement queued; DST
  transitions of every deployed site zone pinned and asserted at build
  time (Stage 6 rulings, item 1).
- Rollback rehearsed with the reverse export: `archivum rollback-export`
  run against real pilot data, replayed into the old server through its
  own ingest endpoint, and the old server serving the restored data
  (`tests/contract/test_rollback.py` against the real export), before
  cutover. Parallel running is phased by device (pilot devices on the new
  client against Archivum, everyone else on the old client against
  FastAPI), never dual-written.
- No shared token anywhere; every pilot device enrolled individually with
  revocation tested end to end.
- Audit trail verified for every mutating endpoint and every lifecycle
  transition; the period audit report producible for the pilot period.
- Segregation-of-duties report shows nobody holding both payroll and
  supervisor.
- Certificate expiry visible in the health endpoint; off-host archive
  configured or the health check fails; break-glass creation, use,
  auto-disable, and audit exercised once.
- Rollback rehearsed with the old server able to resume.
- Integrity check scheduled and alerting.
- IT handbook complete, including the accepted revocation-latency bound.

**v1 closes**, as distinct from ships, when the FastAPI cutover is complete
and the old store is retired at the end of the defined rollback window.

### v1.1 and beyond

In the order of the pipeline document's section 9 with the granted
reorder, and with the rulings applied:

| Item | Weeks |
|---|---|
| Read-only SQL engine, visual builder, SQL editor (from v1) | 7 to 10 |
| Change feed | 1 to 2 |
| Storage interface document, in-memory second implementation, benchmark harness with numeric trigger (conditions on ruling B) | 2 to 3 |
| Parquet export | 2 to 3 |
| Analytical SQL: joins, GROUP BY and HAVING, windows, INSERT INTO SELECT into analytical tables | 5 to 8 |
| Transform engine, data catalog, table-level lineage, expectations | 5 to 8 |
| Finalysis module | 3 to 4 |
| Synthex module with the full confidentiality check | 2 to 3 |
| Orchestration | 4 to 6 |
| Cube model (after the financial-reports FILL), with the semi-additive test suite of hand-computed values | 4 to 6 |
| MDX | 8 to 12 |
| XMLA, plus the per-user token credential | 6 to 10 |
| C client API | 4 to 6 |
| Ingestion: file drop, HTTP push, landing layer | 4 to 6 |
| Monitoring beyond freshness | 2 to 4 |
| Columnar storage layer, only when the benchmark trigger fires | 10 to 16 |
| ODBC driver, deferred, decision pending verification | 10 to 16 |

Whole project without the last two: 94 to 145 weeks. With both: 114 to
177.

## 2. Punchline fold-in: Stage 6 and Stage 8 revised

Everything in `punchline-updates.md` that was not already in estimate v2:

- **Client journal:** a monotonic sequence number per record, used for
  idempotent replay, and compaction that removes records only after the
  server acknowledges them. Crash-tested with the same shim, including the
  compaction path. The journal's format version field (`docs/journal-format.md`)
  covers the format change.
- **Contract test, every endpoint:** a request carrying an employee ID in
  its body is rejected with 400, not ignored.
- **Schedules.** "Punches outside scheduled hours" and "non-scheduled
  days" need a schedule per employee. Assumption: a weekly schedule table,
  HR-sourced, nullable; with no schedule those two checks are skipped and
  the entry is flagged "no schedule on file". `[FILL: schedule source]`.
- **Clock-tamper detection:** server receipt time stored beside device
  time; divergence beyond a configured tolerance routes to the exception
  queue.
- **Bounded retry with backoff** on the client; exhaustion surfaces to the
  user and to the exception queue via the next successful sync.
- **Approver visibility:** "Your approver: name" with a flag action that
  opens a ticket row; nothing changes.
- **Segregation of duties:** payroll and supervisor grantable independently
  and a report of anyone holding both.
- **Device freshness monitoring** in the exception queue (from the
  pipeline document, kept in v1).
- **Entry fields:** attestation source, lifecycle state, device ID,
  employee ID, journal sequence number, pay code, approval audit reference,
  receipt time, and the three time values plus tzdb version.
- **`[[nodiscard]]`** is already on `Status` and `Result`; a dropped
  return value is a compile error on both compilers.

Assumptions stated per section 11, all configuration and none structure:

| Question | Assumed default until answered |
|---|---|
| Domain-joined | No. PIN verifier via libsodium, DPAPI-protected, behind one interface |
| Pay calendar | Closes Sunday; submit Monday; approve Tuesday 12:00 site time; release Wednesday |
| Shift-length threshold | 14 hours |
| Clock divergence tolerance | 5 minutes |
| Device-attested count threshold | 3 per period |
| Site zone | America/Los_Angeles |
| Pilot size | 10 devices |

**Stage 6: 8 to 12 weeks** (was 7 to 10).
**Stage 8-P, Punchline views only: 4 to 6 weeks.** The query builder and
SQL editor, 3 to 4 weeks, move to v1.1 with Stage 7. The full Stage 8 as
previously scoped would be 7 to 10.

## 3. Verification status of the Power BI claim (ruling C)

Not verified. This environment's egress proxy blocks `learn.microsoft.com`,
so I could not read the connector documentation, and the search results I
could reach describe the opposite direction (Power BI Premium acting as an
XMLA server for third-party tools), which does not answer the question.
Status stays **deferred, decision pending verification**. To settle it,
one of:

- A person with Power BI Desktop uses Get Data, "SQL Server Analysis
  Services database", against any non-Microsoft XMLA-over-HTTP endpoint and
  reports whether it connects. That is the test that counts.
- Or, as a first step, read Microsoft's page "Connect to SSAS
  multidimensional models in Power BI Desktop" and the Analysis Services
  page "Configure HTTP access to Analysis Services on IIS", and check
  whether the connector is documented as accepting an arbitrary HTTP XMLA
  URL. I have not read them and make no claim about their content.

## 4. Confidentiality check of the remaining components (ruling on section 4)

Reported in `docs/confidentiality-check.md`, with two new places the rule
reaches that neither of us had listed: process crash dumps, and HTTP
request logging.

## 5. XML parser candidate (ruling on section 4)

Candidate: pugixml, a two-file C++ library. Alternative if it does not fit:
Expat. I am naming the candidate only. Its licence will be read from the
licence file in the release tag at vendoring time, with the repository,
tag, file name, and SHA-256 recorded then, and reported to you before the
vendored copy is committed.
