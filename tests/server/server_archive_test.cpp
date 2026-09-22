// The archive shipper's verification runs after the final rename: a
// destination whose rename delivers a truncated or altered file is caught
// by the read-back under the final name, the file is removed, the pass
// fails, and nothing is counted as shipped. Engine and shipper only, no
// listener.
#include <memory>
#include <string>

#include "archivum/core/module.h"
#include "archivum/engine/recovery.h"
#include "archivum/punchline/schema.h"
#include "archivum/server/archive.h"
#include "archivum/server/log.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;
using namespace archivum::server;

namespace {

// A VFS whose renames into the destination directory deliver a damaged
// file: truncated by one byte, or with one byte flipped.
class DamagingVfs final : public Vfs {
 public:
  explicit DamagingVfs(MemVfs& inner) : inner_(inner) {}
  std::string damage_prefix;  // renames of targets under this prefix are damaged
  bool flip = false;          // flip a byte instead of truncating
  int damaged = 0;

  Result<std::unique_ptr<File>> open(const std::string& path, OpenFlags flags) override { return inner_.open(path, flags); }
  Result<bool> exists(const std::string& path) override { return inner_.exists(path); }
  Status remove(const std::string& path) override { return inner_.remove(path); }
  Status rename(const std::string& from, const std::string& to) override {
    Status s = inner_.rename(from, to);
    if (!s.ok() || damage_prefix.empty() || to.rfind(damage_prefix, 0) != 0) return s;
    std::vector<std::byte> bytes = inner_.contents(to);
    if (bytes.empty()) return s;
    if (flip) bytes[bytes.size() / 2] ^= std::byte{0x01};
    else bytes.pop_back();
    inner_.put(to, bytes);
    ++damaged;
    return s;
  }
  Status sync_directory(const std::string& dir) override { return inner_.sync_directory(dir); }
  Result<std::vector<std::string>> list(const std::string& dir) override { return inner_.list(dir); }

 private:
  MemVfs& inner_;
};

}  // namespace

ARCHIVUM_TEST(shipper_verifies_after_the_final_rename_and_never_counts_a_bad_copy) {
  MemVfs mem;
  DamagingVfs vfs(mem);
  DbOptions o;
  o.page_size = 1024;
  o.checkpoint_threshold_frames = 8;
  o.archive_dir = "a/archive";
  auto st = Store::open(vfs, "a/live.db", o);
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  REQUIRE_OK(core::migrate_all(store, {&punchline::module()}).status());
  for (int i = 1; i <= 40; ++i) {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->insert("pay_codes", {Value::text("c" + std::to_string(i)), Value::text("x"), Value::boolean(true), Value::boolean(true)}));
    REQUIRE_OK(w.value()->commit());
  }
  REQUIRE_OK(store.checkpoint());
  auto segments = vfs.list("a/archive");
  REQUIRE_OK(segments.status());
  REQUIRE(!segments.value().empty());
  mem.put("a/offhost/.keep", {});
  DatabaseConfig db;
  db.path = "a/live.db";
  db.archive_dir = "a/archive";
  BackupConfig bk;
  bk.destination = "a/offhost";
  bk.archive_cadence_seconds = 1;
  bk.backup_cadence_seconds = 3600;
  ArchiveShipper shipper(vfs, store, db, bk);
  // A destination that truncates on rename: the pass fails, nothing counts,
  // and the bad file is gone so the next pass copies again.
  vfs.damage_prefix = "a/offhost/";
  Status s = shipper.ship_once();
  CHECK(!s.ok());
  CHECK_MSG(s.message().find("does not match its source") != std::string::npos, s.message());
  CHECK(vfs.damaged >= 1);
  CHECK(shipper.status().segments_shipped == 0 && shipper.status().backups_shipped == 0);
  CHECK(shipper.status().verification_failures == 1);
  CHECK(!shipper.healthy(store.db().now_us()));
  for (const std::string& name : segments.value()) CHECK(!vfs.exists("a/offhost/" + name).value());
  // A flipped byte in the middle: same size, caught by the CRC.
  vfs.flip = true;
  s = shipper.ship_once();
  CHECK(!s.ok() && shipper.status().verification_failures == 2);
  // An honest destination: everything ships, verifies and counts.
  vfs.damage_prefix.clear();
  REQUIRE_OK(shipper.ship_once());
  CHECK(shipper.status().segments_shipped == segments.value().size());
  CHECK(shipper.status().backups_shipped == 1);
  CHECK(shipper.healthy(store.db().now_us()));
  for (const std::string& name : segments.value()) {
    auto a = file_digest(vfs, "a/archive/" + name);
    auto b = file_digest(vfs, "a/offhost/" + name);
    REQUIRE_OK(a.status());
    REQUIRE_OK(b.status());
    CHECK(a.value() == b.value());
  }
  // A backup that already exists at the destination is never rewritten:
  // a fresh shipper at the same change counter counts it and leaves it.
  ArchiveShipper again(vfs, store, db, bk);
  REQUIRE_OK(again.ship_once());
  CHECK(again.status().backups_shipped == 1);
  // A backup damaged by the rename is caught page by page and removed.
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->insert("pay_codes", {Value::text("late"), Value::text("x"), Value::boolean(true), Value::boolean(true)}));
    REQUIRE_OK(w.value()->commit());
  }
  vfs.damage_prefix = "a/offhost/";
  vfs.flip = true;
  ArchiveShipper second(vfs, store, db, bk);
  s = second.ship_once();
  CHECK(!s.ok());
  CHECK_MSG(s.message().find("checksum mismatch") != std::string::npos || s.message().find("bytes") != std::string::npos, s.message());
  CHECK(second.status().backups_shipped == 0);
  auto names = vfs.list("a/offhost");
  REQUIRE_OK(names.status());
  int backups = 0;
  for (const std::string& n : names.value()) backups += n.rfind("backup-", 0) == 0;
  CHECK(backups == 1);  // the honest one from before; the damaged one was removed
}
