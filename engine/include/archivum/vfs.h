// The virtual filesystem boundary.
//
// Everything the engine does to disk goes through Vfs and File. Production
// code uses the operating-system implementation from make_os_vfs(). Tests use
// archivum::testing::MemVfs (fast, in memory) and archivum::testing::FaultVfs
// (the crash-injection shim). The engine never includes <windows.h> or
// <unistd.h> outside the vfs_*.cpp files.
//
// Semantics every implementation must honour (see docs/testing/fault-model.md):
//   * read()  may return fewer bytes than requested only at end of file.
//   * write() either writes every byte or returns a non-ok Status. A non-ok
//             Status makes no promise about how many bytes reached the OS.
//   * sync()  returns ok only when every prior write() and truncate() on this
//             file is durable on the underlying device, as far as the OS can
//             know. A file created in this session is also durable in its
//             directory after sync() on the target filesystems (NTFS, ext4).
//   * sync_directory() makes pending remove() and rename() durable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "archivum/status.h"

namespace archivum {

struct OpenFlags {
  bool write = false;      // open for reading and writing; false means read-only
  bool create = false;     // create the file if it does not exist (requires write)
  bool exclusive = false;  // with create: fail if the file already exists
  bool truncate = false;   // with write: empty the file on open
};

class File {
 public:
  virtual ~File() = default;

  // Reads up to out.size() bytes at `offset`. `bytes_read` is set on success;
  // a short read means end of file was reached.
  virtual Status read(std::uint64_t offset, std::span<std::byte> out, std::size_t& bytes_read) = 0;

  // Writes all of `data` at `offset`, extending the file if needed.
  virtual Status write(std::uint64_t offset, std::span<const std::byte> data) = 0;

  // Durably flushes data and size to the device.
  virtual Status sync() = 0;

  // Sets the file length. Extending zero-fills.
  virtual Status truncate(std::uint64_t size) = 0;

  virtual Result<std::uint64_t> size() = 0;

  // Closes the handle. Called by the destructor if not called explicitly;
  // an explicit call lets the caller see the error.
  virtual Status close() = 0;
};

class Vfs {
 public:
  virtual ~Vfs() = default;

  virtual Result<std::unique_ptr<File>> open(const std::string& path, OpenFlags flags) = 0;
  virtual Result<bool> exists(const std::string& path) = 0;
  virtual Status remove(const std::string& path) = 0;
  virtual Status rename(const std::string& from, const std::string& to) = 0;

  // Makes directory-level changes (create, remove, rename) under `dir` durable.
  virtual Status sync_directory(const std::string& dir) = 0;

  // Names (not paths) of the regular files directly in `dir`, unordered.
  // NotFound when the directory does not exist.
  virtual Result<std::vector<std::string>> list(const std::string& dir) = 0;
};

// The real filesystem. Paths are UTF-8 on every platform.
std::unique_ptr<Vfs> make_os_vfs();

}  // namespace archivum
