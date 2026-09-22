# Synthetic time zone release for the tooling's tests

`synthetic-2000a.tar.gz` is a two-zone release in IANA's source form
(`version`, `tzdata.zi`) written for this repository: `Test/Pacific`
carries the North American rule (second Sunday of March, first Sunday of
November, at 02:00) on a -8 h base, and `Test/UTC` has no transitions. It
makes no claim about any real place and is never embedded in the binary.
It exists so that the generator, the reader and the build-time check are
proven against real `zic` and `zdump` output on every build, with or
without the vendored IANA release:

- `tzdata_generate_test` (Linux, needs zic and zdump) regenerates
  `generated/` from the tarball and diffs it: the pipeline is
  deterministic and the checked-in output is what the tool produces;
- `tzcheck_synthetic` links the generated database and runs the
  build-time assertion against its pinned transitions on every platform.

The real database is `modules/punchline/tzdata/`, produced the same way
from the IANA tarball in `third_party/tzdata/`.
