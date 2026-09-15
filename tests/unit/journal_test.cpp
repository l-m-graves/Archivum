#include "archivum/journal.h"

#include <vector>

#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using archivum::testing::MemVfs;

namespace {
std::vector<std::byte> bytes(const char* s) {
  std::vector<std::byte> out;
  for (const char* p = s; *p != '\0'; ++p) out.push_back(static_cast<std::byte>(*p));
  return out;
}
}  // namespace

ARCHIVUM_TEST(journal_create_append_reopen) {
  MemVfs vfs;
  {
    auto j = Journal::open(vfs, "dir/j.log");
    REQUIRE_OK(j.status());
    CHECK(j.value()->count() == 0);
    CHECK(j.value()->end_offset() == Journal::kHeaderBytes);
    REQUIRE_OK(j.value()->append(bytes("one")));
    REQUIRE_OK(j.value()->append(bytes("two")));
    CHECK(j.value()->count() == 2);
    auto r = j.value()->read(1);
    REQUIRE_OK(r.status());
    CHECK(r.value() == bytes("two"));
    CHECK(j.value()->read(2).status().code() == ErrorCode::NotFound);
  }
  auto j = Journal::open(vfs, "dir/j.log");
  REQUIRE_OK(j.status());
  CHECK(j.value()->count() == 2);
  CHECK(j.value()->dropped_tail_bytes() == 0);
  auto r = j.value()->read(0);
  REQUIRE_OK(r.status());
  CHECK(r.value() == bytes("one"));
}

ARCHIVUM_TEST(journal_rejects_bad_records) {
  MemVfs vfs;
  auto j = Journal::open(vfs, "j");
  REQUIRE_OK(j.status());
  std::vector<std::byte> empty;
  CHECK(j.value()->append(empty).code() == ErrorCode::InvalidArgument);
  Journal::Options small;
  small.max_record_bytes = 4;
  auto k = Journal::open(vfs, "k", small);
  REQUIRE_OK(k.status());
  CHECK(k.value()->append(bytes("12345")).code() == ErrorCode::InvalidArgument);
  REQUIRE_OK(k.value()->append(bytes("1234")));
}

ARCHIVUM_TEST(journal_truncates_torn_tail) {
  MemVfs vfs;
  {
    auto j = Journal::open(vfs, "j");
    REQUIRE_OK(j.status());
    REQUIRE_OK(j.value()->append(bytes("alpha")));
    REQUIRE_OK(j.value()->append(bytes("beta")));
  }
  // Chop the last record in half: header intact, payload torn.
  auto raw = vfs.contents("j");
  raw.resize(raw.size() - 2);
  vfs.put("j", raw);
  {
    auto j = Journal::open(vfs, "j");
    REQUIRE_OK(j.status());
    CHECK(j.value()->count() == 1);
    CHECK(j.value()->dropped_tail_bytes() == 8 + 4 - 2);
    CHECK(vfs.contents("j").size() == Journal::kHeaderBytes + 8 + 5);
    REQUIRE_OK(j.value()->append(bytes("gamma")));
  }
  auto j = Journal::open(vfs, "j");
  REQUIRE_OK(j.status());
  REQUIRE(j.value()->count() == 2);
  CHECK(j.value()->read(1).value() == bytes("gamma"));
}

ARCHIVUM_TEST(journal_detects_corrupted_payload) {
  MemVfs vfs;
  {
    auto j = Journal::open(vfs, "j");
    REQUIRE_OK(j.status());
    REQUIRE_OK(j.value()->append(bytes("alpha")));
    REQUIRE_OK(j.value()->append(bytes("beta")));
  }
  auto raw = vfs.contents("j");
  raw[Journal::kHeaderBytes + 8 + 1] ^= std::byte{0x01};  // flip a bit in "alpha"
  vfs.put("j", raw);
  auto j = Journal::open(vfs, "j");
  REQUIRE_OK(j.status());
  // The corrupt first record ends the valid prefix; "beta" behind it is gone.
  CHECK(j.value()->count() == 0);
}

ARCHIVUM_TEST(journal_torn_header_is_treated_as_new) {
  MemVfs vfs;
  vfs.put("j", std::vector<std::byte>(5, std::byte{'A'}));
  auto j = Journal::open(vfs, "j");
  REQUIRE_OK(j.status());
  CHECK(j.value()->count() == 0);
  CHECK(j.value()->header_rewritten());
  REQUIRE_OK(j.value()->append(bytes("x")));
}

ARCHIVUM_TEST(journal_bad_header_is_rewritten_and_records_behind_it_survive) {
  MemVfs vfs;
  {
    auto j = Journal::open(vfs, "j");
    REQUIRE_OK(j.status());
    REQUIRE_OK(j.value()->append(bytes("alpha")));
    REQUIRE_OK(j.value()->append(bytes("beta")));
  }
  // Lose the header (as a reordered, unsynced write would) but keep the records.
  auto raw = vfs.contents("j");
  for (std::size_t i = 0; i < Journal::kHeaderBytes; ++i) raw[i] = std::byte{0};
  vfs.put("j", raw);
  auto j = Journal::open(vfs, "j");
  REQUIRE_OK(j.status());
  CHECK(j.value()->header_rewritten());
  REQUIRE(j.value()->count() == 2);
  CHECK(j.value()->read(0).value() == bytes("alpha"));
  CHECK(j.value()->read(1).value() == bytes("beta"));

  // A file of garbage is reinitialised: the journal owns its path.
  vfs.put("g", std::vector<std::byte>(100, std::byte{'A'}));
  auto g = Journal::open(vfs, "g");
  REQUIRE_OK(g.status());
  CHECK(g.value()->count() == 0);
  CHECK(g.value()->header_rewritten());
  CHECK(vfs.contents("g").size() == Journal::kHeaderBytes);
}
