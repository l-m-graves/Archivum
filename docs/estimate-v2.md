# Revised estimate after instructions v2

Instructions v2 changed: SQLite is not available even as a test oracle
(model-based testing replaces it); Drogon is the HTTP library and Stage 1
must pass a four-point gate; a supervisor approval workflow, exception queue,
escalation path, and payroll console are new scope; a columnar export is
added; the Punchline client gets a journal-backed local store; the audit
writer must enforce per-field recordability; and everything is C++20.

## Totals

| | Engineer-weeks |
|---|---|
| Previous total (proposal) | 33 to 51 |
| **Revised total** | **46 to 69** |

Engine stages (0, 2, 3, 4) are 16 to 26 of that. Punchline stages (6, 8)
are 13 to 19.

## Per stage

| Stage | Deliverable | Was | Now | Why it changed |
|---|---|---|---|---|
| 0 | CI both platforms, VFS, crash shim, seeded runner, journal as the toy log | 1 to 2 | 1 to 2 | Done in this session; see the Stage 0 report |
| 1 | Drogon spike: static OpenSSL TLS on Windows, `drogon::HttpClient` JWKS fetch, jwt-cpp against a local test issuer, saturation behaviour; vcpkg manifest with pinned baseline; `std::format` and coroutine checks | 1 to 2 | 2 to 3 | Four-point gate, Drogon plus jsoncpp bridge, event-loop saturation test to report on |
| 2 | Pager, checksums, free list, WAL, recovery, single-writer transactions, crash tests at every point | 4 to 6 | 4 to 6 | Unchanged |
| 3 | B-trees, records and types, catalog, constraints, typed API, snapshot readers; model-based testing, invariant checker, round-trip via dump/reload; timeout-versus-checkpoint test | 6 to 10 | 8 to 13 | Loss of the SQLite oracle: the naive model is cheap, the invariant checker (b-tree order, page linkage, free list, index cross-checks) is not, dump/reload moves here from Stage 4, and bugs the oracle would have found are found by hand |
| 4 | Integrity check, restore, online backup, log archive, point-in-time recovery, restore-and-verify in CI, columnar export | 2 to 3 | 3 to 5 | Columnar export with no library: a minimal Parquet writer (plain encoding, no compression, hand-written Thrift compact metadata). See "expensive items" |
| 5 | Server core: config, structured logs, health with certificate expiry, TLS policy, trusted-proxy list, OIDC with JWKS caching, device enrollment and revocation, local accounts, break-glass with auto-disable, roles on `oid`+`tid`, audit writer with per-field recordability, module framework, migrations, mandatory off-host archive check | 3 to 4 | 4 to 5 | Audit writer enforcement, TLS operational requirements, health checks |
| 6 | Punchline module: schema (instant + zone + local time + tzdb version), per-device enrollment, sync with idempotent batches, corrections, entry lifecycle (recorded to locked), supervisor queue, daily exception queue, escalation after cutoff, effective-dated supervisor relation, pay-code computation (regular, OT, double-time, PTO, holiday) with explicit rounding, notification queue, contract suite against FastAPI, client journal integration, pilot and cutover | 3 to 5 | 7 to 10 | Approval workflow and escalation are new; pay-code computation on local days and weeks is a rules engine; SMTP notification with no library |
| 7 | Read-only SQL: parser, planner, executor, datasets, limits, query log with recordability rules | 4 to 6 | 4 to 6 | Unchanged |
| 8 | GUI: visual builder, SQL editor, saved queries, CSV and XLSX export; employee self-service, supervisor approval queue, payroll console (dashboard, exception dispositions, hours by pay code, release and lock, retroactive adjustment, period audit report) | 3 to 4 | 6 to 9 | Payroll console and the two role-gated queues are a second application's worth of UI |
| 9 | Finalysis module: versioned figures, closed periods, restatements, content-addressed PDFs on disk | 3 to 4 | 3 to 4 | Unchanged; blocked on Q6 facts |
| 10 | Synthex module: allow-list tables, per-endpoint audit field declarations, rejection suite, canary scan of db, wal, archive, and backup bytes | 1 to 2 | 2 to 3 | Canary scan and audit-writer integration |
| 11 | Windows service installer, Linux container, IT handbook (certificates with named owner, CVE rebuild, backup and restore, upgrade and rollback), monthly restore procedure | 2 to 3 | 2 to 3 | Unchanged |

## Expensive items under the self-sufficiency rule (section 0(c))

Said now, before building, as instructed.

1. **Columnar export.** "Columnar-friendly" is read as Apache Parquet, which
   is what analytical tools open. A writer with plain encoding and no
   compression is 1,500 to 2,500 lines including the Thrift compact
   encoding of the footer, plus tests that a reader accepts it. Confirm
   Parquet is the intended format before Stage 4. Without confirmation, CSV
   ships first and Parquet is a separate increment.
2. **Email notification.** "Email may notify" means an SMTP client with
   STARTTLS or implicit TLS, built on OpenSSL, since Drogon has none.
   Roughly 500 lines plus a test double. Placed in Stage 6.
3. **XLSX export.** A zip container plus hand-written spreadsheet XML.
   Requires a vendored single-file zip writer (statically linked, so
   permitted). Small, but it is a format we then own.
4. **Off-host archive shipping.** To an SMB share it is a file copy. To
   blob storage it is an HTTP client plus request signing, which is real
   work. The `[FILL: backup destination]` answer decides this.
5. **Job scheduling.** An in-process timer loop driving archive shipping,
   backups, and the exception queue. Modest, but it is ours to make robust
   across service restarts.

## Where the Punchline client journal lives

`engine/include/archivum/journal.h` and `engine/src/journal.cpp`, in the
`archivum_engine` static library. The client links `archivum_engine` and
uses `Journal` over `make_os_vfs()`; the linker pulls only the VFS, CRC32C,
and journal objects. It is in the engine because it is built on the VFS
boundary the engine owns, and because it is tested by the engine's crash
harness. Compaction (dropping synced punches) is a Stage 6 addition.

## Sequencing that changed

- Dump and reload move from Stage 4 into Stage 3 because round-trip
  verification is now part of the engine's core test suite.
- Stage 6's contract suite is written against the FastAPI server as soon as
  the repository is attached, before the module exists, so the target is
  fixed before the implementation starts.
