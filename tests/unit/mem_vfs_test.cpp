#include "archivum/testing/mem_vfs.h"

#include <vector>

#include "test.h"

using archivum::OpenFlags;
using archivum::testing::MemVfs;

namespace {
std::vector<std::byte> bytes(const char* s) {
  std::vector<std::byte> out;
  for (const char* p = s; *p != '\0'; ++p) out.push_back(static_cast<std::byte>(*p));
  return out;
}
}  // namespace

ARCHIVUM_TEST(mem_vfs_basic) {
  MemVfs vfs;
  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  auto f = vfs.open("x", flags);
  REQUIRE_OK(f.status());
  REQUIRE_OK(f.value()->write(2, bytes("ab")));
  const std::vector<std::byte> expected{std::byte{0}, std::byte{0}, std::byte{'a'}, std::byte{'b'}};
  CHECK(vfs.contents("x") == expected);
  std::vector<std::byte> buf(10);
  std::size_t got = 0;
  REQUIRE_OK(f.value()->read(3, buf, got));
  CHECK(got == 1 && buf[0] == std::byte{'b'});
  REQUIRE_OK(f.value()->truncate(1));
  CHECK(vfs.contents("x").size() == 1);
  REQUIRE_OK(vfs.rename("x", "y"));
  CHECK(!vfs.has("x") && vfs.has("y"));
  REQUIRE_OK(vfs.remove("y"));
  CHECK(vfs.paths().empty());
  CHECK(vfs.open("y", OpenFlags{}).status().code() == archivum::ErrorCode::NotFound);
}

ARCHIVUM_TEST(mem_vfs_clone_is_deep) {
  MemVfs a;
  a.put("f", bytes("one"));
  MemVfs b;
  b.clone_from(a);
  a.put("f", bytes("two"));
  CHECK(b.contents("f") == bytes("one"));
}
