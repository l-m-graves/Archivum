#include "wal.h"

#include <algorithm>
#include <random>

namespace archivum::engine {
namespace {

std::string directory_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  if (slash == 0) return path.substr(0, 1);
  return path.substr(0, slash);
}

// One random_device per call: a function-local generator was global state
// shared by every instance, and ThreadSanitizer found two instances on two
// threads racing on it in their checkpoints (invariant I1: no global state).
std::uint32_t random_salt() {
  std::random_device rd;
  return static_cast<std::uint32_t>(rd());
}

}  // namespace

Wal::Wal(Vfs& vfs, std::string path, std::uint32_t page_size)
    : vfs_(vfs), path_(std::move(path)), page_size_(page_size) {}

Wal::~Wal() { (void)close(); }

Result<std::unique_ptr<Wal>> Wal::open(Vfs& vfs, std::string path, std::uint32_t page_size,
                                       const WalOpenOptions& options, WalRecoveryInfo* info) {
  std::unique_ptr<Wal> w(new Wal(vfs, std::move(path), page_size));
  w->read_only_ = options.read_only;
  auto exists = vfs.exists(w->path_);
  if (!exists.ok()) return exists.status();
  if (options.read_only && !exists.value()) return Status::not_found(w->path_);
  OpenFlags flags;
  flags.write = !options.read_only;
  flags.create = !options.read_only;
  auto file = vfs.open(w->path_, flags);
  if (!file.ok()) return file.status();
  w->file_ = std::move(file).value();
  w->header_.page_size = page_size;
  w->header_.db_id = options.db_id;
  w->header_.base_change_counter = options.new_base_change_counter;
  if (!exists.value()) {
    w->header_.salt1 = random_salt();
    w->header_.salt2 = random_salt();
    if (Status s = w->write_header(); !s.ok()) return s;
    if (Status s = vfs.sync_directory(directory_of(w->path_)); !s.ok()) return s;
    w->chain_ = w->header_.chain_seed();
    if (info != nullptr) *info = WalRecoveryInfo{};
    return w;
  }
  if (Status s = w->recover(options, info); !s.ok()) return s;
  return w;
}

Status Wal::write_header() {
  std::array<std::byte, WalHeader::kEncodedBytes> buf{};
  header_.encode(buf);
  if (Status s = file_->write(0, buf); !s.ok()) return s;
  return file_->sync();
}

Status Wal::recover(const WalOpenOptions& options, WalRecoveryInfo* info) {
  const std::array<std::byte, 16>& db_id = options.db_id;
  WalRecoveryInfo local;
  auto size_r = file_->size();
  if (!size_r.ok()) return size_r.status();
  const std::uint64_t size = size_r.value();

  std::array<std::byte, WalHeader::kEncodedBytes> hbuf{};
  std::size_t got = 0;
  if (Status s = file_->read(0, hbuf, got); !s.ok()) return s;
  bool header_ok = false;
  if (got == WalHeader::kEncodedBytes) {
    auto h = WalHeader::decode(hbuf);
    if (h.ok() && h.value().page_size == page_size_ && h.value().db_id == db_id) {
      header_ = h.value();
      header_ok = true;
    }
  }
  if (!header_ok) {
    if (read_only_) return Status::corrupt("log header invalid or for another database: " + path_);
    // With an honest fsync the header is durable before any frame is
    // acknowledged, so an unreadable header means no committed transaction
    // can be behind it. Start a fresh log.
    local.header_rewritten = true;
    local.dropped_bytes = size;
    header_.salt1 = random_salt();
    header_.salt2 = random_salt();
    if (Status s = file_->truncate(0); !s.ok()) return s;
    if (Status s = write_header(); !s.ok()) return s;
    next_frame_ = 1;
    chain_ = header_.chain_seed();
    if (info != nullptr) *info = local;
    return Status();
  }

  const std::uint64_t frame_bytes = WalFrameHeader::kEncodedBytes + page_size_;
  std::vector<std::byte> buf(frame_bytes);
  std::vector<std::pair<PageNo, FrameNo>> pending;
  FrameNo frame = 1;
  std::uint64_t valid_end = WalHeader::kEncodedBytes;
  std::uint32_t running = header_.chain_seed();
  chain_ = running;
  while (frame_offset(frame) + frame_bytes <= size) {
    if (Status s = file_->read(frame_offset(frame), buf, got); !s.ok()) return s;
    if (got != frame_bytes) break;
    auto fh = WalFrameHeader::decode(std::span<const std::byte>(buf.data(), WalFrameHeader::kEncodedBytes),
                                     std::span<const std::byte>(buf.data() + WalFrameHeader::kEncodedBytes, page_size_),
                                     running);
    if (!fh.ok()) break;
    if (fh.value().salt1 != header_.salt1 || fh.value().salt2 != header_.salt2) break;
    if (!page_checksum_ok(std::span<const std::byte>(buf.data() + WalFrameHeader::kEncodedBytes, page_size_))) break;
    running = fh.value().checksum;
    pending.emplace_back(fh.value().page_no, frame);
    if (fh.value().db_size_after_commit != 0) {
      // The commit frame is the page-0 image; it names the commit.
      if (fh.value().page_no != 0) break;
      auto hdr = DbHeader::decode(std::span<const std::byte>(buf.data() + WalFrameHeader::kEncodedBytes, page_size_));
      if (!hdr.ok()) break;
      WalCommit c;
      c.frame = frame;
      c.db_size = fh.value().db_size_after_commit;
      c.change_counter = hdr.value().change_counter;
      c.commit_time_us = hdr.value().commit_time_us;
      publish(pending, c);
      pending.clear();
      valid_end = frame_offset(frame) + frame_bytes;
      chain_ = running;
    }
    ++frame;
  }
  next_frame_ = last_commit_ + 1;
  if (read_only_) {
    local.dropped_bytes = size > valid_end ? size - valid_end : 0;
    local.committed_frames = last_commit_;
    if (info != nullptr) *info = local;
    return Status();
  }
  if (valid_end < size) {
    local.dropped_bytes = size - valid_end;
    if (Status s = file_->truncate(valid_end); !s.ok()) return s;
    if (Status s = file_->sync(); !s.ok()) return s;
  }
  // Durability barrier, as for the data file: an earlier open may have
  // left the header or a tail truncation unsynced.
  if (local.dropped_bytes == 0) {
    if (Status s = file_->sync(); !s.ok()) return s;
  }
  if (Status s = vfs_.sync_directory(directory_of(path_)); !s.ok()) return s;
  local.committed_frames = last_commit_;
  if (info != nullptr) *info = local;
  return Status();
}

void Wal::publish(const std::vector<std::pair<PageNo, FrameNo>>& frames, const WalCommit& commit) {
  for (const auto& [page, fr] : frames) {
    index_[page].push_back(fr);
    frame_pages_.push_back(page);
  }
  commits_[commit.frame] = commit.db_size;
  commit_list_.push_back(commit);
  last_commit_ = commit.frame;
}

Status Wal::copy_committed_to(Vfs& vfs, const std::string& path) {
  if (!file_) return Status::io("WAL closed");
  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  flags.truncate = true;
  auto out = vfs.open(path, flags);
  if (!out.ok()) return out.status();
  const std::uint64_t total = committed_bytes();
  std::vector<std::byte> buf(1 << 16);
  std::uint64_t off = 0;
  while (off < total) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), total - off));
    std::size_t got = 0;
    if (Status s = file_->read(off, std::span<std::byte>(buf.data(), want), got); !s.ok()) return s;
    if (got != want) return Status::corrupt("short read while archiving the log: " + path_);
    if (Status s = out.value()->write(off, std::span<const std::byte>(buf.data(), got)); !s.ok()) return s;
    off += got;
  }
  if (Status s = out.value()->sync(); !s.ok()) return s;
  if (Status s = out.value()->close(); !s.ok()) return s;
  return vfs.sync_directory(directory_of(path));
}

std::uint64_t Wal::db_size_at(FrameNo commit_frame) const {
  auto it = commits_.find(commit_frame);
  return it == commits_.end() ? 0 : it->second;
}

FrameNo Wal::find(PageNo page, FrameNo snapshot) const {
  auto it = index_.find(page);
  if (it == index_.end()) return 0;
  const auto& frames = it->second;
  auto pos = std::upper_bound(frames.begin(), frames.end(), snapshot);
  if (pos == frames.begin()) return 0;
  return *(pos - 1);
}

Status Wal::read_frame_page(FrameNo frame, std::span<std::byte> out) {
  if (frame == 0 || frame >= next_frame_ || out.size() != page_size_) {
    return Status::invalid_argument("bad WAL frame read");
  }
  std::size_t got = 0;
  const std::uint64_t off = frame_offset(frame) + WalFrameHeader::kEncodedBytes;
  if (Status s = file_->read(off, out, got); !s.ok()) return s;
  if (got != page_size_) return Status::corrupt("short WAL frame read");
  if (!page_checksum_ok(out)) return Status::corrupt("WAL frame page checksum mismatch");
  return Status();
}

Status Wal::append_transaction(const std::vector<PageImage>& images, std::uint64_t db_size_after) {
  if (images.empty()) return Status::invalid_argument("empty transaction");
  if (read_only_) return Status::invalid_argument("log opened read-only: " + path_);
  if (failed_) return Status::io("WAL failed after an earlier I/O error; reopen the database: " + path_);
  const std::uint64_t frame_bytes = WalFrameHeader::kEncodedBytes + page_size_;
  std::vector<std::byte> buf(frame_bytes * images.size());
  std::vector<std::pair<PageNo, FrameNo>> frames;
  frames.reserve(images.size());
  std::uint32_t running = chain_;
  for (std::size_t i = 0; i < images.size(); ++i) {
    if (images[i].data.size() != page_size_) return Status::invalid_argument("page image size");
    WalFrameHeader fh;
    fh.page_no = images[i].page;
    fh.db_size_after_commit = (i + 1 == images.size()) ? db_size_after : 0;
    fh.salt1 = header_.salt1;
    fh.salt2 = header_.salt2;
    std::byte* dst = buf.data() + i * frame_bytes;
    std::copy(images[i].data.begin(), images[i].data.end(), dst + WalFrameHeader::kEncodedBytes);
    fh.encode(std::span<std::byte>(dst, WalFrameHeader::kEncodedBytes), images[i].data, running);
    running = fh.checksum;
    frames.emplace_back(images[i].page, next_frame_ + i);
  }
  // One write for the whole transaction, then one sync. Frames are not
  // visible in the index, and the chain does not advance, until the sync
  // succeeds; a failed transaction's frames are discarded best-effort and
  // are unreachable through the chain regardless.
  Status s = file_->write(frame_offset(next_frame_), buf);
  if (s.ok()) s = file_->sync();
  if (!s.ok()) {
    // Whatever landed is unreachable through the chain; remove it if we can
    // and refuse further appends until reopened, when recovery re-reads the
    // truth from disk.
    const Status d = discard_uncommitted();
    failed_ = true;
    // If the filesystem died during the discard, say so rather than
    // reporting a plain I/O error the caller might retry against.
    return d.code() == ErrorCode::Crashed ? d : s;
  }
  const FrameNo commit_frame = next_frame_ + images.size() - 1;
  WalCommit c;
  c.frame = commit_frame;
  c.db_size = db_size_after;
  if (auto hdr = DbHeader::decode(images.back().data); hdr.ok()) {
    c.change_counter = hdr.value().change_counter;
    c.commit_time_us = hdr.value().commit_time_us;
  }
  publish(frames, c);
  next_frame_ = commit_frame + 1;
  chain_ = running;
  return Status();
}

Status Wal::discard_uncommitted() {
  if (!file_) return Status::io("WAL closed");
  if (Status s = file_->truncate(frame_offset(next_frame_)); !s.ok()) return s;
  return file_->sync();
}

std::map<PageNo, FrameNo> Wal::latest_frames(FrameNo up_to) const {
  std::map<PageNo, FrameNo> out;
  for (const auto& [page, frames] : index_) {
    const FrameNo f = find(page, up_to);
    if (f != 0) out[page] = f;
  }
  return out;
}

Status Wal::reset(std::uint64_t base_change_counter) {
  if (read_only_) return Status::invalid_argument("log opened read-only: " + path_);
  if (failed_) return Status::io("WAL failed after an earlier I/O error; reopen the database: " + path_);
  header_.salt1 = random_salt();
  header_.salt2 = random_salt();
  header_.base_change_counter = base_change_counter;
  if (Status s = file_->truncate(0); !s.ok()) {
    // Unknown whether the frames are gone: fail closed until reopened.
    failed_ = true;
    return s;
  }
  // The frames are gone from the file: the index must say so even if the
  // header write below fails, otherwise reads would follow the index into
  // bytes that no longer exist.
  index_.clear();
  commits_.clear();
  commit_list_.clear();
  frame_pages_.clear();
  last_commit_ = 0;
  next_frame_ = 1;
  chain_ = header_.chain_seed();
  if (Status s = write_header(); !s.ok()) {
    failed_ = true;
    return s;
  }
  return Status();
}

Status Wal::close() {
  if (!file_) return Status();
  auto f = std::move(file_);
  return f->close();
}

}  // namespace archivum::engine
