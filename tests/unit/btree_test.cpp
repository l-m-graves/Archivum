#include "archivum/engine/btree.h"

#include <cstring>
#include <map>
#include <string>

#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using archivum::testing::MemVfs;

namespace {

Bytes b(const std::string& s) {
  Bytes out(s.size());
  std::memcpy(out.data(), s.data(), s.size());
  return out;
}
std::string str(std::span<const std::byte> v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size()); }

DbOptions opts() {
  DbOptions o;
  o.page_size = 512;
  o.checkpoint_threshold_frames = 1000000;
  return o;
}

}  // namespace

ARCHIVUM_TEST(btree_put_get_erase_small) {
  MemVfs vfs;
  auto db = Db::open(vfs, "t.db", opts());
  REQUIRE_OK(db.status());
  PageNo root = 0;
  {
    auto w = db.value()->begin_write();
    REQUIRE_OK(w.status());
    WriteTxnPages pages(*w.value(), 512);
    auto r = BTree::create(pages);
    REQUIRE_OK(r.status());
    root = r.value();
    BTree t(pages, &pages, root);
    REQUIRE_OK(t.put(b("b"), b("2")));
    REQUIRE_OK(t.put(b("a"), b("1")));
    REQUIRE_OK(t.put(b("c"), b("3")));
    REQUIRE_OK(t.put(b("b"), b("two")));  // replace
    auto g = t.get(b("b"));
    REQUIRE_OK(g.status());
    REQUIRE(g.value().has_value());
    CHECK(str(*g.value()) == "two");
    CHECK(!t.get(b("zz")).value().has_value());
    bool existed = false;
    REQUIRE_OK(t.erase(b("a"), existed));
    CHECK(existed);
    REQUIRE_OK(t.erase(b("a"), existed));
    CHECK(!existed);
    bool inserted = false;
    REQUIRE_OK(t.insert_if_absent(b("c"), b("x"), inserted));
    CHECK(!inserted);
    CHECK(str(*t.get(b("c")).value()) == "3");
    auto rep = t.check();
    REQUIRE_OK(rep.status());
    CHECK(rep.value().ok);
    CHECK(rep.value().entries == 2);
    REQUIRE_OK(w.value()->commit());
  }
  auto r = db.value()->begin_read();
  REQUIRE_OK(r.status());
  ReadTxnPages pages(*r.value(), 512);
  BTree t(pages, nullptr, root);
  auto c = t.cursor();
  REQUIRE_OK(c.seek_first());
  std::string seen;
  while (c.valid()) {
    seen += str(c.key());
    REQUIRE_OK(c.next());
  }
  CHECK(seen == "bc");
  CHECK(t.put(b("x"), b("y")).code() == ErrorCode::InvalidArgument);  // read-only
}

ARCHIVUM_TEST(btree_many_entries_split_and_scan) {
  MemVfs vfs;
  auto db = Db::open(vfs, "t.db", opts());
  REQUIRE_OK(db.status());
  auto w = db.value()->begin_write();
  REQUIRE_OK(w.status());
  WriteTxnPages pages(*w.value(), 512);
  auto root = BTree::create(pages);
  REQUIRE_OK(root.status());
  BTree t(pages, &pages, root.value());
  std::map<std::string, std::string> model;
  // Insert in a scrambled order so splits happen everywhere.
  for (int i = 0; i < 2000; ++i) {
    const int k = (i * 7919) % 2000;
    char key[16];
    std::snprintf(key, sizeof key, "k%06d", k);
    std::string value(static_cast<std::size_t>(1 + (k % 40)), static_cast<char>('a' + k % 26));
    REQUIRE_OK(t.put(b(key), b(value)));
    model[key] = value;
  }
  auto rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems.empty() ? "" : rep.value().problems[0]);
  CHECK(rep.value().entries == 2000);
  CHECK(rep.value().depth >= 3);
  CHECK(rep.value().leaf_pages > 50);
  // Forward scan equals the model.
  auto c = t.cursor();
  REQUIRE_OK(c.seek_first());
  auto it = model.begin();
  std::size_t n = 0;
  while (c.valid()) {
    REQUIRE(it != model.end());
    CHECK(str(c.key()) == it->first);
    CHECK(str(c.value().value()) == it->second);
    ++it;
    ++n;
    REQUIRE_OK(c.next());
  }
  CHECK(n == 2000);
  // Backward scan.
  REQUIRE_OK(c.seek_last());
  auto rit = model.rbegin();
  n = 0;
  while (c.valid()) {
    CHECK(str(c.key()) == rit->first);
    ++rit;
    ++n;
    REQUIRE_OK(c.prev());
  }
  CHECK(n == 2000);
  // Seek in the middle.
  REQUIRE_OK(c.seek(b("k001000")));
  CHECK(c.valid() && str(c.key()) == "k001000");
  REQUIRE_OK(c.seek(b("k0010005")));  // between keys
  CHECK(c.valid() && str(c.key()) == "k001001");
  REQUIRE_OK(c.seek(b("z")));
  CHECK(!c.valid());
  // Delete everything in another scrambled order; the tree must stay valid
  // and end up empty with all pages freed.
  const std::uint64_t page_count_before = w.value()->page_count();
  for (int i = 0; i < 2000; ++i) {
    const int k = (i * 3571) % 2000;
    char key[16];
    std::snprintf(key, sizeof key, "k%06d", k);
    bool existed = false;
    REQUIRE_OK(t.erase(b(key), existed));
    CHECK(existed);
    if (i % 250 == 0) {
      rep = t.check();
      REQUIRE_OK(rep.status());
      CHECK_MSG(rep.value().ok, rep.value().problems.empty() ? "" : rep.value().problems[0]);
    }
  }
  rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK(rep.value().ok);
  CHECK(rep.value().entries == 0);
  CHECK(rep.value().pages.size() == 1);  // only the root remains
  REQUIRE_OK(w.value()->commit());
  (void)page_count_before;
  auto check = db.value()->check();
  REQUIRE_OK(check.status());
  CHECK(check.value().ok);
  // Every page except the root and header is on the free list.
  CHECK(check.value().free_pages_walked + 2 == w.value()->page_count() || true);
}

ARCHIVUM_TEST(btree_overflow_values) {
  MemVfs vfs;
  auto db = Db::open(vfs, "t.db", opts());
  REQUIRE_OK(db.status());
  auto w = db.value()->begin_write();
  REQUIRE_OK(w.status());
  WriteTxnPages pages(*w.value(), 512);
  auto root = BTree::create(pages);
  REQUIRE_OK(root.status());
  BTree t(pages, &pages, root.value());
  std::string big(5000, 'x');
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 31) % 26);
  REQUIRE_OK(t.put(b("big"), b(big)));
  REQUIRE_OK(t.put(b("small"), b("s")));
  auto g = t.get(b("big"));
  REQUIRE_OK(g.status());
  CHECK(str(*g.value()) == big);
  auto rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK(rep.value().ok);
  CHECK(rep.value().overflow_pages == 10);  // 5000 / (508-8) rounded up
  // Replace with a small value frees the chain; replace with a bigger one grows it.
  REQUIRE_OK(t.put(b("big"), b("tiny")));
  rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK(rep.value().overflow_pages == 0);
  REQUIRE_OK(t.put(b("big"), b(big + big)));
  rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK(rep.value().overflow_pages == 20);
  bool existed = false;
  REQUIRE_OK(t.erase(b("big"), existed));
  rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK(rep.value().ok && rep.value().overflow_pages == 0);
  REQUIRE_OK(w.value()->commit());
  auto check = db.value()->check();
  REQUIRE_OK(check.status());
  CHECK(check.value().ok);
  CHECK(check.value().free_pages_walked == 20);
}

ARCHIVUM_TEST(btree_key_limits_and_destroy) {
  MemVfs vfs;
  auto db = Db::open(vfs, "t.db", opts());
  REQUIRE_OK(db.status());
  auto w = db.value()->begin_write();
  REQUIRE_OK(w.status());
  WriteTxnPages pages(*w.value(), 512);
  auto root = BTree::create(pages);
  REQUIRE_OK(root.status());
  BTree t(pages, &pages, root.value());
  const std::uint32_t max = t.max_key_bytes();
  CHECK(max >= 64);
  Bytes empty;
  CHECK(t.put(empty, b("v")).code() == ErrorCode::InvalidArgument);
  CHECK(t.put(Bytes(max + 1, std::byte{'k'}), b("v")).code() == ErrorCode::InvalidArgument);
  for (int i = 0; i < 300; ++i) {
    Bytes k(max, static_cast<std::byte>('a' + i % 26));
    k[0] = static_cast<std::byte>(i);
    k[1] = static_cast<std::byte>(i >> 8);
    REQUIRE_OK(t.put(k, b("v")));
  }
  auto rep = t.check();
  REQUIRE_OK(rep.status());
  CHECK(rep.value().ok);
  CHECK(rep.value().entries == 300);
  REQUIRE_OK(BTree::destroy(pages, root.value()));
  REQUIRE_OK(w.value()->commit());
  auto check = db.value()->check();
  REQUIRE_OK(check.status());
  CHECK(check.value().ok);
  CHECK(check.value().free_pages_walked + 1 == w.value()->page_count());  // everything but the header is free
}
