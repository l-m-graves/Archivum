// Tests of the crash shim itself: each fault it claims to inject must be
// observable, and each durability rule it models must hold.
#include "archivum/testing/fault_vfs.h"

#include <memory>
#include <vector>

#include "test.h"

using namespace archivum;
using namespace archivum::testing;

namespace {

std::vector<std::byte> bytes(const char* s) {
  std::vector<std::byte> out;
  for (const char* p = s; *p != '\0'; ++p) out.push_back(static_cast<std::byte>(*p));
  return out;
}

std::unique_ptr<File> create(Vfs& vfs, const std::string& path) {
  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  auto f = vfs.open(path, flags);
  REQUIRE_OK(f.status());
  return std::move(f).value();
}

FaultConfig config(CrashPolicy::Persist persist, bool torn = true, std::uint64_t seed = 7) {
  FaultConfig c;
  c.crash.persist = persist;
  c.crash.torn_writes = torn;
  c.seed = seed;
  return c;
}

}  // namespace

ARCHIVUM_TEST(fault_unsynced_write_is_lost_on_crash) {
  FaultVfs vfs(config(CrashPolicy::Persist::None));
  auto f = create(vfs, "j");
  REQUIRE_OK(f->write(0, bytes("committed?")));
  CHECK(vfs.cache().contents("j") == bytes("committed?"));  // visible to the process
  CHECK(!vfs.durable().has("j"));                           // not on disk
  vfs.crash();
  CHECK(f->write(0, bytes("x")).code() == ErrorCode::Crashed);
  vfs.recover();
  CHECK(!vfs.cache().has("j"));  // after reboot the file never existed
}

ARCHIVUM_TEST(fault_synced_write_survives_crash) {
  FaultVfs vfs(config(CrashPolicy::Persist::None));
  auto f = create(vfs, "j");
  REQUIRE_OK(f->write(0, bytes("durable")));
  REQUIRE_OK(f->sync());
  CHECK(vfs.durable().contents("j") == bytes("durable"));
  REQUIRE_OK(f->write(7, bytes(" and more")));  // not synced
  vfs.crash();
  vfs.recover();
  CHECK(vfs.cache().contents("j") == bytes("durable"));
}

ARCHIVUM_TEST(fault_dropped_fsync_persists_nothing) {
  FaultConfig c = config(CrashPolicy::Persist::None);
  c.drop_all_syncs = true;
  FaultVfs vfs(c);
  auto f = create(vfs, "j");
  REQUIRE_OK(f->write(0, bytes("lie")));
  REQUIRE_OK(f->sync());  // reports success
  CHECK(!vfs.durable().has("j"));
  vfs.crash();
  vfs.recover();
  CHECK(!vfs.cache().has("j"));
}

ARCHIVUM_TEST(fault_prefix_persists_in_order_and_may_tear) {
  // With Persist::Prefix, whatever lands is a prefix of the pending ops, and
  // only the last write may be torn. Try many seeds; every outcome must be a
  // prefix of "AAAABBBBCCCC" at the byte level.
  bool saw_torn = false;
  bool saw_partial_prefix = false;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    FaultVfs vfs(config(CrashPolicy::Persist::Prefix, true, seed));
    auto f = create(vfs, "j");
    REQUIRE_OK(f->sync());  // the create is durable
    REQUIRE_OK(f->write(0, bytes("AAAA")));
    REQUIRE_OK(f->write(4, bytes("BBBB")));
    REQUIRE_OK(f->write(8, bytes("CCCC")));
    vfs.crash();
    const auto got = vfs.durable().contents("j");
    const auto full = bytes("AAAABBBBCCCC");
    REQUIRE(got.size() <= full.size());
    CHECK(std::equal(got.begin(), got.end(), full.begin()));
    if (got.size() % 4 != 0) saw_torn = true;
    if (got.size() < 12 && got.size() > 0) saw_partial_prefix = true;
  }
  CHECK(saw_torn);
  CHECK(saw_partial_prefix);
}

ARCHIVUM_TEST(fault_subset_reorders_writes) {
  // Two overlapping writes without a sync: with Persist::Subset the older one
  // may land after the newer one. Assert that both orders are produced.
  bool saw_old_wins = false;
  bool saw_new_wins = false;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    FaultVfs vfs(config(CrashPolicy::Persist::Subset, false, seed));
    auto f = create(vfs, "j");
    REQUIRE_OK(f->sync());
    REQUIRE_OK(f->write(0, bytes("old")));
    REQUIRE_OK(f->write(0, bytes("new")));
    vfs.crash();
    const auto got = vfs.durable().contents("j");
    if (got == bytes("old")) saw_old_wins = true;
    if (got == bytes("new")) saw_new_wins = true;
  }
  CHECK(saw_old_wins);
  CHECK(saw_new_wins);
}

ARCHIVUM_TEST(fault_sector_granular_tear) {
  FaultConfig c = config(CrashPolicy::Persist::All, true);
  c.crash.sector_bytes = 4;
  // Persist::All never tears; use Prefix with a single write so the tear
  // lands on it, and check the cut is sector aligned across seeds.
  c.crash.persist = CrashPolicy::Persist::Prefix;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    c.seed = seed;
    FaultVfs vfs(c);
    auto f = create(vfs, "j");
    REQUIRE_OK(f->sync());
    REQUIRE_OK(f->write(0, bytes("0123456789ABCDEF")));
    vfs.crash();
    CHECK_MSG(vfs.durable().contents("j").size() % 4 == 0, "seed " << seed);
  }
}

ARCHIVUM_TEST(fault_injected_write_failures) {
  FaultVfs vfs(config(CrashPolicy::Persist::All));
  auto f = create(vfs, "j");
  struct Script final : FaultInjector {
    Decision decide(const OpInfo& op) override {
      if (op.kind != OpKind::Write) return Decision::proceed();
      if (op.offset == 0) return Decision::fail_write();
      if (op.offset == 10) return Decision::partial_write(2);
      return Decision::proceed();
    }
  };
  vfs.set_injector(std::make_shared<Script>());
  CHECK(f->write(0, bytes("nope")).code() == ErrorCode::IoError);
  CHECK(!vfs.cache().has("j") || vfs.cache().contents("j").empty());
  CHECK(f->write(10, bytes("half")).code() == ErrorCode::IoError);
  auto c = vfs.cache().contents("j");
  REQUIRE(c.size() == 12);
  CHECK(c[10] == std::byte{'h'} && c[11] == std::byte{'a'});
  REQUIRE_OK(f->write(20, bytes("ok")));
}

ARCHIVUM_TEST(fault_crash_at_step_kills_handles) {
  FaultVfs vfs(config(CrashPolicy::Persist::All));
  vfs.set_injector(std::make_shared<CrashAtStep>(2));  // ops: 0 open, 1 write, 2 write
  auto f = create(vfs, "j");
  REQUIRE_OK(f->write(0, bytes("a")));
  CHECK(f->write(1, bytes("b")).code() == ErrorCode::Crashed);
  CHECK(vfs.crashed());
  CHECK(f->sync().code() == ErrorCode::Crashed);
  CHECK(vfs.open("j", OpenFlags{}).status().code() == ErrorCode::Crashed);
  vfs.recover();
  CHECK(f->sync().code() == ErrorCode::Crashed);  // old handle stays dead after reboot
  CHECK(vfs.cache().contents("j") == bytes("ab"));  // Persist::All flushed the in-flight write
}

ARCHIVUM_TEST(fault_directory_ops_need_directory_sync) {
  FaultVfs vfs(config(CrashPolicy::Persist::None));
  {
    auto f = create(vfs, "a");
    REQUIRE_OK(f->write(0, bytes("A")));
    REQUIRE_OK(f->sync());
  }
  REQUIRE_OK(vfs.rename("a", "b"));
  REQUIRE_OK(vfs.remove("b"));
  // No directory sync: after a crash the rename and remove never happened.
  vfs.crash();
  vfs.recover();
  CHECK(vfs.cache().contents("a") == bytes("A"));
  CHECK(!vfs.cache().has("b"));

  REQUIRE_OK(vfs.rename("a", "b"));
  REQUIRE_OK(vfs.sync_directory("."));
  vfs.crash();
  vfs.recover();
  CHECK(!vfs.cache().has("a"));
  CHECK(vfs.cache().contents("b") == bytes("A"));
}

ARCHIVUM_TEST(fault_random_injector_is_deterministic) {
  RandomFaultRates rates;
  rates.fail_write = 0.2;
  rates.partial_write = 0.2;
  rates.drop_sync = 0.2;
  rates.crash = 0.1;
  std::vector<Decision::Kind> a, b;
  for (int pass = 0; pass < 2; ++pass) {
    RandomInjector inj(42, rates);
    for (std::uint64_t i = 0; i < 500; ++i) {
      OpInfo op;
      op.index = i;
      op.kind = (i % 2 == 0) ? OpKind::Write : OpKind::Sync;
      op.length = 100;
      (pass == 0 ? a : b).push_back(inj.decide(op).kind);
    }
  }
  CHECK(a == b);
  bool any_fault = false;
  for (auto k : a) any_fault = any_fault || k != Decision::Kind::Proceed;
  CHECK(any_fault);
}
