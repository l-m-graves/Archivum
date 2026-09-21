// Point-in-time recovery: a backup plus archived log segments replayed to
// a target change counter or wall-clock time (docs/backup-recovery.md).
//
// A backup is a database file as of one commit (Db::backup). Every
// checkpoint with DbOptions::archive_dir set writes the log it is about to
// empty to `<archive_dir>/<db id>-<base>.wal`, where `base` is the change
// counter of the file the log continued from. Recovery therefore chains:
// starting from the backup's counter it looks for the segment with that
// base, applies its committed transactions in order, and continues with
// the segment whose base is the last counter applied. The live log of the
// database (not yet archived) may be given as the final segment.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "archivum/status.h"
#include "archivum/vfs.h"

namespace archivum::engine {

struct RecoveryTarget {
  // Apply commits with change counter <= this (all, when absent).
  std::optional<std::uint64_t> change_counter;
  // Apply commits with commit time <= this, µs since the epoch (all, when absent).
  std::optional<std::int64_t> time_us;
};

struct RecoveryReport {
  std::uint64_t start_change_counter = 0;  // the backup's
  std::uint64_t change_counter = 0;        // where the result is
  std::int64_t commit_time_us = 0;         // of the last commit applied (0 if none)
  std::uint64_t transactions_applied = 0;
  std::uint64_t segments_applied = 0;
  bool target_reached = false;  // false when the segments ran out before the target
};

// Copies `backup_path` to `out_path` and replays archived segments from
// `archive_dir` (and `live_wal_path`, if not empty and matching) up to the
// target. `out_path` must not exist, and no log may sit beside it. The
// result is a database file with no log, synced.
Result<RecoveryReport> recover_to_point(Vfs& vfs, const std::string& backup_path, const std::string& archive_dir,
                                        const std::string& live_wal_path, const RecoveryTarget& target,
                                        const std::string& out_path);

// Byte copy of a file through the VFS, synced (file, then its directory).
Status copy_file(Vfs& vfs, const std::string& from, const std::string& to);

// CRC32C of a whole file, with its size. For verifying a copy against its
// source before counting it as shipped (Stage 6 ruling: a truncated copy
// the health check counts as off-host defeats off-host archiving).
struct FileDigest {
  std::uint64_t size = 0;
  std::uint32_t crc32c = 0;
  bool operator==(const FileDigest& o) const { return size == o.size && crc32c == o.crc32c; }
};
Result<FileDigest> file_digest(Vfs& vfs, const std::string& path);

// Verifies a backup file page by page: header decodable, size a whole
// number of pages matching the header's page count, every page's checksum
// intact. Corrupt names the first bad page.
Status verify_backup_file(Vfs& vfs, const std::string& path);

}  // namespace archivum::engine
