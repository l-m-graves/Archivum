# Punchline schema (module `punchline`, migration 1)

`modules/punchline/src/schema.cpp`; applied by `archivum migrate --db`
after the core schema (`docs/server-core.md`). The rules behind the tables
come from instructions v2 (identity, roles, approvals),
punchline-updates.md (entry fields, lifecycle, exceptions, schedules) and
`docs/plan-v1.md` section 2 (assumptions). Everything in the first table
the engine enforces itself; the second table lists what the module
enforces, each with its test.

Since Stage 4: `audit_log`, `role_grants` and `local_accounts` moved to
the core module (they serve every module); `supervisor_assignments` and
`pay_codes` were added (Stage 5 rulings, section 3); `time_entries` gained
a foreign key to `pay_codes` and indexes on `pay_code` and `attestation`.
Since Stage 5, still in migration 1 because nothing is deployed:
`shifts` (punch pairing), `time_entries.journal_id`, `note` and
`superseded_by`, `devices.last_journal_id` and `employee_id_rejections`,
`sync_batches` clock and journal columns, four exception kinds, and the
same-row comparisons moved into engine checks (Stage 6 rulings). The
workflow on top of these tables is `docs/punchline-module.md`.

## Engine-enforced

| Table | Key | Constraints and indexes |
|---|---|---|
| `employees` | `id` (immutable, server-assigned) | `employee_number` unique and non-empty; `(tid, oid)` unique with NULLs distinct, so an employee without an Entra identity is allowed and an identity maps to at most one employee; `email` nullable and never authorizes; `active`; `site_zone` non-empty |
| `devices` | `id` | `device_uuid` unique; `employee_id` → employees (RESTRICT); one credential verifier (Argon2id, bytes); `revoked_at` nullable, so revocation keeps the row for the audit trail; `(employee_id, revoked_at)` indexed; `last_acked_sequence` and `last_journal_id` (the client journal's replay key); `employee_id_rejections` ≥ 0 (the tamper counter) |
| `pay_periods` | `id` | `start_day` unique (site-local days since 1970-01-01); `end_day ≥ start_day` and `approve_by ≥ submit_by` (engine checks); `state` in open, locked (the per-employee lifecycle lives on entries and shifts); `tzdb_version` |
| `supervisor_assignments` | `id` | who reports to whom, effective-dated and half-open: `[effective_from, effective_to)`, `effective_to` NULL meaning open, `effective_to > effective_from` (engine check); `employee_id`, `supervisor_employee_id` → employees; `audit_id` → audit_log NOT NULL; indexed by employee and by supervisor with effective_from. An approval is evaluated against the assignment in force at the action's time, not the current one |
| `pay_codes` | `code` | lookup: `description`, `paid`, `active`; seeded with regular, overtime, double_time, pto, holiday |
| `schedules` | `id` | `employee_id` → employees; weekday 0 to 6; minutes 0 to 1439 (end may be 1440); `end_minute > start_minute`; effective range in local days, half-open, `effective_to_day > effective_from_day` (engine checks). No row means "no schedule on file" |
| `time_entries` | `id` | `entry_uuid` unique (client-generated, idempotent replay); `(journal_id, journal_sequence)` unique with NULLs distinct (the client journal's replay key; a manual entry has no journal); `employee_id`, `device_id` → parents; `kind` in in, out; `attestation` in device, server, manual, indexed with `device_time`; `state` in recorded, submitted, approved, released, locked; the three time values (below); `clock_divergence_us`; `period_id` → pay_periods; `pay_code` → pay_codes; `approval_audit_id` → audit_log; `correction_of` → time_entries with `correction_reason`; `superseded_by` → time_entries (the correction that replaced this entry); `note` free text |
| `shifts` | `id` | an in punch paired with its out punch (PL-6): `in_entry_id` unique and `out_entry_id` unique nullable → time_entries; `employee_id`, `device_id`, `period_id`, `pay_code` → parents; `local_day` of the in punch; `in_time`, `out_time` with `out_time > in_time` (engine check); `duration_us ≥ 0`; `state` as the entries'; indexed by employee and day, employee and state, period |
| `sync_batches` | `id` | `batch_uuid` unique; `device_id` → devices; sequence range and accepted/rejected counts, `client_time_us`, `clock_divergence_us`, `journal_id`, so a replayed batch is answered without re-inserting |
| `approvals` | `id` | one row per lifecycle transition of an employee's period; `period_id`, `employee_id` → parents; `audit_id` → audit_log NOT NULL: no transition without its audit entry; `note` carries the override reason when payroll approved in the supervisor's place |
| `exceptions` | `id` | `kind` in outside_schedule, non_scheduled_day, no_schedule, long_shift, clock_divergence, device_attested_count, retry_exhausted, device_stale, approver_flag, unpaired_punch, employee_id_asserted, past_cutoff, journal_recovery; `state` in open, resolved, dismissed; optional references to employee, device, entry, period, resolution audit entry |

Every foreign key is RESTRICT both ways: a device with entries cannot be
deleted, an entry that a correction points at cannot be deleted, an audit
entry that an approval references cannot be deleted. Deletion is not the
Punchline way of ending anything; revocation, resolution and lifecycle
state are.

## Enforced by the module, not the engine

The engine's checks are `column op constant`, `column IN (list)` and,
since Stage 6, `column op other_column` on the same row
(`docs/store-format.md`). The Stage 6 ruling: a same-row comparison
between two columns is the engine's, so payroll invariants that can be
enforced at rest are; what relates two rows, or a row to the caller, is a
module rule: a function in `modules/punchline/include/archivum/punchline/rules.h`
that every route calls before mutating, returning `Constraint` with the
rule's name, and a test in `tests/unit/punchline_rules_test.cpp` asserting
that the engine accepts the bad row and the module rejects it. The list
is the contract; a rule added without a row here and a test is a defect.

Same-row comparisons now in the engine (each refused at insert and
update, tested in `punchline_migration_test` and `store_test`):
`supervisor_assignments_range_ordered` (`effective_to > effective_from`),
`schedules_minutes_ordered` and `schedules_days_ordered`,
`pay_periods_days_ordered` and `pay_periods_cutoffs_ordered`,
`shifts_times_ordered` (`out_time > in_time`).

| Rule | Invariant | Status |
|---|---|---|
| PL-1 | at most one unrevoked device per employee | enforced (`one_active_device`), tested |
| PL-2 | a supervisor assignment names two different employees and does not overlap another assignment of the same employee; ranges are half-open, `[from, to)`, so two assignments may meet at an instant and no day belongs to two supervisors (`supervisor_at` picks the later one at the boundary) | enforced (`supervisor_assignment_valid`, `supervisor_at`), tested including the boundary instant |
| PL-3 | an entry's `period_id`, when set, is the period containing the local day of its wall clock | enforced (`entry_period_valid`, `period_for_day`), tested |
| PL-4 | a correction (`correction_of`) is of an existing entry of the same employee, carries a reason, and the original is not already corrected nor approved or later | enforced (`correction_valid`, `record_manual_entry`), tested |
| PL-5 | a device-attested entry comes from a device unrevoked at `receipt_time` | enforced (`device_attested_valid`), tested |
| PL-6 | an out punch closes an in punch of the same employee and device and comes after it; a shift at or over the threshold is flagged `long_shift`, an unpaired punch `unpaired_punch` | enforced (`pair_out`, `repair_pairing`), tested at the rule and over sync |
| PL-7 | lifecycle transitions in order only (recorded → submitted → approved → released → locked), by the standing each needs, evaluated against the supervisor assignment in force at `acted_at`; no transition skips an entry that is behind | enforced (`transition_allowed`, `transition`, `lock_period`), tested |
| PL-8 | `employee_id` is never taken from a request body | enforced in the server (`parse_body`), contract-tested on every endpoint (eighteen routes, five bodies), and from a device counted as a tamper signal |

## Times

Three time values per entry (proposal, section 2, "Timestamps"), all
columns of `time_entries`: `device_time` (the instant, microseconds since
the Unix epoch UTC), `local_time` (the wall clock as the device recorded
it, text), `receipt_time` (the server's instant), with `site_zone` (an
IANA zone identifier) and `tzdb_version` (the tzdb in force when the
entry was recorded). Pay rules use the local time and the zone;
clock-tamper detection uses the gap between device time and receipt time
(`clock_divergence_us`).

## Attestation

`time_entries.attestation` is one of device (the device recorded it
offline, attested by its credential), server (recorded through the server
online) or manual (entered by a person with a reason). It is a NOT NULL
column with a check and an index, so every query and report can
distinguish the three without inference.

## Pay codes

`pay_codes` is a lookup table keyed by code, seeded by the migration with
regular, overtime, double_time, pto and holiday, and `time_entries.pay_code`
is a foreign key to it. Adding a code is a row; multipliers and rules per
code are configuration for Stage 6, not schema.

## Assumptions (section 11 of punchline-updates.md, unchanged)

| Question | Assumed default |
|---|---|
| One device per employee | at most one unrevoked device per employee (PL-1), so a replaced device keeps its predecessor's history |
| Employee identity | `employees.(tid, oid)`; the kiosk model (proposal Q5) would add a per-employee PIN verifier column, nothing else changes |
| Supervisor identity | a supervisor is an employee row whose identity holds the supervisor role; `supervisor_assignments` maps employee to supervisor employee |
| Local day of a punch | taken from the wall clock the device recorded (`local_time`), not recomputed from the instant: the server has no tz database yet (`docs/punchline-module.md`, "Local time") |
| Roles | elevated roles only in `role_grants` (core); every employee is an employee by their row |
| Pay calendar, thresholds, zone | configuration, not schema (`docs/plan-v1.md` section 2) |
