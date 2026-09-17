// The operational subcommands, run as a user would: migrate, backup, check,
// restore, pitr on real files in a temporary directory. ARCHIVUM_BIN names
// the binary; ARCHIVUM_TMP the directory.
#include <cstdlib>
#include <string>
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
          Value::null(), Value::boolean(true), Value::null(), Value::text("UTC"), Value::timestamp(id), Value::timestamp(id)};
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
