# Store format: catalog, tables, indexes, keys and rows

Code: `engine/src/engine/store.cpp`, `record.cpp`, `types.cpp`; APIs in
`engine/include/archivum/engine/{store,record,types}.h`.

## Types

| Type | Storage | Notes |
|---|---|---|
| integer | int64 | |
| decimal | int64 | scaled by the column's declared scale (0 to 18); 12.34 at scale 2 is 1234. Never floating point |
| text | bytes | validated as UTF-8 on insert; compared bytewise, which is code point order |
| blob | bytes | |
| timestamp | int64 | microseconds since the Unix epoch, UTC; an instant |
| boolean | u8 | |
| uuid | 16 bytes | |

Every column is nullable unless declared otherwise; primary key columns
are never nullable. NULL sorts before every value.

## Key encoding (order preserving)

A key is the concatenation of its components. Each component starts with
a tag byte: `0x00` for NULL (nothing follows), `0x01` otherwise, then:

- integer, decimal, timestamp: the value with its sign bit flipped, big-endian;
- boolean: one byte;
- uuid: the 16 bytes;
- text, blob: the bytes with `0x00` escaped as `0x00 0xFF`, terminated by
  `0x00 0x00`.

`memcmp` order of the encoding equals the type order component by
component, and a key that starts with the encoding of a prefix of the
columns has exactly those column values, which is what makes prefix
bounds on scans exact. `tests/unit/record_test.cpp` checks the ordering
across the sign boundary, prefixes, embedded NULs, and composites.

## Row encoding

`u16 column count`, then per column `u8 kind` (0 NULL, 1 integer,
2 decimal, 3 text, 4 blob, 5 timestamp, 6 boolean, 7 uuid) and the
payload: 8 bytes little-endian for the integer kinds, 1 byte for boolean,
16 for uuid, `u32 length | bytes` for text and blob. Decoding rejects
truncation, trailing bytes and unknown kinds as `Corrupt`.

## Trees

- **Catalog:** the b-tree rooted at page 1, created when a store is
  first opened. Key `0x00` holds the schema version as a one-column row.
  Key `0x01 || table name` holds the table definition as a row (name,
  root page, columns, primary key, indexes with their root pages and
  uniqueness, foreign keys, checks: name, column, other column or empty,
  op, operand count, operands). A table's catalog row is rewritten
  whole on every change to it.
- **Table:** one b-tree; key = key encoding of the primary key columns,
  value = row encoding of the whole row, primary key columns included.
- **Index:** one b-tree per index; key = key encoding of the indexed
  columns followed by the key encoding of the primary key; value empty.
  A non-unique index therefore has distinct keys, and every entry names
  its row. A unique index is enforced by a prefix lookup before the put;
  entries with a NULL in any indexed column are exempt (NULLs distinct).

The key length limit is the b-tree's (`docs/btree-format.md`): a primary
key, or an index key plus the primary key, longer than it is refused as
`InvalidArgument` at insert, update or index creation.

## Constraints

Enforced in `Writer` and nowhere else, before any page is modified, so a
violation leaves the transaction exactly as it was:

| Constraint | Rule |
|---|---|
| type | a value must match its column's type; text must be valid UTF-8 |
| NOT NULL | declared, or implied for primary key columns |
| primary key | absent before insert; update never changes it (delete and insert instead) |
| unique index | no other row with the same non-NULL indexed values |
| foreign key | RESTRICT both ways: a child row's non-NULL key must find a parent at insert and at update when it changes; a parent row cannot be deleted, or have its referenced values changed, while a child points at it. Referenced columns are the parent's primary key or a unique index. The child table needs its primary key or an index whose leading columns are the foreign key columns, so the parent-side check is a prefix lookup, never a scan |
| check | `column op constant` with op in =, ≠, <, ≤, >, ≥, `column IN (constants)`, or `column op other_column` on the same row (Stage 6; the two columns must share a type and scale); a NULL on either side passes |

Violations return `ErrorCode::Constraint` and the message names the
constraint. Schema errors (unknown column, unsupported definition) are
`InvalidArgument`; a name in use is `AlreadyExists`.

## Schema changes

`create_table`, `drop_table`, `create_index` (built from the existing rows
in one pass, uniqueness checked), `drop_index` (refused while a foreign
key depends on it) and `set_schema_version` run inside the same write
transaction as row changes and commit or roll back with them. A migration
is one `Writer`. Readers hold the catalog of their snapshot; the store
caches the catalog per committed change counter.
