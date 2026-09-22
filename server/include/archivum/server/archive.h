// The off-host archive copy and its health check (mandatory, instructions
// v2 Q8 and the Stage 5 scope). A background thread copies every archived
// log segment the destination lacks, and a full backup at the backup
// cadence, into `backup.destination`. The health endpoint reports the last
// successful copy and fails loudly (503) until one has happened and
// whenever the last one is older than twice the cadence.
//
// Every copy is verified before it counts (Stage 6 ruling), after its
// final rename: a segment's copy is read back under its final name and
// its size and CRC32C compared with the source; a backup is checked page
// by page. A mismatch removes the file, fails the pass, and is an alert.
// Each file is written under a `.part` name, synced, renamed into place
// (MoveFileExW with MOVEFILE_WRITE_THROUGH on Windows; rename plus a
// directory fsync on POSIX; docs/durability.md), then verified.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "archivum/engine/store.h"
#include "archivum/server/config.h"
#include "archivum/status.h"
#include "archivum/vfs.h"

namespace archivum::server {

struct ShipStatus {
  std::int64_t last_success_us = 0;  // 0: never
  std::int64_t last_attempt_us = 0;
  std::int64_t last_backup_us = 0;
  std::string last_error;
  std::uint64_t segments_shipped = 0;
  std::uint64_t backups_shipped = 0;
  std::uint64_t failures = 0;
  std::uint64_t verification_failures = 0;  // copies that did not match their source
};

class ArchiveShipper {
 public:
  ArchiveShipper(Vfs& vfs, engine::Store& store, DatabaseConfig database, BackupConfig backup);
  ~ArchiveShipper();

  // One pass: segments the destination lacks, then a backup if due.
  Status ship_once();
  void start();
  void stop();
  ShipStatus status() const;
  // Healthy when the last successful pass is recent enough.
  bool healthy(std::int64_t now_us) const;
  std::string unhealthy_reason(std::int64_t now_us) const;

 private:
  void run();
  Status verify_copy(const std::string& src, const std::string& copy);
  Vfs& vfs_;
  engine::Store& store_;
  DatabaseConfig database_;
  BackupConfig backup_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  ShipStatus status_;
  std::thread thread_;
  bool stop_ = false;
  std::atomic<std::uint64_t> verification_failures_{0};
};

}  // namespace archivum::server
