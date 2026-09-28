#!/bin/sh
# The rewritten Punchline client against Archivum, end to end (Stage 6-C
# gate: "the contract suite run from the real client against Archivum").
#
#   tests/e2e/client_e2e.sh <archivum build dir> <punchline client build dir>
#
# Starts the end-to-end server (tests/e2e/e2e_server.cpp: the test fixture's
# Archivum with its own PKI, an employee, an open period, and a device
# enrolled for it), then drives punchline-cli, the console front of the
# client's sync layer, through the scenarios the real client can produce:
#
#   1. enrollment with a wrong credential is refused and nothing is kept;
#   2. enrollment with the issued credential heartbeats and learns the
#      employee and the site zone;
#   3. punches are recorded offline-first and delivered: the server stores
#      them under the device's employee with the journal's replay key, and
#      the timesheet shows them (valid_batch_is_accepted);
#   4. a resend after a lost acknowledgement is accepted again without a
#      second row (replay_of_the_same_batch_is_idempotent): the data
#      directory is snapshotted before a sync and restored after it, so the
#      client sends the same punches again from the same journal;
#   5. a revoked device is refused and the client stops resending
#      (unauthenticated_batch_is_refused);
#   6. the client never sends an employee id: every request body the server
#      logged is checked for the key.
#
# The scenarios the client cannot produce by construction (a malformed
# uuid, a duplicate uuid in one batch, a missing field, an asserted
# employee id) are the server's contract tests (tests/server/server_contract_test.cpp)
# and the client's own payload tests; this run proves the real client on
# the real server for the rest.
set -eu
archivum_build="$1"
client_build="$2"
work=$(mktemp -d)
trap 'kill $server_pid 2>/dev/null || true; rm -rf "$work"' EXIT
cli="$client_build/tools/punchline-cli/punchline-cli"
test -x "$cli" || { echo "punchline-cli not built at $cli"; exit 1; }

"$archivum_build/tests/e2e_server" > "$work/server.json" &
server_pid=$!
for i in $(seq 1 100); do
  if [ -s "$work/server.json" ] && grep -q '"ready": *true' "$work/server.json"; then break; fi
  sleep 0.2
done
grep -q '"ready": *true' "$work/server.json" || { echo "e2e server did not start"; cat "$work/server.json"; exit 1; }
field() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" "$work/server.json" "$1"; }
url=$(field url); ca=$(field ca_file); credential=$(field credential); device_uuid=$(field device_uuid)
admin=$(field admin_bearer); employee=$(field employee_number); log=$(field log_file)
echo "e2e: server at $url, device $device_uuid for employee $employee"

data="$work/device"; mkdir -p "$data"
jq_() { python3 -c "import json,sys; d=json.load(sys.stdin); print(d$1)"; }

# 1. wrong credential: refused, nothing kept
if "$cli" enroll --data "$data" --server "$url" --credential "$device_uuid:not-the-secret" --ca "$ca" > "$work/out"; then
  echo "FAIL: enrollment with a wrong credential succeeded"; exit 1
fi
grep -q '"heartbeat (enrollment not kept)"' "$work/out"
test ! -e "$data/device.credential"
echo "e2e: 1. wrong credential refused, nothing kept"

# 2. the issued credential
"$cli" enroll --data "$data" --server "$url" --credential "$credential" --ca "$ca" > "$work/out"
test "$(jq_ '["employee_number"]' < "$work/out")" = "$employee"
test "$(jq_ '["site_zone"]' < "$work/out")" = "America/Los_Angeles"
echo "e2e: 2. enrolled; heartbeat named $employee in America/Los_Angeles"

# 3. punches, offline first, then delivered
"$cli" punch --data "$data" in --note "e2e shift" > "$work/p1"
"$cli" punch --data "$data" out > "$work/p2"
in_uuid=$(jq_ '["entry_uuid"]' < "$work/p1"); out_uuid=$(jq_ '["entry_uuid"]' < "$work/p2")
test "$(jq_ '["pending_punches"]' < "$work/p2")" = "2"
test "$(jq_ '["clocked_in"]' < "$work/p2")" = "False"
"$cli" sync --data "$data" --ca "$ca" > "$work/s1"
test "$(jq_ '["state"]' < "$work/s1")" = "idle"
test "$(jq_ '["accepted"]' < "$work/s1")" = "2"
test "$(jq_ '["pending_punches"]' < "$work/s1")" = "0"
curl -sS --cacert "$ca" -H "Authorization: $admin" "$url/api/v1/timesheets/1/$employee" > "$work/ts"
python3 - "$work/ts" "$in_uuid" "$out_uuid" <<'PY'
import json, sys
ts = json.load(open(sys.argv[1]))
entries = {e["entry_uuid"]: e for e in ts["entries"]}
for u in sys.argv[2:]:
    assert u in entries, f"{u} not on the timesheet"
    e = entries[u]
    assert e["attestation"] == "device", e
    assert e["state"] == "recorded", e
    assert e.get("journal_sequence") in (1, 2), e
assert len(entries) == 2, entries
assert entries[sys.argv[2]]["note"] == "e2e shift"
assert len(ts["shifts"]) == 1 and ts["shifts"][0]["out_entry_id"], ts["shifts"]
print("timesheet: 2 device-attested entries, 1 closed shift, journal sequences 1 and 2")
PY
echo "e2e: 3. punches delivered and on the timesheet"

# 4. resend after a lost acknowledgement: no second row
"$cli" punch --data "$data" in > /dev/null
cp -r "$data" "$work/snapshot"
"$cli" sync --data "$data" --ca "$ca" > "$work/s2"
test "$(jq_ '["accepted"]' < "$work/s2")" = "1"
rm -rf "$data"; cp -r "$work/snapshot" "$data"     # the acknowledgement never reached the disk
"$cli" sync --data "$data" --ca "$ca" > "$work/s3"
test "$(jq_ '["accepted"]' < "$work/s3")" = "1"
test "$(jq_ '["state"]' < "$work/s3")" = "idle"
curl -sS --cacert "$ca" -H "Authorization: $admin" "$url/api/v1/timesheets/1/$employee" > "$work/ts2"
test "$(jq_ '["entries"].__len__()' < "$work/ts2")" = "3"
echo "e2e: 4. the same punch resent from the restored journal: accepted again, 3 rows not 4"
"$cli" heartbeat --data "$data" --ca "$ca" > "$work/hb"
test "$(jq_ '["server_clocked_in"]' < "$work/hb")" = "True"
test "$(jq_ '["clocked_in"]' < "$work/hb")" = "True"
echo "e2e: 4b. the heartbeat says the employee is clocked in by the server's fold"

# 5. revoked device: refused, resending stops
curl -sS --cacert "$ca" -H "Authorization: $admin" -H "Content-Type: application/json" \
  -d '{"reason":"e2e"}' "$url/api/v1/admin/devices/$device_uuid/revoke" > /dev/null
"$cli" punch --data "$data" out > /dev/null
set +e; "$cli" sync --data "$data" --ca "$ca" > "$work/s4"; rc=$?; set -e
test "$rc" = "3"
test "$(jq_ '["state"]' < "$work/s4")" = "credential_rejected"
test "$(jq_ '["pending_punches"]' < "$work/s4")" = "1"
echo "e2e: 5. revoked device refused; the punch stays on the device"

# 6. no employee id ever left the device
if grep -q "employee_id_asserted\|employee_id_rejection" "$log"; then
  echo "FAIL: the server saw an employee id from the client"; exit 1
fi
count=$(grep -c '"sync.batch"' "$log" || true)
test "$count" -ge 3
echo "e2e: 6. $count batches logged, no employee id asserted"
echo "client_e2e: PASS"
