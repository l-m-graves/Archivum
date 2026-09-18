# Audit writer and change feed: one mechanism

`core/include/archivum/core/recorder.h`. The Stage 5 ruling: the audit
writer and the change feed both write row images inside the business
transaction, so they are built as one thing and the Synthex
presence-and-shape rule is enforced once.

## Shape

```
audit_log     id, at, actor (kind, tid, oid, account, device), action,
              target (table, id), request_id, detail
change_feed   id, audit_id -> audit_log, at, table_name, op, key,
              before_image, after_image
```

A request that mutates anything runs one engine `Writer`. It constructs
a `Recorder` on that writer, which inserts the audit row first (who,
what, when, request id). Every row change then goes through the
recorder: `insert`, `update`, `remove` perform the change and append the
change-feed row with the key, the operation, and the images. The writer
commits everything or nothing (engine invariant I12, one transaction):
an audit row without its changes, or a change without its audit row,
cannot exist. `tests/unit/core_test.cpp` checks both directions, and that
a rolled-back transaction leaves no audit row.

Authentication failures that matter (an unknown principal, a revoked or
mis-credentialed device, a refused local account) are audit rows without
changes, written in their own transaction by the authenticator.

## The record policy

`RecordPolicy` maps a table to the columns whose values may be recorded.
Built once at startup from the core's declarations plus every module's
(`core::build_policy`), and passed to every recorder. In an image:

- a recordable column is its value (integers, decimals, timestamps,
  booleans, text, uuid);
- any other column is `{"present": bool, "kind": ..., "length": ...}`:
  presence and shape, never the value;
- a blob is always shape only (kind and length), recordable or not:
  credential verifiers never appear in an image;
- the row's key is always recorded by value, because keys identify rows
  and are never Synthex data by the confidentiality rule.

A module that stores Synthex-classified content declares nothing for
those columns and the recorder records nothing but shape. The canary
test in `core_test.cpp` inserts, updates and deletes rows carrying a
canary string in undeclared text and blob columns and scans every audit
and feed row for it.

## What the feed is for

The change feed is the source for the analytical store's ingestion and
for module consumers (Stage 6 uses it for exception detection after
sync). A consumer reads `change_feed` ordered by id, in one read
transaction, from its last seen id; the id is dense and increasing
because it is allocated inside the writer. Nothing consumes it yet.
