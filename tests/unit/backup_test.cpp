// Online backup, restore, log archive and point-in-time recovery.
#include <atomic>
#include <map>
#include <thread>

#include "archivum/engine/migrate.h"
#include "archivum/engine/recovery.h"
#include "archivum/punchline/schema.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

std::int64_t g_clock = 1'000'000;
std::int64_t fake_now() { return g_clock += 1'000; }

DbOptions opts(const char* archive = "") {
  DbOptions o;
  o.page_size = 1024;
  o.cache_pages = 32;
  o.checkpoint_threshold_frames = 30;
  o.archive_dir = archive;
  o.now_us = &fake_now;
  return o;
}

std::string dump_bytes(Store& s) {
  auto d = s.dump();
  if (!d.ok()) return "error: " + d.status().to_string();
  std::string out = std::to_string(d.value().schema_version) + "|";
  for (const TableDump& t : d.value().tables) {
    const Bytes def = encode_row(encode_table_def(t.def));
    out.append(reinterpret_cast<const char*>(def.data()), def.size());
    for (const Row& r : t.rows) {
      const Bytes b = encode_row(r);
      out.append(reinterpret_cast<const char*>(b.data()), b.size());
    }
  }
  return out;
}

Row employee(std::int64_t id, std::int64_t at) {
  return {Value::integer(id), Value::text("E" + std::to_string(id)), Value::text("Name " + std::to_string(id)),
          Value::null(), Value::null(), Value::null(), Value::boolean(true), Value::null(), Value::text("UTC"),
          Value::timestamp(at), Value::timestamp(at)};
}

}  // namespace

ARCHIVUM_TEST(backup_under_concurrent_writes_restores_and_verifies) {
  MemVfs vfs;
  auto st = Store::open(vfs, "b/live.db", opts());
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  REQUIRE_OK(core::migrate_all(store, {&punchline::module()}).status());
  const std::uint64_t base = store.db().begin_read().value()->change_counter();
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::thread writer([&] {
    for (int i = 1; i <= 300 && !stop.load(); ++i) {
      auto w = store.begin_write();
      if (!w.ok() || !w.value()->insert("employees", employee(i, i)).ok() || !w.value()->commit().ok()) {
        failures.fetch_add(1);
        return;
      }
    }
  });
  std::vector<std::pair<std::uint64_t, std::string>> backups;
  for (int k = 0; k < 5; ++k) {
    const std::string path = "b/backup" + std::to_string(k) + ".db";
    auto cc = store.backup(vfs, path);
    REQUIRE_OK(cc.status());
    backups.emplace_back(cc.value(), path);
  }
  stop.store(true);
  writer.join();
  CHECK(failures.load() == 0);
  for (const auto& [cc, path] : backups) {
    // Restore = open the copy. It is a database on its own, at the change
    // counter reported, with the invariants holding.
    auto b = Store::open(vfs, path, opts());
    REQUIRE_OK(b.status());
    auto rep = b.value()->check();
    REQUIRE_OK(rep.status());
    CHECK_MSG(rep.value().ok, rep.value().problems[0]);
    auto rd = b.value()->begin_read();
    REQUIRE_OK(rd.status());
    // employees count == commits since migration (one insert per commit).
    auto n = rd.value()->count("employees");
    REQUIRE_OK(n.status());
    CHECK_MSG(n.value() + base == cc, "backup at " << cc << " holds " << n.value() << " employees");
  }
}

ARCHIVUM_TEST(restored_backup_beside_a_live_log_is_refused) {
  MemVfs vfs;
  auto st = Store::open(vfs, "c/live.db", opts());
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto cc = st.value()->backup(vfs, "c/backup.db");
  REQUIRE_OK(cc.status());
  for (int i = 1; i <= 20; ++i) {
    auto w = st.value()->begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->insert("employees", employee(i, i)));
    REQUIRE_OK(w.value()->commit());
  }
  REQUIRE_OK(st.value()->close());
  // Someone copies the old backup over the live file but leaves the log.
  REQUIRE_OK(copy_file(vfs, "c/backup.db", "c/live.db"));
  auto reopened = Db::open(vfs, "c/live.db", opts());
  CHECK(reopened.status().code() == ErrorCode::Corrupt);
  // Removing the log deliberately makes the restored file openable, at the backup's state.
  REQUIRE_OK(vfs.remove("c/live.db.wal"));
  auto ok = Store::open(vfs, "c/live.db", opts());
  REQUIRE_OK(ok.status());
  CHECK(ok.value()->begin_read().value()->count("employees").value() == 0);
}

ARCHIVUM_TEST(point_in_time_recovery_by_counter_and_by_time) {
  MemVfs vfs;
  g_clock = 1'000'000;
  auto st = Store::open(vfs, "d/live.db", opts("d/archive"));
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  REQUIRE_OK(core::migrate_all(store, {&punchline::module()}).status());
  auto base_cc = store.backup(vfs, "d/base.db");
  REQUIRE_OK(base_cc.status());
  // Expected dump and commit time at every change counter after the backup.
  std::map<std::uint64_t, std::string> dumps;
  std::map<std::uint64_t, std::int64_t> times;
  for (int i = 1; i <= 120; ++i) {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->insert("employees", employee(i, i)));
    if (i % 3 == 0) {
      Row e = employee(i - 1, i - 1);
      e[2] = Value::text("Renamed");
      REQUIRE_OK(w.value()->update("employees", e));
    }
    if (i % 10 == 0) REQUIRE_OK(w.value()->remove("employees", {Value::integer(i - 5)}));
    REQUIRE_OK(w.value()->commit());
    {
      auto rd = store.db().begin_read();
      REQUIRE_OK(rd.status());
      const std::uint64_t hdr_cc = rd.value()->change_counter();
      rd.value().reset();
      dumps[hdr_cc] = dump_bytes(store);
      times[hdr_cc] = g_clock;
    }
    if (i % 25 == 0) REQUIRE_OK(store.checkpoint());
  }
  const std::uint64_t last_cc = dumps.rbegin()->first;
  CHECK(store.db().stats().archived_segments >= 4);
  // Recover to several counters: through archived segments only, and with
  // the live log as the final segment.
  for (std::uint64_t target : {base_cc.value() + 1, base_cc.value() + 17, base_cc.value() + 50, base_cc.value() + 99, last_cc}) {
    const std::string out = "d/pitr-" + std::to_string(target) + ".db";
    RecoveryTarget t;
    t.change_counter = target;
    auto rep = recover_to_point(vfs, "d/base.db", "d/archive", "d/live.db.wal", t, out);
    REQUIRE_MSG(rep.ok(), "target " << target << ": " << rep.status().to_string());
    CHECK_MSG(rep.value().target_reached && rep.value().change_counter == target, "reached " << rep.value().change_counter);
    auto rs = Store::open(vfs, out, opts());
    REQUIRE_OK(rs.status());
    auto chk = rs.value()->check();
    REQUIRE_OK(chk.status());
    CHECK_MSG(chk.value().ok, chk.value().problems[0]);
    CHECK_MSG(dump_bytes(*rs.value()) == dumps[target], "state at " << target << " differs");
  }
  // By time: the last commit at or before the time.
  for (std::uint64_t at : {base_cc.value() + 8, base_cc.value() + 60}) {
    RecoveryTarget t;
    t.time_us = times[at] + 500;  // between this commit and the next
    const std::string out = "d/pitr-time-" + std::to_string(at) + ".db";
    auto rep = recover_to_point(vfs, "d/base.db", "d/archive", "d/live.db.wal", t, out);
    REQUIRE_OK(rep.status());
    CHECK(rep.value().change_counter == at);
    auto rs = Store::open(vfs, out, opts());
    REQUIRE_OK(rs.status());
    CHECK(dump_bytes(*rs.value()) == dumps[at]);
  }
  // Beyond the end: everything applied, target not reached.
  RecoveryTarget beyond;
  beyond.change_counter = last_cc + 10;
  auto rep = recover_to_point(vfs, "d/base.db", "d/archive", "d/live.db.wal", beyond, "d/pitr-beyond.db");
  REQUIRE_OK(rep.status());
  CHECK(!rep.value().target_reached && rep.value().change_counter == last_cc);
  // Without the live log only the archived segments apply.
  RecoveryTarget all;
  auto arch = recover_to_point(vfs, "d/base.db", "d/archive", "", all, "d/pitr-archived.db");
  REQUIRE_OK(arch.status());
  CHECK(arch.value().change_counter < last_cc && arch.value().segments_applied >= 4);
  CHECK(dump_bytes(*Store::open(vfs, "d/pitr-archived.db", opts()).value()) == dumps[arch.value().change_counter]);
  // Output must not exist.
  CHECK(recover_to_point(vfs, "d/base.db", "d/archive", "", all, "d/pitr-archived.db").status().code() == ErrorCode::AlreadyExists);
}
