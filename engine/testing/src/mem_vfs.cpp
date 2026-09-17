#include "archivum/testing/mem_vfs.h"

#include <algorithm>
#include <cstring>

namespace archivum::testing {
namespace {

class MemFile final : public File {
 public:
  MemFile(std::shared_ptr<MemVfs::FileData> data, bool writable)
      : data_(std::move(data)), writable_(writable) {}

  Status read(std::uint64_t offset, std::span<std::byte> out, std::size_t& bytes_read) override {
    bytes_read = 0;
    if (!data_) return Status::io("mem file closed");
    const std::uint64_t size = data_->bytes.size();
    if (offset >= size) return Status();
    const std::size_t avail = static_cast<std::size_t>(size - offset);
    bytes_read = std::min(avail, out.size());
    if (bytes_read != 0) {
      std::memcpy(out.data(), data_->bytes.data() + offset, bytes_read);
    }
    return Status();
  }

  Status write(std::uint64_t offset, std::span<const std::byte> data) override {
    if (!data_) return Status::io("mem file closed");
    if (!writable_) return Status::io("mem file opened read-only");
    const std::uint64_t end = offset + data.size();
    if (end > data_->bytes.size()) data_->bytes.resize(static_cast<std::size_t>(end));
    if (!data.empty()) std::memcpy(data_->bytes.data() + offset, data.data(), data.size());
    return Status();
  }

  Status sync() override {
    if (!data_) return Status::io("mem file closed");
    return Status();
  }

  Status truncate(std::uint64_t size) override {
    if (!data_) return Status::io("mem file closed");
    if (!writable_) return Status::io("mem file opened read-only");
    data_->bytes.resize(static_cast<std::size_t>(size));
    return Status();
  }

  Result<std::uint64_t> size() override {
    if (!data_) return Status::io("mem file closed");
    return static_cast<std::uint64_t>(data_->bytes.size());
  }

  Status close() override {
    data_.reset();
    return Status();
  }

 private:
  std::shared_ptr<MemVfs::FileData> data_;
  bool writable_;
};

}  // namespace

Result<std::unique_ptr<File>> MemVfs::open(const std::string& path, OpenFlags flags) {
  if (flags.create && !flags.write) return Status::invalid_argument("create requires write: " + path);
  if (flags.truncate && !flags.write) {
    return Status::invalid_argument("truncate requires write: " + path);
  }
  auto it = files_.find(path);
  if (it == files_.end()) {
    if (!flags.create) return Status::not_found("mem open: " + path);
    it = files_.emplace(path, std::make_shared<FileData>()).first;
  } else if (flags.create && flags.exclusive) {
    return Status::already_exists("mem open exclusive: " + path);
  }
  if (flags.truncate) it->second->bytes.clear();
  return std::unique_ptr<File>(new MemFile(it->second, flags.write));
}

Result<bool> MemVfs::exists(const std::string& path) { return files_.count(path) != 0; }

Status MemVfs::remove(const std::string& path) {
  if (files_.erase(path) == 0) return Status::not_found("mem remove: " + path);
  return Status();
}

Status MemVfs::rename(const std::string& from, const std::string& to) {
  auto it = files_.find(from);
  if (it == files_.end()) return Status::not_found("mem rename: " + from);
  auto data = it->second;
  files_.erase(it);
  files_[to] = std::move(data);
  return Status();
}

Status MemVfs::sync_directory(const std::string&) { return Status(); }

Result<std::vector<std::string>> MemVfs::list(const std::string& dir) {
  // Directories exist implicitly: a directory is any prefix of a path.
  const std::string prefix = dir + "/";
  std::vector<std::string> names;
  for (const auto& [path, data] : files_) {
    if (path.rfind(prefix, 0) != 0) continue;
    const std::string rest = path.substr(prefix.size());
    if (rest.find('/') == std::string::npos) names.push_back(rest);
  }
  return names;
}

bool MemVfs::has(const std::string& path) const { return files_.count(path) != 0; }

std::vector<std::byte> MemVfs::contents(const std::string& path) const {
  auto it = files_.find(path);
  if (it == files_.end()) return {};
  return it->second->bytes;
}

void MemVfs::put(const std::string& path, std::vector<std::byte> bytes) {
  auto data = std::make_shared<FileData>();
  data->bytes = std::move(bytes);
  files_[path] = std::move(data);
}

std::vector<std::string> MemVfs::paths() const {
  std::vector<std::string> out;
  out.reserve(files_.size());
  for (const auto& [path, data] : files_) out.push_back(path);
  return out;
}

void MemVfs::clear() { files_.clear(); }

void MemVfs::clone_from(const MemVfs& other) {
  files_.clear();
  for (const auto& [path, data] : other.files_) {
    auto copy = std::make_shared<FileData>();
    copy->bytes = data->bytes;
    files_[path] = std::move(copy);
  }
}

std::shared_ptr<MemVfs::FileData> MemVfs::find(const std::string& path) const {
  auto it = files_.find(path);
  return it == files_.end() ? nullptr : it->second;
}

std::shared_ptr<MemVfs::FileData> MemVfs::create_if_missing(const std::string& path) {
  auto it = files_.find(path);
  if (it == files_.end()) it = files_.emplace(path, std::make_shared<FileData>()).first;
  return it->second;
}

}  // namespace archivum::testing
