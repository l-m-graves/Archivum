// FaultVfs: the crash-injection shim between the engine and the "disk".
//
// Model (full description in docs/testing/fault-model.md):
//
//   * Two in-memory filesystems. `cache` is what the running process sees,
//     like the OS page cache. `durable` is what survives a power loss.
//   * Every mutation (write, truncate, create, remove, rename) is applied to
//     `cache` immediately and appended to a list of *pending* operations.
//   * File::sync() moves that file's pending writes, truncates, and create to
//     `durable`, in order. Vfs::sync_directory() moves pending removes and
//     renames. Unless the fault configuration says the fsync is dropped, in
//     which case sync() reports success and moves nothing.
//   * crash() simulates power loss: a subset of pending operations is applied
//     to `durable` according to the CrashPolicy (nothing, a prefix, an
//     arbitrary subset in arbitrary order, or everything), with the option of
//     tearing a write so that only part of it lands. Everything else is lost.
//     All open handles die. recover() then makes `cache` a copy of `durable`,
//     which is what the process sees after reboot.
//   * A FaultInjector is consulted before every operation and may fail a
//     write, complete it partially, drop or fail a sync, or crash.
//
// Every random choice is drawn from one generator seeded by FaultConfig::seed,
// so a failing test is reproduced by its seed alone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "archivum/testing/mem_vfs.h"
#include "archivum/vfs.h"

namespace archivum::testing {

enum class OpKind : std::uint8_t {
  Open,
  Read,
  Write,
  Sync,
  Truncate,
  Remove,
  Rename,
  SyncDirectory,
};

const char* to_string(OpKind kind);

struct OpInfo {
  std::uint64_t index = 0;  // 0-based sequence number of this operation
  OpKind kind = OpKind::Open;
  std::string path;
  std::uint64_t offset = 0;  // Write and Read
  std::uint64_t length = 0;  // Write, Read, Truncate (new size)
};

struct Decision {
  enum class Kind : std::uint8_t {
    Proceed,       // perform the operation normally
    FailWrite,     // Write only: write nothing, return IoError
    PartialWrite,  // Write only: write the first `partial_bytes`, return IoError
    DropSync,      // Sync only: return ok, persist nothing
    FailSync,      // Sync only: return IoError, persist nothing
    Crash,         // any op: the operation is in flight when power is lost
  };
  Kind kind = Kind::Proceed;
  std::uint64_t partial_bytes = 0;

  static Decision proceed() { return {}; }
  static Decision fail_write() { return {Kind::FailWrite, 0}; }
  static Decision partial_write(std::uint64_t bytes) { return {Kind::PartialWrite, bytes}; }
  static Decision drop_sync() { return {Kind::DropSync, 0}; }
  static Decision fail_sync() { return {Kind::FailSync, 0}; }
  static Decision crash() { return {Kind::Crash, 0}; }
};

class FaultInjector {
 public:
  virtual ~FaultInjector() = default;
  virtual Decision decide(const OpInfo& op) = 0;
};

// Crashes when the operation with sequence number `step` is in flight.
class CrashAtStep final : public FaultInjector {
 public:
  explicit CrashAtStep(std::uint64_t step) : step_(step) {}
  Decision decide(const OpInfo& op) override {
    return op.index == step_ ? Decision::crash() : Decision::proceed();
  }

 private:
  std::uint64_t step_;
};

// Independent per-operation probabilities. Uses its own generator so that
// injector decisions and crash persistence choices do not perturb each other.
struct RandomFaultRates {
  double fail_write = 0.0;
  double partial_write = 0.0;
  double drop_sync = 0.0;
  double fail_sync = 0.0;
  double crash = 0.0;
};

class RandomInjector final : public FaultInjector {
 public:
  RandomInjector(std::uint64_t seed, RandomFaultRates rates);
  Decision decide(const OpInfo& op) override;

 private:
  std::mt19937_64 rng_;
  RandomFaultRates rates_;
};

struct CrashPolicy {
  enum class Persist : std::uint8_t {
    None,    // nothing pending reaches the disk
    Prefix,  // a random-length prefix of pending operations, in order
    Subset,  // an arbitrary subset, applied in an arbitrary order (write reordering)
    All,     // everything pending reaches the disk (the write cache was flushed)
  };
  Persist persist = Persist::Prefix;
  // When true, the last persisted write (Prefix) or any persisted write
  // (Subset) may land only partially.
  bool torn_writes = true;
  // 0: a torn write may stop at any byte. Otherwise it stops at a multiple of
  // `sector_bytes` from the write's start.
  std::uint32_t sector_bytes = 0;
};

struct FaultConfig {
  CrashPolicy crash;
  // Every File::sync() and Vfs::sync_directory() reports success without
  // persisting anything: the "fsync that lies".
  bool drop_all_syncs = false;
  std::uint64_t seed = 1;
};

class FaultVfs final : public Vfs {
 public:
  explicit FaultVfs(FaultConfig config);
  ~FaultVfs() override;

  void set_injector(std::shared_ptr<FaultInjector> injector);

  // Power loss now. Pending operations are persisted per CrashPolicy; the rest
  // are lost; every open handle is dead. Idempotent while crashed.
  void crash();
  // Reboot: the process now sees exactly the durable state.
  void recover();
  bool crashed() const { return crashed_; }

  std::uint64_t op_count() const { return next_op_; }
  std::size_t pending_count() const { return pending_.size(); }
  std::uint64_t crash_count() const { return crashes_; }

  // Direct views for assertions. `durable()` is what a reboot would reveal.
  MemVfs& durable() { return durable_; }
  MemVfs& cache() { return cache_; }

  // Vfs
  Result<std::unique_ptr<File>> open(const std::string& path, OpenFlags flags) override;
  Result<bool> exists(const std::string& path) override;
  Status remove(const std::string& path) override;
  Status rename(const std::string& from, const std::string& to) override;
  Status sync_directory(const std::string& dir) override;

 private:
  friend class FaultFile;

  struct PendingOp {
    OpKind kind;  // Write, Truncate, Open (create), Remove, Rename
    std::string path;
    std::string path2;  // Rename target
    std::uint64_t offset = 0;
    std::vector<std::byte> data;  // Write payload
    std::uint64_t size = 0;       // Truncate
  };

  Decision consult(OpKind kind, const std::string& path, std::uint64_t offset, std::uint64_t length);
  void apply_to_durable(const PendingOp& op);
  void persist_file(const std::string& path);  // sync() semantics
  void persist_directory();                     // sync_directory() semantics
  std::vector<std::byte> tear(const std::vector<std::byte>& data);

  FaultConfig config_;
  std::mt19937_64 rng_;
  std::shared_ptr<FaultInjector> injector_;
  MemVfs cache_;
  MemVfs durable_;
  std::vector<PendingOp> pending_;
  std::uint64_t next_op_ = 0;
  std::uint64_t epoch_ = 0;  // incremented by crash(); handles from older epochs are dead
  std::uint64_t crashes_ = 0;
  bool crashed_ = false;
};

}  // namespace archivum::testing
