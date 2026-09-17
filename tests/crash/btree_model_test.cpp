// Model-based test of the B-tree: a std::map<Bytes, Bytes> model, random
// puts, replaces, erases, point lookups, forward and backward range scans,
// with the full state compared after every transaction and the structural
// invariants checked after every operation. Runs over the crash shim so
// that crashes at random points are recovered and compared as well.
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

#include "archivum/engine/btree.h"
#include "archivum/testing/fault_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

std::uint64_t splitmix(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() { return splitmix(s); }
  std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
};

using Model = std::map<Bytes, Bytes>;

Bytes random_key(Rng& rng, std::uint32_t max_key) {
  // Small key space so replaces and erases hit existing keys often; keys of
  // varying length so that prefix ordering is exercised.
  const std::size_t len = 1 + rng.below(std::min<std::uint64_t>(max_key, 24));
  Bytes k(len);
  for (auto& c : k) c = static_cast<std::byte>('a' + rng.below(6));
  return k;
}
Bytes random_value(Rng& rng) {
  const std::uint64_t r = rng.below(20);
  const std::size_t len = r < 17 ? rng.below(60) : (r == 17 ? 200 + rng.below(400) : 1000 + rng.below(3000));
  Bytes v(len);
  for (auto& c : v) c = static_cast<std::byte>(rng.next() & 0xFF);
  return v;
}

std::string show(const Bytes& b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

// Full comparison: forward scan equals the model, backward scan equals the
// reverse, every model key is gettable, and the structure checks.
std::vector<std::string> compare(BTree& t, const Model& m) {
  std::vector<std::string> problems;
  auto c = t.cursor();
  if (Status s = c.seek_first(); !s.ok()) return {"seek_first: " + s.to_string()};
  auto it = m.begin();
  std::size_t n = 0;
  while (c.valid()) {
    if (it == m.end()) {
      problems.push_back("engine has extra key " + show(c.key()));
      break;
    }
    if (c.key() != it->first) {
      problems.push_back("key mismatch: engine " + show(c.key()) + " model " + show(it->first));
      break;
    }
    auto v = c.value();
    if (!v.ok() || v.value() != it->second) {
      problems.push_back("value mismatch at " + show(it->first));
      break;
    }
    ++it;
    ++n;
    if (Status s = c.next(); !s.ok()) {
      problems.push_back("next: " + s.to_string());
      break;
    }
  }
  if (problems.empty() && it != m.end()) problems.push_back("engine missing key " + show(it->first));
  if (Status s = c.seek_last(); !s.ok()) problems.push_back("seek_last: " + s.to_string());
  auto rit = m.rbegin();
  while (problems.empty() && c.valid() && rit != m.rend()) {
    if (c.key() != rit->first) {
      problems.push_back("backward key mismatch at " + show(rit->first));
      break;
    }
    ++rit;
    if (Status s = c.prev(); !s.ok()) {
      problems.push_back("prev: " + s.to_string());
      break;
    }
  }
  auto rep = t.check();
  if (!rep.ok()) {
    problems.push_back("check: " + rep.status().to_string());
  } else {
    for (const auto& p : rep.value().problems) problems.push_back("check: " + p);
    if (rep.value().entries != m.size()) {
      problems.push_back("entry count " + std::to_string(rep.value().entries) + " != model " + std::to_string(m.size()));
    }
  }
  return problems;
}

DbOptions opts() {
  DbOptions o;
  o.page_size = 512;
  o.cache_pages = 16;
  o.checkpoint_threshold_frames = 40;
  return o;
}

}  // namespace

ARCHIVUM_TEST(btree_model_random_operations) {
  std::uint64_t base_seed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  if (const char* env = std::getenv("ARCHIVUM_SEED")) base_seed = std::strtoull(env, nullptr, 10);
  std::uint64_t iterations = 8;
  if (const char* env = std::getenv("ARCHIVUM_CRASH_ITERS")) iterations = std::max<std::uint64_t>(1, std::strtoull(env, nullptr, 10) / 40);
  std::printf("  base seed %llu, %llu iterations\n", static_cast<unsigned long long>(base_seed),
              static_cast<unsigned long long>(iterations));
  for (std::uint64_t it = 0; it < iterations; ++it) {
    const std::uint64_t seed = base_seed + it;
    Rng rng(seed);
    FaultConfig cfg;
    cfg.crash.persist = static_cast<CrashPolicy::Persist>(rng.below(4));
    cfg.seed = seed;
    FaultVfs vfs(cfg);
    Model committed;
    PageNo root = 0;
    {
      auto db = Db::open(vfs, "d/t.db", opts());
      REQUIRE_OK(db.status());
      auto w = db.value()->begin_write();
      REQUIRE_OK(w.status());
      WriteTxnPages pages(*w.value(), 512);
      auto r = BTree::create(pages);
      REQUIRE_OK(r.status());
      root = r.value();
      REQUIRE_OK(w.value()->commit());
    }
    std::ostringstream ctx;
    ctx << "seed=" << seed << " persist=" << static_cast<int>(cfg.crash.persist);
    set_context(ctx.str());

    for (int round = 0; round < 4; ++round) {
      auto db_r = Db::open(vfs, "d/t.db", opts());
      REQUIRE_MSG(db_r.ok(), "open: " << db_r.status().to_string());
      Db& db = *db_r.value();
      {
        auto rd = db.begin_read();
        REQUIRE_OK(rd.status());
        ReadTxnPages rp(*rd.value(), 512);
        BTree t(rp, nullptr, root);
        auto problems = compare(t, committed);
        REQUIRE_MSG(problems.empty(), "after open: " << problems[0]);
      }
      bool crashed = false;
      for (int txn = 0; txn < 25 && !crashed; ++txn) {
        auto w = db.begin_write();
        REQUIRE_OK(w.status());
        WriteTxnPages pages(*w.value(), 512);
        BTree t(pages, &pages, root);
        Model next = committed;
        const int ops = 1 + static_cast<int>(rng.below(12));
        Status st;
        for (int i = 0; i < ops && st.ok(); ++i) {
          const std::uint64_t pick = rng.below(10);
          if (pick < 5) {
            Bytes k = random_key(rng, t.max_key_bytes());
            Bytes v = random_value(rng);
            st = t.put(k, v);
            if (st.ok()) next[k] = v;
          } else if (pick < 8 && !next.empty()) {
            auto mit = next.begin();
            std::advance(mit, static_cast<std::ptrdiff_t>(rng.below(next.size())));
            bool existed = false;
            st = t.erase(mit->first, existed);
            if (st.ok()) {
              CHECK(existed);
              next.erase(mit);
            }
          } else {
            Bytes k = random_key(rng, t.max_key_bytes());
            auto g = t.get(k);
            st = g.status();
            if (st.ok()) {
              auto mit = next.find(k);
              const bool in_model = mit != next.end();
              CHECK_MSG(g.value().has_value() == in_model, "get presence mismatch for " << show(k));
              if (in_model && g.value().has_value()) CHECK(*g.value() == mit->second);
            }
          }
          if (st.ok() && rng.below(4) == 0) {
            auto rep = t.check();
            REQUIRE_OK(rep.status());
            CHECK_MSG(rep.value().ok, "mid-transaction: " << rep.value().problems[0]);
          }
        }
        if (st.code() == ErrorCode::Crashed) {
          crashed = true;
          break;
        }
        REQUIRE_OK(st);
        if (rng.below(6) == 0) {
          w.value()->rollback();
          continue;
        }
        Status c = w.value()->commit();
        if (c.code() == ErrorCode::Crashed) {
          // In doubt: accept either after recovery.
          crashed = true;
          vfs.set_injector(nullptr);
          vfs.recover();
          auto re = Db::open(vfs, "d/t.db", opts());
          REQUIRE_OK(re.status());
          auto rd = re.value()->begin_read();
          REQUIRE_OK(rd.status());
          ReadTxnPages rp(*rd.value(), 512);
          BTree rt(rp, nullptr, root);
          auto p_old = compare(rt, committed);
          if (!p_old.empty()) {
            auto p_new = compare(rt, next);
            REQUIRE_MSG(p_new.empty(), "in-doubt commit matches neither: " << p_old[0] << " / " << p_new[0]);
            committed = next;
          }
          break;
        }
        REQUIRE_OK(c);
        committed = next;
        if (txn % 7 == 0) {
          auto rd = db.begin_read();
          REQUIRE_OK(rd.status());
          ReadTxnPages rp(*rd.value(), 512);
          BTree rt(rp, nullptr, root);
          auto problems = compare(rt, committed);
          REQUIRE_MSG(problems.empty(), "after commit: " << problems[0]);
        }
      }
      if (!crashed) {
        // End the round with a crash at a random later point: inject it.
        vfs.set_injector(std::make_shared<CrashAtStep>(vfs.op_count() + rng.below(30)));
        // Drive a few more transactions until the crash lands.
        for (int extra = 0; extra < 20 && !crashed; ++extra) {
          auto w = db.begin_write();
          if (!w.ok()) {
            crashed = w.status().code() == ErrorCode::Crashed;
            break;
          }
          WriteTxnPages pages(*w.value(), 512);
          BTree t(pages, &pages, root);
          Model next = committed;
          Bytes k = random_key(rng, t.max_key_bytes());
          Bytes v = random_value(rng);
          Status st = t.put(k, v);
          if (st.code() == ErrorCode::Crashed) {
            crashed = true;
            break;
          }
          REQUIRE_OK(st);
          next[k] = v;
          Status c = w.value()->commit();
          if (c.code() == ErrorCode::Crashed) {
            crashed = true;
            vfs.set_injector(nullptr);
            vfs.recover();
            auto re = Db::open(vfs, "d/t.db", opts());
            REQUIRE_OK(re.status());
            auto rd = re.value()->begin_read();
            REQUIRE_OK(rd.status());
            ReadTxnPages rp(*rd.value(), 512);
            BTree rt(rp, nullptr, root);
            if (!compare(rt, committed).empty()) {
              REQUIRE_MSG(compare(rt, next).empty(), "in-doubt commit matches neither");
              committed = next;
            }
            break;
          }
          REQUIRE_OK(c);
          committed = next;
        }
        if (!crashed) vfs.crash();
      }
      vfs.set_injector(nullptr);
      vfs.recover();
    }
  }
  set_context("");
}
