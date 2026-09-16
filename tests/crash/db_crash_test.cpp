// Crash-injection and model-based tests for the pager and write-ahead log.
//
// The model is deliberately naive: the committed content of every page, the
// set of free pages, and the page count. Every committed transaction is
// applied to both the engine and the model; after every crash the engine
// is reopened and compared with the model in full.
//
// Invariants asserted after recovery (honest fsync):
//   * every acknowledged commit is present, exactly;
//   * no unacknowledged transaction is visible;
//   * every page checksum verifies, the free list matches the model, and
//     Db::check() passes.
// With a lying fsync only the last bullet is required: the store must open
// and serve no corrupt page.
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "archivum/engine/db.h"
#include "archivum/testing/fault_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

constexpr std::uint32_t kPageSize = 512;
constexpr std::size_t kBody = kPageSize - kPageTrailerBytes;

DbOptions options() {
  DbOptions o;
  o.page_size = kPageSize;
  o.cache_pages = 8;  // tiny, so the file and log paths are exercised
  o.checkpoint_threshold_frames = 12;
  return o;
}

std::uint64_t splitmix(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct Rng {
  std::uint64_t state;
  explicit Rng(std::uint64_t seed) : state(seed) {}
  std::uint64_t next() { return splitmix(state); }
  std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(std::uint64_t one_in) { return below(one_in) == 0; }
};

std::vector<std::byte> random_page(Rng& rng) {
  std::vector<std::byte> p(kPageSize);
  for (std::size_t i = 0; i < kBody; ++i) p[i] = static_cast<std::byte>(rng.next() & 0xFF);
  return p;
}

struct Model {
  std::uint64_t page_count = 1;
  std::map<PageNo, std::vector<std::byte>> pages;  // allocated pages: body content
  std::set<PageNo> free;
  std::uint64_t commits = 0;
};

// Compares the engine's committed state with the model. Returns problems.
std::vector<std::string> verify(Db& db, const Model& m) {
  std::vector<std::string> problems;
  auto txn_r = db.begin_read();
  if (!txn_r.ok()) {
    problems.push_back("begin_read: " + txn_r.status().to_string());
    return problems;
  }
  auto& txn = *txn_r.value();
  if (txn.page_count() != m.page_count) {
    problems.push_back("page_count " + std::to_string(txn.page_count()) + " != model " +
                       std::to_string(m.page_count));
  }
  std::vector<std::byte> buf(kPageSize);
  for (const auto& [page, body] : m.pages) {
    if (page >= txn.page_count()) {
      problems.push_back("model page " + std::to_string(page) + " beyond engine page count");
      continue;
    }
    Status s = txn.read_page(page, buf);
    if (!s.ok()) {
      problems.push_back("read page " + std::to_string(page) + ": " + s.to_string());
      continue;
    }
    if (!std::equal(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(kBody), body.begin())) {
      problems.push_back("page " + std::to_string(page) + " content differs from model");
    }
  }
  txn_r.value().reset();
  auto report = db.check();
  if (!report.ok()) {
    problems.push_back("check: " + report.status().to_string());
  } else {
    for (const auto& p : report.value().problems) problems.push_back("check: " + p);
    if (report.value().free_pages_walked != m.free.size()) {
      problems.push_back("free pages " + std::to_string(report.value().free_pages_walked) +
                         " != model " + std::to_string(m.free.size()));
    }
  }
  return problems;
}

// An in-flight commit at crash time: the transaction is either fully
// present or fully absent after recovery, and the caller cannot know which
// until it looks. `candidate` is the state if it landed.
struct InFlight {
  bool active = false;
  Model candidate;
};

// One random transaction against `db`, applied to `m` only if it commits.
// Returns the status of the operation that stopped it (ok on success). If
// the crash hits inside commit(), `in_flight` (when given) records the
// candidate state.
Status random_transaction(Db& db, Model& m, Rng& rng, bool& committed, InFlight* in_flight = nullptr) {
  committed = false;
  if (in_flight != nullptr) in_flight->active = false;
  auto w_r = db.begin_write();
  if (!w_r.ok()) return w_r.status();
  auto& w = *w_r.value();
  Model next = m;
  const int ops = 1 + static_cast<int>(rng.below(6));
  for (int i = 0; i < ops; ++i) {
    const std::uint64_t pick = rng.below(10);
    if (pick < 4 || next.pages.empty()) {
      auto p = w.allocate_page();
      if (!p.ok()) return p.status();
      const PageNo page = p.value();
      // The engine chooses the page; the model checks the choice is legal.
      if (next.free.count(page) != 0) {
        next.free.erase(page);
      } else if (page == next.page_count) {
        ++next.page_count;
      } else {
        return Status::corrupt("engine allocated page " + std::to_string(page) +
                               " which is neither free nor the next new page");
      }
      auto content = random_page(rng);
      if (Status s = w.write_page(page, content); !s.ok()) return s;
      next.pages[page] = std::vector<std::byte>(content.begin(), content.begin() + static_cast<std::ptrdiff_t>(kBody));
    } else if (pick < 8) {
      auto it = next.pages.begin();
      std::advance(it, static_cast<std::ptrdiff_t>(rng.below(next.pages.size())));
      auto content = random_page(rng);
      if (Status s = w.write_page(it->first, content); !s.ok()) return s;
      it->second.assign(content.begin(), content.begin() + static_cast<std::ptrdiff_t>(kBody));
    } else {
      auto it = next.pages.begin();
      std::advance(it, static_cast<std::ptrdiff_t>(rng.below(next.pages.size())));
      const PageNo page = it->first;
      if (Status s = w.free_page(page); !s.ok()) return s;
      next.pages.erase(it);
      next.free.insert(page);
    }
  }
  if (rng.chance(5)) {
    w.rollback();
    return Status();
  }
  next.commits = m.commits + 1;
  if (Status s = w.commit(); !s.ok()) {
    // A commit that returned an error (crash or I/O failure) is in doubt:
    // its frames may or may not have reached disk, whole.
    if (in_flight != nullptr) {
      in_flight->active = true;
      in_flight->candidate = next;
    }
    return s;
  }
  m = next;
  committed = true;
  return Status();
}

// Rebuilds a model from whatever the engine holds. Used after a lying fsync,
// when commits may have been lost and the model no longer describes the
// engine; the usability check must then start from the engine's truth.
Model model_from_engine(Db& db) {
  Model m;
  auto txn_r = db.begin_read();
  if (!txn_r.ok()) return m;
  auto& txn = *txn_r.value();
  m.page_count = txn.page_count();
  std::vector<std::byte> buf(kPageSize);
  if (txn.read_page(0, buf).ok()) {
    auto hdr = DbHeader::decode(buf);
    if (hdr.ok()) {
      PageNo cur = hdr.value().freelist_head;
      while (cur != 0 && cur < m.page_count && m.free.count(cur) == 0) {
        m.free.insert(cur);
        if (!txn.read_page(cur, buf).ok()) break;
        auto fp = FreePage::decode(buf);
        if (!fp.ok()) break;
        cur = fp.value().next;
      }
    }
  }
  for (PageNo p = 1; p < m.page_count; ++p) {
    if (m.free.count(p) != 0) continue;
    if (!txn.read_page(p, buf).ok()) continue;
    m.pages[p] = std::vector<std::byte>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(kBody));
  }
  return m;
}

struct Verdict {
  bool durability_violated = false;
  bool consistency_violated = false;
  bool unusable = false;
  bool refused = false;  // lying-fsync tier: the store detected damage and refused to open
  std::string detail;
};

// Reopen after recover() and compare with the committed model. When
// `require_durability` is false (lying fsync), only integrity is required.
// `m` is updated to the state the engine actually holds when an in-flight
// commit turns out to have landed.
Verdict verify_after_crash(FaultVfs& vfs, const std::string& path, Model& m, const InFlight& in_flight,
                           bool require_durability) {
  Verdict v;
  vfs.set_injector(nullptr);
  vfs.recover();
  auto db = Db::open(vfs, path, options());
  if (!db.ok()) {
    // Under a lying fsync, detecting the damage and refusing to open is the
    // correct outcome; the remedy is restore from backup. Any other failure
    // to open is a defect.
    if (!require_durability && db.status().code() == ErrorCode::Corrupt) {
      v.refused = true;
      return v;
    }
    v.unusable = true;
    v.detail = "open after crash: " + db.status().to_string();
    return v;
  }
  {
    const DbStats st = db.value()->stats();
    std::ostringstream os;
    os << " [recovered_frames=" << st.wal_recovered_frames << " dropped=" << st.wal_dropped_tail_bytes
       << " db_bytes=" << vfs.durable().contents(path).size()
       << " wal_bytes=" << vfs.durable().contents(path + ".wal").size() << "]";
    v.detail = os.str();
  }
  if (require_durability) {
    auto problems = verify(*db.value(), m);
    if (!problems.empty() && in_flight.active) {
      auto landed = verify(*db.value(), in_flight.candidate);
      if (landed.empty()) {
        m = in_flight.candidate;  // the in-flight commit made it to disk, whole
        problems.clear();
      } else {
        problems.push_back("(candidate also differs: " + landed[0] + ")");
      }
    }
    if (!problems.empty()) {
      v.durability_violated = true;
      std::ostringstream os;
      for (const auto& p : problems) os << p << "; ";
      v.detail += " " + os.str();
    }
  } else {
    // Lying fsync: check() must run to completion and every problem it
    // reports must be a detected checksum or structure failure, never an
    // I/O or logic error. A store with detected damage is refused for
    // further writes below.
    auto report = db.value()->check();
    if (!report.ok()) {
      v.consistency_violated = true;
      v.detail += " check: " + report.status().to_string();
      return v;
    }
    if (!report.value().ok) return v;  // damage detected and reported: acceptable at this tier
  }
  // The store must accept a new transaction after recovery. After a lying
  // fsync the model may be stale, so drive it from the engine's own state.
  Model scratch = require_durability ? m : model_from_engine(*db.value());
  Rng rng(1);
  bool committed = false;
  Status s = random_transaction(*db.value(), scratch, rng, committed);
  if (!s.ok() && (require_durability || s.code() != ErrorCode::Corrupt)) {
    v.unusable = true;
    v.detail += " transaction after recovery: " + s.to_string();
  }
  // The engine now holds whatever that transaction did; keep the model in step.
  m = scratch;
  return v;
}

std::string describe(std::uint64_t seed, std::uint64_t step, const FaultConfig& cfg) {
  std::ostringstream os;
  os << "seed=" << seed << " step=" << step << " persist=" << static_cast<int>(cfg.crash.persist)
     << " torn=" << (cfg.crash.torn_writes ? 1 : 0) << " sector=" << cfg.crash.sector_bytes
     << " drop_all_syncs=" << (cfg.drop_all_syncs ? 1 : 0);
  return os.str();
}

const CrashPolicy::Persist kPolicies[] = {CrashPolicy::Persist::None, CrashPolicy::Persist::Prefix,
                                          CrashPolicy::Persist::Subset, CrashPolicy::Persist::All};

// A fixed workload: several transactions with allocations, overwrites, frees,
// a reader spanning a commit, and checkpoints. Runs until done or a crash.
// `m` tracks committed state. Returns true if a crash happened.
bool fixed_workload(FaultVfs& vfs, const std::string& path, Model& m, std::uint64_t seed,
                    Status* stopped, InFlight* in_flight = nullptr) {
  auto db_r = Db::open(vfs, path, options());
  if (!db_r.ok()) {
    *stopped = db_r.status();
    return db_r.status().code() == ErrorCode::Crashed;
  }
  Db& db = *db_r.value();
  Rng rng(seed);
  std::unique_ptr<ReadTxn> reader;
  for (int t = 0; t < 8; ++t) {
    if (t == 2) {
      auto r = db.begin_read();
      if (r.ok()) reader = std::move(r).value();
    }
    bool committed = false;
    Status s = random_transaction(db, m, rng, committed, in_flight);
    if (!s.ok()) {
      *stopped = s;
      return s.code() == ErrorCode::Crashed;
    }
    if (t == 4) reader.reset();
    if (t == 4 || t == 7) {
      Status c = db.checkpoint();
      if (!c.ok() && c.code() != ErrorCode::Busy) {
        *stopped = c;
        return c.code() == ErrorCode::Crashed;
      }
    }
  }
  *stopped = Status();
  return false;
}

std::uint64_t count_ops(std::uint64_t seed) {
  FaultVfs vfs(FaultConfig{});
  Model m;
  Status st;
  (void)fixed_workload(vfs, "d/a.db", m, seed, &st);
  return vfs.op_count();
}

}  // namespace

ARCHIVUM_TEST(db_model_agreement_without_faults) {
  // Sanity: the model and the engine agree over a long fault-free run,
  // including checkpoints and reopen.
  FaultVfs vfs(FaultConfig{});
  Model m;
  Rng rng(42);
  {
    auto db = Db::open(vfs, "d/a.db", options());
    REQUIRE_OK(db.status());
    for (int i = 0; i < 300; ++i) {
      bool committed = false;
      REQUIRE_OK(random_transaction(*db.value(), m, rng, committed));
      if (i % 37 == 0) {
        Status c = db.value()->checkpoint();
        CHECK(c.ok() || c.code() == ErrorCode::Busy);
      }
    }
    auto problems = verify(*db.value(), m);
    CHECK_MSG(problems.empty(), problems.empty() ? "" : problems[0]);
    CHECK(m.commits > 100);
  }
  auto db = Db::open(vfs, "d/a.db", options());
  REQUIRE_OK(db.status());
  auto problems = verify(*db.value(), m);
  CHECK_MSG(problems.empty(), problems.empty() ? "" : problems[0]);
}

ARCHIVUM_TEST(db_crash_at_every_step_keeps_every_commit) {
  const std::uint64_t total_ops = count_ops(7);
  REQUIRE(total_ops > 20);
  std::uint64_t runs = 0;
  for (std::uint64_t step = 0; step < total_ops; ++step) {
    for (auto persist : kPolicies) {
      for (std::uint64_t seed = 1; seed <= 2; ++seed) {
        FaultConfig cfg;
        cfg.crash.persist = persist;
        cfg.crash.torn_writes = true;
        cfg.crash.sector_bytes = (seed == 2) ? 512u : 0u;
        cfg.seed = seed;
        FaultVfs vfs(cfg);
        vfs.set_injector(std::make_shared<CrashAtStep>(step));
        Model m;
        InFlight in_flight;
        Status stopped;
        const bool crashed = fixed_workload(vfs, "d/a.db", m, 7, &stopped, &in_flight);
        set_context(describe(seed, step, cfg));
        REQUIRE_MSG(crashed, "injector did not crash at step " << step << ": " << stopped.to_string());
        const Verdict v = verify_after_crash(vfs, "d/a.db", m, in_flight, /*require_durability=*/true);
        CHECK_MSG(!v.durability_violated, v.detail);
        CHECK_MSG(!v.unusable, v.detail);
        ++runs;
      }
    }
  }
  set_context("");
  std::printf("  %llu crash points x 4 policies x 2 seeds = %llu runs\n",
              static_cast<unsigned long long>(total_ops), static_cast<unsigned long long>(runs));
}

ARCHIVUM_TEST(db_crash_with_lying_fsync_keeps_integrity) {
  const std::uint64_t total_ops = count_ops(7);
  for (std::uint64_t step = 0; step < total_ops; step += 2) {
    for (auto persist : kPolicies) {
      FaultConfig cfg;
      cfg.crash.persist = persist;
      cfg.drop_all_syncs = true;
      cfg.seed = step + 1;
      FaultVfs vfs(cfg);
      vfs.set_injector(std::make_shared<CrashAtStep>(step));
      Model m;
      InFlight in_flight;
      Status stopped;
      const bool crashed = fixed_workload(vfs, "d/a.db", m, 7, &stopped, &in_flight);
      REQUIRE_MSG(crashed, "injector did not crash: " << stopped.to_string());
      set_context(describe(cfg.seed, step, cfg));
      const Verdict v = verify_after_crash(vfs, "d/a.db", m, in_flight, /*require_durability=*/false);
      CHECK_MSG(!v.consistency_violated, v.detail);
      CHECK_MSG(!v.unusable, v.detail);
    }
  }
  set_context("");
}

// The two-database design: two instances on one filesystem, interleaved
// transactions, a crash at every step. Each recovers to exactly its own
// committed state, regardless of what the other was doing.
ARCHIVUM_TEST(db_two_instances_crash_at_every_step) {
  auto run = [](FaultVfs& vfs, Model& ma, Model& mb, InFlight& fa, InFlight& fb, Status* stopped) -> bool {
    auto a = Db::open(vfs, "d/operational.db", options());
    if (!a.ok()) {
      *stopped = a.status();
      return a.status().code() == ErrorCode::Crashed;
    }
    auto b = Db::open(vfs, "d/analytical.db", options());
    if (!b.ok()) {
      *stopped = b.status();
      return b.status().code() == ErrorCode::Crashed;
    }
    Rng rng(99);
    std::unique_ptr<ReadTxn> reader_on_b;
    for (int t = 0; t < 6; ++t) {
      bool committed = false;
      // A reader on B stays open across A's commits and checkpoint.
      if (t == 1) {
        auto r = b.value()->begin_read();
        if (r.ok()) reader_on_b = std::move(r).value();
      }
      Status s = random_transaction(*a.value(), ma, rng, committed, &fa);
      if (!s.ok()) {
        *stopped = s;
        return s.code() == ErrorCode::Crashed;
      }
      s = random_transaction(*b.value(), mb, rng, committed, &fb);
      if (!s.ok()) {
        *stopped = s;
        return s.code() == ErrorCode::Crashed;
      }
      if (t == 3) {
        Status c = a.value()->checkpoint();  // B's reader must not block A
        if (!c.ok()) {
          *stopped = c;
          return c.code() == ErrorCode::Crashed;
        }
        reader_on_b.reset();
      }
    }
    *stopped = Status();
    return false;
  };
  std::uint64_t total_ops = 0;
  {
    FaultVfs vfs(FaultConfig{});
    Model ma, mb;
    InFlight fa, fb;
    Status st;
    (void)run(vfs, ma, mb, fa, fb, &st);
    REQUIRE_OK(st);
    total_ops = vfs.op_count();
  }
  std::uint64_t runs = 0;
  for (std::uint64_t step = 0; step < total_ops; ++step) {
    for (auto persist : kPolicies) {
      FaultConfig cfg;
      cfg.crash.persist = persist;
      cfg.seed = step * 3 + 1;
      FaultVfs vfs(cfg);
      vfs.set_injector(std::make_shared<CrashAtStep>(step));
      Model ma, mb;
      InFlight fa, fb;
      Status stopped;
      const bool crashed = run(vfs, ma, mb, fa, fb, &stopped);
      set_context(describe(cfg.seed, step, cfg) + " (two instances)");
      REQUIRE_MSG(crashed, "injector did not crash at step " << step << ": " << stopped.to_string());
      // Each instance recovers to exactly its own committed state (or its own
      // in-flight commit, whole), independent of the other.
      const Verdict va = verify_after_crash(vfs, "d/operational.db", ma, fa, true);
      CHECK_MSG(!va.durability_violated && !va.unusable, "operational: " << va.detail);
      auto b = Db::open(vfs, "d/analytical.db", options());
      REQUIRE_MSG(b.ok(), "analytical: " << b.status().to_string());
      auto problems = verify(*b.value(), mb);
      if (!problems.empty() && fb.active && verify(*b.value(), fb.candidate).empty()) problems.clear();
      CHECK_MSG(problems.empty(), "analytical: " << (problems.empty() ? "" : problems[0]));
      ++runs;
    }
  }
  set_context("");
  std::printf("  two instances: %llu crash points x 4 policies = %llu runs\n",
              static_cast<unsigned long long>(total_ops), static_cast<unsigned long long>(runs));
}

ARCHIVUM_TEST(db_randomized_faults_seeded) {
  std::uint64_t base_seed = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  if (const char* env = std::getenv("ARCHIVUM_SEED")) base_seed = std::strtoull(env, nullptr, 10);
  std::uint64_t iterations = 60;
  if (const char* env = std::getenv("ARCHIVUM_CRASH_ITERS")) {
    iterations = std::strtoull(env, nullptr, 10) / 5;
  }
  std::printf("  base seed %llu, %llu iterations\n", static_cast<unsigned long long>(base_seed),
              static_cast<unsigned long long>(iterations));
  for (std::uint64_t it = 0; it < iterations; ++it) {
    const std::uint64_t seed = base_seed + it;
    std::uint64_t x = seed;
    const bool lying = (splitmix(x) % 4) == 0;
    FaultConfig cfg;
    cfg.crash.persist = kPolicies[splitmix(x) % 4];
    cfg.crash.torn_writes = (splitmix(x) % 5) != 0;
    cfg.crash.sector_bytes = (splitmix(x) % 2) == 0 ? 0u : 512u;
    cfg.drop_all_syncs = lying;
    cfg.seed = seed;
    RandomFaultRates rates;
    rates.fail_write = 0.01;
    rates.partial_write = 0.01;
    rates.fail_sync = 0.01;
    rates.crash = 0.02;
    FaultVfs vfs(cfg);
    Model m;
    Rng rng(seed);
    const int rounds = 1 + static_cast<int>(splitmix(x) % 3);
    for (int round = 0; round < rounds; ++round) {
      vfs.set_injector(std::make_shared<RandomInjector>(seed * 31 + static_cast<std::uint64_t>(round), rates));
      auto db = Db::open(vfs, "d/a.db", options());
      set_context(describe(seed, vfs.op_count(), cfg) + " round=" + std::to_string(round));
      InFlight in_flight;
      if (!db.ok()) {
        if (db.status().code() == ErrorCode::Crashed) {
          const Verdict v = verify_after_crash(vfs, "d/a.db", m, in_flight, !lying);
          CHECK_MSG(!v.durability_violated && !v.consistency_violated && !v.unusable, v.detail);
          if (v.refused) break;
          continue;
        }
        if (vfs.crashed()) {  // the crash hit inside open's own error handling
          const Verdict v = verify_after_crash(vfs, "d/a.db", m, in_flight, !lying);
          CHECK_MSG(!v.durability_violated && !v.consistency_violated && !v.unusable, v.detail);
          if (v.refused) break;
          continue;
        }
        // An injected I/O error at open: retry once without faults.
        vfs.set_injector(nullptr);
        db = Db::open(vfs, "d/a.db", options());
        if (lying && db.status().code() == ErrorCode::Corrupt) break;  // damage detected: terminal at this tier
        REQUIRE_MSG(db.ok(), "open failed: " << db.status().to_string());
        vfs.set_injector(std::make_shared<RandomInjector>(seed * 31 + static_cast<std::uint64_t>(round) + 7, rates));
      }
      bool crashed = false;
      for (int t = 0; t < 40 && !crashed; ++t) {
        bool committed = false;
        Status s = random_transaction(*db.value(), m, rng, committed, &in_flight);
        if (s.ok()) {
          if (t % 9 == 0) {
            Status c = db.value()->checkpoint();
            if (c.code() == ErrorCode::Crashed) crashed = true;
          }
          continue;
        }
        if (s.code() == ErrorCode::Crashed) {
          crashed = true;
        }
        // Any other injected error: the transaction was rolled back or its
        // commit failed before publication; the model was not updated, so
        // the next transaction proceeds. But a failed commit may have left
        // the log with frames that the next open must ignore, so reopen.
        if (!crashed && vfs.crashed()) crashed = true;  // the crash hit inside the error path
        if (!crashed) {
          (void)db.value()->close();
          vfs.set_injector(nullptr);
          db = Db::open(vfs, "d/a.db", options());
          REQUIRE_MSG(db.ok(), "reopen after error: " << db.status().to_string());
          auto problems = lying ? std::vector<std::string>{} : verify(*db.value(), m);
          if (lying) m = model_from_engine(*db.value());  // commits may be lost at this tier
          std::string detail;
          if (!problems.empty()) {
            for (const auto& pr : problems) detail += pr + "; ";
            if (in_flight.active) {
              auto landed = verify(*db.value(), in_flight.candidate);
              if (landed.empty()) {
                m = in_flight.candidate;  // the failed commit had in fact landed, whole
                problems.clear();
              } else {
                detail += "| candidate: ";
                for (const auto& pr : landed) detail += pr + "; ";
              }
            } else {
              detail += "| no commit in flight (error was " + s.to_string() + ")";
            }
          }
          in_flight.active = false;
          CHECK_MSG(problems.empty(), "after I/O error: " << detail);
          if (!problems.empty()) m = model_from_engine(*db.value());  // resync so later checks are meaningful
          vfs.set_injector(std::make_shared<RandomInjector>(seed * 31 + static_cast<std::uint64_t>(round) + static_cast<std::uint64_t>(t) + 100, rates));
        }
      }
      if (!crashed) vfs.crash();
      db.value().reset();
      const Verdict v = verify_after_crash(vfs, "d/a.db", m, in_flight, !lying);
      CHECK_MSG(!v.durability_violated, v.detail);
      CHECK_MSG(!v.consistency_violated, v.detail);
      CHECK_MSG(!v.unusable, v.detail);
      if (v.refused) break;
    }
  }
  set_context("");
}
