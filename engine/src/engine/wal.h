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

// One committed transaction in the log, with what its page-0 image says.
struct WalCommit {
  FrameNo frame = 0;  // the commit frame (the page-0 image)
  std::uint64_t db_size = 0;
  std::uint64_t change_counter = 0;
  std::int64_t commit_time_us = 0;
};

struct WalOpenOptions {
  std::array<std::byte, 16> db_id{};
  std::uint64_t new_base_change_counter = 0;  // header base when the log is created
  // Read-only: never rewrites or truncates the file; an invalid header is
  // Corrupt instead of a fresh log. For archived segments and recovery.
  bool read_only = false;
};

class Wal {
 public:
  static Result<std::unique_ptr<Wal>> open(Vfs& vfs, std::string path, std::uint32_t page_size,
                                           const WalOpenOptions& options, WalRecoveryInfo* info);
  ~Wal();

  std::uint64_t base_change_counter() const { return header_.base_change_counter; }
  const std::vector<WalCommit>& commits() const { return commit_list_; }
  // Page number carried by `frame` (1-based; frames past the last commit
  // are not addressable).
  PageNo frame_page(FrameNo frame) const { return frame_pages_[frame - 1]; }
  // Bytes of the file that belong to committed transactions, header included.
  std::uint64_t committed_bytes() const { return frame_offset(last_commit_ + 1); }
  // Writes header plus committed frames to a new file at `path` and syncs
  // it and its directory. For log archiving before a reset.
  Status copy_committed_to(Vfs& vfs, const std::string& path);

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

  // Empties the log: truncate to a fresh header with new salts, the given
  // base change counter, and sync. The caller has checkpointed everything
  // it wants to keep, and `base_change_counter` is what the data file says.
  Status reset(std::uint64_t base_change_counter);

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
  Status recover(const WalOpenOptions& options, WalRecoveryInfo* info);
  Status write_header();
  std::uint64_t frame_offset(FrameNo frame) const {
    return WalHeader::kEncodedBytes +
           (frame - 1) * (WalFrameHeader::kEncodedBytes + static_cast<std::uint64_t>(page_size_));
  }
  void publish(const std::vector<std::pair<PageNo, FrameNo>>& frames, const WalCommit& commit);

  Vfs& vfs_;
  std::string path_;
  std::uint32_t page_size_;
  std::unique_ptr<File> file_;
  WalHeader header_;
  FrameNo next_frame_ = 1;   // next frame number to append
  FrameNo last_commit_ = 0;
  std::uint32_t chain_ = 0;  // checksum of the last committed frame, or the header's
  bool failed_ = false;
  bool read_only_ = false;
  std::unordered_map<PageNo, std::vector<FrameNo>> index_;  // ascending frames per page
  std::map<FrameNo, std::uint64_t> commits_;                 // commit frame -> db size
  std::vector<WalCommit> commit_list_;                       // in frame order
  std::vector<PageNo> frame_pages_;                          // frame - 1 -> page, committed frames
};

}  // namespace archivum::engine
