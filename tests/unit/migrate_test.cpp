#include "archivum/engine/migrate.h"

#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

TableDef simple(const char* name) {
  TableDef t;
  t.name = name;
  t.columns = {{"id", ColumnType::Integer, false, 0}, {"v", ColumnType::Text, true, 0}};
  t.primary_key = {"id"};
  return t;
}

Migration step(std::uint64_t version, const char* name, const char* table) {
  return Migration{version, name, [table](Writer& w) { return w.create_table(simple(table)); }};
}

}  // namespace

ARCHIVUM_TEST(migrate_applies_pending_steps_once_and_records_them) {
  MemVfs vfs;
  auto st = Store::open(vfs, "m/a.db");
  REQUIRE_OK(st.status());
  std::vector<Migration> v1 = {step(1, "one", "t1")};
  auto r1 = migrate(*st.value(), v1);
  REQUIRE_OK(r1.status());
  CHECK(r1.value().from_version == 0 && r1.value().to_version == 1 && r1.value().applied.size() == 1);
  // Again: nothing to do.
  auto r1b = migrate(*st.value(), v1);
  REQUIRE_OK(r1b.status());
  CHECK(r1b.value().applied.empty() && r1b.value().to_version == 1);
  // Two more steps.
  std::vector<Migration> v3 = {step(1, "one", "t1"), step(2, "two", "t2"), step(3, "three", "t3")};
  auto r3 = migrate(*st.value(), v3);
  REQUIRE_OK(r3.status());
  CHECK(r3.value().from_version == 1 && r3.value().to_version == 3 && r3.value().applied.size() == 2);
  auto rd = st.value()->begin_read();
  REQUIRE_OK(rd.status());
  CHECK(rd.value()->catalog().schema_version == 3);
  CHECK(rd.value()->catalog().table("t3") != nullptr);
  auto rows = rd.value()->scan_all(kMigrationsTable);
  REQUIRE_OK(rows.status());
  REQUIRE(rows.value().size() == 3);
  CHECK(rows.value()[1][1] == Value::text("two"));
  rd.value().reset();
  // A binary older than the database refuses.
  CHECK(migrate(*st.value(), v1).status().code() == ErrorCode::Unsupported);
  // A renamed history is drift.
  std::vector<Migration> drift = {step(1, "one", "t1"), step(2, "deux", "t2"), step(3, "three", "t3")};
  CHECK(migrate(*st.value(), drift).status().code() == ErrorCode::Corrupt);
  // Bad lists are refused before touching the store.
  CHECK(validate_migrations({step(2, "x", "t")}).code() == ErrorCode::InvalidArgument);
  CHECK(validate_migrations({step(1, "x", "t"), step(2, "x", "u")}).code() == ErrorCode::InvalidArgument);
  auto rep = st.value()->check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
}

ARCHIVUM_TEST(migrate_failing_step_leaves_nothing_behind) {
  MemVfs vfs;
  auto st = Store::open(vfs, "m/b.db");
  REQUIRE_OK(st.status());
  std::vector<Migration> list = {
      step(1, "one", "t1"),
      Migration{2, "two", [](Writer& w) -> Status {
                  if (Status s = w.create_table(simple("t2")); !s.ok()) return s;
                  return w.insert("t2", {Value::integer(1), Value::integer(5)});  // type error
                }},
      step(3, "three", "t3")};
  auto r = migrate(*st.value(), list);
  CHECK(r.status().code() == ErrorCode::InvalidArgument);
  auto rd = st.value()->begin_read();
  REQUIRE_OK(rd.status());
  CHECK(rd.value()->catalog().schema_version == 1);
  CHECK(rd.value()->catalog().table("t1") != nullptr);
  CHECK(rd.value()->catalog().table("t2") == nullptr);
  CHECK(rd.value()->scan_all(kMigrationsTable).value().size() == 1);
}
