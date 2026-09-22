# The vendored IANA time zone database

Put the IANA release here, exactly as published at
`https://data.iana.org/time-zones/releases/`: `tzdata<release>.tar.gz`
and its detached signature `tzdata<release>.tar.gz.asc`. Record the
release name, the tarball's SHA-256 and the signature verification result
in `docs/toolchain.md`. Then:

```
python3 tools/tzdata/generate.py --tarball third_party/tzdata/tzdata<release>.tar.gz \
    --sha256 <that SHA-256> --out modules/punchline/tzdata
```

which replaces the placeholder `modules/punchline/tzdata/embedded_tzdata.cpp`
and writes `pinned-transitions.txt` from zdump for every zone in
`tools/tzdata/pinned-zones.txt`. Set `ARCHIVUM_TZDATA_ALLOW_EMPTY` to OFF
in `CMakePresets.json` at the same time, so a build without the database
fails.

A distribution's copy of the data (Ubuntu's `tzdata` package, the host's
`/usr/share/zoneinfo`) is never accepted in place of the release: the
generator checks the SHA-256 you pass, and that value comes from IANA.
Nothing is in this directory until the release is committed.
