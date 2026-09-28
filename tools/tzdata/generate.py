#!/usr/bin/env python3
"""Embed the IANA time zone database in the Archivum binary.

    python3 tools/tzdata/generate.py \
        --tarball third_party/tzdata/tzdata2026d.tar.gz --sha256 <published SHA-256> \
        --signature third_party/tzdata/tzdata2026d.tar.gz.asc --fingerprint <key fingerprint> \
        --tzcode third_party/tzdata/tzcode2026d.tar.gz --tzcode-sha256 <published SHA-256> \
        --tzcode-signature third_party/tzdata/tzcode2026d.tar.gz.asc \
        --out modules/punchline/tzdata

Steps, each of which fails the run rather than continuing on a doubt:

1. The tarball's SHA-256 must equal --sha256 (the value published by IANA
   and recorded in docs/toolchain.md). A distribution's copy of the data
   is never accepted here; the evidence rule wants the upstream artifact.
   With --signature, the detached signature must verify under gpg and the
   signing key's fingerprint must equal --fingerprint (from the tz-announce
   message); "verified by some key" is not enough.
2. The tarball must contain `version` (the release name) and `tzdata.zi`
   (the single-file source form IANA has shipped since 2018).
3. zic and zdump come from the tzcode tarball of the same release
   (--tzcode, checked the same way): it is extracted and `make zic zdump`
   is run, and the built zic must report the release name. The build
   host's zic and zdump (Ubuntu: libc-bin) are accepted only with
   --host-tools, which exists for the synthetic release the tests use and
   records the tool version so the choice is visible.
4. The pins are release-scoped: the `release` line in --pinned-zones must
   name the tarball's release. A different release is refused unless
   --new-release is given, in which case the line is rewritten, the pinned
   transitions are regenerated and the transition diff is printed; the
   commit carrying the new pins is where that diff is reviewed. Without
   --new-release, a pin file that would change at all (same release,
   different output: a tool difference, a non-determinism) fails the run.
5. `zic -b slim -d <tmp>` compiles tzdata.zi. Slim output relies on the
   TZif footer rule for instants past the last explicit transition; the
   reader (modules/punchline/src/tzif.cpp) implements it, and the
   build-time check proves it against zdump.
6. Every compiled zone is written into embedded_tzdata.cpp as a byte
   array, in sorted name order, with the release name.
7. For every zone in pinned-zones.txt, `zdump -v` over the pinned window
   is normalised into pinned-transitions.txt: a header naming the release,
   then one line per transition,
   "zone<TAB>instant<TAB>utoff_after<TAB>isdst_after<TAB>abbrev_after".
   tzcheck asserts at build time, on every platform, that the embedded
   database is that release and reproduces every line.

Regeneration must be byte-identical for the same tarballs; CI checks that.
"""
from __future__ import annotations

import argparse
import difflib
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
RELEASE_RE = re.compile(r"\d{4}[a-z]")


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


def check_digest(path: Path, expected: str, what: str) -> str:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected.lower():
        raise SystemExit(f"SHA-256 mismatch: {what} {path} is {digest}, expected {expected}")
    return digest


def normalise_fingerprint(text: str) -> str:
    return re.sub(r"[^0-9A-Fa-f]", "", text).upper()


def verify_signature(gpg: str, gnupghome: str | None, path: Path, signature: Path, fingerprint: str, what: str) -> str:
    """Runs gpg --verify and requires a VALIDSIG by exactly the given key."""
    want = normalise_fingerprint(fingerprint)
    if len(want) != 40:
        raise SystemExit(f"--fingerprint must be a 40-hex-digit key fingerprint, got {fingerprint!r}")
    cmd = [gpg, "--batch", "--status-fd", "1"]
    if gnupghome:
        cmd += ["--homedir", gnupghome]
    cmd += ["--verify", str(signature), str(path)]
    run = subprocess.run(cmd, capture_output=True, text=True)
    valid = [l for l in run.stdout.splitlines() if l.startswith("[GNUPG:] VALIDSIG ")]
    if run.returncode != 0 or not valid:
        raise SystemExit(f"{what}: signature {signature} does not verify:\n{run.stderr.strip()}")
    # "[GNUPG:] VALIDSIG <fpr> <date> <ts> <exp> <ver> <res> <pk-algo> <hash-algo> <class> <primary-fpr>"
    fields = valid[0].split()
    signing, primary = fields[2].upper(), fields[-1].upper()
    if want not in (signing, primary):
        raise SystemExit(f"{what}: signed by key {signing} (primary {primary}), not the announced key {want}")
    return signing


def extract(tarball: Path, into: Path) -> list[str]:
    with tarfile.open(tarball) as tf:
        names = tf.getnames()
        tf.extractall(into, filter="data")
    return names


def read_release(path: Path) -> str:
    text = path.read_text()
    m = re.search(r"^\s*#?\s*release\s+(\S+)\s*$", text, re.M)
    if not m:
        raise SystemExit(f"{path} has no 'release <name>' line; the pins must be scoped to a release")
    return m.group(1)


def tool_version(exe: str) -> str:
    run = subprocess.run([exe, "--version"], capture_output=True, text=True)
    text = (run.stdout or run.stderr).strip().splitlines()
    return text[0] if text else "(no --version output)"


def build_tzcode(tzcode: Path, into: Path, version: str) -> tuple[str, str]:
    """Extracts the tzcode release and builds zic and zdump from it."""
    src = into / "tzcode"
    src.mkdir()
    names = extract(tzcode, src)
    if "version" not in names or "Makefile" not in names:
        raise SystemExit("tzcode tarball lacks 'version' or 'Makefile'; not an IANA tzcode release")
    code_version = (src / "version").read_text().strip()
    if code_version != version:
        raise SystemExit(f"tzcode release {code_version!r} does not match tzdata release {version!r}")
    make = shutil.which("make")
    if not make:
        raise SystemExit("make is required to build zic and zdump from the tzcode release")
    run = subprocess.run([make, "-C", str(src), "zic", "zdump"], capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit(f"building zic and zdump from {tzcode} failed:\n{run.stdout[-4000:]}\n{run.stderr[-4000:]}")
    zic, zdump = src / "zic", src / "zdump"
    if not zic.is_file() or not zdump.is_file():
        raise SystemExit("tzcode build produced no zic or zdump")
    for exe in (zic, zdump):
        reported = tool_version(str(exe))
        if version not in reported:
            raise SystemExit(f"{exe.name} built from tzcode reports {reported!r}, which does not name release {version}")
    return str(zic), str(zdump)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarball", required=True, help="tzdata<release>.tar.gz as published by IANA")
    ap.add_argument("--sha256", required=True, help="its published SHA-256")
    ap.add_argument("--signature", help="tzdata<release>.tar.gz.asc; requires --fingerprint")
    ap.add_argument("--tzcode", help="tzcode<release>.tar.gz; zic and zdump are built from it")
    ap.add_argument("--tzcode-sha256", help="its published SHA-256")
    ap.add_argument("--tzcode-signature", help="tzcode<release>.tar.gz.asc")
    ap.add_argument("--fingerprint", help="the signing key's fingerprint from the tz-announce message")
    ap.add_argument("--gnupghome", help="gpg home directory holding the signing key (default: gpg's own)")
    ap.add_argument("--gpg", default=shutil.which("gpg"))
    ap.add_argument("--host-tools", action="store_true", help="use zic and zdump from PATH (tests only)")
    ap.add_argument("--zic", default=shutil.which("zic"), help="with --host-tools")
    ap.add_argument("--zdump", default=shutil.which("zdump"), help="with --host-tools")
    ap.add_argument("--out", required=True, help="modules/punchline/tzdata")
    ap.add_argument("--pinned-zones", default=str(HERE / "pinned-zones.txt"))
    ap.add_argument("--new-release", action="store_true",
                    help="the vendored release is being changed: rewrite the pins' release line and print the transition diff")
    a = ap.parse_args()

    if a.signature and not a.fingerprint:
        raise SystemExit("--signature requires --fingerprint")
    if (a.tzcode is None) == (not a.host_tools):
        raise SystemExit("give exactly one of --tzcode <tzcode tarball> (the release's own zic and zdump) or --host-tools (tests only)")
    if a.tzcode and not a.tzcode_sha256:
        raise SystemExit("--tzcode requires --tzcode-sha256")
    if a.tzcode_signature and not a.fingerprint:
        raise SystemExit("--tzcode-signature requires --fingerprint")

    tarball = Path(a.tarball)
    digest = check_digest(tarball, a.sha256, "tzdata")
    signed_by = ""
    if a.signature:
        if not a.gpg:
            raise SystemExit("gpg is required to verify --signature")
        signed_by = verify_signature(a.gpg, a.gnupghome, tarball, Path(a.signature), a.fingerprint, "tzdata")
    tzcode_digest = ""
    if a.tzcode:
        tzcode_digest = check_digest(Path(a.tzcode), a.tzcode_sha256, "tzcode")
        if a.tzcode_signature:
            verify_signature(a.gpg, a.gnupghome, Path(a.tzcode), Path(a.tzcode_signature), a.fingerprint, "tzcode")

    pinned_path = Path(a.pinned_zones)
    pinned_release = read_release(pinned_path)

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "src"
        src.mkdir()
        names = extract(tarball, src)
        if "version" not in names or "tzdata.zi" not in names:
            raise SystemExit("tarball lacks 'version' or 'tzdata.zi'; not an IANA tzdata release")
        version = (src / "version").read_text().strip()
        if not RELEASE_RE.fullmatch(version):
            raise SystemExit(f"unexpected release name {version!r}")
        zi_version = (src / "tzdata.zi").read_text().splitlines()[0]
        if zi_version.strip() != f"# version {version}":
            raise SystemExit(f"tzdata.zi header {zi_version!r} does not name release {version}")

        if version != pinned_release and not a.new_release:
            raise SystemExit(f"the pins in {pinned_path} are scoped to release {pinned_release}, the tarball is {version}; "
                             "rerun with --new-release to change the vendored release and review the transition diff")

        if a.tzcode:
            zic, zdump = build_tzcode(Path(a.tzcode), Path(tmp), version)
            tools = f"zic and zdump built from tzcode {version} (SHA-256 {tzcode_digest})"
        else:
            if not a.zic or not a.zdump:
                raise SystemExit("--host-tools: zic and zdump must be on PATH (Ubuntu: libc-bin)")
            zic, zdump = a.zic, a.zdump
            # The host tools' version string is not recorded in the output:
            # it varies by distribution patch level and the output must be
            # byte-identical across hosts. It is printed instead.
            tools = "HOST zic and zdump, not the release's own: tests only"
            print(f"warning: {tools} ({tool_version(zic)}; {tool_version(zdump)})", file=sys.stderr)

        compiled = Path(tmp) / "zoneinfo"
        compiled.mkdir()
        subprocess.run([zic, "-b", "slim", "-d", str(compiled), str(src / "tzdata.zi")], check=True)
        zones = []
        for path in sorted(compiled.rglob("*")):
            if path.is_file():
                data = path.read_bytes()
                if data[:4] != b"TZif":
                    continue
                zones.append((path.relative_to(compiled).as_posix(), data))
        if not zones:
            raise SystemExit("zic produced no zones")

        pinned = [l.strip() for l in pinned_path.read_text().splitlines()
                  if l.strip() and not l.startswith("#") and not l.startswith("release ")]
        zdump_name = tool_version(zdump) if a.tzcode else "host zdump"
        lines = [f"# GENERATED from {version} by {zdump_name}; the build asserts the embedded database is this release and reproduces every line.",
                 f"# release {version}",
                 "# zone\tinstant\tutoff_after\tisdst_after\tabbrev_after"]
        for spec in pinned:
            zone, years = spec.split()
            y0, y1 = (int(x) for x in years.split("-"))
            if not (compiled / zone).is_file():
                raise SystemExit(f"pinned zone {zone} is not in release {version}")
            lines += zdump_transitions(zdump, compiled / zone, zone, y0, y1)

        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        pins_file = out / "pinned-transitions.txt"
        if pins_file.is_file():
            old = [l for l in pins_file.read_text(encoding="utf-8").splitlines() if not l.startswith("#")]
            new = [l for l in lines if not l.startswith("#")]
            if old != new:
                diff = "\n".join(difflib.unified_diff(old, new, f"pinned-transitions.txt ({pinned_release})",
                                                      f"pinned-transitions.txt ({version})", lineterm=""))
                if not a.new_release:
                    raise SystemExit("the pinned transitions would change without a release change; refusing "
                                     "(a tool difference or a non-determinism, both to be understood first):\n" + diff)
                print(f"pinned transitions changed from {pinned_release} to {version}; review this diff in the commit:\n{diff}")
            elif a.new_release:
                print(f"pinned transitions are identical between {pinned_release} and {version}")

        with (out / "embedded_tzdata.cpp").open("w", encoding="utf-8", newline="\n") as f:
            f.write(f"// GENERATED by tools/tzdata/generate.py from the IANA release {version}\n")
            f.write(f"// (tarball SHA-256 {digest}); do not edit. {len(zones)} zones, zic -b slim,\n")
            f.write(f"// {tools}.\n")
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
        pins_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
        record = [f"release {version}", f"tzdata-sha256 {digest}"]
        if signed_by:
            record.append(f"tzdata-signed-by {signed_by}")
        if tzcode_digest:
            record.append(f"tzcode-sha256 {tzcode_digest}")
        record.append(f"tools {tools}")
        (out / "VERSION").write_text("\n".join(record) + "\n", encoding="utf-8")

        if version != pinned_release:
            text = pinned_path.read_text()
            text = re.sub(r"^(\s*#?\s*release\s+)\S+", lambda m: m.group(1) + version, text, count=1, flags=re.M)
            pinned_path.write_text(text)
            print(f"{pinned_path}: release line rewritten from {pinned_release} to {version}")
    print(f"embedded {len(zones)} zones of {version}; {len(lines) - 3} pinned transitions; {tools}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
