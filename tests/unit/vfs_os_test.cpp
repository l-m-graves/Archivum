// Smoke tests for the real operating-system VFS. These run on Windows and
// Linux in CI and are the only tests that touch a real disk.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "archivum/vfs.h"
#include "test.h"

using archivum::OpenFlags;
using archivum::Status;

namespace {

std::string scratch_dir() {
  const auto dir = std::filesystem::temp_directory_path() / "archivum-vfs-test";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::string join(const std::string& dir, const char* name) {
  return (std::filesystem::path(dir) / name).string();
}

std::vector<std::byte> bytes(const char* s) {
  std::vector<std::byte> out;
  for (const char* p = s; *p != '\0'; ++p) out.push_back(static_cast<std::byte>(*p));
  return out;
}

}  // namespace

ARCHIVUM_TEST(os_vfs_create_write_read_sync) {
  auto vfs = archivum::make_os_vfs();
  const std::string dir = scratch_dir();
  const std::string path = join(dir, "a.bin");
  (void)vfs->remove(path);

  auto exists = vfs->exists(path);
  REQUIRE_OK(exists.status());
  CHECK(!exists.value());

  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  auto f = vfs->open(path, flags);
  REQUIRE_OK(f.status());
  auto& file = *f.value();

  const auto hello = bytes("hello");
  REQUIRE_OK(file.write(0, hello));
  REQUIRE_OK(file.write(5, bytes(" world")));
  REQUIRE_OK(file.sync());
  REQUIRE_OK(vfs->sync_directory(dir));

  auto size = file.size();
  REQUIRE_OK(size.status());
  CHECK(size.value() == 11);

  std::vector<std::byte> buf(64);
  std::size_t got = 0;
  REQUIRE_OK(file.read(0, buf, got));
  CHECK(got == 11);
  CHECK(std::vector<std::byte>(buf.begin(), buf.begin() + 11) == bytes("hello world"));

  // Short read at end of file, and read past end.
  REQUIRE_OK(file.read(6, buf, got));
  CHECK(got == 5);
  REQUIRE_OK(file.read(100, buf, got));
  CHECK(got == 0);

  // Truncate down, then extend (zero-fill).
  REQUIRE_OK(file.truncate(5));
  size = file.size();
  REQUIRE_OK(size.status());
  CHECK(size.value() == 5);
  REQUIRE_OK(file.truncate(8));
  REQUIRE_OK(file.read(0, buf, got));
  CHECK(got == 8);
  CHECK(buf[5] == std::byte{0} && buf[7] == std::byte{0});
  REQUIRE_OK(file.close());

  // Reopen read-only and verify persistence.
  auto ro = vfs->open(path, OpenFlags{});
  REQUIRE_OK(ro.status());
  REQUIRE_OK(ro.value()->read(0, buf, got));
  CHECK(got == 8);
  CHECK(std::vector<std::byte>(buf.begin(), buf.begin() + 5) == hello);
  REQUIRE_OK(ro.value()->close());

  REQUIRE_OK(vfs->remove(path));
  exists = vfs->exists(path);
  REQUIRE_OK(exists.status());
  CHECK(!exists.value());
}

ARCHIVUM_TEST(os_vfs_open_errors) {
  auto vfs = archivum::make_os_vfs();
  const std::string dir = scratch_dir();
  const std::string path = join(dir, "missing.bin");
  (void)vfs->remove(path);

  auto r = vfs->open(path, OpenFlags{});
  CHECK(!r.ok());
  CHECK(r.status().code() == archivum::ErrorCode::NotFound);

  OpenFlags excl;
  excl.write = true;
  excl.create = true;
  excl.exclusive = true;
  auto first = vfs->open(path, excl);
  REQUIRE_OK(first.status());
  REQUIRE_OK(first.value()->close());
  auto second = vfs->open(path, excl);
  CHECK(!second.ok());
  CHECK(second.status().code() == archivum::ErrorCode::AlreadyExists);

  OpenFlags bad;
  bad.create = true;  // without write
  auto invalid = vfs->open(path, bad);
  CHECK(invalid.status().code() == archivum::ErrorCode::InvalidArgument);

  CHECK(vfs->remove(path).ok());
  CHECK(vfs->remove(path).code() == archivum::ErrorCode::NotFound);
}

ARCHIVUM_TEST(os_vfs_rename_replaces) {
  auto vfs = archivum::make_os_vfs();
  const std::string dir = scratch_dir();
  const std::string a = join(dir, "r-a.bin");
  const std::string b = join(dir, "r-b.bin");
  (void)vfs->remove(a);
  (void)vfs->remove(b);

  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  for (const auto& [path, content] : {std::pair{a, "AAA"}, std::pair{b, "B"}}) {
    auto f = vfs->open(path, flags);
    REQUIRE_OK(f.status());
    REQUIRE_OK(f.value()->write(0, bytes(content)));
    REQUIRE_OK(f.value()->sync());
    REQUIRE_OK(f.value()->close());
  }
  REQUIRE_OK(vfs->rename(a, b));
  REQUIRE_OK(vfs->sync_directory(dir));
  auto ea = vfs->exists(a);
  REQUIRE_OK(ea.status());
  CHECK(!ea.value());
  auto f = vfs->open(b, OpenFlags{});
  REQUIRE_OK(f.status());
  std::vector<std::byte> buf(8);
  std::size_t got = 0;
  REQUIRE_OK(f.value()->read(0, buf, got));
  CHECK(got == 3);
  REQUIRE_OK(f.value()->close());
  REQUIRE_OK(vfs->remove(b));
}
