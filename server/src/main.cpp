// archivum: one binary. `serve` runs the application server; the
// operational subcommands (check, backup, restore, pitr, migrate) work on
// a database file directly and never need the server running, though
// backup and pitr may run beside a running server (docs/backup-recovery.md).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "archivum/engine/migrate.h"
#include "archivum/engine/recovery.h"
#include "archivum/engine/store.h"
#include "archivum/punchline/schema.h"
#include "archivum/server/app.h"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  archivum serve --config <path>                   run the application server\n"
               "  archivum check --db <path>                       verify every invariant of a database\n"
               "  archivum migrate --db <path>                     apply pending schema migrations\n"
               "  archivum backup --db <path> --to <file>          online backup as of one snapshot\n"
               "  archivum restore --from <file> --to <path> [--discard-log]\n"
               "                                                   restore a backup as a database\n"
               "  archivum pitr --backup <file> --archive <dir> --to <path>\n"
               "                [--live-log <file>] [--change-counter N | --time-us T]\n"
               "                                                   point-in-time recovery\n"
               "  archivum version\n");
  return 2;
}

// --key value pairs; every key must be in `allowed`.
bool parse(int argc, char** argv, int from, const std::vector<std::string>& allowed,
           std::map<std::string, std::string>& out) {
  for (int i = from; i < argc; ++i) {
    const std::string key = argv[i];
    if (key.rfind("--", 0) != 0) return false;
    bool known = false;
    for (const auto& a : allowed) known = known || a == key;
    if (!known) return false;
    if (key == "--discard-log") {
      out[key] = "1";
      continue;
    }
    if (i + 1 >= argc) return false;
    out[key] = argv[++i];
  }
  return true;
}

int fail(const char* what, const archivum::Status& s) {
  std::fprintf(stderr, "%s: %s\n", what, s.to_string().c_str());
  return 1;
}

int cmd_check(const std::map<std::string, std::string>& a) {
  auto vfs = archivum::make_os_vfs();
  archivum::engine::DbOptions o;
  o.create_if_missing = false;
  auto st = archivum::engine::Store::open(*vfs, a.at("--db"), o);
  if (!st.ok()) return fail("open", st.status());
  auto rep = st.value()->check();
  if (!rep.ok()) return fail("check", rep.status());
  for (const auto& p : rep.value().problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
  std::printf("%s: %llu pages, %llu tables, %llu rows, %llu index entries, %s\n", a.at("--db").c_str(),
              static_cast<unsigned long long>(rep.value().pager.pages_checked),
              static_cast<unsigned long long>(rep.value().tables), static_cast<unsigned long long>(rep.value().rows),
              static_cast<unsigned long long>(rep.value().index_entries), rep.value().ok ? "ok" : "PROBLEMS");
  return rep.value().ok ? 0 : 1;
}

int cmd_migrate(const std::map<std::string, std::string>& a) {
  auto vfs = archivum::make_os_vfs();
  auto st = archivum::engine::Store::open(*vfs, a.at("--db"));
  if (!st.ok()) return fail("open", st.status());
  auto rep = archivum::engine::migrate(*st.value(), archivum::punchline::migrations());
  if (!rep.ok()) return fail("migrate", rep.status());
  for (const auto& n : rep.value().applied) std::printf("applied %s\n", n.c_str());
  std::printf("schema version %llu -> %llu\n", static_cast<unsigned long long>(rep.value().from_version),
              static_cast<unsigned long long>(rep.value().to_version));
  return 0;
}

int cmd_backup(const std::map<std::string, std::string>& a) {
  auto vfs = archivum::make_os_vfs();
  archivum::engine::DbOptions o;
  o.create_if_missing = false;
  auto db = archivum::engine::Db::open(*vfs, a.at("--db"), o);
  if (!db.ok()) return fail("open", db.status());
  auto cc = db.value()->backup(*vfs, a.at("--to"));
  if (!cc.ok()) return fail("backup", cc.status());
  std::printf("backup %s at change counter %llu\n", a.at("--to").c_str(), static_cast<unsigned long long>(cc.value()));
  return 0;
}

int cmd_restore(const std::map<std::string, std::string>& a) {
  auto vfs = archivum::make_os_vfs();
  const std::string to = a.at("--to");
  for (const std::string& p : {to, to + ".wal"}) {
    auto e = vfs->exists(p);
    if (!e.ok()) return fail("restore", e.status());
    if (e.value()) {
      if (p == to + ".wal" && a.count("--discard-log")) {
        if (archivum::Status s = vfs->remove(p); !s.ok()) return fail("restore", s);
        continue;
      }
      std::fprintf(stderr, "restore: %s exists; remove it first%s\n", p.c_str(),
                   p == to ? "" : " or pass --discard-log");
      return 1;
    }
  }
  if (archivum::Status s = archivum::engine::copy_file(*vfs, a.at("--from"), to); !s.ok()) return fail("restore", s);
  std::map<std::string, std::string> chk = {{"--db", to}};
  return cmd_check(chk);
}

int cmd_pitr(const std::map<std::string, std::string>& a) {
  auto vfs = archivum::make_os_vfs();
  archivum::engine::RecoveryTarget target;
  if (a.count("--change-counter")) target.change_counter = std::strtoull(a.at("--change-counter").c_str(), nullptr, 10);
  if (a.count("--time-us")) target.time_us = std::strtoll(a.at("--time-us").c_str(), nullptr, 10);
  const std::string live = a.count("--live-log") ? a.at("--live-log") : "";
  auto rep = archivum::engine::recover_to_point(*vfs, a.at("--backup"), a.at("--archive"), live, target, a.at("--to"));
  if (!rep.ok()) return fail("pitr", rep.status());
  std::printf("recovered %s: change counter %llu -> %llu, %llu transactions from %llu segments, target %s\n",
              a.at("--to").c_str(), static_cast<unsigned long long>(rep.value().start_change_counter),
              static_cast<unsigned long long>(rep.value().change_counter),
              static_cast<unsigned long long>(rep.value().transactions_applied),
              static_cast<unsigned long long>(rep.value().segments_applied),
              rep.value().target_reached ? "reached" : "NOT reached");
  std::map<std::string, std::string> chk = {{"--db", a.at("--to")}};
  if (int rc = cmd_check(chk); rc != 0) return rc;
  return rep.value().target_reached ? 0 : 3;
}

int cmd_serve(const std::map<std::string, std::string>& a) {
  auto config = archivum::server::load_config(a.at("--config"));
  if (!config.ok()) {
    std::fprintf(stderr, "configuration rejected: %s\n", config.status().to_string().c_str());
    return 1;
  }
  archivum::server::App app(std::move(config).value());
  if (archivum::Status s = app.configure(); !s.ok()) {
    std::fprintf(stderr, "startup failed: %s\n", s.to_string().c_str());
    return 1;
  }
  std::fprintf(stderr, "archivum listening on %s:%u (TLS)\n", app.config().listen_address.c_str(),
               static_cast<unsigned>(app.config().listen_port));
  archivum::Status s = app.run();
  if (!s.ok()) {
    std::fprintf(stderr, "server stopped: %s\n", s.to_string().c_str());
    return 1;
  }
  return 0;
}

bool has_all(const std::map<std::string, std::string>& a, const std::vector<std::string>& keys) {
  for (const auto& k : keys) {
    if (!a.count(k)) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string command = argv[1];
  std::map<std::string, std::string> a;
  if (command == "version") {
    std::printf("archivum 0.0.1 (stage 4)\n");
    return 0;
  }
  if (command == "serve") {
    if (!parse(argc, argv, 2, {"--config"}, a) || !has_all(a, {"--config"})) return usage();
    return cmd_serve(a);
  }
  if (command == "check") {
    if (!parse(argc, argv, 2, {"--db"}, a) || !has_all(a, {"--db"})) return usage();
    return cmd_check(a);
  }
  if (command == "migrate") {
    if (!parse(argc, argv, 2, {"--db"}, a) || !has_all(a, {"--db"})) return usage();
    return cmd_migrate(a);
  }
  if (command == "backup") {
    if (!parse(argc, argv, 2, {"--db", "--to"}, a) || !has_all(a, {"--db", "--to"})) return usage();
    return cmd_backup(a);
  }
  if (command == "restore") {
    if (!parse(argc, argv, 2, {"--from", "--to", "--discard-log"}, a) || !has_all(a, {"--from", "--to"})) return usage();
    return cmd_restore(a);
  }
  if (command == "pitr") {
    if (!parse(argc, argv, 2, {"--backup", "--archive", "--to", "--live-log", "--change-counter", "--time-us"}, a) ||
        !has_all(a, {"--backup", "--archive", "--to"})) {
      return usage();
    }
    return cmd_pitr(a);
  }
  return usage();
}
