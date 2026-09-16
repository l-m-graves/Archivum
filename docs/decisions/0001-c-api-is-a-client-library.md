# Decision 0001: the C API is a client library, not an embedding API

Status: decided (rulings v3, section 3A).

## Context

The pipeline document asked for a SQLite-style `extern "C"` API in the
model of `sqlite3_open(path)`, so that Python, R, C#, and Excel VBA could
call Archivum without C++.

## Decision

The public C API is a **client library**: `archivum_connect(url,
credential)` and the prepare, bind, step, column, error, and free surface
specified in the pipeline document, with opaque handles, versioned structs
(size as first member), error codes only, no global state, and documented
thread safety. It speaks the server's HTTPS API. An ODBC driver, if ever
built, sits on this library.

The only local-open path is the Punchline client's own journal store,
compiled into that client alone and scoped to that one application.

## Why

Instructions v2 make `archivum.exe` the only process that opens the
database, and place authorization, the audit trail, and the query log in
that process. An API that opens the file from another process bypasses all
three. That is not a convenience trade-off; it is a control failure that
would make Archivum indefensible to an auditor, and it would have to be
removed later at greater cost.

The embedding form is the obvious design and will be proposed again. The
answer is the same each time unless the single-opener rule changes.
