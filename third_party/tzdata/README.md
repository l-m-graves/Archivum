# The vendored IANA time zone database

Put the IANA release here, exactly as published at
`https://data.iana.org/time-zones/releases/`: the data tarball
`tzdata<release>.tar.gz`, the code tarball `tzcode<release>.tar.gz`, and
each one's detached signature (`.asc`). Import the signing key named in
the tz-announce message for the release into gpg and note its
fingerprint from that message. Record the release name, both SHA-256
values, the fingerprint and the verification result in
`docs/toolchain.md`. Then, from the repository root:

```
python3 tools/tzdata/generate.py \
    --tarball third_party/tzdata/tzdata<release>.tar.gz --sha256 <published SHA-256> \
    --signature third_party/tzdata/tzdata<release>.tar.gz.asc --fingerprint <fingerprint> \
    --tzcode third_party/tzdata/tzcode<release>.tar.gz --tzcode-sha256 <published SHA-256> \
    --tzcode-signature third_party/tzdata/tzcode<release>.tar.gz.asc \
    --out modules/punchline/tzdata
```

The generator verifies both SHA-256 values and both signatures against
that one key, builds `zic` and `zdump` from the code tarball (they must
report the release), compiles the data with `zic -b slim`, replaces the
placeholder `modules/punchline/tzdata/embedded_tzdata.cpp`, and writes
`pinned-transitions.txt` from `zdump` for every zone in
`tools/tzdata/pinned-zones.txt`. Set `ARCHIVUM_TZDATA_ALLOW_EMPTY` to OFF
in `CMakePresets.json` at the same time, so a build without the database
fails.

## The pins are scoped to the release

`tools/tzdata/pinned-zones.txt` carries a `release <name>` line and the
generated pin file a `# release <name>` header. `tzcheck` fails the build
when the embedded database is any other release. The generator refuses a
tarball of a release the pins are not scoped to; to change the vendored
release, run it with `--new-release`: it rewrites the release line,
regenerates the pins, and prints the transition diff between the old and
the new pins. Commit the tarballs, the regenerated `modules/punchline/tzdata/`
and the rewritten `pinned-zones.txt` together, and read the diff in that
commit: a transition that moved or vanished for a deployed site zone is
the reason the release is being taken, or a reason not to take it.
Nothing accepts it silently. When the pins would change without a
release change, the generator refuses; that is a tool difference or a
non-determinism to understand first.

A distribution's copy of the data (Ubuntu's `tzdata` package, the host's
`/usr/share/zoneinfo`) and the host's `zic` are never accepted in place
of the release: the generator checks the SHA-256 you pass, and that
value comes from IANA. Nothing is in this directory until the release is
committed.
