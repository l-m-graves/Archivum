# Append-only journal format

`engine/include/archivum/journal.h`. Version 1. All integers little-endian.

```
offset  size  field
0       8     magic  "ARCHJNL1"
8       4     version = 1
12      4     crc32c(bytes 0..12)
16      ...   records
```

Each record:

```
offset  size  field
0       4     length      payload bytes, 1 <= length <= max_record_bytes (default 16 MiB)
4       4     crc32c      over (length field bytes || payload)
8       len   payload
```

## Append

One `write()` of the whole frame at the current end, then `sync()`. The
record is acknowledged only after `sync()` returns ok.

## Recovery (every open of an existing file)

1. Read the header. If it is torn or missing, rewrite it (its content is a
   constant) and sync. Records behind a torn header are still scanned:
   with an honest fsync none can exist, but with a lying fsync and reordered
   writes they can, and each carries its own checksum.
2. Walk records from offset 16. Stop at the first record whose header is
   short, whose length is 0 or over the limit or past end of file, or whose
   checksum does not match.
3. Truncate the file to the end of the last valid record and sync. The
   number of bytes removed is reported as `dropped_tail_bytes()`.

## Failure policy

After any I/O error on append the journal refuses further appends until it
is reopened. Reopening runs recovery. Rationale: after a failed fsync the OS
may have discarded dirty pages, so nothing since the last successful sync
can be trusted; recovery re-reads the truth from disk.

## Ownership

The journal owns its path. A non-journal file found there is reinitialised.
Callers must not point a journal at a file they want preserved.
