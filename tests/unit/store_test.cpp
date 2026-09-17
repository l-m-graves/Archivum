#include "archivum/engine/store.h"

#include <thread>
#include <atomic>
#include <optional>

#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

DbOptions opts() {
  DbOptions o;
  o.page_size = 1024;
  o.cache_pages = 64;
  o.checkpoint_threshold_frames = 50;
  return o;
}

TableDef employees() {
  TableDef t;
  t.name = "employees";
  t.columns = {{"id", ColumnType::Integer, false, 0},
               {"badge", ColumnType::Text, false, 0},
               {"name", ColumnType::Text, false, 0},
               {"manager_id", ColumnType::Integer, true, 0},
               {"rate", ColumnType::Decimal, false, 2},
               {"active", ColumnType::Boolean, false, 0},
               {"hired", ColumnType::Timestamp, true, 0},
               {"device", ColumnType::Uuid, true, 0},
               {"photo", ColumnType::Blob, true, 0}};
  t.primary_key = {"id"};
  t.indexes = {{"employees_badge", {"badge"}, true, 0},
               {"employees_manager", {"manager_id"}, false, 0},
               {"employees_name_id", {"name", "id"}, false, 0}};
  t.foreign_keys = {{"fk_manager", {"manager_id"}, "employees", {"id"}}};
  t.checks = {{"rate_positive", "rate", CheckOp::Gt, {Value::decimal(0)}}};
  return t;
}

TableDef punches() {
  TableDef t;
  t.name = "punches";
  t.columns = {{"employee_id", ColumnType::Integer, false, 0},
               {"at", ColumnType::Timestamp, false, 0},
               {"kind", ColumnType::Text, false, 0}};
  t.primary_key = {"employee_id", "at"};
  t.indexes = {{"punches_kind", {"kind"}, false, 0}};
  t.foreign_keys = {{"fk_employee", {"employee_id"}, "employees", {"id"}}};
  t.checks = {{"kind_known", "kind", CheckOp::In, {Value::text("in"), Value::text("out")}}};
  return t;
}

Row emp(std::int64_t id, const std::string& badge, const std::string& name, std::optional<std::int64_t> mgr,
        std::int64_t rate = 1500) {
  return {Value::integer(id), Value::text(badge), Value::text(name),
          mgr ? Value::integer(*mgr) : Value::null(), Value::decimal(rate), Value::boolean(true),
          Value::null(), Value::null(), Value::null()};
}

}  // namespace

ARCHIVUM_TEST(store_schema_rows_and_constraints) {
  MemVfs vfs;
  auto st = Store::open(vfs, "s/a.db", opts());
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->create_table(employees()));
    REQUIRE_OK(w.value()->create_table(punches()));
    REQUIRE_OK(w.value()->set_schema_version(1));
    // Punches cannot reference a missing table's index.
    TableDef bad = punches();
    bad.name = "bad";
    bad.indexes.clear();
    bad.primary_key = {"at", "employee_id"};  // neither key nor index leads with employee_id
    CHECK(w.value()->create_table(bad).code() == ErrorCode::InvalidArgument);
    REQUIRE_OK(w.value()->commit());
  }
  {
    auto r = store.begin_read();
    REQUIRE_OK(r.status());
    CHECK(r.value()->catalog().schema_version == 1);
    CHECK(r.value()->catalog().tables.size() == 2);
    const TableDef* e = r.value()->catalog().table("employees");
    REQUIRE(e != nullptr);
    CHECK(e->root != 0);
    CHECK(e->indexes.size() == 3);
    CHECK(!e->columns[0].nullable);
  }
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    Writer& wr = *w.value();
    REQUIRE_OK(wr.insert("employees", emp(1, "B1", "Ada", std::nullopt)));
    REQUIRE_OK(wr.insert("employees", emp(2, "B2", "Bob", 1)));
    REQUIRE_OK(wr.insert("employees", emp(3, "B3", "Cy", 1)));
    // Constraints.
    CHECK(wr.insert("employees", emp(1, "B9", "Dup", std::nullopt)).code() == ErrorCode::Constraint);
    CHECK(wr.insert("employees", emp(4, "B1", "Dup badge", std::nullopt)).code() == ErrorCode::Constraint);
    CHECK(wr.insert("employees", emp(4, "B4", "No mgr", 99)).code() == ErrorCode::Constraint);
    CHECK(wr.insert("employees", emp(4, "B4", "Cheap", std::nullopt, 0)).code() == ErrorCode::Constraint);
    Row nn = emp(4, "B4", "x", std::nullopt);
    nn[2] = Value::null();
    CHECK(wr.insert("employees", nn).code() == ErrorCode::Constraint);
    Row wrong = emp(4, "B4", "x", std::nullopt);
    wrong[4] = Value::integer(5);
    CHECK(wr.insert("employees", wrong).code() == ErrorCode::InvalidArgument);
    Row bad_utf8 = emp(4, "B4", "\xff\xfe", std::nullopt);
    CHECK(wr.insert("employees", bad_utf8).code() == ErrorCode::InvalidArgument);
    // A failed insert left nothing behind.
    auto c = wr.count("employees");
    REQUIRE_OK(c.status());
    CHECK(c.value() == 3);

    REQUIRE_OK(wr.insert("punches", {Value::integer(2), Value::timestamp(100), Value::text("in")}));
    REQUIRE_OK(wr.insert("punches", {Value::integer(2), Value::timestamp(200), Value::text("out")}));
    CHECK(wr.insert("punches", {Value::integer(2), Value::timestamp(300), Value::text("lunch")}).code() ==
          ErrorCode::Constraint);
    CHECK(wr.insert("punches", {Value::integer(7), Value::timestamp(300), Value::text("in")}).code() ==
          ErrorCode::Constraint);
    // Restrict: a referenced employee cannot go.
    CHECK(wr.remove("employees", {Value::integer(2)}).code() == ErrorCode::Constraint);
    CHECK(wr.remove("employees", {Value::integer(1)}).code() == ErrorCode::Constraint);
    // Update: badge change, uniqueness, manager change to a missing parent.
    Row bob = emp(2, "B2x", "Bob", 1);
    REQUIRE_OK(wr.update("employees", bob));
    bob[1] = Value::text("B3");
    CHECK(wr.update("employees", bob).code() == ErrorCode::Constraint);
    bob[1] = Value::text("B2x");
    bob[3] = Value::integer(42);
    CHECK(wr.update("employees", bob).code() == ErrorCode::Constraint);
    CHECK(wr.update("employees", emp(9, "B9", "Nobody", std::nullopt)).code() == ErrorCode::NotFound);
    // Delete a punch, then the employee's children are gone: still restricted by the other punch.
    REQUIRE_OK(wr.remove("punches", {Value::integer(2), Value::timestamp(100)}));
    CHECK(wr.remove("employees", {Value::integer(2)}).code() == ErrorCode::Constraint);
    REQUIRE_OK(wr.remove("punches", {Value::integer(2), Value::timestamp(200)}));
    REQUIRE_OK(wr.remove("employees", {Value::integer(2)}));
    REQUIRE_OK(wr.insert("employees", emp(2, "B2", "Bob", 1)));
    REQUIRE_OK(wr.commit());
  }
  auto rep = store.check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
  CHECK(rep.value().rows == 3);
  CHECK(rep.value().index_entries == 9);

  // Lookups and scans.
  auto r = store.begin_read();
  REQUIRE_OK(r.status());
  Reader& rd = *r.value();
  auto g = rd.get("employees", {Value::integer(2)});
  REQUIRE_OK(g.status());
  REQUIRE(g.value().has_value());
  CHECK((*g.value())[2] == Value::text("Bob"));
  CHECK(!rd.get("employees", {Value::integer(5)}).value().has_value());
  CHECK(rd.get("employees", {Value::text("2")}).status().code() == ErrorCode::InvalidArgument);
  auto by_badge = rd.scan_all("employees", "employees_badge");
  REQUIRE_OK(by_badge.status());
  REQUIRE(by_badge.value().size() == 3);
  CHECK(by_badge.value()[0][1] == Value::text("B1"));
  CHECK(by_badge.value()[2][1] == Value::text("B3"));
  auto reports = rd.scan_all("employees", "employees_manager", Bound{{Value::integer(1)}}, Bound{{Value::integer(1)}});
  REQUIRE_OK(reports.status());
  CHECK(reports.value().size() == 2);
  auto nobody = rd.scan_all("employees", "employees_manager", Bound{{Value::null()}}, Bound{{Value::null()}});
  REQUIRE_OK(nobody.status());
  CHECK(nobody.value().size() == 1);
  auto rev = rd.scan_all("employees", "", std::nullopt, Bound{{Value::integer(3)}, false}, true);
  REQUIRE_OK(rev.status());
  REQUIRE(rev.value().size() == 2);
  CHECK(rev.value()[0][0] == Value::integer(2));
  CHECK(rev.value()[1][0] == Value::integer(1));
  auto excl = rd.scan_all("employees", "", Bound{{Value::integer(1)}, false});
  REQUIRE_OK(excl.status());
  CHECK(excl.value().size() == 2);
  CHECK(rd.scan_all("nope").status().code() == ErrorCode::NotFound);
  CHECK(rd.scan_all("employees", "nope").status().code() == ErrorCode::NotFound);
}

ARCHIVUM_TEST(store_migration_is_atomic_and_reopens) {
  MemVfs vfs;
  {
    auto st = Store::open(vfs, "s/m.db", opts());
    REQUIRE_OK(st.status());
    auto w = st.value()->begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->create_table(employees()));
    for (int i = 1; i <= 50; ++i) REQUIRE_OK(w.value()->insert("employees", emp(i, "B" + std::to_string(i), "N", std::nullopt)));
    REQUIRE_OK(w.value()->commit());
    // A migration that adds an index over existing rows, adds a table and
    // bumps the version, then rolls back: nothing of it remains.
    auto m = st.value()->begin_write();
    REQUIRE_OK(m.status());
    REQUIRE_OK(m.value()->create_index("employees", {"employees_rate", {"rate", "name"}, false, 0}));
    REQUIRE_OK(m.value()->create_table(punches()));
    REQUIRE_OK(m.value()->set_schema_version(7));
    CHECK(m.value()->catalog().table("employees")->indexes.size() == 4);
    m.value()->rollback();
    auto r = st.value()->begin_read();
    REQUIRE_OK(r.status());
    CHECK(r.value()->catalog().schema_version == 0);
    CHECK(r.value()->catalog().table("punches") == nullptr);
    CHECK(r.value()->catalog().table("employees")->indexes.size() == 3);
    // The same migration, committed.
    auto m2 = st.value()->begin_write();
    REQUIRE_OK(m2.status());
    REQUIRE_OK(m2.value()->create_index("employees", {"employees_rate", {"rate", "name"}, false, 0}));
    // A unique index over duplicate data is refused and leaves the transaction usable.
    CHECK(m2.value()->create_index("employees", {"employees_name_u", {"name"}, true, 0}).code() == ErrorCode::Constraint);
    REQUIRE_OK(m2.value()->create_table(punches()));
    REQUIRE_OK(m2.value()->insert("punches", {Value::integer(3), Value::timestamp(5), Value::text("in")}));
    REQUIRE_OK(m2.value()->set_schema_version(7));
    REQUIRE_OK(m2.value()->commit());
    // Dropping the index a foreign key relies on is refused; dropping a referenced table too.
    auto d = st.value()->begin_write();
    REQUIRE_OK(d.status());
    CHECK(d.value()->drop_index("punches", "punches_kind").ok());
    CHECK(d.value()->drop_index("employees", "employees_manager").code() == ErrorCode::InvalidArgument);
    CHECK(d.value()->drop_table("employees").code() == ErrorCode::InvalidArgument);
    REQUIRE_OK(d.value()->drop_table("punches"));
    REQUIRE_OK(d.value()->drop_index("employees", "employees_rate"));
    REQUIRE_OK(d.value()->commit());
    auto rep = st.value()->check();
    REQUIRE_OK(rep.status());
    CHECK_MSG(rep.value().ok, rep.value().problems[0]);
    REQUIRE_OK(st.value()->close());
  }
  auto st = Store::open(vfs, "s/m.db", opts());
  REQUIRE_OK(st.status());
  auto r = st.value()->begin_read();
  REQUIRE_OK(r.status());
  CHECK(r.value()->catalog().schema_version == 7);
  CHECK(r.value()->catalog().table("punches") == nullptr);
  CHECK(r.value()->catalog().table("employees")->indexes.size() == 3);
  auto c = r.value()->count("employees");
  REQUIRE_OK(c.status());
  CHECK(c.value() == 50);
  auto rep = st.value()->check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
  // Every page is accounted for: catalog + 4 trees + free list.
  CHECK(rep.value().pages_owned + rep.value().pager.free_pages.size() + 1 == rep.value().pager.pages_checked);
}

ARCHIVUM_TEST(store_dump_and_restore_round_trip) {
  MemVfs vfs;
  auto a = Store::open(vfs, "s/a.db", opts());
  REQUIRE_OK(a.status());
  {
    auto w = a.value()->begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->create_table(employees()));
    REQUIRE_OK(w.value()->create_table(punches()));
    REQUIRE_OK(w.value()->insert("employees", emp(1, "B1", "Ada", std::nullopt)));
    // Self-reference that only resolves once both rows exist.
    REQUIRE_OK(w.value()->insert("employees", emp(2, "B2", "Bob", 1)));
    Row ada = emp(1, "B1", "Ada", 2);
    REQUIRE_OK(w.value()->update("employees", ada));
    for (int i = 0; i < 300; ++i) {
      REQUIRE_OK(w.value()->insert("punches", {Value::integer(1 + i % 2), Value::timestamp(i), Value::text(i % 2 ? "in" : "out")}));
    }
    REQUIRE_OK(w.value()->set_schema_version(3));
    REQUIRE_OK(w.value()->commit());
  }
  auto d = a.value()->dump();
  REQUIRE_OK(d.status());
  CHECK(d.value().schema_version == 3);
  REQUIRE(d.value().tables.size() == 2);
  auto b = Store::open(vfs, "s/b.db", opts());
  REQUIRE_OK(b.status());
  REQUIRE_OK(b.value()->restore(d.value()));
  auto d2 = b.value()->dump();
  REQUIRE_OK(d2.status());
  CHECK(d2.value().schema_version == 3);
  REQUIRE(d2.value().tables.size() == 2);
  for (std::size_t i = 0; i < 2; ++i) {
    const TableDump& x = d.value().tables[i];
    const TableDump& y = d2.value().tables[i];
    CHECK(x.def.name == y.def.name);
    CHECK(encode_row(encode_table_def(x.def)) == encode_row(encode_table_def(y.def)));
    REQUIRE(x.rows.size() == y.rows.size());
    for (std::size_t k = 0; k < x.rows.size(); ++k) CHECK(encode_row(x.rows[k]) == encode_row(y.rows[k]));
  }
  auto rep = b.value()->check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
  CHECK(rep.value().rows == 302);
  // Restore into a non-empty store is refused.
  CHECK(b.value()->restore(d.value()).code() == ErrorCode::InvalidArgument);
}

ARCHIVUM_TEST(store_snapshot_readers_and_pending_checkpoint) {
  MemVfs vfs;
  auto st = Store::open(vfs, "s/c.db", opts());
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->create_table(employees()));
    REQUIRE_OK(w.value()->commit());
  }
  // A long-running query holds its snapshot while the writer commits and
  // wants to checkpoint: the checkpoint reports Busy until the query ends,
  // and the query sees exactly the state it started on.
  auto long_query = store.begin_read();
  REQUIRE_OK(long_query.status());
  std::atomic<bool> stop{false};
  std::atomic<int> busy{0};
  std::atomic<int> reads_ok{0};
  std::atomic<int> reads_bad{0};
  std::thread writer([&] {
    for (int i = 1; i <= 200; ++i) {
      auto w = store.begin_write();
      if (!w.ok()) {
        reads_bad.fetch_add(1);
        return;
      }
      if (!w.value()->insert("employees", emp(i, "B" + std::to_string(i), "N", std::nullopt)).ok() ||
          !w.value()->commit().ok()) {
        reads_bad.fetch_add(1);
        return;
      }
      if (store.checkpoint().code() == ErrorCode::Busy) busy.fetch_add(1);
    }
    stop.store(true);
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        auto rd = store.begin_read();
        if (!rd.ok()) {
          reads_bad.fetch_add(1);
          return;
        }
        // Row count and the maximum id agree with each other inside the snapshot.
        auto rows = rd.value()->scan_all("employees");
        if (!rows.ok()) {
          reads_bad.fetch_add(1);
          return;
        }
        const std::int64_t max_id = rows.value().empty() ? 0 : rows.value().back()[0].as_int64();
        if (static_cast<std::int64_t>(rows.value().size()) != max_id) {
          reads_bad.fetch_add(1);
        } else {
          reads_ok.fetch_add(1);
        }
      }
    });
  }
  writer.join();
  for (auto& t : readers) t.join();
  CHECK(reads_bad.load() == 0);
  CHECK(reads_ok.load() > 0);
  CHECK_MSG(busy.load() > 0, "checkpoint never reported Busy while the long query held its snapshot");
  auto old = long_query.value()->count("employees");
  REQUIRE_OK(old.status());
  CHECK(old.value() == 0);
  long_query.value().reset();
  REQUIRE_OK(store.checkpoint());
  auto now = store.begin_read();
  REQUIRE_OK(now.status());
  CHECK(now.value()->count("employees").value() == 200);
  auto rep = store.check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
}
