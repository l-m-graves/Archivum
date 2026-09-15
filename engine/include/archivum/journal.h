// Append-only journal: length-prefixed records, CRC32C per record, fsync on
// append, recovery reads the longest valid prefix.
//
// This is the Stage 0 "toy log" the crash-injection harness is proven
// against, and it is also the Punchline client's local store until the
// engine is embedded in the client (instructions v2, section 0(b)). It is
// the write-ahead log's durability model in miniature. Format is documented
// in docs/journal-format.md.
//
// Guarantees (see docs/durability.md):
//   * When append() returns ok, the record is durable, provided the OS and
//     device honour fsync.
//   * After any crash, open() yields a prefix of the records that were
//     appended, in order, with no torn or corrupted record. A record whose
//     append() had not returned may or may not be present.
//   * After an I/O error the journal refuses further appends until reopened.
//     Reopening re-runs recovery. This is deliberate: after a failed fsync
//     the OS cache may have dropped dirty data, so nothing written since the
//     last successful sync can be trusted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "archivum/status.h"
#include "archivum/vfs.h"

namespace archivum {

struct JournalOptions {
  std::uint32_t max_record_bytes = 16u * 1024u * 1024u;
  // TEST ONLY. Skips the fsync after each append so that the crash harness
  // can demonstrate that it detects lost commits. Never set in production
  // code.
  bool unsafe_skip_sync = false;
};

class Journal {
 public:
  using Options = JournalOptions;

  static constexpr std::uint64_t kHeaderBytes = 16;
  static constexpr std::uint64_t kRecordHeaderBytes = 8;

  // Creates the file if it does not exist, otherwise recovers it.
  static Result<std::unique_ptr<Journal>> open(Vfs& vfs, std::string path, Options options = {});

  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Durable when ok is returned.
  Status append(std::span<const std::byte> payload);

  std::size_t count() const { return records_.size(); }
  Result<std::vector<std::byte>> read(std::size_t index) const;

  // Bytes discarded by recovery at the last open(): a torn tail.
  std::uint64_t dropped_tail_bytes() const { return dropped_tail_bytes_; }
  // True if the last open() found the header torn or missing and rewrote it.
  bool header_rewritten() const { return header_rewritten_; }
  std::uint64_t end_offset() const { return end_; }

  Status close();

 private:
  struct RecordRef {
    std::uint64_t offset;  // of the record header
    std::uint32_t length;  // payload bytes
  };

  Journal(Vfs& vfs, std::string path, Options options);
  Status create_new(const std::string& dir);
  Status recover();

  Vfs& vfs_;
  std::string path_;
  Options options_;
  std::unique_ptr<File> file_;
  std::vector<RecordRef> records_;
  std::uint64_t end_ = 0;
  std::uint64_t dropped_tail_bytes_ = 0;
  bool header_rewritten_ = false;
  bool failed_ = false;
};

}  // namespace archivum
