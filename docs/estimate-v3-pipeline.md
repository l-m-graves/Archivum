# Estimate v3: data pipeline and query interfaces

Answers to section 10 of `archivum-pipeline-and-interfaces.md`. Units are
engineer-weeks for one person, consistent with `docs/estimate-v2.md`. Ranges
are honest, not padded: the low end assumes the FILL items resolve cleanly
and nothing is redone; the high end assumes the usual.

## 1. Estimates per component

| # | Component | Weeks | Notes |
|---|---|---|---|
| 1 | Two-database design in the Stage 2 pager and `Db` | 0.5 to 1 | Design discipline, not code. See section 2 below |
| 2 | Change feed, with the audit writer in Stage 5 | 1 to 2 | Same transaction as the business change. Row images fall under the Synthex recordability rule, see section 4 |
| 3 | Freshness monitoring on device sync, in the exception queue | 0.5 to 1 | Last-sync per device, threshold as configuration, queue entry |
| 4 | Parquet export | 2 to 3 | Plain encoding, no compression, hand-written Thrift compact footer. Verified in CI by reading the files back with pyarrow (test toolchain only) |
| 5 | SQL extensions for the analytical store: joins across analytical tables, GROUP BY and HAVING, window functions, `INSERT INTO ... SELECT` into analytical tables only | 5 to 8 | On top of Stage 7's SELECT-only engine. Subqueries and CTEs deferred, as instructed |
| 6 | C API, reshaped as a client library (see section 3, disagreement A) | 4 to 6 | Opaque handles, versioned structs, error codes, exception barrier. Wire protocol over the existing HTTPS API |
| 7 | Columnar storage layer | 10 to 16 | A second storage engine: chunks, encodings, zone maps, versions, tombstones, compaction, its own crash tests and model tests. See disagreement B |
| 8 | Transform engine, data catalog, table-level lineage, expectations | 5 to 8 | Transforms as versioned rows; dependency graph and lineage derived from declared inputs and outputs; incremental via change-feed watermark |
| 9 | Cube model: dimensions, hierarchies, levels, members, measures with aggregation rules including semi-additive, cube definitions as catalog rows | 4 to 6 | Blocked on `[FILL: which financial reports finance builds today]` for the first cube |
| 10 | MDX: parser, member and tuple resolution, the listed subset compiled to the shared plan | 8 to 12 | Needs new plan nodes: pivot, hierarchy closure traversal, relative time navigation, calculated members with solve order. SQL reaches them too |
| 11 | XMLA endpoint: Discover rowsets and Execute with a multidimensional result | 5 to 8 | Needs an XML parser (vendored, statically linked). Needs a credential Excel can present, see section 4. Needs a human with Excel to test |
| 12 | Orchestration: tasks and dependencies as rows, cron triggers, topological execution, retry with backoff, backfill, run history, concurrency limits | 4 to 6 | Single process. Task table shaped so workers could be distributed; they will not be |
| 13 | Ingestion and landing: file drop (CSV, fixed-width, JSON), HTTP push, source descriptors, generalized idempotency keys, immutable landing tables with hash and retention | 4 to 6 | Excludes pull connectors, see section 3 |
| 14 | Monitoring beyond freshness: volume anomaly, null-rate drift, schema drift, run duration, expectation failures, SLA alerting | 2 to 4 | Mostly queries over run history plus alert routing |
| 15 | Delivery extras: webhooks on change-feed events, scheduled report distribution | 3 to 5 | Report distribution needs the SMTP client already counted in Stage 6 |
| 16 | ODBC driver, ODBC 3.8 Core, on the client C API | 10 to 16 | Only if asked. See disagreement C |

Pipeline additions, items 1 to 15: **58 to 92 weeks**. With ODBC: 68 to
108.

## Revised total for the whole project

| | Weeks |
|---|---|
| Estimate v2 (engine, server, GUI, Punchline approval workflow, model-based testing, Finalysis, Synthex, deployment) | 46 to 69 |
| Pipeline and interfaces, items 1 to 15 | 58 to 92 |
| **Total without ODBC** | **104 to 161** |
| ODBC driver | 10 to 16 |
| **Total with ODBC** | **114 to 177** |

That is two to three and a half years of one engineer. With the cuts and
deferrals in section 3 applied, the total without the column store, pull
connectors, webhooks, report distribution, and ODBC is **86 to 131 weeks**.

Where the risk concentrates, in order: the columnar storage layer (a second
engine with its own durability proof), MDX semantics (calculated members
and semi-additive measures are where financial statements go quietly
wrong), XMLA against real Excel (a protocol where the test client is a
closed application), and the analytical SQL planner (joins and windows make
the planner a real planner).

## 2. Confirmation: two databases in the Stage 2 pager and `Db`

Confirmed, and designed in as follows. None of it is assumed away.

- **`Db` is an instance, never a singleton.** `Db::open(Vfs&, path,
  options)` returns an independent object owning its pager, page cache,
  WAL, writer mutex, snapshot registry, checkpointer, and log-archive
  directory. There is no static state anywhere in the engine. Stage 0
  already follows this: the VFS is an instance and the journal takes a
  `Vfs&`.
- **Process-wide things are only the VFS and a thread pool.** Background
  checkpointing is scheduled per `Db`; two instances never share a lock.
- **Paths are per instance.** `operational.db`, `operational.wal`,
  `operational-archive/`, and the same for `analytical`. Every operational
  subcommand (check, dump, restore, backup, point-in-time recovery) takes a
  file argument and operates on one instance.
- **Transactions are per instance by type.** A `WriteTxn` is created by
  one `Db` and holds only that `Db`'s writer lock. There is no API that
  takes two databases, so a transaction cannot span both.
- **Read-only by construction for transforms.** The transform path
  receives `ReadTxn` for the operational instance and `WriteTxn` for the
  analytical instance. The compiler enforces the direction.
- **Tested in Stage 2.** Two `Db` instances on one `FaultVfs`, a workload
  writing both, crashes at every step, and the invariant that each file
  recovers independently. The Stage 0 shim already models per-file syncs
  and per-directory syncs, so this needs no shim changes.
- **Recorded.** `docs/engine-design.md` (Stage 2) will carry this as a
  named invariant with the test that enforces it.

## 3. What I would cut or defer, with reasons

**A. The C API should be a client library, not an embedding API.**
Instructions v2 say `archivum.exe` is the only process that opens the
database, and that authorization, audit, and the query log live in that
process. A SQLite-style `archivum_open(path)` that Python, R, C#, or Excel
VBA call in their own process opens the file directly and bypasses all
three. Those goals are in direct conflict. The resolution used by every
client-server database is a client library: `archivum_connect(url,
credential)`, `archivum_prepare`, `bind`, `step`, `column_*`,
`archivum_free`, with opaque handles, versioned structs, and error codes
exactly as specified, speaking the server's HTTPS API. The ODBC driver, if
ever built, sits on that, which is how psqlODBC sits on libpq. The only
embedding-style entry point is `archivum_open_local` for the Punchline
client's own store, compiled only into the client. I have estimated the
client-library form. If you want the embedding form for another reason,
say so and I will explain what it costs in authorization terms before
building it.

**B. Defer the columnar storage layer behind a storage interface.**
This is the item the document itself calls the first genuinely large
piece, and I think it is not needed for the pipeline to exist. Everything
else in the document, the two files, the change feed, transforms, the
cube, MDX, XMLA, works over a second row-store instance of the same engine
in `analytical.db`. At 2,000 employees, 10,000 writes a day, and financial
statements by period, the largest analytical table is millions of rows at
most, and a clustered b-tree with the right secondary indexes answers
every query listed in milliseconds. Column encodings and zone maps pay off
at hundreds of millions of rows. The cost of building it now is 10 to 16
weeks plus a second durability proof and a second set of crash tests,
carried forever. What I propose instead: the shared executor reads through
one narrow storage interface (scan with projection and predicate, by
table), the analytical instance is a row store in the first release, and
the columnar layer is added behind the same interface only if a measured
query on real data is too slow. If you want the column store regardless,
it is estimated above and the sequencing in section 5 holds.

**C. Cut the ODBC driver from the plan.** The document already says last
and only if asked. I would remove it entirely. Excel reaches Archivum
through XMLA. Power BI reads Parquet and also connects to XMLA endpoints
through its Analysis Services connector. The only consumer left is
Tableau, which nobody has named. If someone does, the client C API in A is
the foundation and the estimate above stands.

**D. Cut pull connectors for external APIs** until a specific external
source is named. File drop and HTTP push cover every source described. A
generic pull connector with per-source authentication is a framework built
for a hypothetical.

**E. Defer webhooks and scheduled report distribution.** There is no
consumer outside Archivum today, and notification ("open Punchline") is
already in Stage 6. Both are small when a real consumer appears.

**F. Defer the drift monitors** (volume anomaly, null-rate drift, schema
drift) until expectations exist and have run for a few periods; they are
queries over that history. Freshness on device sync stays first.

**G. Keep, but sequence honestly:** the cube model needs the FILL on which
financial reports finance builds today. Designing dimensions before that
answer produces the wrong cube. MDX and XMLA follow it.

One reorder request against section 9: I would build the SQL extensions
and the transform engine (steps 7) before the columnar layer (step 6),
over a row-store `analytical.db`, because transforms do not depend on the
column store and they are what makes the second file worth having. That
is the concrete form of B.

## 4. Consequences the document did not state

- **The Synthex rule reaches the change feed and the landing layer.** The
  change feed records before and after row images; for Synthex tables it
  must record presence and shape only, enforced in the same writer as the
  audit rule. Synthex tables must also be unreachable as transform inputs,
  so no value can be copied into `analytical.db`. Both `analytical.db`
  and the landing tables join the canary scan, as the document says for
  landing.
- **Excel needs a credential it can present.** Excel's Analysis Services
  provider speaks HTTP basic or Windows integrated authentication to an
  XMLA endpoint, not OIDC bearer tokens. Under the no-shared-tokens rule,
  the workable path is a per-user, individually revocable access token
  generated in the GUI after an Entra sign-in, presented as the basic-auth
  password over TLS. This is a new credential type in Stage 5's model and
  needs your decision before XMLA is built.
- **XMLA and ODBC cannot be verified from this environment.** Both need a
  person running real Excel or Power BI against a deployment. Plan for
  that person's time.
- **An XML parser must be vendored** for XMLA, statically linked, with
  licence file and hash recorded per the evidence rule.
- **Two files, two backup policies.** `analytical.db` is rebuildable, so
  the handbook states a lighter policy for it, as instructed. The change
  feed retention must then cover the longest rebuild window, or a rebuild
  needs a full refresh from `operational.db`. Full refresh is the safe
  default.
