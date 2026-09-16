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
| Audit trail | Before and after row images | Per-endpoint declared recordable fields; everything else presence and shape only (name, type, length) | The audit writer itself, not per route |
| Change feed | Before and after row images | Same declaration, same writer | The change feed writer is the audit writer |
| Query log | SQL text, parameters, row counts | Synthex tables are never datasets, so no Synthex row can be returned or logged. SQL text typed by a user is the user's own input | Dataset allow-list at catalog level |
| Transform inputs | Copying values into `analytical.db` | Synthex tables cannot be declared as transform inputs | Rejected at transform declaration; `analytical.db` in the canary scan |
| Landing tables | Raw payload as received | Synthex has no code path that writes a landing table | No reachable path; landing tables in the canary scan |
| Expectations | Results are derived statistics (min, max, null rate, distinct counts) | Expectations cannot be declared on Synthex tables | Rejected at declaration |
| Run history | Job parameters, file names, counts | Synthex jobs record only the generation-log fields: who, when, rule-set and vocabulary versions, output row count, input hash. Never a file name, path, or offset | Synthex job runner writes through the generation log, not a generic parameter blob |
| Structured server logs | Values in log lines | No personal or financial values in any log line, suite-wide | Log call sites take field names, not payloads |
| HTTP request logging | Request bodies in access logs | Access log records method, path, status, duration, actor id. Never a body or query parameter value | Drogon access-log format fixed in server core |
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
