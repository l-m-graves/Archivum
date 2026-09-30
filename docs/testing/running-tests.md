# Running the tests

**One command, and nothing else:**

```
ctest --preset <preset>          # CI presets: linux-debug, linux-release, linux-tsan, windows-debug, windows-release
ctest --test-dir build/local-linux    # a local build tree
```

`ctest` builds first. Every test in `tests/CMakeLists.txt` requires the
fixture `archivum_build`, whose setup test `build_is_current` runs
`cmake --build` on the whole tree, unconditionally, before anything else
starts. It is a no-op when the tree is current and a rebuild when it is
not, so `ctest`, `ctest -R name`, `ctest -L concurrency` and `ctest -j` all
run binaries built from the source on disk. CTest adds the fixture's setup
test itself when a filter would have left it out.

Why: a server test once "passed" because a header edit had not landed and
the old binary ran. A result from a binary older than the source it claims
to test is not a result, and local runs, which are most runs, were
exposed. Do not run a test executable by hand to get a result you will
quote; if you need its output, `ctest -R name -V` gives it.

## The time zone database test

`tzcheck_embedded_database` runs the real embedded database against its
pins and **fails while the database is the empty placeholder**.
`ARCHIVUM_TZDATA_ALLOW_EMPTY` (default OFF) exists for local development on
a machine without the vendored IANA release (`third_party/tzdata/`); the
local presets set it and configure prints a warning. CI and production
builds never set it, and combining it with `ARCHIVUM_PRODUCTION_BUILD` is a
configure error. A production build also runs `tzcheck` after linking it,
so no production binary exists without the database.

## Iteration counts

The crash and model tests read `ARCHIVUM_CRASH_ITERS`; CI sets it per job
(1000 debug, 200 tsan, 10000 release). `ARCHIVUM_SEED` fixes the base seed.

## ThreadSanitizer

`linux-tsan` runs **the whole suite**, not only the tests labelled
`concurrency`: a race in a path nobody labelled concurrent is the one the
label would never have found. The crash and model tests shrink to 200
iterations under it, because they are single-threaded and ThreadSanitizer
only slows them; the `concurrency`-labelled tests do not read
`ARCHIVUM_CRASH_ITERS` and run at their own full counts. Runtime, CI run 39
(`ubuntu-latest`, gcc 12): the test step took **7 min 38 s** (ctest wall
457.9 s; the concurrency-labelled tests 274 s of process time). Before the
change, with the label filter, the same step took 4 min 23 s (run 37). The
job's whole length is about 11 min, of which configure and build are 3.5.
Locally the full suite takes about 150 s at `-j2` under the local
sanitizers.
