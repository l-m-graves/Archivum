// Multi-threaded paths that the single-threaded model-based suites never
// exercise. Run under ThreadSanitizer in CI (label "concurrency") as well
// as under ASan and Release. Each test states the path it covers.
#include <atomic>
#include <thread>
#include <vector>

#include "archivum/engine/migrate.h"
#include "archivum/engine/recovery.h"
#include "archivum/engine/store.h"
#include "archivum/punchline/schema.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

DbOptions opts(const char* archive = "") {
  DbOptions o;
  o.page_size = 1024;
  o.cache_pages = 16;
  o.checkpoint_threshold_frames = 8;
  o.archive_dir = archive;
  return o;
}

TableDef kv(const char* name) {
  TableDef t;
  t.name = name;
  t.columns = {{"k", ColumnType::Integer, false, 0}, {"v", ColumnType::Text, true, 0}, {"w", ColumnType::Integer, true, 0}};
  t.primary_key = {"k"};
  return t;
}

}  // namespace

// Two instances, two threads, no shared state: each thread commits and
// checkpoints its own database while the other does the same. Stage 2
// tested two instances single-threaded only.
ARCHIVUM_TEST(concurrency_two_instances_on_two_threads) {
  MemVfs vfs;
  auto a = Store::open(vfs, "t/a.db", opts());
  auto b = Store::open(vfs, "t/b.db", opts());
  REQUIRE_OK(a.status());
  REQUIRE_OK(b.status());
  std::atomic<int> bad{0};
  auto work = [&](Store& s, const char* table) {
    auto w = s.begin_write();
    if (!w.ok() || !w.value()->create_table(kv(table)).ok() || !w.value()->commit().ok()) {
      bad.fetch_add(1);
      return;
    }
    for (int i = 0; i < 300; ++i) {
      auto wr = s.begin_write();
      if (!wr.ok() || !wr.value()->insert(table, {Value::integer(i), Value::text("x"), Value::null()}).ok() ||
          !wr.value()->commit().ok()) {
        bad.fetch_add(1);
        return;
      }
      if (i % 10 == 0) {
        Status c = s.checkpoint();
        if (!c.ok() && c.code() != ErrorCode::Busy) bad.fetch_add(1);
      }
      auto rd = s.begin_read();
      if (!rd.ok() || rd.value()->count(table).value() != static_cast<std::uint64_t>(i + 1)) bad.fetch_add(1);
    }
  };
  std::thread ta([&] { work(*a.value(), "ta"); });
  std::thread tb([&] { work(*b.value(), "tb"); });
  ta.join();
  tb.join();
  CHECK(bad.load() == 0);
  for (Store* s : {a.value().get(), b.value().get()}) {
    auto rep = s->check();
    REQUIRE_OK(rep.status());
    CHECK_MSG(rep.value().ok, rep.value().problems[0]);
  }
}

// Schema changes under concurrent readers: the catalog cache is keyed by
// change counter and shared between threads; readers must see the
// catalog of their snapshot while a writer creates and drops indexes and
// tables. Stage 3's reader test had a writer inserting rows only.
ARCHIVUM_TEST(concurrency_ddl_under_readers) {
  MemVfs vfs;
  auto st = Store::open(vfs, "t/ddl.db", opts());
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->create_table(kv("base")));
    for (int i = 0; i < 50; ++i) REQUIRE_OK(w.value()->insert("base", {Value::integer(i), Value::text("v"), Value::integer(i % 5)}));
    REQUIRE_OK(w.value()->commit());
  }
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread writer([&] {
    for (int round = 0; round < 40; ++round) {
      auto w = store.begin_write();
      if (!w.ok()) {
        bad.fetch_add(1);
        return;
      }
      const std::string idx = "i" + std::to_string(round);
      const std::string tbl = "t" + std::to_string(round);
      bool ok = w.value()->create_index("base", {idx, {"w", "v"}, false, 0}).ok() && w.value()->create_table(kv(tbl.c_str())).ok() &&
                w.value()->insert(tbl, {Value::integer(1), Value::null(), Value::null()}).ok() &&
                w.value()->set_schema_version(static_cast<std::uint64_t>(round + 1)).ok();
      if (round > 0) {
        ok = ok && w.value()->drop_index("base", "i" + std::to_string(round - 1)).ok() &&
             w.value()->drop_table("t" + std::to_string(round - 1)).ok();
      }
      if (!ok || !w.value()->commit().ok()) bad.fetch_add(1);
    }
    stop.store(true);
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        auto rd = store.begin_read();
        if (!rd.ok()) {
          bad.fetch_add(1);
          return;
        }
        const Catalog& cat = rd.value()->catalog();
        // The catalog of this snapshot: exactly one t<n> table and one
        // i<n> index, both at version n+1, and the index is scannable.
        const std::uint64_t v = cat.schema_version;
        int tables = 0, indexes = 0;
        for (const auto& [name, t] : cat.tables) {
          if (name[0] == 't' && name != "base") ++tables;
        }
        const TableDef* base = cat.table("base");
        if (base == nullptr) {
          bad.fetch_add(1);
          continue;
        }
        for (const IndexDef& i : base->indexes) {
          if (i.name[0] == 'i') ++indexes;
        }
        if (v > 0 && (tables != 1 || indexes != 1)) bad.fetch_add(1);
        if (v > 0) {
          const std::string idx = "i" + std::to_string(v - 1);
          auto rows = rd.value()->scan_all("base", idx, Bound{{Value::integer(2)}}, Bound{{Value::integer(2)}});
          if (!rows.ok() || rows.value().size() != 10) bad.fetch_add(1);
          if (!rd.value()->get("t" + std::to_string(v - 1), {Value::integer(1)}).ok()) bad.fetch_add(1);
        }
      }
    });
  }
  writer.join();
  for (auto& t : readers) t.join();
  CHECK_MSG(bad.load() == 0, bad.load() << " bad observations");
  auto rep = store.check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
}

// Two threads running the same migration list at once: one applies each
// step and the other sees "schema changed underneath" or nothing to do,
// never a double application.
ARCHIVUM_TEST(concurrency_migrate_from_two_threads) {
  MemVfs vfs;
  auto st = Store::open(vfs, "t/mig.db", opts());
  REQUIRE_OK(st.status());
  std::vector<Migration> list;
  for (std::uint64_t v = 1; v <= 6; ++v) {
    const std::string name = "m" + std::to_string(v);
    list.push_back(Migration{v, name, [name](Writer& w) { return w.create_table(kv(name.c_str())); }});
  }
  std::atomic<int> unexpected{0};
  auto run = [&] {
    for (int attempt = 0; attempt < 20; ++attempt) {
      auto r = migrate(*st.value(), "t", list);
      if (r.ok()) return;
      if (r.status().code() != ErrorCode::Busy) {
        unexpected.fetch_add(1);
        return;
      }
    }
  };
  std::thread a(run), b(run);
  a.join();
  b.join();
  CHECK(unexpected.load() == 0);
  auto rd = st.value()->begin_read();
  REQUIRE_OK(rd.status());
  CHECK(rd.value()->catalog().schema_version == 6);
  CHECK(rd.value()->scan_all(kMigrationsTable).value().size() == 6);
  auto rep = st.value()->check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
}

// Backups, checkpoints with archiving, and writers at the same time; then
// recovery from each backup through the archive reproduces the writer's
// state at that counter.
ARCHIVUM_TEST(concurrency_backup_checkpoint_archive_and_writers) {
  MemVfs vfs;
  auto st = Store::open(vfs, "t/arch.db", opts("t/archive"));
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  REQUIRE_OK(core::migrate_all(store, {&punchline::module()}).status());
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread writer([&] {
    for (int i = 1; i <= 400; ++i) {
      auto w = store.begin_write();
      if (!w.ok() || !w.value()->insert("employees", {Value::integer(i), Value::text("E" + std::to_string(i)), Value::text("N"),
                                                       Value::null(), Value::null(), Value::null(), Value::boolean(true),
                                                       Value::null(), Value::text("UTC"), Value::timestamp(i), Value::timestamp(i)}).ok() ||
          !w.value()->commit().ok()) {
        bad.fetch_add(1);
        return;
      }
    }
    stop.store(true);
  });
  std::thread checkpointer([&] {
    while (!stop.load()) {
      Status c = store.checkpoint();
      if (!c.ok() && c.code() != ErrorCode::Busy) bad.fetch_add(1);
      std::this_thread::yield();
    }
  });
  std::vector<std::pair<std::uint64_t, std::string>> backups;
  for (int k = 0; k < 6; ++k) {
    const std::string path = "t/bk" + std::to_string(k) + ".db";
    auto cc = store.backup(vfs, path);
    if (!cc.ok()) {
      bad.fetch_add(1);
      break;
    }
    backups.emplace_back(cc.value(), path);
  }
  writer.join();
  checkpointer.join();
  CHECK(bad.load() == 0);
  REQUIRE_OK(store.checkpoint());
  const std::uint64_t base = store.db().begin_read().value()->change_counter() - 400;
  for (const auto& [cc, path] : backups) {
    RecoveryTarget t;
    t.change_counter = cc + 37;
    const std::string out = path + ".pitr";
    auto rep = recover_to_point(vfs, path, "t/archive", "t/arch.db.wal", t, out);
    REQUIRE_OK(rep.status());
    CHECK(rep.value().target_reached);
    auto rs = Store::open(vfs, out, opts());
    REQUIRE_OK(rs.status());
    CHECK(rs.value()->begin_read().value()->count("employees").value() == cc + 37 - base);
    auto chk = rs.value()->check();
    REQUIRE_OK(chk.status());
    CHECK_MSG(chk.value().ok, chk.value().problems[0]);
  }
}
