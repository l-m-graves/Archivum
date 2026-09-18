# Confidentiality check: mandatory for every component that persists row content

The Synthex rule is absolute: no original data, no statistics derived from
original data, no input file names or paths, and no date-shift offset may
reach disk, ever. Twice now a component added after the rule was written
has broken it (the audit writer, then the change feed). So:

**Any new component that writes row content, parameters, or statistics to
disk must be checked against this rule before it is designed, and the
result recorded in this file.** "It does not touch Synthex" is a claim to
verify, not an assumption.

## Places covered, and how

| Component | Risk | Rule | Enforced where |
|---|---|---|---|
| Audit trail | Before and after row images | Per-table declared recordable columns (`RecordPolicy`); everything else presence and shape only (kind, length); blobs never by value | `core::Recorder`, the one writer both go through (Stage 5, `docs/audit-and-change-feed.md`); canary test `recorder_writes_audit_and_feed_in_one_transaction_under_the_policy` |
| Change feed | Before and after row images | Same policy, same recorder | The change feed writer is the audit writer: one class, one code path |
| Query log | SQL text, parameters, row counts | Synthex tables are never datasets, so no Synthex row can be returned or logged. SQL text typed by a user is the user's own input | Dataset allow-list at catalog level |
| Transform inputs | Copying values into `analytical.db` | Synthex tables cannot be declared as transform inputs | Rejected at transform declaration; `analytical.db` in the canary scan |
| Landing tables | Raw payload as received | Synthex has no code path that writes a landing table | No reachable path; landing tables in the canary scan |
| Expectations | Results are derived statistics (min, max, null rate, distinct counts) | Expectations cannot be declared on Synthex tables | Rejected at declaration |
| Run history | Job parameters, file names, counts | Synthex jobs record only the generation-log fields: who, when, rule-set and vocabulary versions, output row count, input hash. Never a file name, path, or offset | Synthex job runner writes through the generation log, not a generic parameter blob |
| Structured server logs | Values in log lines | No personal or financial values in any log line, suite-wide | `server::log` takes named fields; call sites pass identifiers, outcomes and request ids (Stage 5). Drogon's own log level is warn, so it never prints bodies |
| HTTP request logging | Request bodies in access logs | No access log. Each route logs its outcome with the request id; never a body or query parameter value | Stage 5: there is no Drogon access-log plugin configured, and `parse_body` errors name a key, not its value |
| Process crash dumps | Memory contents including the in-flight offset | Crash dumps disabled for `archivum.exe` (Windows Error Reporting excluded; no core files in the container). Buffers holding an offset are locked against paging where the OS allows | Service installer and container image |
| `archivum.db`, `archivum.wal`, log archive, backups | Anything persisted | Canary scan | `tests/synthex/canary_scan_test` (Stage 10) |

## The canary scan

A test submits a job whose input contains known canary values, a known file
name and path, and a known offset, then scans the raw bytes of every file
Archivum wrote: both database files, both WALs, the archive directory, a
backup, and the landing directory. Any hit fails the test. This is the
proof; the table above is the design.

## Not yet covered

None known. Add a row here before adding a component that persists row
content anywhere.
