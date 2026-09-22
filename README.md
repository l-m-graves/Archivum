# Archivum

Storage engine, application server, and low-code SQL interface for the
Punchline, Finalysis, and Synthex applications. One C++20 binary, one data
directory, no external database.

Status: **Stage 6** (the Punchline module: sync with idempotent batches,
punch pairing, the approval lifecycle with every transition audited,
corrections, the exception queue with freshness monitoring and escalation
after cutoff, employee self-service, the supervisor queue, the payroll
export and period audit report, the contract suite shared with the
FastAPI prototype). No SQL yet (v1.1). See `docs/plan-v1.md` for the
plan, `docs/engine-design.md` for the engine, `docs/server-core.md` for
the server and `docs/punchline-module.md` for the module.

## Build

Requires CMake 3.22+, MSVC 2022 (Windows) or GCC 12 (Linux), and vcpkg
checked out at the commit in `vcpkg.json` with `VCPKG_ROOT` pointing at
it. Dependencies (Drogon, OpenSSL, jwt-cpp, nlohmann-json) are built by
the manifest on first configure.

```
cmake --preset linux-debug          # or linux-release, or windows
cmake --build --preset linux-debug  # or windows-debug / windows-release
ctest --preset linux-debug
```

`linux-debug` builds with AddressSanitizer and UndefinedBehaviorSanitizer.

## Layout

```
server/            application server library and the archivum executable
  include/archivum/server/  config.h oidc.h identity.h log.h archive.h modules.h app.h
  src/               config.cpp log.cpp archive.cpp app.cpp main.cpp auth/{oidc,identity}.cpp
                     http/{json_bridge,request}.cpp modules/punchline_{module,routes}.cpp
core/              what every module shares: core schema, recorder (audit + change feed),
                   credentials (Argon2id), local accounts, roles and dataset grants, module interface
engine/            storage engine library (archivum_engine)
  include/archivum/  status.h crc32c.h vfs.h journal.h entropy.h
                     engine/{db,page_format,btree,types,record,store,recovery,migrate}.h
  src/               crc32c.cpp journal.cpp entropy.cpp vfs_posix.cpp vfs_win32.cpp
                     engine/{db,wal,page_format,btree,types,record,store,recovery,migrate}.cpp
  testing/           test-only doubles: MemVfs, FaultVfs (the crash shim)
modules/punchline/   the Punchline module: schema, rules, data, local time, the TZif reader and embedded
                     time zone database (tzdata/), sync and pairing, lifecycle, exceptions, rollback export
tools/tzdata/        the database generator (IANA tarball -> zic -> embedded arrays) and the build-time check
third_party/tzdata/  where the IANA release tarball and signature go (not yet vendored)
tests/
  support/           minimal test framework (no dependency)
  unit/              per-component tests
  crash/             crash-injection tests (the Stage 0 gate)
  cli/               the binary's operational subcommands on real files
  server/            test PKI, test issuer, integration, saturation, core, Punchline and contract tests
  contract/          the batch-ingest contract shared with the FastAPI prototype (scenarios + pytest runner),
                     and the rollback replay into it
  tzdata/            a synthetic release proving the time zone tooling on every build
config/            archivum.example.json
docs/                design, formats, durability model, testing model
.github/workflows/   CI: Windows (MSVC 2022) and Linux (Ubuntu 22.04, GCC 12: ASan+UBSan, Release, ThreadSanitizer)
```

## Documents

- `docs/proposal.md`: the original proposal and architecture diagram.
- `docs/estimate-v2.md`: revised estimate and stage plan after instructions v2.
- `docs/estimate-v3-pipeline.md`: estimates, cuts, and the two-database confirmation for the pipeline document.
- `docs/plan-v1.md`: the v1 release boundary, gates, Punchline fold-in, and current figures.
- `docs/decisions/`: recorded decisions with reasoning.
- `docs/confidentiality-check.md`: the mandatory check for any component that persists row content.
- `docs/testing/fault-model.md`: what the crash shim simulates and how tests use it.
- `docs/durability.md`: what a durable write promises, and what it cannot.
- `docs/journal-format.md`: on-disk format of the append-only journal.
- `docs/cpp-subset.md`: the C++20 subset this project uses.
- `docs/json-boundary.md`: nlohmann/json inside, jsoncpp only at the HTTP edge.
- `docs/toolchain.md`: pinned versions and what is not yet pinned.
- `docs/stage1-report.md`: the Drogon spike gate, saturation numbers, and findings.
- `docs/engine-design.md`: the pager and log design with its named invariants.
- `docs/page-format.md`: on-disk formats of the database file and the log.
- `docs/btree-format.md`: node, cell and overflow layout of the b-tree, its split and erase rules.
- `docs/store-format.md`: types, order-preserving key encoding, row encoding, catalog, indexes, constraints.
- `docs/stage3-report.md`: the Stage 3 gate, test figures and findings.
- `docs/backup-recovery.md`: backup, restore, log archive and point-in-time recovery, for operators.
- `docs/punchline-schema.md`: the Punchline tables, their constraints and the assumptions behind them.
- `docs/stage4-report.md`: the Stage 4 gate, test figures and findings.
- `docs/server-core.md`: configuration, logging, identity, roles, local accounts, the off-host archive check, modules, routes.
- `docs/audit-and-change-feed.md`: the recorder, the record policy and the presence-and-shape rule.
- `docs/stage5-report.md`: the Stage 5 gate, ThreadSanitizer findings, schema answers.
- `docs/punchline-module.md`: sync, pairing, the lifecycle, the exception queue, the monitor, the contract suite.
- `docs/stage6-report.md`: the Stage 6 gate, rulings applied, the Windows-only threaded surface, the v1 release boundary.
