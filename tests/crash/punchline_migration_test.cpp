// The Punchline schema migration: applied to a fresh store, idempotent on
// reapplication, exercising every table's constraints with a few rows,
// and crash-safe: the migration is run under the crash shim with a crash
// at every step until it completes, and after each crash the store reopens
// and the migration is rerun, ending in the same state every time.
#include <cstdio>

#include "archivum/core/module.h"
#include "archivum/core/schema.h"
#include "archivum/engine/migrate.h"
#include "archivum/punchline/schema.h"
#include "archivum/testing/fault_vfs.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

DbOptions opts() {
  DbOptions o;
  o.page_size = 1024;
  o.checkpoint_threshold_frames = 40;
  return o;
}

UuidBytes uuid(std::uint8_t seed) {
  UuidBytes u{};
  for (std::size_t i = 0; i < 16; ++i) u[i] = static_cast<std::byte>(seed + i);
  return u;
}

Bytes bytes(const char* s) {
  Bytes b;
  for (const char* p = s; *p; ++p) b.push_back(static_cast<std::byte>(*p));
  return b;
}

}  // namespace

ARCHIVUM_TEST(punchline_schema_applies_and_holds_its_constraints) {
  MemVfs vfs;
  auto st = Store::open(vfs, "p/punchline.db", opts());
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  auto r = core::migrate_all(store, {&punchline::module()});
  REQUIRE_OK(r.status());
  REQUIRE(r.value().modules.size() == 2);
  CHECK(r.value().modules[0].first == core::kModule && r.value().modules[0].second.to_version == 1);
  CHECK(r.value().modules[1].first == punchline::kModule && r.value().modules[1].second.to_version == 1);
  const std::size_t kCoreTables = 5;
  {
    auto rd = store.begin_read();
    REQUIRE_OK(rd.status());
    for (const std::string& name : punchline::v1_tables()) {
      CHECK_MSG(rd.value()->catalog().table(name) != nullptr, "missing table " << name);
    }
    CHECK(rd.value()->catalog().tables.size() == punchline::v1_tables().size() + kCoreTables + 1);  // + archivum_migrations
    CHECK(rd.value()->catalog().schema_version == 2);
    auto codes = rd.value()->scan_all("pay_codes");
    REQUIRE_OK(codes.status());
    CHECK(codes.value().size() == 5);
  }
  auto again = core::migrate_all(store, {&punchline::module()});
  REQUIRE_OK(again.status());
  CHECK(again.value().modules[1].second.applied.empty());

  auto w = store.begin_write();
  REQUIRE_OK(w.status());
  Writer& wr = *w.value();
  const Value now = Value::timestamp(1'700'000'000'000'000);
  // Employee with an Entra identity; a second with none (oid nullable).
  REQUIRE_OK(wr.insert("employees", {Value::integer(1), Value::text("E001"), Value::text("Ada"), Value::text("ada@example.test"),
                                     Value::text("tid-1"), Value::text("oid-1"), Value::boolean(true), Value::null(),
                                     Value::text("America/Los_Angeles"), now, now, Value::null(), Value::null()}));
  REQUIRE_OK(wr.insert("employees", {Value::integer(2), Value::text("E002"), Value::text("Bob"), Value::null(), Value::null(),
                                     Value::null(), Value::boolean(true), Value::null(), Value::text("America/Los_Angeles"), now, now, Value::null(), Value::null()}));
  // The same oid twice in one tenant is refused; an empty employee number too.
  CHECK(wr.insert("employees", {Value::integer(3), Value::text("E003"), Value::text("Cy"), Value::null(), Value::text("tid-1"),
                                Value::text("oid-1"), Value::boolean(true), Value::null(), Value::text("UTC"), now, now, Value::null(), Value::null()})
            .code() == ErrorCode::Constraint);
  CHECK(wr.insert("employees", {Value::integer(3), Value::text(""), Value::text("Cy"), Value::null(), Value::null(), Value::null(),
                                Value::boolean(true), Value::null(), Value::text("UTC"), now, now, Value::null(), Value::null()})
            .code() == ErrorCode::Constraint);
  // Roles: supervisor and payroll are separate grants on the same principal; a bogus role is refused.
  REQUIRE_OK(wr.insert("role_grants", {Value::integer(1), Value::text("tid-1"), Value::text("oid-9"), Value::text("supervisor"),
                                       Value::text("installer"), now}));
  REQUIRE_OK(wr.insert("role_grants", {Value::integer(2), Value::text("tid-1"), Value::text("oid-9"), Value::text("payroll"),
                                       Value::text("installer"), now}));
  CHECK(wr.insert("role_grants", {Value::integer(3), Value::text("tid-1"), Value::text("oid-9"), Value::text("supervisor"),
                                  Value::text("installer"), now}).code() == ErrorCode::Constraint);
  CHECK(wr.insert("role_grants", {Value::integer(3), Value::text("tid-1"), Value::text("oid-9"), Value::text("root"),
                                  Value::text("installer"), now}).code() == ErrorCode::Constraint);
  // A device bound to an employee; a device for a missing employee is refused.
  REQUIRE_OK(wr.insert("devices", {Value::integer(1), Value::uuid(uuid(1)), Value::text("kiosk-1"), Value::integer(1),
                                   Value::blob(bytes("hash")), now, Value::text("installer"), Value::null(), Value::null(),
                                   Value::null(), Value::null(), Value::null(), Value::integer(0)}));
  CHECK(wr.insert("devices", {Value::integer(2), Value::uuid(uuid(2)), Value::text("kiosk-2"), Value::integer(99),
                              Value::blob(bytes("hash")), now, Value::text("installer"), Value::null(), Value::null(),
                              Value::null(), Value::null(), Value::null(), Value::integer(0)}).code() == ErrorCode::Constraint);
  REQUIRE_OK(wr.insert("pay_periods", {Value::integer(1), Value::integer(20000), Value::integer(20006), Value::text("America/Los_Angeles"),
                                       Value::text("2024a"), Value::text("open"), Value::null(), Value::null(), Value::null()}));
  // Same-row comparisons are engine checks (Stage 6): an inverted period, an
  // inverted schedule, an inverted assignment range.
  CHECK(wr.insert("pay_periods", {Value::integer(2), Value::integer(20010), Value::integer(20006), Value::text("UTC"),
                                  Value::text("2024a"), Value::text("open"), Value::null(), Value::null(), Value::null()})
            .code() == ErrorCode::Constraint);
  CHECK(wr.insert("schedules", {Value::integer(2), Value::integer(1), Value::integer(1), Value::integer(1020), Value::integer(540),
                                Value::integer(20000), Value::null()}).code() == ErrorCode::Constraint);
  CHECK(wr.insert("schedules", {Value::integer(1), Value::integer(1), Value::integer(7), Value::integer(540), Value::integer(1020),
                                Value::integer(20000), Value::null()}).code() == ErrorCode::Constraint);
  REQUIRE_OK(wr.insert("schedules", {Value::integer(1), Value::integer(1), Value::integer(1), Value::integer(540), Value::integer(1020),
                                     Value::integer(20000), Value::null()}));
  // A device-attested offline punch with its journal sequence; the same
  // sequence from the same device is a replay and is refused.
  Row entry = {Value::integer(1), Value::uuid(uuid(10)), Value::integer(1), Value::integer(1), Value::uuid(uuid(40)), Value::integer(7),
               Value::text("in"), now, Value::text("2023-11-14T14:13:20"), Value::text("America/Los_Angeles"),
               Value::text("2024a"), Value::timestamp(1'700'000'000'500'000), Value::text("device"), Value::integer(500'000),
               Value::integer(1), Value::text("recorded"), Value::null(), Value::null(), Value::null(), Value::null(), now,
               Value::null(), Value::null()};
  REQUIRE_OK(wr.insert("time_entries", entry));
  Row replay = entry;
  replay[0] = Value::integer(2);
  replay[1] = Value::uuid(uuid(11));
  CHECK(wr.insert("time_entries", replay).code() == ErrorCode::Constraint);
  replay[5] = Value::integer(8);
  replay[6] = Value::text("lunch");
  CHECK(wr.insert("time_entries", replay).code() == ErrorCode::Constraint);
  replay[6] = Value::text("out");
  replay[15] = Value::text("paid");
  CHECK(wr.insert("time_entries", replay).code() == ErrorCode::Constraint);
  replay[15] = Value::text("recorded");
  replay[18] = Value::integer(1);  // correction of entry 1
  replay[19] = Value::text("wrong kind");
  REQUIRE_OK(wr.insert("time_entries", replay));
  // The corrected entry cannot be deleted while its correction points at it; nor the device while entries reference it.
  CHECK(wr.remove("time_entries", {Value::integer(1)}).code() == ErrorCode::Constraint);
  CHECK(wr.remove("devices", {Value::integer(1)}).code() == ErrorCode::Constraint);
  // Approval transition referencing its audit entry.
  REQUIRE_OK(wr.insert("audit_log", {Value::integer(1), now, Value::text("principal"), Value::text("tid-1"), Value::text("oid-9"),
                                     Value::null(), Value::null(), Value::text("approve"), Value::text("time_entries"),
                                     Value::text("1"), Value::null(), Value::null()}));
  // Supervisor assignment referencing its audit row; pay code must exist.
  REQUIRE_OK(wr.insert("supervisor_assignments", {Value::integer(1), Value::integer(2), Value::integer(1), now, Value::null(),
                                                  Value::text("installer"), Value::integer(1)}));
  CHECK(wr.insert("supervisor_assignments", {Value::integer(2), Value::integer(1), Value::integer(2), now, Value::timestamp(1),
                                             Value::text("installer"), Value::integer(1)}).code() == ErrorCode::Constraint);
  Row coded = entry;
  coded[0] = Value::integer(3);
  coded[1] = Value::uuid(uuid(12));
  coded[5] = Value::integer(9);
  coded[16] = Value::text("bonus");
  CHECK(wr.insert("time_entries", coded).code() == ErrorCode::Constraint);
  coded[16] = Value::text("overtime");
  REQUIRE_OK(wr.insert("time_entries", coded));
  // A shift pairs entry 1 (in) with a later out; out before in is an engine check.
  Row bad_shift = {Value::integer(1), Value::integer(1), Value::integer(1), Value::integer(1), Value::integer(3), Value::integer(19675),
                   Value::integer(1), now, Value::timestamp(1'699'999'999'000'000), Value::integer(1), Value::null(), Value::text("recorded")};
  CHECK(wr.insert("shifts", bad_shift).code() == ErrorCode::Constraint);
  Row shift = bad_shift;
  shift[8] = Value::timestamp(1'700'000'030'000'000);
  shift[9] = Value::integer(30'000'000);
  REQUIRE_OK(wr.insert("shifts", shift));
  REQUIRE_OK(wr.insert("approvals", {Value::integer(1), Value::integer(1), Value::integer(1), Value::text("submitted"), Value::text("approved"),
                                     Value::text("tid-1"), Value::text("oid-9"), Value::null(), now, Value::null(), Value::integer(1)}));
  CHECK(wr.insert("approvals", {Value::integer(2), Value::integer(1), Value::integer(1), Value::text("submitted"), Value::text("approved"),
                                Value::text("tid-1"), Value::text("oid-9"), Value::null(), now, Value::null(), Value::integer(77)})
            .code() == ErrorCode::Constraint);
  REQUIRE_OK(wr.insert("exceptions", {Value::integer(1), Value::text("clock_divergence"), Value::text("open"), Value::integer(1),
                                      Value::integer(1), Value::integer(1), Value::integer(1), Value::text("500ms"), now, Value::null(),
                                      Value::null(), Value::null()}));
  REQUIRE_OK(wr.insert("sync_batches", {Value::integer(1), Value::uuid(uuid(20)), Value::integer(1), now, Value::integer(7),
                                        Value::integer(8), Value::integer(2), Value::integer(0), Value::null(), Value::null(),
                                        Value::uuid(uuid(40))}));
  REQUIRE_OK(wr.commit());
  auto rep = store.check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
  CHECK(rep.value().tables == punchline::v1_tables().size() + kCoreTables + 1);
}

ARCHIVUM_TEST(punchline_migration_survives_a_crash_at_every_step) {
  // Reference: the dump of a store migrated without faults.
  Dump reference;
  {
    MemVfs vfs;
    auto st = Store::open(vfs, "p/ref.db", opts());
    REQUIRE_OK(st.status());
    REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
    auto d = st.value()->dump();
    REQUIRE_OK(d.status());
    reference = std::move(d.value());
  }
  auto same_as_reference = [&](Store& s) -> std::string {
    auto d = s.dump();
    if (!d.ok()) return d.status().to_string();
    if (d.value().schema_version != reference.schema_version) return "schema version differs";
    if (d.value().tables.size() != reference.tables.size()) return "table count differs";
    for (std::size_t i = 0; i < reference.tables.size(); ++i) {
      if (encode_row(encode_table_def(d.value().tables[i].def)) != encode_row(encode_table_def(reference.tables[i].def))) {
        return "table " + reference.tables[i].def.name + " differs";
      }
      if (d.value().tables[i].rows.size() != reference.tables[i].rows.size()) return "rows differ";
    }
    return "";
  };

  int crashes = 0;
  for (int persist = 0; persist < 4; ++persist) {
    FaultConfig cfg;
    cfg.crash.persist = static_cast<CrashPolicy::Persist>(persist);
    cfg.seed = 7;
    FaultVfs vfs(cfg);
    for (std::uint64_t step = 1;; ++step) {
      vfs.set_injector(std::make_shared<CrashAtStep>(vfs.op_count() + step));
      auto st = Store::open(vfs, "p/crash.db", opts());
      bool crashed = false;
      if (!st.ok()) {
        REQUIRE_MSG(st.status().code() == ErrorCode::Crashed, "open: " << st.status().to_string());
        crashed = true;
      } else {
        auto r = core::migrate_all(*st.value(), {&punchline::module()});
        if (!r.ok()) {
          REQUIRE_MSG(r.status().code() == ErrorCode::Crashed || r.status().code() == ErrorCode::IoError,
                      "migrate: " << r.status().to_string());
          crashed = true;
        }
      }
      st = Result<std::unique_ptr<Store>>(Status::io("released"));
      if (!crashed && !vfs.crashed()) {
        // Completed without the crash landing: the migration is done.
        vfs.set_injector(nullptr);
        auto final = Store::open(vfs, "p/crash.db", opts());
        REQUIRE_OK(final.status());
        CHECK_MSG(same_as_reference(*final.value()).empty(), same_as_reference(*final.value()));
        auto again = core::migrate_all(*final.value(), {&punchline::module()});
        REQUIRE_OK(again.status());
            CHECK(again.value().modules[1].second.applied.empty());
        break;
      }
      ++crashes;
      vfs.set_injector(nullptr);
      vfs.recover();
      // After recovery the store is at a step boundary: either not migrated
      // or fully migrated, and rerunning finishes it.
      auto re = Store::open(vfs, "p/crash.db", opts());
      REQUIRE_MSG(re.ok(), "reopen after crash at step " << step << ": " << re.status().to_string());
      auto chk = re.value()->check();
      REQUIRE_OK(chk.status());
      REQUIRE_MSG(chk.value().ok, "after crash at step " << step << ": " << chk.value().problems[0]);
      {
        auto rd = re.value()->begin_read();
        REQUIRE_OK(rd.status());
        // Step boundaries: nothing, the core schema, or both.
        const std::uint64_t v = rd.value()->catalog().schema_version;
        REQUIRE_MSG(v <= 2, "schema version " << v << " after crash at step " << step);
        const std::size_t n = rd.value()->catalog().tables.size();
        REQUIRE_MSG((v == 0 && n == 0) || (v == 1 && n == 6) || (v == 2 && n == 17),
                    "partial schema (" << n << " tables at version " << v << ") after crash at step " << step);
      }
      auto r = core::migrate_all(*re.value(), {&punchline::module()});
      REQUIRE_MSG(r.ok(), "migrate after crash at step " << step << ": " << r.status().to_string());
      CHECK_MSG(same_as_reference(*re.value()).empty(), same_as_reference(*re.value()));
      re = Result<std::unique_ptr<Store>>(Status::io("released"));
      // Start over from an empty file for the next crash point.
      vfs.set_injector(nullptr);
      vfs.crash();
      vfs.recover();
      REQUIRE_OK(vfs.remove("p/crash.db"));
      auto wal_exists = vfs.exists("p/crash.db.wal");
      if (wal_exists.ok() && wal_exists.value()) REQUIRE_OK(vfs.remove("p/crash.db.wal"));
    }
  }
  std::printf("  %d crash points\n", crashes);
  CHECK(crashes > 100);
}
