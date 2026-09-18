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

## Engine-enforced

| Table | Key | Constraints and indexes |
|---|---|---|
| `employees` | `id` (immutable, server-assigned) | `employee_number` unique and non-empty; `(tid, oid)` unique with NULLs distinct, so an employee without an Entra identity is allowed and an identity maps to at most one employee; `email` nullable and never authorizes; `active`; `site_zone` non-empty |
| `devices` | `id` | `device_uuid` unique; `employee_id` → employees (RESTRICT); one credential verifier (Argon2id, bytes); `revoked_at` nullable, so revocation keeps the row for the audit trail; `(employee_id, revoked_at)` indexed |
| `pay_periods` | `id` | `start_day` unique (site-local days since 1970-01-01); `state` in open, submitted, approved, released, locked; `tzdb_version` |
| `supervisor_assignments` | `id` | who reports to whom, effective-dated: `employee_id`, `supervisor_employee_id` → employees; `effective_from`, `effective_to` (NULL: open); `audit_id` → audit_log NOT NULL; indexed by employee and by supervisor with effective_from. An approval is evaluated against the assignment in force at the action's time, not the current one |
| `pay_codes` | `code` | lookup: `description`, `paid`, `active`; seeded with regular, overtime, double_time, pto, holiday |
| `schedules` | `id` | `employee_id` → employees; weekday 0 to 6; minutes 0 to 1439 (end may be 1440); effective range in days. No row means "no schedule on file" |
| `time_entries` | `id` | `entry_uuid` unique (client-generated, idempotent replay); `(device_id, journal_sequence)` unique (the journal's own idempotency key); `employee_id`, `device_id` → parents; `kind` in in, out; `attestation` in device, server, manual, indexed with `device_time`; `state` in recorded, submitted, approved, released, locked; the three time values (below); `clock_divergence_us`; `period_id` → pay_periods; `pay_code` → pay_codes; `approval_audit_id` → audit_log; `correction_of` → time_entries with `correction_reason` |
| `sync_batches` | `id` | `batch_uuid` unique; `device_id` → devices; sequence range and accepted/rejected counts, so a replayed batch is answered without re-inserting |
| `approvals` | `id` | one row per lifecycle transition of an employee's period; `period_id`, `employee_id` → parents; `audit_id` → audit_log NOT NULL: no transition without its audit entry |
| `exceptions` | `id` | `kind` in outside_schedule, non_scheduled_day, no_schedule, long_shift, clock_divergence, device_attested_count, retry_exhausted, device_stale, approver_flag; `state` in open, resolved, dismissed; optional references to employee, device, entry, period, resolution audit entry |

Every foreign key is RESTRICT both ways: a device with entries cannot be
deleted, an entry that a correction points at cannot be deleted, an audit
entry that an approval references cannot be deleted. Deletion is not the
Punchline way of ending anything; revocation, resolution and lifecycle
state are.

## Enforced by the module, not the engine

The engine's checks are `column op constant` and `column IN (list)`
(`docs/store-format.md`). Anything relating two columns or two rows is a
module rule: a function in `modules/punchline/include/archivum/punchline/rules.h`
that every route calls before mutating, returning `Constraint` with the
rule's name, and a test in `tests/unit/punchline_rules_test.cpp` asserting
that the engine accepts the bad row and the module rejects it. This is
the Stage 5 ruling (section 2): module enforcement, documented here, with
a test per invariant. The list is the contract; a rule added without a
row here and a test is a defect.

| Rule | Invariant | Status |
|---|---|---|
| PL-1 | at most one unrevoked device per employee | enforced (`one_active_device`), tested |
| PL-2 | a supervisor assignment names two different employees, its range is ordered, and it does not overlap another assignment of the same employee | enforced (`supervisor_assignment_valid`), tested |
| PL-3 | an entry's `period_id`, when set, is the period containing its `device_time` in site-local days | Stage 6, with the sync endpoint; test required |
| PL-4 | a correction (`correction_of`) is of an entry of the same employee, and `correction_reason` is present | Stage 6; test required |
| PL-5 | a device-attested entry comes from a device unrevoked at `receipt_time` | Stage 6; test required |
| PL-6 | an out punch follows an in punch of the same employee and device (`clock_out > clock_in` at pairing), and a shift is under the length threshold or flagged `long_shift` | Stage 6, the pairing rule the ruling names; test required |
| PL-7 | lifecycle transitions in order only (recorded → submitted → approved → released → locked) by a principal holding the role the transition needs, evaluated against the supervisor assignment in force at `acted_at` | Stage 6; test required |
| PL-8 | `employee_id` is never taken from a request body | enforced in the server (`parse_body`), contract-tested on every endpoint |

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
| Roles | elevated roles only in `role_grants` (core); every employee is an employee by their row |
| Pay calendar, thresholds, zone | configuration, not schema (`docs/plan-v1.md` section 2) |
