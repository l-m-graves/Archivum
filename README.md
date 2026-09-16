# Archivum

Storage engine, application server, and low-code SQL interface for the
Punchline, Finalysis, and Synthex applications. One C++20 binary, one data
directory, no external database.

Status: **Stage 2** (pager, write-ahead log, recovery, page-level
transactions, two-instance design). No b-tree or catalog code yet. See
`docs/plan-v1.md` for the plan and `docs/engine-design.md` for the engine.

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
  include/archivum/server/  config.h oidc.h app.h
  src/               config.cpp app.cpp main.cpp auth/oidc.cpp http/json_bridge.cpp
engine/            storage engine library (archivum_engine)
  include/archivum/  status.h crc32c.h vfs.h journal.h engine/db.h engine/page_format.h
  src/               crc32c.cpp journal.cpp vfs_posix.cpp vfs_win32.cpp engine/{db,wal,page_format}.cpp
  testing/           test-only doubles: MemVfs, FaultVfs (the crash shim)
tests/
  support/           minimal test framework (no dependency)
  unit/              per-component tests
  crash/             crash-injection tests (the Stage 0 gate)
  server/            test PKI, test issuer, integration and saturation tests (Stage 1)
config/            archivum.example.json
docs/                design, formats, durability model, testing model
.github/workflows/   CI: Windows (MSVC 2022) and Linux (Ubuntu 22.04, GCC 12)
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
