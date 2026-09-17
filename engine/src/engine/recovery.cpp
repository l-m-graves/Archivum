#include "archivum/engine/recovery.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

#include "archivum/engine/db.h"
#include "archivum/engine/page_format.h"
#include "wal.h"

namespace archivum::engine {
namespace {

std::string dir_of(const std::string& path) {
  const auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

}  // namespace

Status copy_file(Vfs& vfs, const std::string& from, const std::string& to) {
  auto in = vfs.open(from, OpenFlags{});
  if (!in.ok()) return in.status();
  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  flags.truncate = true;
  auto out = vfs.open(to, flags);
  if (!out.ok()) return out.status();
  auto size = in.value()->size();
  if (!size.ok()) return size.status();
  std::vector<std::byte> buf(1 << 16);
  std::uint64_t off = 0;
  while (off < size.value()) {
    std::size_t got = 0;
    if (Status s = in.value()->read(off, buf, got); !s.ok()) return s;
    if (got == 0) break;
    if (Status s = out.value()->write(off, std::span<const std::byte>(buf.data(), got)); !s.ok()) return s;
    off += got;
  }
  if (Status s = out.value()->sync(); !s.ok()) return s;
  if (Status s = out.value()->close(); !s.ok()) return s;
  return vfs.sync_directory(dir_of(to));
}

Result<RecoveryReport> recover_to_point(Vfs& vfs, const std::string& backup_path, const std::string& archive_dir,
                                        const std::string& live_wal_path, const RecoveryTarget& target,
                                        const std::string& out_path) {
  for (const std::string& p : {out_path, out_path + ".wal"}) {
    auto e = vfs.exists(p);
    if (!e.ok()) return e.status();
    if (e.value()) return Status::already_exists("recovery output exists: " + p);
  }
  if (Status s = copy_file(vfs, backup_path, out_path); !s.ok()) return s;

  OpenFlags flags;
  flags.write = true;
  auto out = vfs.open(out_path, flags);
  if (!out.ok()) return out.status();
  File& file = *out.value();
  std::vector<std::byte> probe(kMinPageSize);
  std::size_t got = 0;
  if (Status s = file.read(0, probe, got); !s.ok()) return s;
  if (got < DbHeader::kEncodedBytes) return Status::corrupt("backup too short: " + backup_path);
  auto hdr = DbHeader::decode(probe);
  if (!hdr.ok()) return hdr.status();
  const std::uint32_t page_size = hdr.value().page_size;
  std::vector<std::byte> page0(page_size);
  if (Status s = file.read(0, page0, got); !s.ok()) return s;
  if (got != page_size || !page_checksum_ok(page0)) return Status::corrupt("backup page 0 checksum mismatch");

  RecoveryReport rep;
  rep.start_change_counter = hdr.value().change_counter;
  rep.change_counter = hdr.value().change_counter;
  rep.commit_time_us = hdr.value().commit_time_us;
  const auto db_id = hdr.value().db_id;

  auto within_target = [&](const WalCommit& c) {
    if (target.change_counter && c.change_counter > *target.change_counter) return false;
    if (target.time_us && c.commit_time_us > *target.time_us) return false;
    return true;
  };
  auto target_done = [&]() {
    return target.change_counter && rep.change_counter >= *target.change_counter;
  };

  // Archived segments of this database, by base change counter. A backup
  // is usually taken mid-segment, so the segment to start from is the one
  // with the largest base at or below the backup's counter; commits at or
  // below the counter are skipped.
  std::map<std::uint64_t, std::string> segments;
  if (!archive_dir.empty()) {
    auto names = vfs.list(archive_dir);
    if (!names.ok() && names.status().code() != ErrorCode::NotFound) return names.status();
    if (names.ok()) {
      std::string id_hex;
      static const char* hex = "0123456789abcdef";
      for (std::byte b : db_id) {
        id_hex += hex[std::to_integer<unsigned>(b) >> 4];
        id_hex += hex[std::to_integer<unsigned>(b) & 15];
      }
      for (const std::string& n : names.value()) {
        if (n.size() != id_hex.size() + 1 + 20 + 4 || n.rfind(id_hex + "-", 0) != 0 || n.substr(n.size() - 4) != ".wal") continue;
        const std::string digits = n.substr(id_hex.size() + 1, 20);
        if (digits.find_first_not_of("0123456789") != std::string::npos) continue;
        segments[std::strtoull(digits.c_str(), nullptr, 10)] = archive_dir + "/" + n;
      }
    }
  }

  std::vector<std::byte> page(page_size);
  bool stop = false;
  bool live_tried = false;
  while (!stop && !target_done()) {
    // The archived segment with the largest base <= the current counter,
    // else the live log.
    std::string seg;
    auto it = segments.upper_bound(rep.change_counter);
    if (it != segments.begin()) {
      --it;
      seg = it->second;
      segments.erase(it);
    } else if (!live_wal_path.empty() && !live_tried) {
      live_tried = true;
      auto e = vfs.exists(live_wal_path);
      if (!e.ok()) return e.status();
      if (!e.value()) break;
      seg = live_wal_path;
    } else {
      break;
    }
    const bool is_live = seg == live_wal_path;
    WalOpenOptions wo;
    wo.db_id = db_id;
    wo.read_only = true;
    WalRecoveryInfo info;
    auto wal = Wal::open(vfs, seg, page_size, wo, &info);
    if (!wal.ok()) {
      if (is_live) break;  // a live log for another database is not ours to read
      return wal.status();
    }
    if (wal.value()->base_change_counter() > rep.change_counter) {
      if (is_live) break;
      return Status::corrupt("segment " + seg + " continues from change counter " +
                             std::to_string(wal.value()->base_change_counter()) + ", beyond " +
                             std::to_string(rep.change_counter));
    }
    FrameNo prev = 0;
    bool applied_any = false;
    for (const WalCommit& c : wal.value()->commits()) {
      if (c.change_counter <= rep.change_counter) {
        prev = c.frame;  // already in the file
        continue;
      }
      if (!within_target(c)) {
        stop = true;
        break;
      }
      if (c.change_counter != rep.change_counter + 1) {
        return Status::corrupt("segment " + seg + " commit " + std::to_string(c.change_counter) +
                               " does not follow " + std::to_string(rep.change_counter));
      }
      for (FrameNo f = prev + 1; f <= c.frame; ++f) {
        if (Status s = wal.value()->read_frame_page(f, page); !s.ok()) return s;
        const PageNo pn = wal.value()->frame_page(f);
        if (Status s = file.write(pn * page_size, page); !s.ok()) return s;
      }
      auto size = file.size();
      if (!size.ok()) return size.status();
      if (size.value() > c.db_size * page_size) {
        if (Status s = file.truncate(c.db_size * page_size); !s.ok()) return s;
      }
      prev = c.frame;
      rep.change_counter = c.change_counter;
      rep.commit_time_us = c.commit_time_us;
      ++rep.transactions_applied;
      applied_any = true;
      if (target_done()) {
        stop = true;
        break;
      }
    }
    if (applied_any) ++rep.segments_applied;
    if (is_live) break;
  }
  if (Status s = file.sync(); !s.ok()) return s;
  if (Status s = file.close(); !s.ok()) return s;
  if (Status s = vfs.sync_directory(dir_of(out_path)); !s.ok()) return s;
  // A counter target is reached exactly or not at all; a time-only target
  // is reached by definition (everything at or before it was applied).
  rep.target_reached = !target.change_counter || rep.change_counter == *target.change_counter;
  return rep;
}

}  // namespace archivum::engine
