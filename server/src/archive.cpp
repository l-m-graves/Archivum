#include "archivum/server/archive.h"

#include <set>

#include "archivum/engine/recovery.h"
#include "archivum/server/log.h"

namespace archivum::server {

ArchiveShipper::ArchiveShipper(Vfs& vfs, engine::Store& store, DatabaseConfig database, BackupConfig backup)
    : vfs_(vfs), store_(store), database_(std::move(database)), backup_(std::move(backup)) {}

ArchiveShipper::~ArchiveShipper() { stop(); }

Status ArchiveShipper::ship_once() {
  const std::int64_t now = store_.db().now_us();
  {
    std::lock_guard<std::mutex> lock(mu_);
    status_.last_attempt_us = now;
  }
  auto finish = [&](Status s, std::uint64_t segments, bool backup_done) {
    std::lock_guard<std::mutex> lock(mu_);
    if (s.ok()) {
      status_.last_success_us = now;
      status_.last_error.clear();
      status_.segments_shipped += segments;
      if (backup_done) {
        status_.last_backup_us = now;
        ++status_.backups_shipped;
      }
    } else {
      status_.last_error = s.to_string();
      ++status_.failures;
      log(LogLevel::Alert, "archive.ship_failed", {{"error", s.to_string()}, {"destination", backup_.destination}});
    }
    return s;
  };
  auto dest_names = vfs_.list(backup_.destination);
  if (!dest_names.ok()) return finish(Status::io("destination unavailable: " + dest_names.status().to_string()), 0, false);
  std::set<std::string> have(dest_names.value().begin(), dest_names.value().end());
  auto local = vfs_.list(database_.archive_dir);
  if (!local.ok() && local.status().code() != ErrorCode::NotFound) return finish(local.status(), 0, false);
  std::uint64_t shipped = 0;
  if (local.ok()) {
    for (const std::string& name : local.value()) {
      if (name.size() < 4 || name.substr(name.size() - 4) != ".wal" || have.count(name)) continue;
      // Copy to a temporary name, verify the copy against the source
      // (size and CRC32C, read back from the destination), then rename
      // and sync the destination directory: a reader of the destination
      // never sees a half-written segment, a truncated or corrupted copy
      // is never counted as shipped, and the name is durable.
      const std::string src = database_.archive_dir + "/" + name;
      const std::string tmp = backup_.destination + "/" + name + ".part";
      if (Status s = engine::copy_file(vfs_, src, tmp); !s.ok()) return finish(s, shipped, false);
      if (Status s = verify_copy(src, tmp); !s.ok()) return finish(s, shipped, false);
      if (Status s = vfs_.rename(tmp, backup_.destination + "/" + name); !s.ok()) return finish(s, shipped, false);
      if (Status s = vfs_.sync_directory(backup_.destination); !s.ok()) return finish(s, shipped, false);
      ++shipped;
    }
  }
  bool backup_done = false;
  std::int64_t last_backup = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_backup = status_.last_backup_us;
  }
  if (last_backup == 0 || now - last_backup >= static_cast<std::int64_t>(backup_.backup_cadence_seconds) * 1'000'000) {
    const std::string tmp = backup_.destination + "/backup.part";
    auto cc = store_.backup(vfs_, tmp);
    if (!cc.ok()) return finish(cc.status(), shipped, false);
    // The backup is written straight to the destination; verify it page by
    // page (every page carries its checksum) before it gets its name.
    if (Status s = engine::verify_backup_file(vfs_, tmp); !s.ok()) {
      (void)vfs_.remove(tmp);
      ++verification_failures_;
      log(LogLevel::Alert, "archive.backup_verification_failed", {{"error", s.to_string()}, {"destination", backup_.destination}});
      return finish(s, shipped, false);
    }
    const std::string final_name = backup_.destination + "/backup-" + std::to_string(cc.value()) + ".db";
    if (Status s = vfs_.rename(tmp, final_name); !s.ok()) return finish(s, shipped, false);
    if (Status s = vfs_.sync_directory(backup_.destination); !s.ok()) return finish(s, shipped, false);
    backup_done = true;
  }
  if (shipped > 0 || backup_done) {
    log(LogLevel::Info, "archive.shipped", {{"segments", shipped}, {"backup", backup_done}, {"destination", backup_.destination}});
  }
  return finish(Status(), shipped, backup_done);
}

Status ArchiveShipper::verify_copy(const std::string& src, const std::string& copy) {
  auto a = engine::file_digest(vfs_, src);
  if (!a.ok()) return a.status();
  auto b = engine::file_digest(vfs_, copy);
  if (!b.ok()) return b.status();
  if (a.value() == b.value()) return Status();
  (void)vfs_.remove(copy);
  ++verification_failures_;
  log(LogLevel::Alert, "archive.copy_verification_failed",
      {{"source", src}, {"source_bytes", a.value().size}, {"copy_bytes", b.value().size}, {"destination", backup_.destination}});
  return Status::io("off-host copy of " + src + " does not match its source (" + std::to_string(a.value().size) + " bytes, crc " +
                    std::to_string(a.value().crc32c) + " vs " + std::to_string(b.value().size) + " bytes, crc " +
                    std::to_string(b.value().crc32c) + ")");
}

void ArchiveShipper::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void ArchiveShipper::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void ArchiveShipper::run() {
  while (true) {
    (void)ship_once();
    std::unique_lock<std::mutex> lock(mu_);
    if (cv_.wait_for(lock, std::chrono::seconds(backup_.archive_cadence_seconds), [this] { return stop_; })) return;
  }
}

ShipStatus ArchiveShipper::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  ShipStatus s = status_;
  s.verification_failures = verification_failures_.load();
  return s;
}

bool ArchiveShipper::healthy(std::int64_t now_us) const { return unhealthy_reason(now_us).empty(); }

std::string ArchiveShipper::unhealthy_reason(std::int64_t now_us) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_.last_success_us == 0) {
    return status_.last_error.empty() ? "no off-host copy has succeeded yet" : "off-host copy failing: " + status_.last_error;
  }
  const std::int64_t limit = static_cast<std::int64_t>(backup_.archive_cadence_seconds) * 2 * 1'000'000;
  if (now_us - status_.last_success_us > limit) {
    return "last off-host copy is older than twice the cadence" +
           (status_.last_error.empty() ? std::string() : ": " + status_.last_error);
  }
  return "";
}

}  // namespace archivum::server
