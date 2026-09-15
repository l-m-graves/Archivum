#include "archivum/testing/fault_vfs.h"

#include <algorithm>
#include <cstring>

namespace archivum::testing {

const char* to_string(OpKind kind) {
  switch (kind) {
    case OpKind::Open:
      return "open";
    case OpKind::Read:
      return "read";
    case OpKind::Write:
      return "write";
    case OpKind::Sync:
      return "sync";
    case OpKind::Truncate:
      return "truncate";
    case OpKind::Remove:
      return "remove";
    case OpKind::Rename:
      return "rename";
    case OpKind::SyncDirectory:
      return "sync_directory";
  }
  return "unknown";
}

RandomInjector::RandomInjector(std::uint64_t seed, RandomFaultRates rates)
    : rng_(seed), rates_(rates) {}

Decision RandomInjector::decide(const OpInfo& op) {
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  // Draw once per op so that the sequence of decisions is a pure function of
  // the seed and the op sequence.
  const double roll = unit(rng_);
  double threshold = rates_.crash;
  if (roll < threshold) return Decision::crash();
  if (op.kind == OpKind::Write) {
    threshold += rates_.fail_write;
    if (roll < threshold) return Decision::fail_write();
    threshold += rates_.partial_write;
    if (roll < threshold) {
      std::uniform_int_distribution<std::uint64_t> cut(0, op.length);
      return Decision::partial_write(cut(rng_));
    }
  } else if (op.kind == OpKind::Sync || op.kind == OpKind::SyncDirectory) {
    threshold += rates_.drop_sync;
    if (roll < threshold) return Decision::drop_sync();
    threshold += rates_.fail_sync;
    if (roll < threshold) return Decision::fail_sync();
  }
  return Decision::proceed();
}

// ---------------------------------------------------------------------------

class FaultFile final : public File {
 public:
  FaultFile(FaultVfs& vfs, std::string path, std::unique_ptr<File> cache_file, std::uint64_t epoch)
      : vfs_(vfs), path_(std::move(path)), cache_file_(std::move(cache_file)), epoch_(epoch) {}

  Status read(std::uint64_t offset, std::span<std::byte> out, std::size_t& bytes_read) override {
    bytes_read = 0;
    if (Status s = alive(); !s.ok()) return s;
    const Decision d = vfs_.consult(OpKind::Read, path_, offset, out.size());
    if (d.kind == Decision::Kind::Crash) {
      vfs_.crash();
      return Status::crashed("read " + path_);
    }
    return cache_file_->read(offset, out, bytes_read);
  }

  Status write(std::uint64_t offset, std::span<const std::byte> data) override {
    if (Status s = alive(); !s.ok()) return s;
    const Decision d = vfs_.consult(OpKind::Write, path_, offset, data.size());
    switch (d.kind) {
      case Decision::Kind::FailWrite:
        return Status::io("injected write failure: " + path_);
      case Decision::Kind::PartialWrite: {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(d.partial_bytes, data.size()));
        record_write(offset, data.first(n));
        return Status::io("injected partial write (" + std::to_string(n) + " of " +
                          std::to_string(data.size()) + " bytes): " + path_);
      }
      case Decision::Kind::Crash:
        // The write is in flight when power is lost: it is pending and the
        // crash policy decides whether any of it lands.
        record_write(offset, data);
        vfs_.crash();
        return Status::crashed("write " + path_);
      default:
        break;
    }
    record_write(offset, data);
    return Status();
  }

  Status sync() override {
    if (Status s = alive(); !s.ok()) return s;
    const Decision d = vfs_.consult(OpKind::Sync, path_, 0, 0);
    switch (d.kind) {
      case Decision::Kind::DropSync:
        return Status();
      case Decision::Kind::FailSync:
        return Status::io("injected sync failure: " + path_);
      case Decision::Kind::Crash:
        vfs_.crash();
        return Status::crashed("sync " + path_);
      default:
        break;
    }
    if (vfs_.config_.drop_all_syncs) return Status();
    vfs_.persist_file(path_);
    return Status();
  }

  Status truncate(std::uint64_t size) override {
    if (Status s = alive(); !s.ok()) return s;
    const Decision d = vfs_.consult(OpKind::Truncate, path_, 0, size);
    if (d.kind == Decision::Kind::Crash) {
      vfs_.crash();
      return Status::crashed("truncate " + path_);
    }
    if (Status s = cache_file_->truncate(size); !s.ok()) return s;
    FaultVfs::PendingOp op;
    op.kind = OpKind::Truncate;
    op.path = path_;
    op.size = size;
    vfs_.pending_.push_back(std::move(op));
    return Status();
  }

  Result<std::uint64_t> size() override {
    if (Status s = alive(); !s.ok()) return s;
    return cache_file_->size();
  }

  Status close() override {
    if (!cache_file_) return Status();
    auto f = std::move(cache_file_);
    return f->close();
  }

 private:
  Status alive() const {
    if (!cache_file_) return Status::io("fault file closed: " + path_);
    if (vfs_.crashed_ || epoch_ != vfs_.epoch_) {
      return Status::crashed("handle predates crash: " + path_);
    }
    return Status();
  }

  void record_write(std::uint64_t offset, std::span<const std::byte> data) {
    (void)cache_file_->write(offset, data);
    FaultVfs::PendingOp op;
    op.kind = OpKind::Write;
    op.path = path_;
    op.offset = offset;
    op.data.assign(data.begin(), data.end());
    vfs_.pending_.push_back(std::move(op));
  }

  FaultVfs& vfs_;
  std::string path_;
  std::unique_ptr<File> cache_file_;
  std::uint64_t epoch_;
};

// ---------------------------------------------------------------------------

FaultVfs::FaultVfs(FaultConfig config) : config_(config), rng_(config.seed) {}

FaultVfs::~FaultVfs() = default;

void FaultVfs::set_injector(std::shared_ptr<FaultInjector> injector) {
  injector_ = std::move(injector);
}

Decision FaultVfs::consult(OpKind kind, const std::string& path, std::uint64_t offset,
                           std::uint64_t length) {
  OpInfo info;
  info.index = next_op_++;
  info.kind = kind;
  info.path = path;
  info.offset = offset;
  info.length = length;
  if (!injector_) return Decision::proceed();
  return injector_->decide(info);
}

Result<std::unique_ptr<File>> FaultVfs::open(const std::string& path, OpenFlags flags) {
  if (crashed_) return Status::crashed("open " + path);
  const Decision d = consult(OpKind::Open, path, 0, 0);
  if (d.kind == Decision::Kind::Crash) {
    crash();
    return Status::crashed("open " + path);
  }
  const bool existed = cache_.has(path);
  auto r = cache_.open(path, flags);
  if (!r.ok()) return r.status();
  if (!existed) {
    PendingOp op;
    op.kind = OpKind::Open;  // create
    op.path = path;
    pending_.push_back(std::move(op));
  }
  if (existed && flags.truncate) {
    PendingOp op;
    op.kind = OpKind::Truncate;
    op.path = path;
    op.size = 0;
    pending_.push_back(std::move(op));
  }
  return std::unique_ptr<File>(new FaultFile(*this, path, std::move(r).value(), epoch_));
}

Result<bool> FaultVfs::exists(const std::string& path) {
  if (crashed_) return Status::crashed("exists " + path);
  return cache_.exists(path);
}

Status FaultVfs::remove(const std::string& path) {
  if (crashed_) return Status::crashed("remove " + path);
  const Decision d = consult(OpKind::Remove, path, 0, 0);
  if (d.kind == Decision::Kind::Crash) {
    crash();
    return Status::crashed("remove " + path);
  }
  if (Status s = cache_.remove(path); !s.ok()) return s;
  PendingOp op;
  op.kind = OpKind::Remove;
  op.path = path;
  pending_.push_back(std::move(op));
  return Status();
}

Status FaultVfs::rename(const std::string& from, const std::string& to) {
  if (crashed_) return Status::crashed("rename " + from);
  const Decision d = consult(OpKind::Rename, from, 0, 0);
  if (d.kind == Decision::Kind::Crash) {
    crash();
    return Status::crashed("rename " + from);
  }
  if (Status s = cache_.rename(from, to); !s.ok()) return s;
  PendingOp op;
  op.kind = OpKind::Rename;
  op.path = from;
  op.path2 = to;
  pending_.push_back(std::move(op));
  return Status();
}

Status FaultVfs::sync_directory(const std::string& dir) {
  if (crashed_) return Status::crashed("sync_directory " + dir);
  const Decision d = consult(OpKind::SyncDirectory, dir, 0, 0);
  switch (d.kind) {
    case Decision::Kind::DropSync:
      return Status();
    case Decision::Kind::FailSync:
      return Status::io("injected directory sync failure: " + dir);
    case Decision::Kind::Crash:
      crash();
      return Status::crashed("sync_directory " + dir);
    default:
      break;
  }
  if (config_.drop_all_syncs) return Status();
  persist_directory();
  return Status();
}

void FaultVfs::apply_to_durable(const PendingOp& op) {
  switch (op.kind) {
    case OpKind::Open: {  // create
      durable_.create_if_missing(op.path);
      break;
    }
    case OpKind::Write: {
      auto data = durable_.find(op.path);
      if (!data) break;  // the file's creation never became durable: the write is lost
      const std::uint64_t end = op.offset + op.data.size();
      if (end > data->bytes.size()) data->bytes.resize(static_cast<std::size_t>(end));
      if (!op.data.empty()) {
        std::memcpy(data->bytes.data() + op.offset, op.data.data(), op.data.size());
      }
      break;
    }
    case OpKind::Truncate: {
      auto data = durable_.find(op.path);
      if (!data) break;
      data->bytes.resize(static_cast<std::size_t>(op.size));
      break;
    }
    case OpKind::Remove: {
      (void)durable_.remove(op.path);
      break;
    }
    case OpKind::Rename: {
      if (durable_.has(op.path)) (void)durable_.rename(op.path, op.path2);
      break;
    }
    default:
      break;
  }
}

void FaultVfs::persist_file(const std::string& path) {
  // fsync(file): this file's create, writes, and truncates become durable, in
  // order. Removes and renames (directory entries) stay pending.
  std::vector<PendingOp> keep;
  keep.reserve(pending_.size());
  for (auto& op : pending_) {
    const bool file_op =
        op.kind == OpKind::Open || op.kind == OpKind::Write || op.kind == OpKind::Truncate;
    if (file_op && op.path == path) {
      apply_to_durable(op);
    } else {
      keep.push_back(std::move(op));
    }
  }
  pending_ = std::move(keep);
}

void FaultVfs::persist_directory() {
  // fsync(dir): pending creates, removes, and renames become durable. A
  // create makes an empty file durable; its data still needs File::sync().
  std::vector<PendingOp> keep;
  keep.reserve(pending_.size());
  for (auto& op : pending_) {
    const bool dir_op = op.kind == OpKind::Open || op.kind == OpKind::Remove || op.kind == OpKind::Rename;
    if (dir_op) {
      apply_to_durable(op);
    } else {
      keep.push_back(std::move(op));
    }
  }
  pending_ = std::move(keep);
}

std::vector<std::byte> FaultVfs::tear(const std::vector<std::byte>& data) {
  if (data.empty()) return data;
  std::uniform_int_distribution<std::size_t> cut(0, data.size());
  std::size_t n = cut(rng_);
  if (config_.crash.sector_bytes != 0) {
    n -= n % config_.crash.sector_bytes;
  }
  return std::vector<std::byte>(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
}

void FaultVfs::crash() {
  if (crashed_) return;
  crashed_ = true;
  ++crashes_;
  ++epoch_;

  // Choose which pending operations reached the disk.
  std::vector<std::size_t> chosen;
  const std::size_t n = pending_.size();
  switch (config_.crash.persist) {
    case CrashPolicy::Persist::None:
      break;
    case CrashPolicy::Persist::All:
      for (std::size_t i = 0; i < n; ++i) chosen.push_back(i);
      break;
    case CrashPolicy::Persist::Prefix: {
      std::uniform_int_distribution<std::size_t> len(0, n);
      const std::size_t k = len(rng_);
      for (std::size_t i = 0; i < k; ++i) chosen.push_back(i);
      break;
    }
    case CrashPolicy::Persist::Subset: {
      std::uniform_int_distribution<int> coin(0, 1);
      for (std::size_t i = 0; i < n; ++i) {
        if (coin(rng_) == 1) chosen.push_back(i);
      }
      std::shuffle(chosen.begin(), chosen.end(), rng_);
      break;
    }
  }

  // Tear writes. Prefix: only the last persisted write may be torn (it is the
  // one in flight). Subset: any persisted write may be torn, with probability
  // one in four.
  if (config_.crash.torn_writes && !chosen.empty()) {
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<int> quarter(0, 3);
    if (config_.crash.persist == CrashPolicy::Persist::Subset) {
      for (std::size_t idx : chosen) {
        if (pending_[idx].kind == OpKind::Write && quarter(rng_) == 0) {
          pending_[idx].data = tear(pending_[idx].data);
        }
      }
    } else if (config_.crash.persist != CrashPolicy::Persist::All) {
      PendingOp& last = pending_[chosen.back()];
      if (last.kind == OpKind::Write && coin(rng_) == 1) last.data = tear(last.data);
    }
  }

  for (std::size_t idx : chosen) apply_to_durable(pending_[idx]);
  pending_.clear();
  cache_.clear();
}

void FaultVfs::recover() {
  cache_.clone_from(durable_);
  pending_.clear();
  crashed_ = false;
}

}  // namespace archivum::testing
