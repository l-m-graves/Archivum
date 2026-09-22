// Win32 implementation of the VFS boundary. Used on Windows Server, the
// primary deployment target. Durability: FlushFileBuffers() for data, size,
// and (on NTFS) the file's own metadata; MOVEFILE_WRITE_THROUGH for renames.
// NTFS journals directory metadata, so sync_directory() has no separate
// operation to perform and returns ok.
#include <windows.h>

#include <string>

#include "archivum/vfs.h"

namespace archivum {
namespace {

std::string last_error_message(const char* what, const std::string& path) {
  const DWORD code = ::GetLastError();
  char* buffer = nullptr;
  const DWORD len = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string text = len != 0 && buffer != nullptr ? std::string(buffer, len) : "unknown error";
  if (buffer != nullptr) ::LocalFree(buffer);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return std::string(what) + " " + path + ": " + text + " (" + std::to_string(code) + ")";
}

// UTF-8 to UTF-16. Returns an empty string on failure; callers treat that as
// an invalid path.
std::wstring to_wide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), needed);
  return wide;
}

OVERLAPPED at_offset(std::uint64_t offset) {
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  return ov;
}

class Win32File final : public File {
 public:
  Win32File(HANDLE handle, std::string path) : handle_(handle), path_(std::move(path)) {}
  ~Win32File() override { (void)close(); }

  Status read(std::uint64_t offset, std::span<std::byte> out, std::size_t& bytes_read) override {
    bytes_read = 0;
    while (bytes_read < out.size()) {
      const std::size_t want = out.size() - bytes_read;
      const DWORD chunk = want > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<DWORD>(want);
      OVERLAPPED ov = at_offset(offset + bytes_read);
      DWORD got = 0;
      if (!::ReadFile(handle_, out.data() + bytes_read, chunk, &got, &ov)) {
        if (::GetLastError() == ERROR_HANDLE_EOF) break;
        return Status::io(last_error_message("ReadFile", path_));
      }
      if (got == 0) break;  // end of file
      bytes_read += got;
    }
    return Status();
  }

  Status write(std::uint64_t offset, std::span<const std::byte> data) override {
    std::size_t done = 0;
    while (done < data.size()) {
      const std::size_t want = data.size() - done;
      const DWORD chunk = want > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<DWORD>(want);
      OVERLAPPED ov = at_offset(offset + done);
      DWORD wrote = 0;
      if (!::WriteFile(handle_, data.data() + done, chunk, &wrote, &ov)) {
        return Status::io(last_error_message("WriteFile", path_));
      }
      if (wrote == 0) return Status::io("WriteFile " + path_ + ": wrote zero bytes");
      done += wrote;
    }
    return Status();
  }

  Status sync() override {
    if (!::FlushFileBuffers(handle_)) {
      return Status::io(last_error_message("FlushFileBuffers", path_));
    }
    return Status();
  }

  Status truncate(std::uint64_t size) override {
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN)) {
      return Status::io(last_error_message("SetFilePointerEx", path_));
    }
    if (!::SetEndOfFile(handle_)) return Status::io(last_error_message("SetEndOfFile", path_));
    return Status();
  }

  Result<std::uint64_t> size() override {
    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(handle_, &sz)) return Status::io(last_error_message("GetFileSizeEx", path_));
    return static_cast<std::uint64_t>(sz.QuadPart);
  }

  Status close() override {
    if (handle_ == INVALID_HANDLE_VALUE) return Status();
    const HANDLE h = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    if (!::CloseHandle(h)) return Status::io(last_error_message("CloseHandle", path_));
    return Status();
  }

 private:
  HANDLE handle_;
  std::string path_;
};

class Win32Vfs final : public Vfs {
 public:
  Result<std::unique_ptr<File>> open(const std::string& path, OpenFlags flags) override {
    if (flags.create && !flags.write) {
      return Status::invalid_argument("create requires write: " + path);
    }
    if (flags.truncate && !flags.write) {
      return Status::invalid_argument("truncate requires write: " + path);
    }
    const std::wstring wpath = to_wide(path);
    if (wpath.empty()) return Status::invalid_argument("invalid UTF-8 path: " + path);

    const DWORD access = flags.write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    if (flags.create) {
      if (flags.exclusive) {
        disposition = CREATE_NEW;
      } else {
        disposition = flags.truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
      }
    } else if (flags.truncate) {
      disposition = TRUNCATE_EXISTING;
    }
    // Readers may share the file (backup tooling); no other writer may.
    const HANDLE h = ::CreateFileW(wpath.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD code = ::GetLastError();
      if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return Status::not_found(last_error_message("CreateFileW", path));
      }
      if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
        return Status::already_exists(last_error_message("CreateFileW", path));
      }
      return Status::io(last_error_message("CreateFileW", path));
    }
    return std::unique_ptr<File>(new Win32File(h, path));
  }

  Result<bool> exists(const std::string& path) override {
    const std::wstring wpath = to_wide(path);
    if (wpath.empty()) return Status::invalid_argument("invalid UTF-8 path: " + path);
    const DWORD attrs = ::GetFileAttributesW(wpath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return false;
    return Status::io(last_error_message("GetFileAttributesW", path));
  }

  Status remove(const std::string& path) override {
    const std::wstring wpath = to_wide(path);
    if (wpath.empty()) return Status::invalid_argument("invalid UTF-8 path: " + path);
    if (!::DeleteFileW(wpath.c_str())) {
      const DWORD code = ::GetLastError();
      if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return Status::not_found(last_error_message("DeleteFileW", path));
      }
      return Status::io(last_error_message("DeleteFileW", path));
    }
    return Status();
  }

  Status rename(const std::string& from, const std::string& to) override {
    const std::wstring wfrom = to_wide(from);
    const std::wstring wto = to_wide(to);
    if (wfrom.empty() || wto.empty()) return Status::invalid_argument("invalid UTF-8 path");
    if (!::MoveFileExW(wfrom.c_str(), wto.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD code = ::GetLastError();
      if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return Status::not_found(last_error_message("MoveFileExW", from));
      }
      return Status::io(last_error_message("MoveFileExW", from));
    }
    return Status();
  }

  Status sync_directory(const std::string& dir) override {
    // There is no directory fsync on Win32. MoveFileExW's
    // MOVEFILE_WRITE_THROUGH above is documented to flush a cross-volume
    // (copy-and-delete) move before returning; a same-volume rename is
    // atomic with no documented flush guarantee (docs/durability.md). The
    // archive does not rely on the rename for durability: the shipper
    // re-copies and re-verifies anything a lost rename leaves absent.
    (void)dir;
    return Status();
  }

  Result<std::vector<std::string>> list(const std::string& dir) override {
    const std::wstring pattern = to_wide(dir + "\\*");
    WIN32_FIND_DATAW data{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD code = ::GetLastError();
      if (code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND) return Status::not_found(dir);
      return Status::io(last_error_message("FindFirstFile", dir));
    }
    std::vector<std::string> names;
    do {
      if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const int needed = ::WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
      if (needed <= 1) continue;
      std::string name(static_cast<std::size_t>(needed - 1), '\0');
      ::WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name.data(), needed, nullptr, nullptr);
      names.push_back(std::move(name));
    } while (::FindNextFileW(h, &data));
    ::FindClose(h);
    return names;
  }
};

}  // namespace

std::unique_ptr<Vfs> make_os_vfs() { return std::make_unique<Win32Vfs>(); }

}  // namespace archivum
