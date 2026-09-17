// The storage engine's page-level API: one database file plus its
// write-ahead log, opened by one Db instance.
//
// Model (docs/engine-design.md, docs/durability.md):
//   * Fixed-size pages, page 0 is the header, every page carries a CRC32C.
//   * All writes go through the WAL as full page images; the data file is
//     written only by checkpoint. A committed transaction is durable once
//     its frames are synced.
//   * One writer at a time (begin_write blocks). Readers take a snapshot of
//     the committed state at begin and see exactly that state until they
//     end; a writer never blocks readers and readers never block a writer.
//   * Db is an instance. There is no global state: two Db objects on two
//     files are fully independent, never share a lock, and a transaction
//     never spans both. This is the two-database design decided for the
//     operational and analytical stores.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "archivum/engine/page_format.h"
#include "archivum/status.h"
#include "archivum/vfs.h"

namespace archivum::engine {

struct DbOptions {
  std::uint32_t page_size = kDefaultPageSize;  // used only when creating
  std::size_t cache_pages = 1024;
  // A commit that leaves at least this many frames in the log triggers a
  // checkpoint attempt (skipped while readers are active).
  std::uint64_t checkpoint_threshold_frames = 1000;
  bool create_if_missing = true;
};

struct DbStats {
  std::uint64_t page_count = 0;
  std::uint64_t freelist_count = 0;
  std::uint64_t change_counter = 0;
  std::uint64_t wal_frames = 0;
  std::uint64_t checkpoints = 0;
  std::uint64_t active_readers = 0;
  std::uint64_t wal_recovered_frames = 0;    // at the last open
  std::uint64_t wal_dropped_tail_bytes = 0;  // at the last open
};

struct CheckReport {
  bool ok = true;
  std::vector<std::string> problems;
  std::uint64_t pages_checked = 0;
  std::uint64_t free_pages_walked = 0;
  std::vector<PageNo> free_pages;  // the free list in walk order, for ownership cross-checks
};

class Db;

class ReadTxn {
 public:
  ~ReadTxn();
  ReadTxn(const ReadTxn&) = delete;
  ReadTxn& operator=(const ReadTxn&) = delete;

  // `out` must be exactly page_size bytes. The page is returned as stored,
  // trailer included, and its checksum has been verified.
  Status read_page(PageNo page, std::span<std::byte> out) const;
  std::uint64_t page_count() const { return page_count_; }
  std::uint64_t change_counter() const { return change_counter_; }

 private:
  friend class Db;
  ReadTxn(Db& db, FrameNo snapshot, std::uint64_t page_count, std::uint64_t change_counter);
  Db& db_;
  FrameNo snapshot_;
  std::uint64_t page_count_;
  std::uint64_t change_counter_;
};

class WriteTxn {
 public:
  // Rolls back if neither commit() nor rollback() was called.
  ~WriteTxn();
  WriteTxn(const WriteTxn&) = delete;
  WriteTxn& operator=(const WriteTxn&) = delete;

  // Reads see this transaction's own writes.
  Status read_page(PageNo page, std::span<std::byte> out) const;
  // `data` must be exactly page_size bytes; the last kPageTrailerBytes are
  // replaced by the checksum at commit. Page 0 is not writable.
  Status write_page(PageNo page, std::span<const std::byte> data);
  // Returns a zeroed page, from the free list or by growing the file.
  Result<PageNo> allocate_page();
  Status free_page(PageNo page);
  std::uint64_t page_count() const { return header_.page_count; }
  // The committed state this transaction started from; a commit that writes
  // anything produces change_counter() + 1.
  std::uint64_t change_counter() const { return snapshot_header_.change_counter; }

  // Durable when ok is returned.
  Status commit();
  void rollback();

 private:
  friend class Db;
  WriteTxn(Db& db, FrameNo snapshot, DbHeader header, std::unique_lock<std::mutex> lock);
  void finish();
  Db& db_;
  FrameNo snapshot_;
  DbHeader header_;
  DbHeader snapshot_header_;
  std::map<PageNo, std::vector<std::byte>> dirty_;
  std::set<PageNo> freed_;
  std::unique_lock<std::mutex> lock_;
  bool finished_ = false;
};

class Db {
 public:
  static Result<std::unique_ptr<Db>> open(Vfs& vfs, std::string path, DbOptions options = {});
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  Result<std::unique_ptr<ReadTxn>> begin_read();
  // Blocks while another write transaction is active.
  Result<std::unique_ptr<WriteTxn>> begin_write();

  // Copies committed pages from the log into the data file and empties the
  // log. Returns Busy while read transactions are active.
  Status checkpoint();

  // Verifies the committed state: header, every page checksum, and the
  // free list. Runs as a reader.
  Result<CheckReport> check();

  DbStats stats() const;
  std::uint32_t page_size() const { return page_size_; }
  std::uint32_t usable_page_bytes() const { return page_size_ - kPageTrailerBytes; }
  const std::string& path() const { return path_; }
  const std::string& wal_path() const { return wal_path_; }
  Status close();

 private:
  friend class ReadTxn;
  friend class WriteTxn;
  struct Impl;
  explicit Db(Vfs& vfs, std::string path, DbOptions options);

  // Read a committed page as of `snapshot` (WAL first, then the file).
  Status read_committed(PageNo page, FrameNo snapshot, std::span<std::byte> out);
  Result<DbHeader> committed_header(FrameNo snapshot);
  Result<bool> recover_page0_from_wal(std::span<std::byte> page0);
  Status commit_frames(std::vector<std::pair<PageNo, std::vector<std::byte>>>& images,
                       const DbHeader& header, std::uint64_t db_size);
  Status checkpoint_locked();
  void reader_ended();
  void writer_ended();

  Vfs& vfs_;
  std::string path_;
  std::string wal_path_;
  DbOptions options_;
  std::uint32_t page_size_ = kDefaultPageSize;
  std::unique_ptr<Impl> impl_;
};

}  // namespace archivum::engine
