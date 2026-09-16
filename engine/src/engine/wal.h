// Write-ahead log: full page images, one file per database instance.
//
// A transaction is a run of frames ending in a commit frame (one whose
// db_size_after_commit is non-zero). Commit is durable when the frames are
// written and the file is synced. Recovery replays the longest valid prefix
// of committed transactions and truncates anything after it. The index is
// in memory only: this process is the only one that opens the file.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "archivum/engine/page_format.h"
#include "archivum/status.h"
#include "archivum/vfs.h"

namespace archivum::engine {

struct WalRecoveryInfo {
  FrameNo committed_frames = 0;    // frames that belong to complete transactions
  std::uint64_t dropped_bytes = 0;  // torn tail removed
  bool header_rewritten = false;
};

class Wal {
 public:
  static Result<std::unique_ptr<Wal>> open(Vfs& vfs, std::string path, std::uint32_t page_size,
                                           const std::array<std::byte, 16>& db_id,
                                           WalRecoveryInfo* info);
  ~Wal();

  // Last committed frame; 0 when the log holds no committed transaction.
  FrameNo last_commit() const { return last_commit_; }
  // Page count recorded by the commit at `commit_frame` (0 for none).
  std::uint64_t db_size_at(FrameNo commit_frame) const;
  // Latest frame for `page` at or before `snapshot`, or 0.
  FrameNo find(PageNo page, FrameNo snapshot) const;
  Status read_frame_page(FrameNo frame, std::span<std::byte> out);

  struct PageImage {
    PageNo page;
    std::span<const std::byte> data;  // exactly page_size bytes, already sealed
  };
  // Appends one transaction. The last image is the commit frame. Durable
  // and visible in the index when ok is returned.
  Status append_transaction(const std::vector<PageImage>& images, std::uint64_t db_size_after);

  // Latest frame per page among committed frames up to `up_to`.
  std::map<PageNo, FrameNo> latest_frames(FrameNo up_to) const;

  // Empties the log: truncate to a fresh header with new salts and sync.
  // The caller has checkpointed everything it wants to keep.
  Status reset();

  // After a failed append: removes whatever was written past the last
  // commit, best effort. The chain makes leftovers harmless even if this
  // fails; it exists so that a transaction the caller was told failed does
  // not reappear after a restart.
  Status discard_uncommitted();

  FrameNo frame_count() const { return next_frame_ - 1; }
  // After an I/O error the log's on-disk state is uncertain; it refuses
  // further appends until the database is reopened. Reads of committed
  // frames still work.
  bool failed() const { return failed_; }
  Status close();

 private:
  Wal(Vfs& vfs, std::string path, std::uint32_t page_size);
  Status recover(const std::array<std::byte, 16>& db_id, WalRecoveryInfo* info);
  Status write_header();
  std::uint64_t frame_offset(FrameNo frame) const {
    return WalHeader::kEncodedBytes +
           (frame - 1) * (WalFrameHeader::kEncodedBytes + static_cast<std::uint64_t>(page_size_));
  }
  void publish(const std::vector<std::pair<PageNo, FrameNo>>& frames, FrameNo commit_frame,
               std::uint64_t db_size);

  Vfs& vfs_;
  std::string path_;
  std::uint32_t page_size_;
  std::unique_ptr<File> file_;
  WalHeader header_;
  FrameNo next_frame_ = 1;   // next frame number to append
  FrameNo last_commit_ = 0;
  std::uint32_t chain_ = 0;  // checksum of the last committed frame, or the header's
  bool failed_ = false;
  std::unordered_map<PageNo, std::vector<FrameNo>> index_;  // ascending frames per page
  std::map<FrameNo, std::uint64_t> commits_;                 // commit frame -> db size
};

}  // namespace archivum::engine
