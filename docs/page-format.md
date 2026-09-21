# Page and log formats

Normative for `engine/include/archivum/engine/page_format.h`. All integers
are little-endian. Format version 3 (2 added `commit_time_us`; 3 changed the
catalog row layout, `docs/store-format.md`; nothing was deployed under 1 or 2).

## Database file

A sequence of fixed-size pages. The page size is a power of two in
[512, 65536], chosen at creation and recorded in the header; the default is
4096. Page numbers start at 0. Page 0 is the header page.

### Every page

```
offset            size  field
0                 N-4   page body (owned by the page's user)
N-4               4     CRC32C of bytes [0, N-4)
```

A page whose trailer does not verify is never returned to a caller: the
read fails with `Corrupt`. `Db::usable_page_bytes()` is N-4.

### Page 0, the header (inside the body)

```
offset  size  field
0       8     magic "ARCHVDB1"
8       4     page_size
12      4     format_version = 3
16      8     page_count      pages in the logical database, header included
24      8     freelist_head   page number of the first free page, 0 = none
32      8     freelist_count
40      8     change_counter  incremented by every committed write transaction
48      16    db_id           random; the log carries the same value
64      ...   reserved, zero
```

The header is rewritten by every committing transaction as that
transaction's commit frame, so `page_count`, the free list, and
`change_counter` are always the values as of the last commit.

### Free pages

A free page's body begins with:

```
offset  size  field
0       8     magic "ARCHFREE"
8       8     next            next free page, 0 = end of list
```

The rest of the body is zero. Allocation pops the head; freeing pushes.
`Db::check()` walks the list and verifies bounds, absence of cycles, page
validity, and that the walked count equals `freelist_count`.

## Write-ahead log (`<db>.wal`)

```
header (48 bytes)
frame 1: frame header (32 bytes) + page (page_size bytes)
frame 2: ...
```

### Log header

```
offset  size  field
0       8     magic "ARCHWAL1"
8       4     page_size
12      4     salt1           fresh random values every time the log is reset
16      4     salt2
20      16    db_id           must equal the database header's db_id
36      8     reserved, zero
44      4     CRC32C of bytes [0, 44)
```

A log whose header does not verify, or whose page size or db_id do not
match the database, is treated as empty and rewritten: with an honest fsync
the header is durable before any frame can be acknowledged, so no committed
transaction can be behind an unreadable header.

### Frame header

```
offset  size  field
0       8     page_no
8       8     db_size_after_commit   non-zero only on the last frame of a transaction: the page count
16      4     salt1                  must equal the log header's
20      4     salt2
24      4     checksum               chained, see below
28      4     reserved, zero
```

`checksum` = CRC32C over (previous frame's checksum || this header with the
checksum field zero || the page). For the first frame the previous value is
the log header's checksum. The chain makes a frame left over from an
earlier, failed transaction unverifiable as the continuation of a later
one, even though its own bytes are intact. The page inside a frame also
carries its own trailer, which is verified independently.

### Transactions in the log

A transaction is a run of frames whose last frame has a non-zero
`db_size_after_commit`. Frames of one transaction are written with one
write and made durable with one sync; the in-memory index publishes them
only after the sync returns. Recovery replays the longest prefix of the log
in which every frame verifies (salt, chain, page trailer) and that ends in
a commit frame; everything after it is truncated.

### Reset

After a checkpoint has copied every committed page into the data file and
synced it, the log is truncated to zero, a header with new salts is written,
and the file is synced. If the truncate succeeds and the header write fails,
the in-memory index is already empty (the frames are gone) and the log
refuses further appends until the database is reopened.
