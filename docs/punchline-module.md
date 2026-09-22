# Punchline module (Stage 6)

The timesheet workflow behind the schema in `docs/punchline-schema.md`:
sync with idempotent batches, punch pairing, the approval lifecycle, the
exception queue with freshness monitoring and escalation, corrections,
employee self-service, the supervisor queue, the payroll export and the
period audit report. Business logic is engine-only in `modules/punchline/`
(`sync.h`, `lifecycle.h`, `exceptions.h`, `rules.h`, `localtime.h`), tested
without the server in `tests/unit/punchline_sync_test.cpp` and
`punchline_rules_test.cpp`; the routes in
`server/src/modules/punchline_routes.cpp` are thin over it and are tested
over HTTPS in `tests/server/server_punchline_test.cpp`.

## Sync

`POST /api/v1/device/sync`, device credential only:

```
{ "batch_uuid": uuid, "journal_id": uuid, "client_time_us": int?,
  "entries": [ { "entry_uuid": uuid, "journal_sequence": int, "kind": "in"|"out",
                 "device_time_us": int, "local_time": "YYYY-MM-DDTHH:MM:SS",
                 "site_zone": "IANA zone", "tzdb_version": "2024a",
                 "pay_code": text?, "note": text? } ],
  "reports": [ { "kind": "retry_exhausted"|"journal_recovery", "detail": text } ]? }
```

answered by

```
{ "accepted": [uuid], "rejected": [ { "uuid", "reason" } ],
  "acked_sequences": [int], "last_acked_sequence": int, "replayed": bool,
  "clock_divergence_us": int?, "exceptions_opened": int, "server_time_us": int }
```

One batch is one write transaction under one audit row (`sync.batch`,
actor the device). Rules:

- **The employee is the enrollment record's.** An `employee_id` anywhere
  in the body is 400 naming the key, and from a device it is a tamper
  signal (below).
- **Idempotent by entry uuid**: an entry already stored is accepted again
  without a second row (replay after an outage must succeed or the client
  retries forever). The same uuid twice in one batch is one row. A batch
  uuid seen before is answered again with `replayed: true`.
- **`(journal_id, journal_sequence)` is the client journal's replay key**
  (`docs/client-journal.md` in the Punchline repository): a sequence
  number already stored under another uuid is rejected by name, since a
  lying disk can reissue a number and the uuid is the truth.
- **Per-entry outcomes.** A malformed entry, a reused sequence number, an
  unknown pay code, or an entry from another device is rejected with its
  uuid as sent and a reason; the rest of the batch proceeds and is
  acknowledged. Refusal is never per batch: one bad entry cannot block
  the punches behind it. A refused entry is the same refusal on every
  resend, and the queue shows it (`entry_rejected`). Only shape that
  cannot be attributed to an entry (not an object, `entries` not an
  array) refuses the request, and a batch over `max_batch_entries` is 413
  before anything is looked at.
- Every accepted punch is `attestation: device`, `state: recorded`,
  stamped with the server's receipt time and the batch's clock divergence,
  and assigned to the pay period containing its local day (PL-3), or to
  none when no open period covers it.
- The device's `last_seen_at`, `last_acked_sequence` and
  `last_journal_id` advance, and an open `device_stale` item for it is
  resolved.

## Local time

**Ruling (Stage 6, item 1): day assignment must not trust the device's
clock.** Day assignment decides daily and weekly overtime, pay period
membership and which side of a cutoff a punch falls on; a device with a
wrong or changed time zone would move hours between days silently. The
design, approved as option B:

- the IANA time zone database is vendored as the release tarball
  (`third_party/tzdata/`), compiled with `zic` and embedded in the binary
  as byte arrays (`tools/tzdata/generate.py` →
  `modules/punchline/tzdata/embedded_tzdata.cpp`); the reader is ours
  (`archivum/punchline/tzif.h`: TZif versions 1 to 4 with the footer
  POSIX rule, the gap and the fold);
- the server computes local time from `device_time` and the employee's
  `site_zone`, and that result drives day assignment, pairing, schedules
  and every check;
- the device's recorded `local_time` is kept and compared with the
  server's; a difference beyond `local_clock_tolerance_seconds` opens
  `local_clock_mismatch` naming the device;
- `tzdb_version` on every entry and period is the embedded release, not
  anything the client says;
- the build asserts the embedded database against zdump: `tzcheck` runs
  after every build and fails it if the embedded bytes disagree with any
  transition pinned at generation time for the zones in
  `tools/tzdata/pinned-zones.txt`, which must list every deployed site
  zone (the section 11 answer extends it); on Linux the regeneration is
  also diffed byte for byte.

**Status**: the reader, the generator, the build-time check and their
tests are in (`tzif_test` on synthetic fixtures including both 2026
Pacific transitions and the ambiguous hour; `tzcheck_synthetic` and
`tzdata_generate_test` on a synthetic two-zone release compiled by zic
and pinned by zdump, `tests/tzdata/`). The IANA release itself has not
arrived (the attachment did not reach the repository), so the binary
still carries the placeholder, `tzcheck` reports it, and the sync path
has **not** been switched: it still takes the device's wall clock for day
assignment. Switching it is the commit that lands with the release
tarball; no payroll export may run on real data before that. The
`localtime.h` calendar arithmetic (proleptic Gregorian, Sunday-based
weekdays) stays as the day and minute helpers under the zone
computation.

## Pairing (PL-6)

A fold over the employee's current entries in device-time order: an in
punch opens a shift, the next out punch of the same device closes it
(`shifts` row with the duration and the in punch's local day, period and
pay code). An in punch while a shift is open flags the open one
`unpaired_punch` (a missed out); an out punch with none open is flagged
itself; an out from another device is flagged and the open shift stays
open. A shift open at the end of the fold is someone clocked in, not an
exception, until it has been open past `long_shift_hours` (the monitor
re-checks). A closed shift at or over `long_shift_hours` is `long_shift`.

The fold reruns from the earliest new punch whenever a batch, a
late-arriving punch or a correction lands, rebuilding shifts still in
`recorded` or `submitted`. The rebuild range is closed under overlap, so
a shift is wholly rebuilt or wholly kept. Shifts approved or later are
settled: a punch that lands in a settled span is folded on its own and
ends up flagged, so a late sync never fails and never alters what a
supervisor approved. Any punch landing in a period where the employee's
entries are already past `recorded` also opens `late_punch` for the
supervisor, whether or not it pairs, and the item closes when that
employee's period is approved again.

Schedule checks run when a shift closes: no schedule rows in force for
the employee is `no_schedule`; no row for the weekday is
`non_scheduled_day`; an in punch before the start or an out punch after
the end by more than `schedule_tolerance_minutes` is `outside_schedule`
(the out punch is compared only when it is on the same local day).

## The approval lifecycle (PL-7)

```
recorded -> submitted -> approved -> released -> locked
```

per employee per pay period, on the entries and shifts, with one
`approvals` row per transition under the request's audit row. `POST
/api/v1/periods/{id}/submit|approve|release` with
`{employee_number?, note?, override_reason?}`; without `employee_number`
the caller acts for themself. Standing, evaluated against the supervisor
assignment in force at the time of the action:

| Transition | Who |
|---|---|
| submit | the employee, their supervisor, or payroll |
| approve | their supervisor (role `supervisor`); or payroll with a non-empty `override_reason`, which is the documented override of the escalation rule and is recorded in the approvals row |
| release | payroll |
| lock (`POST /api/v1/periods/{id}/lock`, per period) | payroll or admin; every entry in the period must be released |

A transition moves every entry in the predecessor state and refuses to
skip one that is behind: a punch that arrives after submission stays
`recorded` until resubmitted, so approval cannot pass over it. Approval
stamps `approval_audit_id` on the entries and resolves the employee's
`device_attested_count` and `past_cutoff` items: device-attested marking
is resolved through approval, by a named supervisor. Locking sets the
period `locked`; a punch for a locked period is still accepted (never
lost), left unassigned, and flagged `past_cutoff` for a retroactive
adjustment, which is the payroll console's action (Stage 8-P).

**Nothing reaches payroll except by an authenticated transition**: `GET
/api/v1/payroll/periods/{id}` (payroll role; `?format=csv` for the
export) sums hours by employee by pay code over released and locked
shifts only and reports how many shifts it withheld.

## Corrections and manual entries (PL-4)

`POST /api/v1/entries/manual` by the employee's supervisor at the time
or by payroll: `{employee_number, kind, device_time_us, local_time,
site_zone, tzdb_version, reason, correction_of?, pay_code?, note?}`. The
entry is `attestation: manual`, has no journal, and carries the reason.
With `correction_of` the original must be the same employee's, not
already corrected, and not approved or later; it is marked
`superseded_by` the correction, excluded from pairing and payroll, and
never edited or deleted. Pairing is rebuilt from the earlier of the two.

## The exception queue

`exceptions` rows, opened idempotently per (kind, employee, device,
entry, period) while one is open, always through the Recorder of the
action that produced them. Kinds:

| Kind | Opened by | Resolved by |
|---|---|---|
| `unpaired_punch` | pairing; the monitor for a shift open past the threshold | pairing when the punch pairs; a person |
| `long_shift` | pairing | a person |
| `no_schedule`, `non_scheduled_day`, `outside_schedule` | pairing | a person |
| `clock_divergence` | sync, when the batch's clock is off by more than the tolerance | a person |
| `device_attested_count` | sync, at the threshold per employee per period | approval |
| `retry_exhausted`, `journal_recovery` | the client's reports in a batch | a person |
| `device_stale` | the monitor | the next sync or heartbeat |
| `employee_id_asserted` | the tamper signal (below) | a person |
| `past_cutoff` | the monitor after a cutoff; sync for a locked period | approval, or a person |
| `late_punch` | sync, when a punch lands in a period where the employee's entries are already submitted or approved | approval of that employee's period, or a person |
| `entry_rejected` | sync, when any entry of a batch is refused; one open item per device holding the current list, replaced when the list changes, so a client resending a refused entry never duplicates it | the next batch with nothing refused does not close it (the refused entries are still on the device); a person does, once the device is fixed |
| `approver_flag` | `POST /api/v1/me/flag` | a person |

`GET /api/v1/exceptions?kind=` lists open items in the caller's scope:
their own (employee), their reports' (supervisor, at now), everything
(payroll, admin; device-only items go to them). `POST
/api/v1/exceptions/{id}/resolve|dismiss {note?}` by the supervisor,
payroll or admin, never by the employee on their own item; audited as
`exception.resolve` or `exception.dismiss` with the closer and the audit
row on the exception.

## Freshness monitoring and escalation

The module's monitor thread runs every `monitor_interval_seconds` (one
pass is `run_once` for tests) in one write transaction that commits only
when it opened something, so an idle tick leaves no audit row:

- **device freshness**: an unrevoked device silent (no sync, no
  heartbeat; enrollment counts as the first contact) for
  `device_stale_seconds` is `device_stale`, with an alert log line
  `device.stale`;
- **open shifts** past `long_shift_hours` are `unpaired_punch`;
- **escalation after cutoff**: an open period past `submit_by` with an
  employee's entries still `recorded`, or past `approve_by` with entries
  still `submitted`, opens `past_cutoff` for that employee and period and
  alerts `period.past_cutoff`. An unapproved timesheet at cutoff is
  defined behaviour: it is queued for payroll, who approve with a
  documented override or return it, and it is visible in the audit
  report either way.

## The tamper signal

A request body carrying `employee_id` at any depth is 400 naming the key
on every endpoint (contract-tested with five body shapes on eighteen
endpoints). It is never audited as a business action, because there is
none, and never silent: the server identifies the caller anyway and logs
`request.employee_id_asserted` at warning with the route, the key path
and the caller. From a device it also increments
`devices.employee_id_rejections`, and at
`employee_id_rejection_threshold` opens `employee_id_asserted` for the
device (audited `device.tamper_signal`, alerted). A device that does this
once is a bug; one that does it repeatedly is something else.

## Self-service, supervisor queue, reports

| Route | Credential | What |
|---|---|---|
| `GET /api/v1/me` | bearer with an employee row | the employee, "your approver" (the supervisor at now), open exception count, whether clocked in |
| `POST /api/v1/me/flag {note}` | | opens `approver_flag`; one open ticket at a time |
| `GET /api/v1/timesheets/{period}/{employee_number or me}` | in scope | entries, shifts, states, device-attested count, total hours, open items |
| `GET /api/v1/supervisor/periods/{id}` | supervisor | each report's states, device-attested count, awaiting-approval flag, open items |
| `GET /api/v1/payroll/periods/{id}[?format=csv]` | payroll | hours by employee by pay code, released and locked only |
| `GET /api/v1/payroll/periods/{id}/audit` | payroll or admin | every transition with its actor and time and whether it was an override, every manual entry with its reason, every exception with its resolution |
| `GET /api/v1/admin/roles/conflicts` | admin or payroll | principals holding both supervisor and payroll (segregation of duties) |
| `GET/POST /api/v1/admin/periods` | admin or payroll | `{start_day, end_day, site_zone, tzdb_version, submit_by_us?, approve_by_us?}`; no overlap; unassigned entries in range are assigned |
| `POST /api/v1/admin/schedules` | admin or payroll | `{employee_number, weekday, start_minute, end_minute, effective_from_day, effective_to_day?}` |
| `GET /api/v1/admin/supervisors` | admin or payroll | the assignments |

Hours are reported in hundredths; no monetary arithmetic exists yet
(pay-code multipliers and rounding are Stage 8-P with the payroll
console, and are configuration).

## Configuration (`modules.punchline`)

Every value is a default the section 11 answers replace; unknown keys
and non-positive values fail startup.

| Key | Default | Used by |
|---|---|---|
| `long_shift_hours` | 16 | pairing, the monitor |
| `clock_divergence_tolerance_seconds` | 300 | sync |
| `device_attested_threshold` | 20 | sync (per employee per period) |
| `device_stale_seconds` | 86400 | the monitor |
| `schedule_tolerance_minutes` | 30 | pairing |
| `employee_id_rejection_threshold` | 5 | the tamper signal |
| `monitor_interval_seconds` | 60 | the monitor |
| `max_batch_entries` | 500 | sync (413 above it; at most 10000) |

## The contract suite

`tests/contract/scenarios.json` is the batch-ingest contract both servers
must satisfy, run against the FastAPI prototype by
`tests/contract/test_contract.py` (pytest, `PUNCHLINE_REPO` pointing at
the Punchline repository; the CI job `contract-fastapi` runs it when a
token for that repository is configured and says so when not) and
against Archivum by `tests/server/server_contract_test.cpp`. The wire
shapes differ by design (the prototype posts completed shifts with a
self-asserted employee id under a shared token; Archivum posts punches
under a per-device credential and derives the employee), so each runner
maps the abstract entry to its server's shape; the semantics asserted are
the ones that must not differ: a valid batch is accepted, replay is
idempotent, one bad entry does not sink the batch, a duplicate uuid in
one batch stores one row, a shift over the limit is named (the prototype
rejects it with a reason, Archivum accepts it and opens `long_shift` for
the supervisor), a malformed uuid is rejected by name, an
unauthenticated batch is 401, and `employee_id` in the body is the one
deliberate difference, recorded as such.

## Rollback

`archivum rollback-export --db <path> --period <id> --to <file>
[--released-only]` writes a period's closed shifts as the batches the old
FastAPI server ingests on `POST /api/v1/timesheets`, so that rolling a
pilot device back means replaying the file into the old server through
its own uuid-idempotent ingest path. One closed shift is one old-store
entry: `uuid` is the in punch's entry uuid, `employee_id` the employee
number, name, company and cost centre from the employee record
(`employees.company`, `employees.cost_center`, server-owned), clock in
and out from the punches, minutes computed, the note from the in punch,
and `archivum_state` for the operator (a key the old server ignores).
Open shifts are skipped and counted; missing routing is exported empty
and counted, never refused. Batched per device, 100 entries per batch
like the old client. Proven three ways: the module test, the CLI round
trip, and `tests/contract/test_rollback.py`, which replays an export into
the FastAPI backend and asserts the old server serves every entry back
with the same values and that a second replay stores nothing twice. The
gate is that replay run against the real pilot export.

## Not in Stage 6

- Return-to-employee (approved back to recorded) and retroactive
  adjustments against a locked period: the payroll console, Stage 8-P.
- Pay-code computation (regular, overtime, double time) and rounding:
  configuration for Stage 8-P; shifts carry the pay code the punch named.
- Notification (email may notify, never authorizes): Stage 8-P.
- Rate limiting of the sync endpoint by client address: `trusted_proxies`
  is parsed but nothing reads `X-Forwarded-For` yet.
- The client's journal integration and the sync client are Punchline
  repository work against this contract.
