#include "archivum/engine/db.h"

#include <cstring>
#include <vector>

#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using archivum::testing::MemVfs;

namespace {

DbOptions small() {
  DbOptions o;
  o.page_size = 512;
  o.cache_pages = 16;
  o.checkpoint_threshold_frames = 1000000;  // manual checkpoints unless a test says otherwise
  return o;
}

std::vector<std::byte> pattern(std::uint32_t page_size, std::uint8_t seed) {
  std::vector<std::byte> p(page_size);
  for (std::size_t i = 0; i < p.size(); ++i) p[i] = static_cast<std::byte>((seed + i * 7) & 0xFF);
  return p;
}

bool body_equal(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
  const std::size_t n = a.size() - kPageTrailerBytes;
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), n) == 0;
}

}  // namespace

ARCHIVUM_TEST(db_create_open_reopen) {
  MemVfs vfs;
  {
    auto db = Db::open(vfs, "dir/a.db", small());
    REQUIRE_OK(db.status());
    CHECK(db.value()->page_size() == 512);
    CHECK(db.value()->usable_page_bytes() == 508);
    auto r = db.value()->begin_read();
    REQUIRE_OK(r.status());
    CHECK(r.value()->page_count() == 1);
    CHECK(vfs.has("dir/a.db"));
    CHECK(vfs.has("dir/a.db.wal"));
  }
  auto db = Db::open(vfs, "dir/a.db", small());
  REQUIRE_OK(db.status());
  auto report = db.value()->check();
  REQUIRE_OK(report.status());
  CHECK(report.value().ok);
  DbOptions no_create = small();
  no_create.create_if_missing = false;
  CHECK(Db::open(vfs, "dir/missing.db", no_create).status().code() == ErrorCode::NotFound);
}

ARCHIVUM_TEST(db_short_or_torn_header_is_reinitialised) {
  MemVfs vfs;
  // A crash during creation can leave any prefix of page 0, or a stale log.
  vfs.put("a.db", std::vector<std::byte>(10, std::byte{'x'}));
  vfs.put("a.db.wal", std::vector<std::byte>(200, std::byte{'w'}));
  auto db = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db.status());
  auto r = db.value()->begin_read();
  REQUIRE_OK(r.status());
  CHECK(r.value()->page_count() == 1);
  CHECK(vfs.contents("a.db").size() == 512);
  r.value().reset();
  REQUIRE_OK(db.value()->close());
  // A longer file with a bad header is real corruption, never reinitialised.
  vfs.put("b.db", std::vector<std::byte>(2000, std::byte{'x'}));
  CHECK(Db::open(vfs, "b.db", small()).status().code() == ErrorCode::Corrupt);
  CHECK(vfs.contents("b.db").size() == 2000);
}

ARCHIVUM_TEST(db_write_commit_read_and_reopen) {
  MemVfs vfs;
  auto db_r = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db_r.status());
  Db& db = *db_r.value();
  PageNo p1 = 0, p2 = 0;
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    auto a = w.value()->allocate_page();
    auto b = w.value()->allocate_page();
    REQUIRE_OK(a.status());
    REQUIRE_OK(b.status());
    p1 = a.value();
    p2 = b.value();
    CHECK(p1 == 1 && p2 == 2);
    REQUIRE_OK(w.value()->write_page(p1, pattern(512, 1)));
    REQUIRE_OK(w.value()->write_page(p2, pattern(512, 2)));
    // Own writes are visible before commit.
    std::vector<std::byte> buf(512);
    REQUIRE_OK(w.value()->read_page(p1, buf));
    CHECK(body_equal(buf, pattern(512, 1)));
    REQUIRE_OK(w.value()->commit());
  }
  {
    auto r = db.begin_read();
    REQUIRE_OK(r.status());
    CHECK(r.value()->page_count() == 3);
    CHECK(r.value()->change_counter() == 1);
    std::vector<std::byte> buf(512);
    REQUIRE_OK(r.value()->read_page(p2, buf));
    CHECK(body_equal(buf, pattern(512, 2)));
    CHECK(page_checksum_ok(buf));
    CHECK(r.value()->read_page(3, buf).code() == ErrorCode::InvalidArgument);
  }
  REQUIRE_OK(db.close());
  auto again = Db::open(vfs, "a.db", small());
  REQUIRE_OK(again.status());
  CHECK(again.value()->stats().wal_recovered_frames == 3);  // two pages plus the header
  auto r = again.value()->begin_read();
  REQUIRE_OK(r.status());
  std::vector<std::byte> buf(512);
  REQUIRE_OK(r.value()->read_page(p1, buf));
  CHECK(body_equal(buf, pattern(512, 1)));
}

ARCHIVUM_TEST(db_snapshot_isolation) {
  MemVfs vfs;
  auto db_r = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db_r.status());
  Db& db = *db_r.value();
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->allocate_page().status());
    REQUIRE_OK(w.value()->write_page(1, pattern(512, 10)));
    REQUIRE_OK(w.value()->commit());
  }
  auto old_reader = db.begin_read();
  REQUIRE_OK(old_reader.status());
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->write_page(1, pattern(512, 11)));
    REQUIRE_OK(w.value()->allocate_page().status());
    REQUIRE_OK(w.value()->commit());
  }
  std::vector<std::byte> buf(512);
  REQUIRE_OK(old_reader.value()->read_page(1, buf));
  CHECK(body_equal(buf, pattern(512, 10)));
  CHECK(old_reader.value()->page_count() == 2);
  auto new_reader = db.begin_read();
  REQUIRE_OK(new_reader.status());
  REQUIRE_OK(new_reader.value()->read_page(1, buf));
  CHECK(body_equal(buf, pattern(512, 11)));
  CHECK(new_reader.value()->page_count() == 3);
  // Checkpoint is refused while readers are active, then succeeds.
  CHECK(db.checkpoint().code() == ErrorCode::Busy);
  old_reader.value().reset();
  new_reader.value().reset();
  REQUIRE_OK(db.checkpoint());
  CHECK(db.stats().wal_frames == 0);
  CHECK(vfs.contents("a.db").size() == 3 * 512);
  auto r = db.begin_read();
  REQUIRE_OK(r.status());
  REQUIRE_OK(r.value()->read_page(1, buf));
  CHECK(body_equal(buf, pattern(512, 11)));
}

ARCHIVUM_TEST(db_rollback_and_validation) {
  MemVfs vfs;
  auto db_r = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db_r.status());
  Db& db = *db_r.value();
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->allocate_page().status());
    REQUIRE_OK(w.value()->write_page(1, pattern(512, 1)));
    w.value()->rollback();
  }
  auto r = db.begin_read();
  REQUIRE_OK(r.status());
  CHECK(r.value()->page_count() == 1);
  r.value().reset();
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    CHECK(w.value()->write_page(0, pattern(512, 1)).code() == ErrorCode::InvalidArgument);
    CHECK(w.value()->write_page(5, pattern(512, 1)).code() == ErrorCode::InvalidArgument);
    std::vector<std::byte> wrong(100);
    REQUIRE_OK(w.value()->allocate_page().status());
    CHECK(w.value()->write_page(1, wrong).code() == ErrorCode::InvalidArgument);
    REQUIRE_OK(w.value()->free_page(1));
    CHECK(w.value()->free_page(1).code() == ErrorCode::InvalidArgument);
    CHECK(w.value()->write_page(1, pattern(512, 1)).code() == ErrorCode::InvalidArgument);
    REQUIRE_OK(w.value()->commit());
  }
  // An unfinished transaction is rolled back by its destructor.
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->allocate_page().status());
  }
  auto w2 = db.begin_write();  // would deadlock if the destructor had not released the lock
  REQUIRE_OK(w2.status());
  CHECK(w2.value()->page_count() == 2);
}

ARCHIVUM_TEST(db_free_list_reuse_and_check) {
  MemVfs vfs;
  auto db_r = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db_r.status());
  Db& db = *db_r.value();
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    for (int i = 0; i < 5; ++i) {
      auto p = w.value()->allocate_page();
      REQUIRE_OK(p.status());
      REQUIRE_OK(w.value()->write_page(p.value(), pattern(512, static_cast<std::uint8_t>(i))));
    }
    REQUIRE_OK(w.value()->free_page(2));
    REQUIRE_OK(w.value()->free_page(4));
    REQUIRE_OK(w.value()->commit());
  }
  auto report = db.check();
  REQUIRE_OK(report.status());
  CHECK_MSG(report.value().ok, report.value().problems.empty() ? "" : report.value().problems[0]);
  CHECK(report.value().free_pages_walked == 2);
  {
    auto w = db.begin_write();
    REQUIRE_OK(w.status());
    auto a = w.value()->allocate_page();
    auto b = w.value()->allocate_page();
    auto c = w.value()->allocate_page();
    REQUIRE_OK(a.status());
    REQUIRE_OK(b.status());
    REQUIRE_OK(c.status());
    CHECK(a.value() == 4);  // last freed, first reused
    CHECK(b.value() == 2);
    CHECK(c.value() == 6);  // free list exhausted: grow
    std::vector<std::byte> buf(512);
    REQUIRE_OK(w.value()->read_page(a.value(), buf));
    bool zero = true;
    for (std::size_t i = 0; i < 508; ++i) zero = zero && buf[i] == std::byte{0};
    CHECK(zero);  // reused pages come back blank
    REQUIRE_OK(w.value()->commit());
  }
  report = db.check();
  REQUIRE_OK(report.status());
  CHECK(report.value().ok);
  CHECK(report.value().free_pages_walked == 0);
  REQUIRE_OK(db.checkpoint());
  report = db.check();
  REQUIRE_OK(report.status());
  CHECK(report.value().ok);
}

ARCHIVUM_TEST(db_detects_corrupted_page_in_file) {
  MemVfs vfs;
  auto db_r = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db_r.status());
  {
    auto w = db_r.value()->begin_write();
    REQUIRE_OK(w.status());
    REQUIRE_OK(w.value()->allocate_page().status());
    REQUIRE_OK(w.value()->write_page(1, pattern(512, 3)));
    REQUIRE_OK(w.value()->commit());
  }
  REQUIRE_OK(db_r.value()->checkpoint());
  REQUIRE_OK(db_r.value()->close());
  auto raw = vfs.contents("a.db");
  raw[512 + 100] ^= std::byte{0x01};
  vfs.put("a.db", raw);
  auto db = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db.status());
  auto r = db.value()->begin_read();
  REQUIRE_OK(r.status());
  std::vector<std::byte> buf(512);
  CHECK(r.value()->read_page(1, buf).code() == ErrorCode::Corrupt);
  r.value().reset();
  auto report = db.value()->check();
  REQUIRE_OK(report.status());
  CHECK(!report.value().ok);
}

ARCHIVUM_TEST(db_wal_torn_tail_and_foreign_wal) {
  MemVfs vfs;
  auto db_r = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db_r.status());
  for (int i = 0; i < 3; ++i) {
    auto w = db_r.value()->begin_write();
    REQUIRE_OK(w.status());
    auto p = w.value()->allocate_page();
    REQUIRE_OK(p.status());
    REQUIRE_OK(w.value()->write_page(p.value(), pattern(512, static_cast<std::uint8_t>(i))));
    REQUIRE_OK(w.value()->commit());
  }
  REQUIRE_OK(db_r.value()->close());
  // Append half a frame of garbage.
  auto wal = vfs.contents("a.db.wal");
  const std::size_t before = wal.size();
  wal.resize(before + 300, std::byte{0xAB});
  vfs.put("a.db.wal", wal);
  auto db = Db::open(vfs, "a.db", small());
  REQUIRE_OK(db.status());
  CHECK(db.value()->stats().wal_recovered_frames == 6);
  CHECK(db.value()->stats().wal_dropped_tail_bytes == 300);
  auto r = db.value()->begin_read();
  REQUIRE_OK(r.status());
  CHECK(r.value()->page_count() == 4);
  r.value().reset();
  REQUIRE_OK(db.value()->close());
  // A log from another database is not accepted.
  auto other = Db::open(vfs, "b.db", small());
  REQUIRE_OK(other.status());
  REQUIRE_OK(other.value()->close());
  vfs.put("b.db.wal", vfs.contents("a.db.wal"));
  auto b = Db::open(vfs, "b.db", small());
  REQUIRE_OK(b.status());
  CHECK(b.value()->stats().wal_recovered_frames == 0);
  auto rb = b.value()->begin_read();
  REQUIRE_OK(rb.status());
  CHECK(rb.value()->page_count() == 1);
}

ARCHIVUM_TEST(db_auto_checkpoint_threshold) {
  MemVfs vfs;
  DbOptions o = small();
  o.checkpoint_threshold_frames = 4;
  auto db_r = Db::open(vfs, "a.db", o);
  REQUIRE_OK(db_r.status());
  for (int i = 0; i < 3; ++i) {
    auto w = db_r.value()->begin_write();
    REQUIRE_OK(w.status());
    auto p = w.value()->allocate_page();
    REQUIRE_OK(p.status());
    REQUIRE_OK(w.value()->write_page(p.value(), pattern(512, 9)));
    REQUIRE_OK(w.value()->commit());  // 2 frames each: threshold crossed on the second commit
  }
  CHECK(db_r.value()->stats().checkpoints >= 1);
  auto report = db_r.value()->check();
  REQUIRE_OK(report.status());
  CHECK(report.value().ok);
}

ARCHIVUM_TEST(db_two_instances_are_independent) {
  MemVfs vfs;
  auto a = Db::open(vfs, "a.db", small());
  auto b = Db::open(vfs, "b.db", small());
  REQUIRE_OK(a.status());
  REQUIRE_OK(b.status());
  // A writer on A does not block a writer on B.
  auto wa = a.value()->begin_write();
  auto wb = b.value()->begin_write();
  REQUIRE_OK(wa.status());
  REQUIRE_OK(wb.status());
  REQUIRE_OK(wa.value()->allocate_page().status());
  REQUIRE_OK(wb.value()->allocate_page().status());
  REQUIRE_OK(wb.value()->allocate_page().status());
  REQUIRE_OK(wb.value()->commit());
  REQUIRE_OK(wa.value()->commit());
  auto ra = a.value()->begin_read();
  auto rb = b.value()->begin_read();
  REQUIRE_OK(ra.status());
  REQUIRE_OK(rb.status());
  CHECK(ra.value()->page_count() == 2);
  CHECK(rb.value()->page_count() == 3);
  // A reader on B does not stop A from checkpointing.
  ra.value().reset();
  REQUIRE_OK(a.value()->checkpoint());
  CHECK(b.value()->checkpoint().code() == ErrorCode::Busy);
}
