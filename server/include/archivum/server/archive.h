// The off-host archive copy and its health check (mandatory, instructions
// v2 Q8 and the Stage 5 scope). A background thread copies every archived
// log segment the destination lacks, and a full backup at the backup
// cadence, into `backup.destination`. The health endpoint reports the last
// successful copy and fails loudly (503) until one has happened and
// whenever the last one is older than twice the cadence.
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
  Vfs& vfs_;
  engine::Store& store_;
  DatabaseConfig database_;
  BackupConfig backup_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  ShipStatus status_;
  std::thread thread_;
  bool stop_ = false;
};

}  // namespace archivum::server
