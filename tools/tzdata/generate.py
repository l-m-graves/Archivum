#!/usr/bin/env python3
"""Embed the IANA time zone database in the Archivum binary.

    python3 tools/tzdata/generate.py --tarball third_party/tzdata/tzdata2025b.tar.gz \
        --sha256 <published SHA-256> --out modules/punchline/tzdata

Steps, each of which fails the run rather than continuing on a doubt:

1. The tarball's SHA-256 must equal --sha256 (the value published by IANA
   and recorded in docs/toolchain.md). A distribution's copy of the data
   is never accepted here; the evidence rule wants the upstream artifact.
2. The tarball must contain `version` (the release name) and `tzdata.zi`
   (the single-file source form IANA has shipped since 2018).
3. `zic -b slim -d <tmp>` compiles tzdata.zi. Slim output relies on the
   TZif footer rule for instants past the last explicit transition; the
   reader (modules/punchline/src/tzif.cpp) implements it, and the
   build-time check proves it against zdump.
4. Every compiled zone is written into embedded_tzdata.cpp as a byte
   array, in sorted name order, with the release name.
5. For every zone in pinned-zones.txt, `zdump -v` over the pinned window
   is normalised into pinned-transitions.txt: one line per transition,
   "zone<TAB>instant<TAB>utoff_after<TAB>isdst_after<TAB>abbrev_after".
   The tzcheck tool asserts at build time, on every platform, that the
   embedded bytes reproduce every line; on Linux it also re-runs zdump on
   the embedded bytes and diffs.

Needs zic and zdump on PATH (Ubuntu: libc-bin). Regeneration must be
byte-identical for the same tarball; CI checks that.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MONTHS = {m: i + 1 for i, m in enumerate(["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"])}


def days_from_civil(y: int, m: int, d: int) -> int:
    y -= m <= 2
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def parse_zdump_ut(text: str) -> int:
    # "Sun Mar  8 09:59:59 2026 UT"
    m = re.match(r"\w{3}\s+(\w{3})\s+(\d+)\s+(\d+):(\d+):(\d+)\s+(-?\d+)\s+UT", text)
    if not m:
        raise SystemExit(f"cannot parse zdump time: {text!r}")
    mon, day, hh, mm, ss, year = m.groups()
    return days_from_civil(int(year), MONTHS[mon], int(day)) * 86400 + int(hh) * 3600 + int(mm) * 60 + int(ss)


def zdump_transitions(zdump: str, tzif_path: Path, zone: str, year_from: int, year_to: int) -> list[str]:
    out = subprocess.run([zdump, "-v", "-c", f"{year_from},{year_to}", str(tzif_path)], check=True, capture_output=True, text=True).stdout
    lines = []
    prev = None
    for line in out.splitlines():
        # "<path>  Sun Mar  8 10:00:00 2026 UT = Sun Mar  8 03:00:00 2026 PDT isdst=1 gmtoff=-25200"
        m = re.match(r"^(\S+)\s+(.+?\d{4} UT) = (.+?) (\S+) isdst=(\d) gmtoff=(-?\d+)$", line)
        if not m:
            continue
        instant = parse_zdump_ut(m.group(2))
        abbrev, isdst, gmtoff = m.group(4), int(m.group(5)), int(m.group(6))
        state = (gmtoff, isdst, abbrev)
        # zdump prints the second before and the second of each transition; keep the "of".
        if prev is not None and prev[0] == instant - 1 and prev[1] != state:
            lines.append(f"{zone}\t{instant}\t{gmtoff}\t{isdst}\t{abbrev}")
        prev = (instant, state)
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarball", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--out", required=True, help="modules/punchline/tzdata")
    ap.add_argument("--pinned-zones", default=str(HERE / "pinned-zones.txt"))
    ap.add_argument("--zic", default=shutil.which("zic"))
    ap.add_argument("--zdump", default=shutil.which("zdump"))
    a = ap.parse_args()
    if not a.zic or not a.zdump:
        raise SystemExit("zic and zdump are required (Ubuntu: libc-bin)")
    tarball = Path(a.tarball)
    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if digest != a.sha256.lower():
        raise SystemExit(f"SHA-256 mismatch: {tarball} is {digest}, expected {a.sha256}")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "src"
        src.mkdir()
        with tarfile.open(tarball) as tf:
            names = tf.getnames()
            if "version" not in names or "tzdata.zi" not in names:
                raise SystemExit("tarball lacks 'version' or 'tzdata.zi'; not an IANA tzdata release")
            tf.extractall(src, filter="data")
        version = (src / "version").read_text().strip()
        if not re.fullmatch(r"\d{4}[a-z]", version):
            raise SystemExit(f"unexpected release name {version!r}")
        zi_version = (src / "tzdata.zi").read_text().splitlines()[0]
        if zi_version.strip() != f"# version {version}":
            raise SystemExit(f"tzdata.zi header {zi_version!r} does not name release {version}")
        compiled = Path(tmp) / "zoneinfo"
        compiled.mkdir()
        subprocess.run([a.zic, "-b", "slim", "-d", str(compiled), str(src / "tzdata.zi")], check=True)
        zones = []
        for path in sorted(compiled.rglob("*")):
            if path.is_file():
                data = path.read_bytes()
                if data[:4] != b"TZif":
                    continue
                zones.append((path.relative_to(compiled).as_posix(), data))
        if not zones:
            raise SystemExit("zic produced no zones")
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        with (out / "embedded_tzdata.cpp").open("w", encoding="utf-8", newline="\n") as f:
            f.write(f"// GENERATED by tools/tzdata/generate.py from the IANA release {version}\n")
            f.write(f"// (tarball SHA-256 {digest}); do not edit. {len(zones)} zones, zic -b slim.\n")
            f.write('#include "archivum/punchline/tzif.h"\n\nnamespace archivum::punchline::tz {\nnamespace {\n\n')
            for i, (name, data) in enumerate(zones):
                f.write(f"// {name}\nconst unsigned char z{i}[] = {{")
                for k in range(0, len(data), 24):
                    f.write("\n  " + ",".join(f"{b}" for b in data[k:k + 24]) + ",")
                f.write("\n};\n")
            f.write("\nconst EmbeddedZone kZones[] = {\n")
            for i, (name, data) in enumerate(zones):
                f.write(f'  {{"{name}", z{i}, {len(data)}}},\n')
            f.write("};\n\n}  // namespace\n\n")
            f.write("const EmbeddedDatabase& embedded_database() {\n")
            f.write(f'  static const EmbeddedDatabase db{{"{version}", kZones, {len(zones)}}};\n  return db;\n}}\n\n')
            f.write("}  // namespace archivum::punchline::tz\n")
        pinned = [l.strip() for l in Path(a.pinned_zones).read_text().splitlines() if l.strip() and not l.startswith("#")]
        lines = [f"# GENERATED from {version} by zdump; the build asserts the embedded database reproduces every line.",
                 "# zone\tinstant\tutoff_after\tisdst_after\tabbrev_after"]
        for spec in pinned:
            zone, years = spec.split()
            y0, y1 = (int(x) for x in years.split("-"))
            if not (compiled / zone).is_file():
                raise SystemExit(f"pinned zone {zone} is not in release {version}")
            lines += zdump_transitions(a.zdump, compiled / zone, zone, y0, y1)
        (out / "pinned-transitions.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        (out / "VERSION").write_text(f"{version}\n{digest}\n", encoding="utf-8")
    print(f"embedded {len(zones)} zones of {version}; {len(lines) - 2} pinned transitions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
