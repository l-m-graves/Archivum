// In-memory VFS for fast, deterministic tests. Everything is "durable" the
// moment it is written; there is no crash model here. FaultVfs builds its
// crash model out of two MemVfs instances.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "archivum/vfs.h"

namespace archivum::testing {

class MemVfs final : public Vfs {
 public:
  // Thread-safe: multi-threaded tests share one MemVfs. Each file's bytes
  // are guarded by the file's own mutex, the path table by the VFS's.
  struct FileData {
    std::mutex mu;
    std::vector<std::byte> bytes;
  };

  Result<std::unique_ptr<File>> open(const std::string& path, OpenFlags flags) override;
  Result<bool> exists(const std::string& path) override;
  Status remove(const std::string& path) override;
  Status rename(const std::string& from, const std::string& to) override;
  Status sync_directory(const std::string& dir) override;
  Result<std::vector<std::string>> list(const std::string& dir) override;

  // Inspection and manipulation for tests.
  bool has(const std::string& path) const;
  std::vector<std::byte> contents(const std::string& path) const;  // empty if missing
  void put(const std::string& path, std::vector<std::byte> bytes);  // create or replace
  std::vector<std::string> paths() const;
  void clear();
  // Deep-copies every file from `other`, replacing this instance's contents.
  void clone_from(const MemVfs& other);

  // Raw access used by FaultVfs to apply persisted operations.
  std::shared_ptr<FileData> find(const std::string& path) const;
  std::shared_ptr<FileData> create_if_missing(const std::string& path);

 private:
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<FileData>> files_;
};

}  // namespace archivum::testing
