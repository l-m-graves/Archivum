# Archivum: proposal, questions, and staged plan

Status: proposal. No code exists yet. Stage 0 starts only after the questions in
section 1 are answered.

## 1. Questions

### Blocking before Stage 0

**Q0. Is the engine a means or an end?**
The single most important question. Building a storage engine is the most
expensive and riskiest item in the brief, and the stated benefit (nothing
third-party to install, patch, tune, or back up) is fully achievable by
embedding SQLite in the binary. See section 5. If the answer is "the engine is
the point, build it", the rest of this plan stands unchanged.

**Q1. Identity provider (Entra ID).** What I need from IT:

- Tenant ID, and whether the tenant is cloud-only or hybrid with on-premises AD.
- An app registration for the Archivum API: an Application ID URI such as
  `api://archivum`, one exposed scope, v2 access tokens, and the optional claims
  `oid`, `preferred_username`, and `email`.
- A second registration for the browser GUI as a public SPA client using
  authorization code with PKCE, redirect URI `https://<host>/gui/`.
- Role source. Are Archivum roles assigned inside Archivum by an admin, or mapped
  from Entra security groups? If groups, IT must enable the groups claim or app
  roles on the registration. My recommendation: assign roles in Archivum, keyed
  by the immutable `oid` claim, so payroll and finance separation is our data
  and not a group membership someone in IT can change by accident.
- Any conditional-access policy that would block a server-side fetch of the
  discovery document and signing keys from the Archivum host.
- Who issues the server TLS certificate (internal AD CS or public CA), and
  whether the Punchline machines are domain-joined and already trust it.

**Q2. Hosting shape.** Recommendation: Windows Server install first, Linux
container second. Reasons: Punchline is Win32, the brief says Windows first,
and the durability code (FlushFileBuffers, write-through, sector size) must be
proven on the OS it will run on. Windows containers are a poor fit, so the
container would be Linux, which makes the Linux durability path a production
path and not only a CI path. I need: Windows Server version, whether the host is
domain-joined, the service account model (gMSA preferred), the backup
destination (SMB share or blob storage), and whether IT wants a reverse proxy
they manage in front of Archivum or wants Archivum to terminate TLS itself.

**Q3. Punchline's FastAPI backend.** This repository is empty and I have no
access to Punchline, Finalysis, or Synthex. Please attach those repositories to
the session, plus the prior server review. Until then I cannot list what the
FastAPI server does. What I can say now:

- Byte-compatibility cannot be complete, because the two findings you cite are
  contract-level: a shared admin token and self-asserted identity are part of
  the current wire contract. The client must change once, in its
  authentication only. Everything else (paths, JSON shapes, batch semantics,
  error codes) should be kept identical and proven identical by a contract
  test suite run against both servers.
- Yes, keep FastAPI running against its current store until Archivum passes the
  same contract tests plus the crash and restore tests, then run both in
  parallel for a pilot group of devices before cutover. Keep the old store
  read-only after cutover as the rollback path.
- I also need: what the current store is, its row counts, whether it is in
  production today, and how many devices are deployed.

**Q4. Stack.** Answered in section 5. A single binary for engine plus server is
right. The operational subcommands (check, dump, restore, backup) should be the
same binary too.

### Other questions I consider blocking or nearly so

**Q5. Punchline identity model.** Does each employee sign in to their own
desktop client, or is a device a shared kiosk where employees punch with a badge
or PIN? "Employee identity from the authenticated session" is only possible in
the first model. In the kiosk model the session identifies the device, and the
employee must be authenticated separately (a per-employee PIN verified by the
server, never by the device). The two designs differ materially.

**Q6. Finalysis.** What language is it today, where does Excel-to-PDF
conversion run, what data volume (statements, periods, file sizes), and are
generated PDFs stored inside Archivum or on disk with a hash in the database?
I recommend files on disk with content hashes in the database.

**Q7. Money and time.** Largest monetary magnitude and scale you need (I
propose 64-bit integers with a declared scale, which covers 10^18 minor
units). Number of currencies. Which time zones the sites are in, and whether
payroll rules need the local wall-clock time of a punch as well as the instant.

**Q8. Retention and recovery targets.** Acceptable data-loss window (this sets
the log archive interval), retention period for payroll and audit records, and
where archived logs and backups go.

**Q9. Break-glass admin.** Who sets its password at install and where it is
kept. I propose it is created only by a local console command on the host,
disabled by default, and every use is audited and alerts.

**Q10. Targets.** Windows Server 2019 or 2022, MSVC 2022, x64 only? Linux
distribution for the container?

**Q11. GUI users.** Expected number of analysts, concurrency, and the largest
dataset they will query. This sets default limits.

## 2. Architecture

```
  Punchline (Win32)          Browser GUI            Finalysis / Synthex
  device credential          OIDC bearer token      OIDC bearer token
        |                        |                         |
        +-------- HTTPS ---------+---------- HTTPS --------+
                                 |
 +-------------------------------v---------------------------------------+
 | archivum.exe  (one process, one binary)                               |
 |                                                                       |
 |  HTTP + TLS (cpp-httplib, OpenSSL)   embedded static GUI files        |
 |  ---------------------------------------------------------------------|
 |  auth   : OIDC validation (jwt-cpp), device credentials, local accts  |
 |  authz  : roles and dataset grants, stored as data                    |
 |  audit  : append-only writer, before/after values                     |
 |  ---------------------------------------------------------------------|
 |  app modules (tables, migrations, roles, routes, datasets)            |
 |   core | punchline | finalysis | synthex | <fourth app>               |
 |  ---------------------------------------------------------------------|
 |  query service: read-only SQL (SELECT-only grammar, planner,          |
 |    executor over datasets), limits, query log, CSV/XLSX export        |
 |  ---------------------------------------------------------------------|
 |  typed API: Db, ReadTxn, WriteTxn, Table, Index, Cursor               |
 |  ---------------------------------------------------------------------|
 |  engine: catalog | constraints | b-trees | records/types | snapshots  |
 |          WAL + recovery | pager + checksums + free list | VFS         |
 |                                                  (real | crash shim) |
 +----------------------------+------------------------------------------+
                              |
        archivum.db   archivum.wal   wal-archive/   backups/
```

**Engine design in one paragraph.** The engine is deliberately the SQLite WAL
model, because it is the simplest design that satisfies every durability
requirement in the brief. Pages are 4 KiB with a CRC32C in every page header.
The write-ahead log holds full page images, each frame checksummed and salted,
and a commit is a commit frame followed by one fsync of the log. The data file
is only written by checkpoint, which copies committed frames into place, fsyncs
the data file, then retires the log segment into the archive. Because the data
file is never the only copy of a page until the checkpoint has been fsynced,
torn pages, reordered writes, and partial writes to the data file are
recoverable by construction. Recovery replays the longest valid prefix of
committed frames and discards the rest. Readers take a snapshot by recording
the log end position at transaction start; there is one writer at a time,
serialized by an in-process lock. Because only our process opens the file, the
log index lives in memory, which removes SQLite's shared-memory complexity.

**Records and types.** Row format with a null bitmap and typed columns:
integer (int64), decimal (int64 with declared scale, checked arithmetic, never
floating point), text (UTF-8), blob (overflow pages beyond a threshold),
timestamp (int64 microseconds since the Unix epoch, always UTC; local zone is an
application column when needed), boolean, UUID (16 bytes). Tables are clustered
b-trees on the primary key; secondary indexes are b-trees from key to primary
key, with range scans in both directions.

**Catalog and constraints.** The catalog is itself a set of tables in the file,
rooted at page 1, with a schema version and a migrations table. A migration is
one write transaction. Primary key, unique, not-null, foreign key (restrict
only, no cascades), and check are enforced in the engine's insert, update, and
delete paths.

**Durability model, stated plainly.** A committed transaction is durable if
the operating system and disk honour fsync. If fsync lies, the guarantee that
survives is consistency: recovery yields a prefix of committed transactions,
never a torn or mixed state, and checksums cause corruption to be reported
rather than served. Off-host log archiving is the only defence against a lying
fsync, and the documentation will say so.

**Server.** An app module is a directory with a schema, migrations, role
definitions, dataset definitions, and a router. Adding a fourth application
means adding a directory and one line in the module list; core code is not
touched. Datasets and roles are rows, added by migration. Every mutating
request runs inside one write transaction that also writes the audit rows, so
an audit entry cannot be lost while its business change survives.

**Read-only SQL.** Our own parser accepts only a SELECT grammar. The executor
is linked only against the read-transaction API, so no write is reachable from
the query path by construction. Datasets are named, parameterized plans whose
mandatory predicates are bound from the session (for example the supervisor's
team). Limits (statement timeout, rows scanned, rows returned, concurrent
queries) are enforced inside the executor and read from configuration.

**Repository layout.**

```
archivum/
  CMakeLists.txt  vcpkg.json  vcpkg-configuration.json   pinned baseline
  engine/      src/ include/ tests/        no dependency on server/
  server/      src/ include/ tests/        HTTP, auth, authz, audit, modules
  apps/        core/ punchline/ finalysis/ synthex/
               each: schema/ migrations/ roles/ datasets/ routes.cpp tests/
  sql/         parser, planner, executor, tests
  gui/         index.html app.js style.css   embedded at build
  tests/       crash/ differential/ randomized/ restore-verify/
  docs/        page-format.md durability.md install.md extending.md
  .github/workflows/   windows.yml linux.yml
```

**On-host layout (Windows).**

```
C:\ProgramData\Archivum\
  config.toml
  data\archivum.db   data\archivum.wal
  archive\wal-000001.log ...            shipped off host on a schedule
  backups\
  logs\                                 structured, no personal values
```

## 3. Staged plan

Every stage ends with a green CI on Windows and Linux and a demonstrable
result. Effort is in engineer-weeks for one person.

| Stage | Deliverable | Weeks |
|---|---|---|
| 0 | CI on Windows and Linux. VFS interface and crash-injection shim (fail, half-write, reorder, drop fsync). Seeded randomized runner. Proven against a toy append log before any engine code. | 1 to 2 |
| 1 | Web slice: cpp-httplib with OpenSSL serving one TLS endpoint that validates an Entra token with jwt-cpp and returns JSON. vcpkg manifest, Windows CI build. Decision gate on the C++ web stack. | 1 to 2 |
| 2 | Engine core: pager, checksums, free list, WAL, recovery, single-writer transactions. Crash tests at every injection point. | 4 to 6 |
| 3 | B-trees, records and types, catalog, constraints, typed API, snapshot readers. Differential tests against SQLite, randomized workloads, concurrency tests. | 6 to 10 |
| 4 | Tooling: integrity check, dump, restore, online backup, log archive, point-in-time recovery. Restore-and-verify script running in CI. | 2 to 3 |
| 5 | Server core: configuration, structured logs, health, OIDC, device enrollment and revocation, local accounts, break-glass, roles, audit trail, module framework, migrations. | 3 to 4 |
| 6 | Punchline module: schema, sync endpoints, idempotent batches, corrections with reason. Contract tests against the FastAPI server. Pilot, then cutover. | 3 to 5 |
| 7 | Read-only SQL: parser, planner, executor, datasets, limits, query log. | 4 to 6 |
| 8 | GUI: visual builder, SQL editor with autocomplete, saved and shared queries, CSV and XLSX export with audit. | 3 to 4 |
| 9 | Finalysis module: versioned figures, immutable closed periods, restatements. | 3 to 4 |
| 10 | Synthex module and the rejection test suite. | 1 to 2 |
| 11 | Deployment: Windows service installer, Linux container, IT handbook, upgrade and rollback, monthly restore procedure. | 2 to 3 |

Total: roughly 33 to 51 engineer-weeks. The engine (stages 0, 2, 3, 4) is
about half.

**The first stage that puts real payroll data at risk is Stage 6.** Financial
data is first at risk in Stage 9. Before Stage 6 ships to production, all of
the following must be true:

- Crash-injection, differential, randomized, and concurrency suites green for a
  large fixed set of seeds, and green on the exact Windows build being deployed.
- Backups and archived logs shipped off host on a schedule, and the
  restore-and-verify script passing against a real backup of the pilot data.
- Contract tests identical between FastAPI and Archivum for every endpoint the
  client uses, with idempotent replay verified.
- No shared token in any path; every pilot device enrolled with its own
  credential and revocation tested end to end.
- Audit trail verified for every mutating endpoint.
- Rollback rehearsed: the FastAPI server and its store kept read-only and able
  to resume.
- Integrity check scheduled and alerting.

## 4. Estimate and risk

**Size.** Engine: 15,000 to 25,000 lines of C++ plus 10,000 to 15,000 lines of
tests. Read-only SQL: another 8,000 to 12,000 lines. Server and modules:
10,000 to 15,000. GUI: 3,000 to 5,000 lines of plain JavaScript. For scale,
SQLite's core is around 150,000 lines including a full SQL engine.

**Highest risk, in order.**

1. Recovery correctness under reordering and torn writes. Mitigated by the
   full-page-image log design, which avoids logical redo entirely.
2. B-tree deletion, rebalancing, and overflow pages. Historically where the
   bugs live. Mitigated by the SQLite differential oracle and randomized
   workloads.
3. Snapshot readers versus checkpoint. A long analyst query pins the log and
   blocks checkpoint; bounded by statement timeout, but the interaction must be
   tested explicitly.
4. Windows durability semantics: FlushFileBuffers, write-through flags, 4K
   native versus 512e drives, and virtualized disks that acknowledge early.
5. The read-only SQL engine. Easy to underestimate; the planner is small only
   if we refuse features (no subqueries at first, joins only within a dataset).
6. Point-in-time recovery and online backup consistency.
7. Atomic schema migration, which requires DDL to be logged like any write.

## 5. Disagreements

**The engine.** I would not build it. Embedding SQLite gives you the same
deployment shape (one binary, one file, nothing to install), a WAL with
snapshot readers, a backup API, and twenty years of crash testing, at zero
runtime cost and zero license cost. The brief already treats SQLite as
trustworthy enough to be the oracle. Building our own engine is 40 to 60
percent of the whole project and carries nearly all of its data-loss risk. If
the engine exists for learning or for ownership, that is a legitimate reason
and I will build it as specified. But it should be a conscious choice, which is
why it is Q0.

**Drogon.** Replace with cpp-httplib. The engine has one writer, so every
write serializes at the engine regardless of how asynchronous the HTTP layer
is. At 10,000 writes per day and a handful of analysts, synchronous
thread-per-request with a bounded pool is more than sufficient, is a single
header, has no controller macros for IT to learn, and is far easier to debug.
It also serves as the HTTPS client for fetching the OIDC discovery document
and keys, which removes libcurl and WinHTTP. If you want an async server
anyway, Crow, not Drogon: Drogon brings jsoncpp, an ORM, and a framework style
that is a poor fit for the "IT extends it" goal.

**TLS.** OpenSSL, statically linked through vcpkg. Schannel would be patched by
Windows Update, but no candidate HTTP library supports it, so it would mean our
own TLS layer and a Windows-only server. The cost of OpenSSL is that a CVE
means a rebuild and redeploy of Archivum; this goes in the handbook.

**libsodium.** Drop it. OpenSSL 3.2 and later provides Argon2id through its
KDF interface. One fewer dependency.

**The "fsync that lies" requirement.** Not satisfiable as written. No design
can keep a commit durable if the disk drops it after acknowledging. What can
be promised is consistency and detection, plus off-host log archiving. I have
reworded it that way in section 2 and will document it.

**Byte-compatible Punchline contract.** Impossible in full, because the
authentication model is part of the contract and is exactly what must change.
Keep everything else identical and change the client's auth once.

**Timestamps.** Storing only the instant is correct for the engine but not
sufficient for payroll. A punch at 01:30 on the night daylight saving ends is
ambiguous without the site's zone and the recorded local time. The Punchline
schema stores the instant, the site zone identifier, and the local wall-clock
time as recorded, and pay rules use the latter two.

**Synthex rejection tests.** "Assert rejection of statistics derived from
data" cannot be recognized syntactically. The enforceable rule is a strict
allow-list schema per endpoint: exactly the declared fields, with declared
types and lengths, and anything else is rejected with 400. Tests then submit
each forbidden class (samples, min and max, category lists, file names, paths,
date offsets) as extra or misplaced fields and assert rejection. The vocabulary
import additionally requires source, license, approver, and date, or it is
rejected.

**Excel export.** An XLSX file is a zip archive of XML. With no third-party
dependency this means vendoring a single-file zip writer (miniz, MIT) and
writing the XML ourselves. Feasible, but CSV should ship first and XLSX in a
later increment.

**Extending without touching core code.** True for tables, migrations, roles,
and datasets, which are data. A new application's HTTP routes are C++ and need
one registration line. I would not promise IT a runtime plugin system; the
handbook will show the directory-and-one-line path instead.

**Container.** Fine as an option, but it will be a Linux container, so Linux
becomes a production platform for the engine, not just a CI convenience. The
crash harness must run on both, and it will.
