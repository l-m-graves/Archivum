# Server core

Stage 5. The application server around the engine: configuration,
logging, identity, the recorder, the archive shipper, modules and the
operational subcommands. Code: `server/`, `core/`, `modules/punchline/`.

## Configuration (`config/archivum.example.json`)

Every section is validated with an allow-list of keys; an unknown key is
an error. Required, and the server does not start without them:

| Section | Keys | Fails closed when |
|---|---|---|
| `tls` | certificate, key, min version, ciphers | missing or unreadable files, version below 1.2 |
| `oidc` | issuer, audience, discovery URL, CA bundle, skew, JWKS timings | non-https, skew over 300 s; a production build refuses a local issuer |
| `database` | `path`, `archive_dir` | either missing |
| `backup` | `destination`, `archive_cadence_seconds`, `backup_cadence_seconds` (default 86400) | destination or cadence missing or zero; destination equal to the archive directory |
| `logging` | `level` (info, warn, error), `file` (empty: stderr) | unknown level |
| `modules.<name>` | the module's own section, parsed by the module with its own allow-list (`docs/punchline-module.md`, "Configuration") | unknown key or module, non-positive value |

`backup.destination` is a directory: a mounted share, a UNC path, or a
directory another agent replicates. `[FILL: backup destination]` and
the cadences are Q8's answers; until then the example holds placeholders.

## Logging

JSON lines, one object per event, with `ts`, `level`, `event` and named
fields (`server/include/archivum/server/log.h`). Levels info, warn,
error and alert; alert is never filtered and marks events a person must
see: `auth.unknown_principal`, `auth.device_revoked`,
`auth.device_bad_credential`, `auth.account_used` (every break-glass or
device-administration use), `auth.account_failed`, `archive.ship_failed`,
`archive.copy_verification_failed`, `archive.backup_verification_failed`,
`device.tamper_signal`, `device.stale`, `period.past_cutoff`. Warnings
worth a search: `request.employee_id_asserted` (route, key path, caller
or device, count), `auth.rejected`.
No log line carries a request body or a value from one; identifiers,
outcomes and the request id only (`docs/confidentiality-check.md`).
Every response carries `X-Request-Id` and the same id appears in its log
lines and audit rows.

## Identity

Three credentials, one `Authenticator` (`server/include/archivum/server/identity.h`):

- **Bearer**: an Entra token validated as in Stage 1 (JWKS refreshes are
  single-flight since Stage 6: N requests that hit an expired cache or an
  unknown kid at once share one fetch and are all answered from it,
  tested with twelve threads under ThreadSanitizer). The principal's
  standing is then read from data: roles from `role_grants` on
  `(tid, oid)`, and an employee row from `employees.(tid, oid)`. A valid
  token with neither is an unknown principal: 403, an audit row, an
  alert. Email is never consulted (the nullable-oid rule: an employee
  without an identity exists; an identity without a mapping does not act).
- **Device**: `Authorization: Device <uuid>:<secret>`, the per-device
  credential issued at enrollment (32 random bytes, shown once, stored
  as an Argon2id verifier). A revoked device is 403 and audited; a wrong
  secret is 401 and audited. The employee behind a device request is the
  enrollment record's.
- **Basic**: a local account (`docs/server-core.md`, below). Break-glass
  acts as admin; device_admin can enroll and revoke devices.

## Roles and dataset grants

Data, not configuration: `role_grants` (supervisor, payroll, admin,
analyst) and `dataset_grants` (dataset, permission) keyed on `(tid, oid)`.
Granted and revoked through `/api/v1/admin/roles` by an admin, each
through the recorder. Supervisor and payroll are separate grants; the
segregation-of-duties report (Stage 8-P) lists principals holding both.

## Local accounts (instructions v2, Q9)

Created only by a console command on the host, never over HTTP:

```
archivum account create  --db <path> --username <u> --kind break_glass|device_admin
archivum account enable  --db <path> --username <u> --hours <n>     (1 to 168)
archivum account disable --db <path> --username <u>
archivum account list    --db <path>
```

`create` prints the generated password once and stores an Argon2id
verifier. An account is disabled at creation, enabled for a bounded
time, disabled again when it expires (checked at every use, in the
store), and every successful use writes an audit row and an alert log
line. The verifier never appears in the change feed (blobs are shape
only).

## Off-host archive check

`ArchiveShipper` (`server/include/archivum/server/archive.h`) runs on its
own thread every `archive_cadence_seconds`: it copies every archived log
segment the destination lacks (to a `.part` name, then renamed), and a
full backup when the last one is older than `backup_cadence_seconds`.
Every copy is verified before it counts (Stage 6 ruling), and after its
final rename: a segment's copy is read back from the destination under
its final name and its size and CRC32C compared with the source; a backup
is checked page by page against its own checksums. A mismatch removes the
file, fails the pass, is an alert, and is counted in `/healthz` as
`archive.verification_failures`. The name: the file is synced and renamed into place
(`MoveFileExW` with `MOVEFILE_WRITE_THROUGH` on Windows, `rename` plus a
directory `fsync` on POSIX), but the archive's durability argument is
the idempotent re-ship, not the rename: a rename lost to a crash leaves
the segment absent and the next pass copies and verifies it again. What
the flag does and does not guarantee, and what an SMB share does and
does not, is in `docs/durability.md`.
`/healthz` returns 503 with `archive.problem` until the first successful
pass and whenever the last success is older than twice the cadence; a
failing pass is an alert. Tested end to end in
`tests/server/server_core_test.cpp`: healthy after the first pass, 503
when the destination disappears, healthy again when it returns, and the
shipped backup opens and passes `check`.

## Modules

`core::Module` (data: migrations under the module's name, recordable
columns) plus routes registered on the App (`server/include/archivum/server/modules.h`).
Startup applies the core migrations, then each module's, keyed by module
in `archivum_migrations`; then builds the one record policy; then
registers routes. Punchline is the built-in module.

## Routes (Stage 5; the Stage 6 routes are in `docs/punchline-module.md`)

| Route | Credential | Role | What |
|---|---|---|---|
| `GET /healthz` | none | | status, TLS expiry, JWKS, schema version, archive state; 503 while the off-host copy is failing |
| `GET /api/v1/whoami` | any | | kind, identity, roles, employee id |
| `POST /api/v1/admin/roles` | bearer or break-glass | admin | grant `{tid, oid, role}` |
| `POST /api/v1/admin/roles/{id}/revoke` | | admin | |
| `GET/POST /api/v1/admin/employees` | | admin (list: or payroll) | create `{employee_number, display_name, site_zone, email?, tid?, oid?, pay_group?}` |
| `GET/POST /api/v1/admin/devices` | | admin or device_admin | enroll `{employee_number, name}`; answers the credential once |
| `POST /api/v1/admin/devices/{uuid}/revoke` | | admin or device_admin | `{reason}` |
| `POST /api/v1/admin/supervisors` | | admin | `{employee_number, supervisor_employee_number, effective_from_us, effective_to_us?}` |
| `POST /api/v1/device/heartbeat` | device | | `{client_time_us?}`; answers the enrollment's employee and the clock divergence |

Every body is parsed by `parse_body`: not an object, an unknown key, or
`employee_id` at any depth is 400 with a message naming the key. The
contract test runs every endpoint (eighteen since Stage 6) with five
such bodies, and from a device the rejection is a counted tamper signal
(`docs/punchline-module.md`).

## Modules, background work

A `ServerModule` has its data layer, a `configure` hook for its section,
`register_routes`, and `start`/`stop`/`run_once` for background work
(Punchline's freshness and cutoff monitor). Modules start after
authentication is initialised and stop before the store closes.

## Not yet

Trusted proxies are configured and validated but no route reads
`X-Forwarded-For` yet: nothing needs the client address until rate
limiting of the sync endpoint.
