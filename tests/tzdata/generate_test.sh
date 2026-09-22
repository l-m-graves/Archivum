#!/bin/sh
# Regenerates the synthetic release's embedded database and pinned
# transitions into a temporary directory and diffs them against the
# checked-in copies. Linux only: needs zic and zdump.
set -eu
src="$1"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 "$src/tools/tzdata/generate.py" --tarball "$src/tests/tzdata/synthetic-2000a.tar.gz" \
  --sha256 "$(cat "$src/tests/tzdata/synthetic-2000a.sha256")" --out "$out" \
  --pinned-zones "$src/tests/tzdata/pinned-zones.txt"
for f in embedded_tzdata.cpp pinned-transitions.txt VERSION; do
  diff -u "$src/tests/tzdata/generated/$f" "$out/$f"
done
echo "tzdata_generate_test: regeneration is byte-identical"
