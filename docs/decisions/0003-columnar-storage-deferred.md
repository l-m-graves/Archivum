# Decision 0003: columnar storage deferred behind a storage interface

Status: decided with conditions (rulings v3, section 3B).

## Decision

The analytical instance (`analytical.db`) is a row-store instance of the
same engine in its first release. The shared executor reads storage
through one narrow interface. A columnar layer is added behind that
interface only when a numeric trigger fires.

## Conditions, each of which is a deliverable

1. `docs/storage-interface.md` written **before** the analytical instance
   is built: the operations, what the executor may assume, what it may
   not.
2. A second implementation of the interface, in-memory, used in tests, so
   that the interface is not shaped around the b-tree implementation.
3. A benchmark harness: a named set of representative queries over
   generated data at expected volumes, run in CI, with a recorded
   threshold per query. When a listed query exceeds its threshold on real
   data, the columnar layer is scheduled. Current numbers are reported
   every time the harness changes.

## Why

At this data volume a clustered b-tree with the right indexes answers the
listed queries in milliseconds; column encodings pay off two orders of
magnitude further out; the transform engine materializes aggregates, which
substitutes for scan speed at this scale; and a second storage engine
carries a second durability proof forever.
