# Punchline schema (migration 1)

`modules/punchline/src/schema.cpp`; applied by `archivum migrate --db`.
Every table, its key, and the constraints the engine enforces. The rules
behind them come from instructions v2 (identity, roles, approvals),
punchline-updates.md (entry fields, lifecycle, exceptions, schedules) and
`docs/plan-v1.md` section 2 (assumptions). Everything below the engine
enforces itself; application rules that the schema cannot express are
listed at the end and are Stage 6 work.

| Table | Key | Constraints and indexes |
|---|---|---|
| `employees` | `id` (immutable, server-assigned) | `employee_number` unique and non-empty; `(tid, oid)` unique with NULLs distinct, so an employee without an Entra identity is allowed and an identity maps to at most one employee; `email` nullable and never authorizes; `active`; `site_zone` non-empty |
| `role_grants` | `id` | `(tid, oid, role)` unique; `role` in supervisor, payroll, admin. Supervisor and payroll are separate rows: the segregation-of-duties report lists principals with both |
| `local_accounts` | `id` | `username` unique; `kind` in break_glass, device_admin; Argon2id verifier as bytes; `enabled`, `expires_at` for the break-glass auto-disable |
| `devices` | `id` | `device_uuid` unique; `employee_id` → employees (RESTRICT); one credential hash; `revoked_at` nullable, so revocation keeps the row for the audit trail; `(employee_id, revoked_at)` indexed |
| `pay_periods` | `id` | `start_day` unique (site-local days since 1970-01-01); `state` in open, submitted, approved, released, locked; `tzdb_version` |
| `schedules` | `id` | `employee_id` → employees; weekday 0 to 6; minutes 0 to 1439 (end may be 1440); effective range in days. No row means "no schedule on file" |
| `audit_log` | `id` | actor as `(tid, oid)` or local account; `device_id` → devices; `action` non-empty; `(target_table, target_id)` and `at` indexed. Which fields are recorded is the audit writer's decision |
| `time_entries` | `id` | `entry_uuid` unique (client-generated, idempotent replay); `(device_id, journal_sequence)` unique (the journal's own idempotency key); `employee_id`, `device_id` → parents; `kind` in in, out; `attestation` in device, server, manual; `state` in recorded, submitted, approved, released, locked; `device_time` (instant), `local_time` (wall clock as recorded), `receipt_time` (server), `site_zone`, `tzdb_version`; `clock_divergence_us`; `period_id` → pay_periods; `pay_code`; `approval_audit_id` → audit_log; `correction_of` → time_entries with `correction_reason` |
| `sync_batches` | `id` | `batch_uuid` unique; `device_id` → devices; sequence range and accepted/rejected counts, so a replayed batch is answered without re-inserting |
| `approvals` | `id` | one row per lifecycle transition of an employee's period; `period_id`, `employee_id` → parents; `audit_id` → audit_log, NOT NULL: no transition without its audit entry |
| `exceptions` | `id` | `kind` in outside_schedule, non_scheduled_day, no_schedule, long_shift, clock_divergence, device_attested_count, retry_exhausted, device_stale, approver_flag; `state` in open, resolved, dismissed; optional references to employee, device, entry, period, resolution audit entry |

Plus `archivum_migrations` (version, name, applied_at), owned by the
migration framework.

Every foreign key is RESTRICT both ways: a device with entries cannot be
deleted, an entry that a correction points at cannot be deleted, an audit
entry that an approval references cannot be deleted. Deletion is not the
Punchline way of ending anything; revocation, resolution and lifecycle
state are.

## Times

Three time values per entry (proposal, section 2, "Timestamps"): the
device's instant, the local wall clock as the device recorded it, and the
server's receipt instant, plus the site zone and the tzdb version. Pay
rules use the local time and the zone; clock-tamper detection uses the
gap between device time and receipt time (`clock_divergence_us`).

## Assumptions (section 11 of punchline-updates.md, unchanged)

| Question | Assumed default |
|---|---|
| One device per employee | at most one unrevoked device per employee, enforced by enrollment (Stage 6), not by the schema, so that a replaced device keeps its predecessor's history |
| Employee identity | `employees.(tid, oid)`; the kiosk model (proposal Q5) would add a per-employee PIN verifier column, nothing else changes |
| Roles | elevated roles only in `role_grants`; every employee is an employee by their row |
| Pay calendar, thresholds, zone | configuration, not schema (`docs/plan-v1.md` section 2) |

## Not expressible in the schema (Stage 6 application rules)

- Lifecycle transitions in order only (recorded → submitted → approved →
  released → locked) and by the right role; the schema restricts states
  to the five names.
- An entry's period must be the period its `device_time` falls in.
- A correction's `correction_of` must be an entry of the same employee.
- A device-attested entry (`attestation = device`) must come from an
  unrevoked device at receipt time.
- `employee_id` is never taken from a request body (the contract test).
