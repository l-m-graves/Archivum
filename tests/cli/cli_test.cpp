// The operational subcommands, run as a user would: migrate, backup, check,
// restore, pitr on real files in a temporary directory. ARCHIVUM_BIN names
// the binary; ARCHIVUM_TMP the directory.
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "archivum/engine/migrate.h"
#include "archivum/engine/store.h"
#include "archivum/punchline/schema.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;

namespace {

std::string bin() {
  const char* b = std::getenv("ARCHIVUM_BIN");
  return b ? b : "archivum";
}
std::string tmp() {
  const char* t = std::getenv("ARCHIVUM_TMP");
  return t ? t : ".";
}
int run(const std::string& args) {
  const std::string cmd = "\"" + bin() + "\" " + args;
  std::printf("  $ %s\n", cmd.c_str());
  std::fflush(stdout);
#ifdef _WIN32
  // cmd.exe strips the outer quotes of a command line that starts with one.
  const int rc = std::system(("\"" + cmd + "\"").c_str());
  return rc;
#else
  const int rc = std::system(cmd.c_str());
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}

Row employee(std::int64_t id) {
  return {Value::integer(id), Value::text("E" + std::to_string(id)), Value::text("Name"), Value::null(), Value::null(),
          Value::null(), Value::boolean(true), Value::null(), Value::text("UTC"), Value::timestamp(id), Value::timestamp(id), Value::null(), Value::null()};
}

}  // namespace

ARCHIVUM_TEST(cli_migrate_backup_check_restore_pitr) {
  const std::string dir = tmp();
  const std::string db = dir + "/cli.db";
  const std::string archive = dir + "/archive";
  auto vfs = make_os_vfs();
  for (const char* f : {"/cli.db", "/cli.db.wal", "/cli-backup.db", "/cli-backup.db.wal", "/cli-restored.db",
                        "/cli-restored.db.wal", "/cli-pitr.db", "/cli-pitr.db.wal"}) {
    auto e = vfs->exists(dir + f);
    if (e.ok() && e.value()) REQUIRE_OK(vfs->remove(dir + f));
  }
  CHECK(run("version") == 0);
  CHECK(run("migrate --db \"" + db + "\"") == 0);
  CHECK(run("migrate --db \"" + db + "\"") == 0);
  CHECK(run("check --db \"" + db + "\"") == 0);
  // Data and archived log segments, through the library on the same file.
  std::uint64_t cc_after_backup = 0;
  {
    DbOptions o;
    o.archive_dir = archive;
    o.checkpoint_threshold_frames = 20;
    o.create_if_missing = false;
    auto st = Store::open(*vfs, db, o);
    REQUIRE_OK(st.status());
    for (int i = 1; i <= 10; ++i) {
      auto w = st.value()->begin_write();
      REQUIRE_OK(w.status());
      REQUIRE_OK(w.value()->insert("employees", employee(i)));
      REQUIRE_OK(w.value()->commit());
    }
    REQUIRE_OK(st.value()->close());
  }
  CHECK(run("backup --db \"" + db + "\" --to \"" + dir + "/cli-backup.db\"") == 0);
  {
    DbOptions o;
    o.archive_dir = archive;
    o.checkpoint_threshold_frames = 20;
    o.create_if_missing = false;
    auto st = Store::open(*vfs, db, o);
    REQUIRE_OK(st.status());
    cc_after_backup = st.value()->db().begin_read().value()->change_counter();
    for (int i = 11; i <= 40; ++i) {
      auto w = st.value()->begin_write();
      REQUIRE_OK(w.status());
      REQUIRE_OK(w.value()->insert("employees", employee(i)));
      REQUIRE_OK(w.value()->commit());
    }
    REQUIRE_OK(st.value()->checkpoint());
    REQUIRE_OK(st.value()->close());
  }
  CHECK(run("check --db \"" + db + "\"") == 0);
  CHECK(run("restore --from \"" + dir + "/cli-backup.db\" --to \"" + dir + "/cli-restored.db\"") == 0);
  CHECK(run("restore --from \"" + dir + "/cli-backup.db\" --to \"" + dir + "/cli-restored.db\"") == 1);
  {
    DbOptions o;
    o.create_if_missing = false;
    auto st = Store::open(*vfs, dir + "/cli-restored.db", o);
    REQUIRE_OK(st.status());
    CHECK(st.value()->begin_read().value()->count("employees").value() == 10);
  }
  const std::uint64_t target = cc_after_backup + 15;
  CHECK(run("pitr --backup \"" + dir + "/cli-backup.db\" --archive \"" + archive + "\" --live-log \"" + db +
            ".wal\" --to \"" + dir + "/cli-pitr.db\" --change-counter " + std::to_string(target)) == 0);
  {
    DbOptions o;
    o.create_if_missing = false;
    auto st = Store::open(*vfs, dir + "/cli-pitr.db", o);
    REQUIRE_OK(st.status());
    CHECK(st.value()->begin_read().value()->count("employees").value() == 25);
    CHECK(st.value()->db().begin_read().value()->change_counter() == target);
  }
  CHECK(run("check --db \"" + dir + "/does-not-exist.db\"") == 1);
  CHECK(run("bogus") == 2);
}

// rollback-export: the CLI writes the old server's batches for a period
// (docs/punchline-module.md, "Rollback"); the replay into the old server
// itself is tests/contract/test_rollback.py against the Punchline repository.
ARCHIVUM_TEST(cli_rollback_export_writes_old_store_batches) {
  const std::string dir = tmp();
  const std::string db = dir + "/rb.db";
  auto vfs = make_os_vfs();
  for (const char* f : {"/rb.db", "/rb.db.wal", "/rb.json"}) {
    auto e = vfs->exists(dir + f);
    if (e.ok() && e.value()) REQUIRE_OK(vfs->remove(dir + f));
  }
  CHECK(run("migrate --db \"" + db + "\"") == 0);
  {
    DbOptions o;
    o.create_if_missing = false;
    auto st = Store::open(*vfs, db, o);
    REQUIRE_OK(st.status());
    auto w = st.value()->begin_write();
    REQUIRE_OK(w.status());
    Row emp = employee(1);
    emp[11] = Value::text("ACME");
    emp[12] = Value::text("4400");
    REQUIRE_OK(w.value()->insert("employees", emp));
    UuidBytes du{};
    du[0] = std::byte{9};
    REQUIRE_OK(w.value()->insert("devices", {Value::integer(1), Value::uuid(du), Value::text("k"), Value::integer(1), Value::blob({std::byte{1}}),
                                             Value::timestamp(1), Value::text("t"), Value::null(), Value::null(), Value::null(), Value::null(),
                                             Value::null(), Value::integer(0)}));
    REQUIRE_OK(w.value()->insert("pay_periods", {Value::integer(1), Value::integer(19786), Value::integer(19792), Value::text("UTC"),
                                                 Value::text("2024a"), Value::text("open"), Value::null(), Value::null(), Value::null()}));
    const std::int64_t in_at = 1'709'542'800LL * 1'000'000;  // 2024-03-04T09:00:00Z
    for (int k = 1; k <= 2; ++k) {
      UuidBytes eu{};
      eu[15] = static_cast<std::byte>(k);
      REQUIRE_OK(w.value()->insert("time_entries", {Value::integer(k), Value::uuid(eu), Value::integer(1), Value::integer(1), Value::uuid(du),
                                                    Value::integer(k), Value::text(k == 1 ? "in" : "out"),
                                                    Value::timestamp(in_at + (k - 1) * 8 * 3600 * 1'000'000LL),
                                                    Value::text(k == 1 ? "2024-03-04T09:00:00" : "2024-03-04T17:00:00"), Value::text("UTC"),
                                                    Value::text("2024a"), Value::timestamp(in_at), Value::text("device"), Value::null(),
                                                    Value::integer(1), Value::text("released"), Value::null(), Value::null(), Value::null(),
                                                    Value::null(), Value::timestamp(in_at), k == 1 ? Value::text("hello") : Value::null(),
                                                    Value::null()}));
    }
    REQUIRE_OK(w.value()->insert("shifts", {Value::integer(1), Value::integer(1), Value::integer(1), Value::integer(1), Value::integer(2),
                                            Value::integer(19786), Value::integer(1), Value::timestamp(in_at),
                                            Value::timestamp(in_at + 8 * 3600 * 1'000'000LL), Value::integer(8 * 3600 * 1'000'000LL),
                                            Value::null(), Value::text("released")}));
    REQUIRE_OK(w.value()->commit());
    REQUIRE_OK(st.value()->close());
  }
  CHECK(run("rollback-export --db \"" + db + "\" --period 1 --to \"" + dir + "/rb.json\" --released-only") == 0);
  CHECK(run("rollback-export --db \"" + db + "\" --period 7 --to \"" + dir + "/rb2.json\"") == 1);
  std::ifstream in(dir + "/rb.json");
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  const auto j = nlohmann::json::parse(ss.str(), nullptr, false);
  REQUIRE(j.is_array() && j.size() == 1);
  CHECK(j[0]["entries"].size() == 1);
  CHECK(j[0]["entries"][0]["employee_id"] == "E1" && j[0]["entries"][0]["company"] == "ACME" && j[0]["entries"][0]["minutes"] == 480);
  CHECK(j[0]["entries"][0]["clock_in"] == "2024-03-04T09:00:00Z" && j[0]["entries"][0]["note"] == "hello");
}
