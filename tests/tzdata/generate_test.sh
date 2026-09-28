#!/bin/sh
# The generator against the synthetic release (tests/tzdata/README.md).
# Linux only: needs zic, zdump, gpg and make.
#
# 1. Regeneration with the host tools is byte-identical to the checked-in
#    output (the pipeline is deterministic).
# 2. A pinned-zones file scoped to another release is refused, and the same
#    tarball is accepted again with --new-release, which rewrites the
#    release line and prints the (empty here) transition diff.
# 3. Pins that would drift without a release change are refused.
# 4. A detached signature is verified against the announced fingerprint: a
#    throwaway key signs the tarball in a temporary keyring, the right
#    fingerprint passes, a wrong one is refused.
# 5. zic and zdump are built from a tzcode tarball of the release: a
#    synthetic tzcode whose Makefile builds wrappers that report the release
#    proves the extraction, build, version check and use; a tzcode of another
#    release is refused.
set -eu
src="$1"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gen="$src/tools/tzdata/generate.py"
tarball="$src/tests/tzdata/synthetic-2000a.tar.gz"
sha=$(cat "$src/tests/tzdata/synthetic-2000a.sha256")

# 1
mkdir "$out/gen"
python3 "$gen" --tarball "$tarball" --sha256 "$sha" --out "$out/gen" \
  --pinned-zones "$src/tests/tzdata/pinned-zones.txt" --host-tools
for f in embedded_tzdata.cpp pinned-transitions.txt VERSION; do
  diff -u "$src/tests/tzdata/generated/$f" "$out/gen/$f"
done
echo "tzdata_generate_test: regeneration is byte-identical"

# 2
cp "$src/tests/tzdata/pinned-zones-wrong-release.txt" "$out/pins.txt"
mkdir "$out/gen2"
if python3 "$gen" --tarball "$tarball" --sha256 "$sha" --out "$out/gen2" --pinned-zones "$out/pins.txt" --host-tools 2>"$out/err"; then
  echo "FAIL: a release the pins are not scoped to was accepted"; exit 1
fi
grep -q "scoped to release 2000b" "$out/err"
python3 "$gen" --tarball "$tarball" --sha256 "$sha" --out "$out/gen2" --pinned-zones "$out/pins.txt" --host-tools --new-release >"$out/log"
grep -q "^release 2000a$" "$out/pins.txt"
grep -q "release line rewritten from 2000b to 2000a" "$out/log"
diff -u "$src/tests/tzdata/generated/pinned-transitions.txt" "$out/gen2/pinned-transitions.txt"
echo "tzdata_generate_test: release scoping refused and then rewritten with --new-release"

# 3
mkdir "$out/gen3"
sed 's/^Test\/Pacific\t1583661600\t.*/Test\/Pacific\t1583661600\t-25200\t1\tXDT/' \
  "$src/tests/tzdata/generated/pinned-transitions.txt" > "$out/gen3/pinned-transitions.txt"
if python3 "$gen" --tarball "$tarball" --sha256 "$sha" --out "$out/gen3" \
  --pinned-zones "$src/tests/tzdata/pinned-zones.txt" --host-tools 2>"$out/err"; then
  echo "FAIL: drifting pins without a release change were accepted"; exit 1
fi
grep -q "would change without a release change" "$out/err"
grep -q "XDT" "$out/gen3/pinned-transitions.txt"   # nothing was written
echo "tzdata_generate_test: pin drift without a release change is refused"

# 4
export GNUPGHOME="$out/gnupg"
mkdir -m 700 "$GNUPGHOME"
gpg --batch --quiet --pinentry-mode loopback --passphrase '' --quick-generate-key "tz test <tz@example.invalid>" ed25519 sign never 2>/dev/null
fpr=$(gpg --batch --with-colons --fingerprint tz@example.invalid | awk -F: '/^fpr:/ {print $10; exit}')
gpg --batch --quiet --pinentry-mode loopback --passphrase '' --armor --detach-sign --output "$out/tarball.asc" "$tarball"
mkdir "$out/gen4"
python3 "$gen" --tarball "$tarball" --sha256 "$sha" --signature "$out/tarball.asc" --fingerprint "$fpr" \
  --gnupghome "$GNUPGHOME" --out "$out/gen4" --pinned-zones "$src/tests/tzdata/pinned-zones.txt" --host-tools >/dev/null
grep -q "^tzdata-signed-by $fpr$" "$out/gen4/VERSION"
wrong="0123456789ABCDEF0123456789ABCDEF01234567"
if python3 "$gen" --tarball "$tarball" --sha256 "$sha" --signature "$out/tarball.asc" --fingerprint "$wrong" \
  --gnupghome "$GNUPGHOME" --out "$out/gen4" --pinned-zones "$src/tests/tzdata/pinned-zones.txt" --host-tools 2>"$out/err"; then
  echo "FAIL: a signature by another key was accepted"; exit 1
fi
grep -q "not the announced key" "$out/err"
echo "tzdata_generate_test: signature verified against the announced fingerprint, another key refused"

# 5
mktzcode() {  # $1 release name, $2 output tarball
  d="$out/tzcode-$1"; mkdir -p "$d"
  printf '%s\n' "$1" > "$d/version"
  cat > "$d/Makefile" <<MK
zic zdump:
	printf '#!/bin/sh\\ncase "\$\$1" in --version) echo "\$@ (tz) \$(shell cat version)";; *) exec \$@ "\$\$@";; esac\\n' > \$@
	chmod +x \$@
MK
  tar -C "$d" -czf "$2" version Makefile
}
mktzcode 2000a "$out/tzcode-2000a.tar.gz"
mktzcode 2000b "$out/tzcode-2000b.tar.gz"
mkdir "$out/gen5"
python3 "$gen" --tarball "$tarball" --sha256 "$sha" --out "$out/gen5" --pinned-zones "$src/tests/tzdata/pinned-zones.txt" \
  --tzcode "$out/tzcode-2000a.tar.gz" --tzcode-sha256 "$(sha256sum "$out/tzcode-2000a.tar.gz" | cut -d' ' -f1)" >"$out/log"
grep -q "built from tzcode 2000a" "$out/gen5/VERSION"
grep -q "^# release 2000a$" "$out/gen5/pinned-transitions.txt"
grep -q "^# GENERATED from 2000a by zdump (tz) 2000a;" "$out/gen5/pinned-transitions.txt"
grep -v '^#' "$out/gen5/pinned-transitions.txt" > "$out/a"
grep -v '^#' "$src/tests/tzdata/generated/pinned-transitions.txt" > "$out/b"
diff -u "$out/b" "$out/a"
if python3 "$gen" --tarball "$tarball" --sha256 "$sha" --out "$out/gen5" --pinned-zones "$src/tests/tzdata/pinned-zones.txt" \
  --tzcode "$out/tzcode-2000b.tar.gz" --tzcode-sha256 "$(sha256sum "$out/tzcode-2000b.tar.gz" | cut -d' ' -f1)" 2>"$out/err"; then
  echo "FAIL: a tzcode of another release was accepted"; exit 1
fi
grep -q "does not match tzdata release" "$out/err"
echo "tzdata_generate_test: zic and zdump built from the tzcode tarball of the release; another release refused"
