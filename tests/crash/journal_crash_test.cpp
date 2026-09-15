// Crash-injection tests for the append-only journal. This is the Stage 0
// gate: the harness must (1) prove the journal keeps every acknowledged
// record and never serves a torn one, at every crash point under every
// persistence policy, and (2) demonstrably catch a journal that skips fsync.
//
// Reproduction: every failure prints the seed, step, and policy that
// produced it. Set ARCHIVUM_SEED to re-run the randomized test with a given
// seed and ARCHIVUM_CRASH_ITERS to change its iteration count.
#include <chrono>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "archivum/journal.h"
#include "archivum/testing/fault_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::testing;

namespace {

constexpr const char* kPath = "data/punch.jnl";

// Deterministic record content: a pure function of (seed, index).
std::uint64_t splitmix(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::vector<std::byte> payload(std::uint64_t seed, std::size_t index) {
  std::uint64_t x = seed * 1000003ull + index;
  const std::size_t length = 1 + static_cast<std::size_t>(splitmix(x) % 200);
  std::vector<std::byte> out(length);
  for (auto& b : out) b = static_cast<std::byte>(splitmix(x) & 0xFFu);
  return out;
}

const char* to_string(CrashPolicy::Persist p) {
  switch (p) {
    case CrashPolicy::Persist::None:
      return "none";
    case CrashPolicy::Persist::Prefix:
      return "prefix";
    case CrashPolicy::Persist::Subset:
      return "subset";
    case CrashPolicy::Persist::All:
      return "all";
  }
  return "?";
}

struct WorkloadResult {
  std::size_t committed_max = 0;  // highest count() observed after an ok append
  std::size_t attempted_max = 0;  // highest index ever handed to append()
  bool crashed = false;
  bool open_failed = false;
};

// Opens the journal and appends until `target` records exist or a fault
// stops it. On an I/O error the journal is reopened (recovery) and the
// workload continues, as the Punchline client would.
WorkloadResult run_workload(FaultVfs& vfs, std::uint64_t seed, std::size_t target,
                            Journal::Options options, WorkloadResult carry = {}) {
  WorkloadResult r = carry;
  r.crashed = false;
  r.open_failed = false;
  int reopens = 0;
  while (true) {
    auto opened = Journal::open(vfs, kPath, options);
    if (!opened.ok()) {
      if (opened.status().code() == ErrorCode::Crashed) {
        r.crashed = true;
        return r;
      }
      // An I/O error while opening (e.g. an injected failure of the header
      // write or directory sync) is retried, as the client would.
      if (++reopens > 1000) {
        r.open_failed = true;
        return r;
      }
      continue;
    }
    auto& j = *opened.value();
    bool need_reopen = false;
    while (j.count() < target) {
      const std::size_t index = j.count();
      if (index + 1 > r.attempted_max) r.attempted_max = index + 1;
      Status s = j.append(payload(seed, index));
      if (s.ok()) {
        if (j.count() > r.committed_max) r.committed_max = j.count();
        continue;
      }
      if (s.code() == ErrorCode::Crashed) {
        r.crashed = true;
        return r;
      }
      need_reopen = true;  // IoError: recover by reopening
      break;
    }
    if (!need_reopen) return r;
    if (++reopens > 1000) {
      r.open_failed = true;  // the injector is failing everything; give up
      return r;
    }
  }
}

struct Verdict {
  bool durability_violated = false;  // an acknowledged record is missing
  bool prefix_violated = false;      // a record is wrong, torn, or beyond what was attempted
  bool unusable = false;             // the journal cannot be opened or appended to after recovery
  std::size_t recovered = 0;
  std::string detail;
};

// After recover(): the journal must open, hold a correct prefix, contain
// every acknowledged record when `require_durability`, and accept new
// appends.
Verdict verify(FaultVfs& vfs, std::uint64_t seed, const WorkloadResult& w, bool require_durability) {
  Verdict v;
  std::ostringstream detail;
  vfs.set_injector(nullptr);
  vfs.recover();
  auto opened = Journal::open(vfs, kPath);
  if (!opened.ok()) {
    v.unusable = true;
    v.detail = "open after recovery failed: " + opened.status().to_string();
    return v;
  }
  auto& j = *opened.value();
  v.recovered = j.count();
  if (j.count() > w.attempted_max) {
    v.prefix_violated = true;
    detail << "recovered " << j.count() << " records but only " << w.attempted_max
           << " were ever attempted; ";
  }
  if (require_durability && j.count() < w.committed_max) {
    v.durability_violated = true;
    detail << "recovered " << j.count() << " records but " << w.committed_max
           << " were acknowledged; ";
  }
  for (std::size_t i = 0; i < j.count(); ++i) {
    auto rec = j.read(i);
    if (!rec.ok() || rec.value() != payload(seed, i)) {
      v.prefix_violated = true;
      detail << "record " << i << " does not match what was appended; ";
      break;
    }
  }
  // The recovered journal must be usable.
  const std::size_t before = j.count();
  for (int k = 0; k < 3; ++k) {
    Status s = j.append(payload(seed, j.count()));
    if (!s.ok()) {
      v.unusable = true;
      detail << "append after recovery failed: " << s.to_string() << "; ";
      break;
    }
  }
  if (!v.unusable && j.count() != before + 3) {
    v.unusable = true;
    detail << "append after recovery did not advance count; ";
  }
  v.detail = detail.str();
  return v;
}

std::string describe(std::uint64_t seed, std::uint64_t step, const FaultConfig& cfg) {
  std::ostringstream os;
  os << "seed=" << seed << " step=" << step << " persist=" << to_string(cfg.crash.persist)
     << " torn=" << (cfg.crash.torn_writes ? 1 : 0) << " sector=" << cfg.crash.sector_bytes
     << " drop_all_syncs=" << (cfg.drop_all_syncs ? 1 : 0);
  return os.str();
}

std::uint64_t count_ops(std::size_t records, Journal::Options options) {
  FaultVfs vfs(FaultConfig{});
  (void)run_workload(vfs, 1, records, options);
  return vfs.op_count();
}

const CrashPolicy::Persist kPolicies[] = {CrashPolicy::Persist::None, CrashPolicy::Persist::Prefix,
                                          CrashPolicy::Persist::Subset, CrashPolicy::Persist::All};

}  // namespace

// Crash during every operation of the workload, under every persistence
// policy, with several seeds for the random choices inside each policy.
// A correct journal never loses an acknowledged record and never serves a
// torn one.
ARCHIVUM_TEST(crash_at_every_step_keeps_every_acknowledged_record) {
  constexpr std::size_t kRecords = 12;
  const std::uint64_t total_ops = count_ops(kRecords, {});
  REQUIRE(total_ops > 2 * kRecords);
  std::uint64_t runs = 0;
  for (std::uint64_t step = 0; step < total_ops; ++step) {
    for (auto persist : kPolicies) {
      for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        for (std::uint32_t sector : {0u, 512u}) {
          FaultConfig cfg;
          cfg.crash.persist = persist;
          cfg.crash.torn_writes = true;
          cfg.crash.sector_bytes = sector;
          cfg.seed = seed;
          FaultVfs vfs(cfg);
          vfs.set_injector(std::make_shared<CrashAtStep>(step));
          const WorkloadResult w = run_workload(vfs, seed, kRecords, {});
          set_context(describe(seed, step, cfg));
          REQUIRE_MSG(w.crashed, "the injector did not crash at step " << step);
          const Verdict v = verify(vfs, seed, w, /*require_durability=*/true);
          CHECK_MSG(!v.durability_violated, v.detail);
          CHECK_MSG(!v.prefix_violated, v.detail);
          CHECK_MSG(!v.unusable, v.detail);
          ++runs;
        }
      }
    }
  }
  set_context("");
  std::printf("  %llu crash points x policies x seeds = %llu runs\n",
              static_cast<unsigned long long>(total_ops), static_cast<unsigned long long>(runs));
}

// A second crash during recovery-and-continue must also be survivable.
ARCHIVUM_TEST(crash_twice_including_during_recovery) {
  constexpr std::size_t kRecords = 8;
  const std::uint64_t total_ops = count_ops(kRecords, {});
  for (std::uint64_t first = 0; first < total_ops; first += 3) {
    for (std::uint64_t second = 0; second < 12; ++second) {
      for (auto persist : {CrashPolicy::Persist::Prefix, CrashPolicy::Persist::Subset}) {
        FaultConfig cfg;
        cfg.crash.persist = persist;
        cfg.seed = first * 131 + second;
        FaultVfs vfs(cfg);
        vfs.set_injector(std::make_shared<CrashAtStep>(first));
        WorkloadResult w = run_workload(vfs, cfg.seed, kRecords, {});
        REQUIRE(w.crashed);
        vfs.recover();
        // Crash again `second` ops into the recovery-and-continue.
        vfs.set_injector(std::make_shared<CrashAtStep>(vfs.op_count() + second));
        w = run_workload(vfs, cfg.seed, kRecords * 2, {}, w);
        set_context(describe(cfg.seed, second, cfg) + " (second crash)");
        if (!w.crashed) {
          // The workload finished before the second crash point; that is fine.
        }
        const Verdict v = verify(vfs, cfg.seed, w, /*require_durability=*/true);
        CHECK_MSG(!v.durability_violated, v.detail);
        CHECK_MSG(!v.prefix_violated, v.detail);
        CHECK_MSG(!v.unusable, v.detail);
      }
    }
  }
  set_context("");
}

// With an fsync that lies, durability cannot be promised, but consistency
// still must be: whatever is recovered is a correct prefix and the journal
// remains usable.
ARCHIVUM_TEST(crash_with_lying_fsync_keeps_consistency) {
  constexpr std::size_t kRecords = 10;
  const std::uint64_t total_ops = count_ops(kRecords, {});
  for (std::uint64_t step = 0; step < total_ops; ++step) {
    for (auto persist : kPolicies) {
      FaultConfig cfg;
      cfg.crash.persist = persist;
      cfg.drop_all_syncs = true;
      cfg.seed = step + 1;
      FaultVfs vfs(cfg);
      vfs.set_injector(std::make_shared<CrashAtStep>(step));
      const WorkloadResult w = run_workload(vfs, cfg.seed, kRecords, {});
      REQUIRE(w.crashed);
      set_context(describe(cfg.seed, step, cfg));
      const Verdict v = verify(vfs, cfg.seed, w, /*require_durability=*/false);
      CHECK_MSG(!v.prefix_violated, v.detail);
      CHECK_MSG(!v.unusable, v.detail);
    }
  }
  set_context("");
}

// Proof that the harness has teeth: a journal that skips fsync must be
// caught losing an acknowledged record at some crash point.
ARCHIVUM_TEST(harness_catches_journal_without_fsync) {
  constexpr std::size_t kRecords = 10;
  Journal::Options unsafe;
  unsafe.unsafe_skip_sync = true;
  const std::uint64_t total_ops = count_ops(kRecords, unsafe);
  std::uint64_t violations = 0;
  for (std::uint64_t step = 0; step < total_ops; ++step) {
    for (auto persist : {CrashPolicy::Persist::None, CrashPolicy::Persist::Prefix,
                         CrashPolicy::Persist::Subset}) {
      FaultConfig cfg;
      cfg.crash.persist = persist;
      cfg.seed = step + 1;
      FaultVfs vfs(cfg);
      vfs.set_injector(std::make_shared<CrashAtStep>(step));
      const WorkloadResult w = run_workload(vfs, cfg.seed, kRecords, unsafe);
      REQUIRE(w.crashed);
      const Verdict v = verify(vfs, cfg.seed, w, /*require_durability=*/true);
      if (v.durability_violated) ++violations;
      // Even the unsafe journal must never serve a torn record.
      set_context(describe(cfg.seed, step, cfg) + " (unsafe journal)");
      CHECK_MSG(!v.prefix_violated, v.detail);
      CHECK_MSG(!v.unusable, v.detail);
    }
  }
  set_context("");
  std::printf("  harness caught %llu lost-commit outcomes for the fsync-free journal\n",
              static_cast<unsigned long long>(violations));
  CHECK_MSG(violations > 0, "the harness failed to detect a journal that never calls fsync");
}

// Randomized: random faults of every kind, multiple crashes per run, seed
// printed on failure. Honest fsync: durability required. Lying fsync:
// consistency only.
ARCHIVUM_TEST(randomized_faults_seeded) {
  std::uint64_t base_seed = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  if (const char* env = std::getenv("ARCHIVUM_SEED")) base_seed = std::strtoull(env, nullptr, 10);
  std::uint64_t iterations = 300;
  if (const char* env = std::getenv("ARCHIVUM_CRASH_ITERS")) {
    iterations = std::strtoull(env, nullptr, 10);
  }
  std::printf("  base seed %llu, %llu iterations (ARCHIVUM_SEED / ARCHIVUM_CRASH_ITERS)\n",
              static_cast<unsigned long long>(base_seed),
              static_cast<unsigned long long>(iterations));

  for (std::uint64_t it = 0; it < iterations; ++it) {
    const std::uint64_t seed = base_seed + it;
    std::uint64_t x = seed;
    const bool lying_fsync = (splitmix(x) % 4) == 0;
    FaultConfig cfg;
    cfg.crash.persist = kPolicies[splitmix(x) % 4];
    cfg.crash.torn_writes = (splitmix(x) % 5) != 0;
    cfg.crash.sector_bytes = (splitmix(x) % 2) == 0 ? 0u : 512u;
    cfg.drop_all_syncs = lying_fsync;
    cfg.seed = seed;

    RandomFaultRates rates;
    rates.fail_write = 0.03;
    rates.partial_write = 0.03;
    rates.fail_sync = 0.02;
    rates.drop_sync = lying_fsync ? 0.0 : 0.0;  // dropped syncs are modelled by drop_all_syncs
    rates.crash = 0.04;

    FaultVfs vfs(cfg);
    WorkloadResult w;
    const int rounds = 1 + static_cast<int>(splitmix(x) % 4);
    for (int round = 0; round < rounds; ++round) {
      vfs.set_injector(std::make_shared<RandomInjector>(seed * 7919 + static_cast<std::uint64_t>(round), rates));
      w = run_workload(vfs, seed, 30 * static_cast<std::size_t>(round + 1), {}, w);
      set_context(describe(seed, vfs.op_count(), cfg) + " round=" + std::to_string(round));
      REQUIRE_MSG(!w.open_failed, "workload could not proceed");
      if (!w.crashed) vfs.crash();  // end the round with a crash regardless
      const Verdict v = verify(vfs, seed, w, /*require_durability=*/!lying_fsync);
      CHECK_MSG(!v.durability_violated, v.detail);
      CHECK_MSG(!v.prefix_violated, v.detail);
      CHECK_MSG(!v.unusable, v.detail);
      // verify() appended three records; carry that into the next round.
      if (v.recovered + 3 > w.committed_max) w.committed_max = v.recovered + 3;
      if (v.recovered + 3 > w.attempted_max) w.attempted_max = v.recovered + 3;
    }
  }
  set_context("");
}
