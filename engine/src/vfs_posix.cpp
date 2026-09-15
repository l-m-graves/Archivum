// POSIX implementation of the VFS boundary. Used on Linux (CI and the
// container deployment). Durability: fsync() on the file for data and size,
// fsync() on the directory for create/remove/rename.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "archivum/vfs.h"

namespace archivum {
namespace {

std::string errno_message(const char* what, const std::string& path) {
  return std::string(what) + " " + path + ": " + std::strerror(errno);
}

class PosixFile final : public File {
 public:
  PosixFile(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~PosixFile() override { (void)close(); }

  Status read(std::uint64_t offset, std::span<std::byte> out, std::size_t& bytes_read) override {
    bytes_read = 0;
    while (bytes_read < out.size()) {
      const ssize_t n = ::pread(fd_, out.data() + bytes_read, out.size() - bytes_read,
                                static_cast<off_t>(offset + bytes_read));
      if (n < 0) {
        if (errno == EINTR) continue;
        return Status::io(errno_message("pread", path_));
      }
      if (n == 0) break;  // end of file
      bytes_read += static_cast<std::size_t>(n);
    }
    return Status();
  }

  Status write(std::uint64_t offset, std::span<const std::byte> data) override {
    std::size_t done = 0;
    while (done < data.size()) {
      const ssize_t n =
          ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
      if (n < 0) {
        if (errno == EINTR) continue;
        return Status::io(errno_message("pwrite", path_));
      }
      done += static_cast<std::size_t>(n);
    }
    return Status();
  }

  Status sync() override {
    if (::fsync(fd_) != 0) return Status::io(errno_message("fsync", path_));
    return Status();
  }

  Status truncate(std::uint64_t size) override {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
      return Status::io(errno_message("ftruncate", path_));
    }
    return Status();
  }

  Result<std::uint64_t> size() override {
    struct stat st{};
    if (::fstat(fd_, &st) != 0) return Status::io(errno_message("fstat", path_));
    return static_cast<std::uint64_t>(st.st_size);
  }

  Status close() override {
    if (fd_ < 0) return Status();
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) return Status::io(errno_message("close", path_));
    return Status();
  }

 private:
  int fd_;
  std::string path_;
};

class PosixVfs final : public Vfs {
 public:
  Result<std::unique_ptr<File>> open(const std::string& path, OpenFlags flags) override {
    int oflags = O_CLOEXEC;
    oflags |= flags.write ? O_RDWR : O_RDONLY;
    if (flags.create) {
      if (!flags.write) return Status::invalid_argument("create requires write: " + path);
      oflags |= O_CREAT;
      if (flags.exclusive) oflags |= O_EXCL;
    }
    if (flags.truncate) {
      if (!flags.write) return Status::invalid_argument("truncate requires write: " + path);
      oflags |= O_TRUNC;
    }
    const int fd = ::open(path.c_str(), oflags, 0644);
    if (fd < 0) {
      if (errno == ENOENT) return Status::not_found(errno_message("open", path));
      if (errno == EEXIST) return Status::already_exists(errno_message("open", path));
      return Status::io(errno_message("open", path));
    }
    return std::unique_ptr<File>(new PosixFile(fd, path));
  }

  Result<bool> exists(const std::string& path) override {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) return true;
    if (errno == ENOENT) return false;
    return Status::io(errno_message("stat", path));
  }

  Status remove(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) {
      if (errno == ENOENT) return Status::not_found(errno_message("unlink", path));
      return Status::io(errno_message("unlink", path));
    }
    return Status();
  }

  Status rename(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) {
      if (errno == ENOENT) return Status::not_found(errno_message("rename", from));
      return Status::io(errno_message("rename", from));
    }
    return Status();
  }

  Status sync_directory(const std::string& dir) override {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return Status::io(errno_message("open directory", dir));
    Status st = Status();
    if (::fsync(fd) != 0) st = Status::io(errno_message("fsync directory", dir));
    ::close(fd);
    return st;
  }
};

}  // namespace

std::unique_ptr<Vfs> make_os_vfs() { return std::make_unique<PosixVfs>(); }

}  // namespace archivum
