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
                 "site_zone": "IANA zone"?, "tzdb_version": text?,
                 "pay_code": text?, "note": text?, "break_code": text? } ],
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
- **`site_zone` and `tzdb_version` are the server's, never the
  device's.** Both are accepted on the wire as tolerance (the rewritten
  client sends the zone it last heard at a heartbeat and never a release
  name) and neither is ever stored or used. The entry's `site_zone` is the
  employee record's, always; its `tzdb_version` is the embedded database's
  release, always (`release_stamp()`: the literal `unavailable` when no
  database is embedded, so a stamp never reads as a release it is not).
  A device's zone claim is compared: a claim that differs from the
  employee's record opens `local_clock_mismatch` naming both values (one
  open item per device and employee), and a later batch whose claim agrees
  closes it. The same holds off the device path: a manual entry and a pay
  period refuse `site_zone` (manual entry) and `tzdb_version` (both) as
  request fields with 400, and stamp from the same two sources. Found by
  the end-to-end run, which first showed the sync path demanding a release
  name from the device, and then corrected again on review: the first fix
  stored a device's value when one was sent, which made the device a
  source of truth; it no longer does.
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
  also diffed byte for byte;
- **the pins are release-scoped** (follow-up ruling): `pinned-zones.txt`
  and the generated `pinned-transitions.txt` both name the IANA release
  they were taken from, and `tzcheck` fails the build when the embedded
  database's version is not that release, before it looks at a single
  transition. Pins from one release prove nothing about another. Changing
  the vendored release is an explicit act: the generator refuses a
  tarball of another release unless run with `--new-release`, and then it
  rewrites the release line, regenerates the pins and prints the
  transition diff, which the commit carries and the reviewer reads. Pins
  that would change without a release change (a tool difference, a
  non-determinism) are refused outright;
- `zic` and `zdump` are the release's own, built from the `tzcode`
  tarball of the same release, whose version is checked against the data
  tarball's; the build host's copies are accepted only for the synthetic
  release the tests use, and the output records which was used.

**tzdb models legal intent and sometimes lags it.** The database encodes
what the maintainers have confirmed a jurisdiction will do, on the date
they model it taking effect, and both can differ from the law as
announced: Alberta's permanent −06 is effective in June 2026 by the
province's act, and the database models the change at the 1 November
2026 transition, the same way British Columbia's was modelled in release
2026b. A punch in the window between the legal date and the modelled
date is assigned by the modelled rule, and a later release can move it.
That is why the `tzdb_version` stamp on every entry and every period is
load-bearing rather than decorative: it names the rule set a local day
was computed under, so that a re-computation under a later release is a
visible change with a reason, never a silent one. Nothing recomputes an
entry's local day on its own; a period's entries are settled under the
release that was embedded when they were assigned.

**Status**: the reader, the generator, the build-time check and their
tests are in (`tzif_test` on synthetic fixtures including both 2026
Pacific transitions and the ambiguous hour; `tzcheck_synthetic` and
`tzdata_generate_test` on a synthetic two-zone release compiled by zic
and pinned by zdump, `tests/tzdata/`). The IANA release itself has not
arrived (the attachment did not reach the repository), so the binary
still carries the placeholder, `tzcheck` reports it, and the sync path
has **not** been switched: it still takes the device's wall clock for day
assignment, which is the device-clock problem this design exists to remove:
`period_for_day`, the schedule checks and the pairing's local day all read
the `local_time` string the device sent, and the site zone is not consulted
for any of them. What is already server-only is the stamp on every entry
(zone from the employee record, release from the embedded database), so the
switch changes where the local day comes from and nothing else. Switching it is the commit that lands with the release
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
site_zone, tzdb_version, reason, reason_code, correction_of?, pay_code?,
break_code?, note?}`. The edit's class (`payroll` or `supervisor`, from who
made it) and its `reason_code` are recorded with it, and the audit action
is `payroll.edit` or `supervisor.edit` (below). The
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
| `local_clock_mismatch` | sync, when a device reports a site zone other than the employee record's (the server's record was used). The comparison of the device's `local_time` with the server's own computation joins it with the switch commit | a later batch whose zone claim agrees, or a person |
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
| `week_start_weekday` | 0 (Sunday) | the weekly-hours flag (an assumption to confirm, below) |

## Payroll reference rules

Rules the business supplied from its payroll reference after Stage 6 was
built. **The reference document was not available to the session that
wrote this: not in either repository, not on any branch of the Punchline
repository, not attached.** Every rule below comes from the requester's
written description of it, not from the document, and is marked
*confirmed* only where the requester's statement leaves no room. Where it
does, the assumption is named and is a configuration value or a recorded
row that is changed without a migration. Migration 2 of the module (and
migration 2 of the core module, for the `people` role) carries the schema;
`docs/punchline-schema.md` lists the tables. Code: `payroll.h` and
`payroll.cpp` in the module; routes at the end of `punchline_routes.cpp`;
tests in `punchline_sync_test.cpp` and `server_punchline_test.cpp` (the
`i_`, `j_` and `k_` scenarios).

### Who does what

| Type | Does | Not |
|---|---|---|
| Employee | punches | anything else |
| Supervisor | approves timesheets; authorizes overtime | release to payroll |
| Payroll | edits employee punches (its edit view is the GUI reference for Stage 8-P) | approve (without an override reason), authorize overtime |
| People | grants and revokes supervisors' access (`role_grants`, audited) | any other role change |
| Admin | device enrollment, configuration | not a business role; never stands in for one |

`people` is a role in `role_grants`; the granting routes are
`/api/v1/people/supervisors` (GET lists, POST grants by employee number)
and `/api/v1/people/supervisors/revoke`. A grant goes to the employee's
Entra identity, so an employee with none cannot be made a supervisor (409).
Audit actions `people.grant_supervisor` and `people.revoke_supervisor`.

**Two places where the code still differs from that table, for a ruling
rather than a quiet change:**

1. `POST /api/v1/admin/roles` (administrators) still grants any role,
   `supervisor` included, as Stage 4 built it. If only People may grant
   supervisor access, admin's route should refuse it. Until told, both
   can, and the audit action differs (`role.grant` against
   `people.grant_supervisor`), so the two are distinguishable in the
   report.
2. A supervisor may enter a manual punch for a report
   (`supervisor.edit`), as PL-4 has said since Stage 6. The reference says
   payroll edits punches. Both are kept and recorded as different classes;
   whether the supervisor's route stays is the requester's call.

### Payroll edits are a class of their own

A manual entry carries `edit_class` (`payroll` or `supervisor`, taken from
the caller's standing, never from the body) and a required `reason_code`
(1 to 16 characters, upper-case letters, digits and underscore). It lands
in `entry_edits` and in the audit log as `payroll.edit` or
`supervisor.edit`. A payroll edit is an audited correction with a reason
code and is distinct from a supervisor's approval, which is a lifecycle
transition in `approvals`. The segregation-of-duties report
(`/api/v1/admin/roles/conflicts`) lists `edits_by_class` and
`edits_by_editor` beside the role conflicts, so payroll edits are counted
as their own class. **The reason-code list is not known to me**: the code
takes any well-formed code and records it, and a lookup table with
descriptions is the follow-up once the list is supplied.

### Scheduled hours and the flag

The requester asked whether `schedules` carries `scheduled_daily_hours`
and `scheduled_weekly_hours`, effective-dated per employee. **It does
not**: `schedules` is one row per weekday with a start and an end minute
(shifts of the day), which is what the late/early check reads. Hours per
day and per week are a different fact, so a new table, `schedule_hours`,
carries them: per employee, effective-dated (`effective_from_day`, open
`effective_to_day`), stored in minutes, no overlap for one employee
(refused with 409). Written and read in hours by
`/api/v1/admin/schedule-hours` (admin or payroll).

A timesheet (one employee in one period) is **flagged** when a day's
closed-shift hours exceed `scheduled_daily_hours`, or a week's exceed
`scheduled_weekly_hours`, for the limits in force on that day (the week's
limits are those in force on its first day). Weeks start on
`week_start_weekday` (Sunday by default; **an assumption**, the
description does not say). An employee with no `schedule_hours` row has
nothing to compare with and is never flagged. The flag is evaluated after
every pairing change (sync, correction) and when a limit is filed.

The flag is a **hold** (`timesheet_holds`), and it is not a lifecycle
state and not an exception. The two stay separate on purpose:

| | Exception queue | Hold |
|---|---|---|
| What it is | a problem a reviewer should look at | a timesheet withheld from payroll |
| Effect | surfaces in queues; some block approval | hidden from payroll; release refused for everyone |
| Cleared by | resolution, or the approval that covers it | the supervisor's authorization, or the hours coming back within the limits |
| Tables | `exceptions` | `timesheet_holds` |

While a hold is open, payroll cannot see the timesheet: the payroll
export omits it, its view of the timesheet, exceptions and lifecycle
routes answers 404 "no such timesheet" (not 403; there is nothing there for
it), and a payroll edit of it is refused the same way. Supervisors see it
in their queue with `held_for_overtime_authorization` and the detail.
`released` is refused with PL-7 for any caller, override or not, while the
hold is open. Approval is still the supervisor's and still happens.

Authorization: `POST /api/v1/supervisor/periods/{id}/authorize-overtime`
with `{employee_number, note?}`, by **the employee's own supervisor at that
time** only (not the employee, payroll, an administrator or another
supervisor), audited as `overtime.authorize`. It records the **overtime
excess** the supervisor was looking at (the larger of the minutes over the
daily limits summed over days and the minutes over the weekly limits
summed over weeks) and clears the hold. More overtime later than that
reopens the hold; hours worked within the schedule do not. The first
version of this recorded total period minutes and so reopened the hold for
a normal day added after an authorization; the test that found it is
`authorization_covers_the_overtime_seen_more_overtime_reopens_the_hold_and_normal_days_do_not`.

### Codes

Three kinds, kept apart (`pay_codes`, `annotation_codes`, `break_codes`):

- **Pay codes that change pay**: `OT` and `DT`, added to `pay_codes`. The
  earlier `overtime` and `double_time` seeds are kept (rows may point at
  them) and deactivated. Computing which hours are OT or DT is still the
  Stage 8-P payroll configuration; nothing assigns them automatically.
- **Annotations, informational, never changing pay**: `L` (late) and `E`
  (early), recorded per shift in `shift_annotations` and rebuilt with the
  shift. **The definitions are my assumption**: `L` is a shift whose first
  in punch is more than `schedule_tolerance_minutes` after the day's
  scheduled start; `E` is a same-day out punch more than the tolerance
  before the scheduled end. The reference's own thresholds are to be
  confirmed. A day without a schedule row is not annotated.
- **Break codes**: `M` (meal), carried on the out punch that begins the
  meal (`break_code` on a sync entry or a manual entry; an in punch with
  one is refused as malformed, an unknown or inactive code is rejected per
  entry). The meal is the time from that out punch to the employee's next
  punch (`meal_breaks`; open while they have not come back).

### The meal premium

`M` carries a premium marker. Whether a meal earns the premium is the
configurable rule in `break_premium_rules`: break code, threshold minutes,
comparison (`greater_than` or `at_least`), effective dates, and, required,
its `definition` in words and its `source`. The requester's description is
that the premium triggers when the meal exceeds 45 minutes "per the
reference".

**That trigger is not confirmed.** I cannot cite the reference, so no
rule is seeded, and a rule stored through `/api/v1/admin/break-premium-rules`
is `confirmed: false` unless the request says otherwise. An unconfirmed
rule is stored and shown, **never applied**: meals get no premium. The
open questions the reference answers and I cannot: is the threshold
"more than 45" or "45 or more" (`comparison`), does it run from out to in
punch as here, is the meal itself paid or unpaid, and is the premium
per meal or per day. When someone quotes the page, the rule is added with
that page as `source` and `confirmed: true`; meals are recomputed as their
pairing is rebuilt. Until then the tests prove the mechanism (50 minutes
against a confirmed 45 is premium, exactly 45 is not under `greater_than`,
an unconfirmed rule changes nothing), not the business fact.

### Employee number

`employees.employee_number` is text, the payroll system's canonical
string, stored and displayed exactly as given (leading zeros kept, no
numeric folding: `employee_by_number("123")` does not find `000123`). The
rollback export's `employee_id` key carries it unchanged. Tested in
`employee_numbers_are_stored_and_exported_as_the_payroll_system_wrote_them`
and, over HTTP, in scenario `i_`.

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

**What rollback preserves, and what it cannot** (ruling, recorded for the
handbook: `docs/handbook.md`). The old store has no schema for approval
state, for exception records or for device attestation. A shift exported
back to it arrives as a completed entry with hours, note and routing,
exactly as the old client would have posted it, and nothing else: the
`archivum_state` key is for the operator's eyes and the old server
ignores it. Rollback therefore preserves **hours, not the approval
trail**: which supervisor approved what and when, every exception and
its resolution, whether a punch was attested by a device, and every
transition in the period's audit report stay in Archivum's store and its
archived logs. A rollback plan has to say where those are kept and for
how long; it cannot say they were carried across.

## Not in Stage 6

- Return-to-employee (approved back to recorded) and retroactive
  adjustments against a locked period: the payroll console, Stage 8-P.
- Pay-code computation (regular, `OT`, `DT`) and rounding:
  configuration for Stage 8-P; shifts carry the pay code the punch named.
- Notification (email may notify, never authorizes): Stage 8-P.
- Rate limiting of the sync endpoint by client address: `trusted_proxies`
  is parsed but nothing reads `X-Forwarded-For` yet.
- The client's journal integration and the sync client are Punchline
  repository work against this contract.
