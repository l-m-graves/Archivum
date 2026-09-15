#include "archivum/journal.h"

#include <array>
#include <cstring>

#include "archivum/crc32c.h"

namespace archivum {
namespace {

constexpr std::array<std::byte, 8> kMagic = {std::byte{'A'}, std::byte{'R'}, std::byte{'C'},
                                             std::byte{'H'}, std::byte{'J'}, std::byte{'N'},
                                             std::byte{'L'}, std::byte{'1'}};
constexpr std::uint32_t kVersion = 1;

void put_u32_le(std::byte* out, std::uint32_t v) {
  out[0] = static_cast<std::byte>(v & 0xFFu);
  out[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
  out[2] = static_cast<std::byte>((v >> 16) & 0xFFu);
  out[3] = static_cast<std::byte>((v >> 24) & 0xFFu);
}

std::uint32_t get_u32_le(const std::byte* in) {
  return std::to_integer<std::uint32_t>(in[0]) | (std::to_integer<std::uint32_t>(in[1]) << 8) |
         (std::to_integer<std::uint32_t>(in[2]) << 16) |
         (std::to_integer<std::uint32_t>(in[3]) << 24);
}

// Header: magic[8] version_le[4] crc32c(magic || version)[4]
std::array<std::byte, Journal::kHeaderBytes> make_header() {
  std::array<std::byte, Journal::kHeaderBytes> h{};
  std::memcpy(h.data(), kMagic.data(), kMagic.size());
  put_u32_le(h.data() + 8, kVersion);
  put_u32_le(h.data() + 12, crc32c(std::span<const std::byte>(h.data(), 12)));
  return h;
}

std::string directory_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  if (slash == 0) return path.substr(0, 1);
  return path.substr(0, slash);
}

}  // namespace

Journal::Journal(Vfs& vfs, std::string path, Options options)
    : vfs_(vfs), path_(std::move(path)), options_(options) {}

Journal::~Journal() { (void)close(); }

Result<std::unique_ptr<Journal>> Journal::open(Vfs& vfs, std::string path, Options options) {
  std::unique_ptr<Journal> j(new Journal(vfs, std::move(path), options));
  auto exists = vfs.exists(j->path_);
  if (!exists.ok()) return exists.status();

  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  auto file = vfs.open(j->path_, flags);
  if (!file.ok()) return file.status();
  j->file_ = std::move(file).value();

  if (!exists.value()) {
    if (Status s = j->create_new(directory_of(j->path_)); !s.ok()) return s;
  } else {
    if (Status s = j->recover(); !s.ok()) return s;
  }
  return j;
}

Status Journal::create_new(const std::string& dir) {
  const auto header = make_header();
  if (Status s = file_->write(0, header); !s.ok()) return s;
  if (Status s = file_->sync(); !s.ok()) return s;
  if (Status s = vfs_.sync_directory(dir); !s.ok()) return s;
  end_ = kHeaderBytes;
  return Status();
}

Status Journal::recover() {
  auto size_r = file_->size();
  if (!size_r.ok()) return size_r.status();
  std::uint64_t size = size_r.value();

  // Header. The header is a constant, so a torn or missing header carries no
  // information and is simply rewritten. Records behind it are still
  // scanned: with an honest fsync none can exist (the header is durable
  // before the first append is acknowledged), but with a lying fsync and
  // reordered writes they can, and each is protected by its own checksum.
  // The journal owns its file: a non-journal file at this path is
  // reinitialised, not preserved.
  std::array<std::byte, kHeaderBytes> header{};
  std::size_t got = 0;
  if (Status s = file_->read(0, header, got); !s.ok()) return s;
  const bool header_ok = got == kHeaderBytes && header == make_header();
  if (!header_ok) {
    header_rewritten_ = true;
    if (size < kHeaderBytes) {
      if (Status s = file_->truncate(kHeaderBytes); !s.ok()) return s;
      size = kHeaderBytes;
    }
    if (Status s = file_->write(0, make_header()); !s.ok()) return s;
    if (Status s = file_->sync(); !s.ok()) return s;
    if (Status s = vfs_.sync_directory(directory_of(path_)); !s.ok()) return s;
  }

  // Records: longest valid prefix.
  records_.clear();
  std::uint64_t off = kHeaderBytes;
  std::vector<std::byte> payload;
  while (off + kRecordHeaderBytes <= size) {
    std::array<std::byte, kRecordHeaderBytes> rh{};
    if (Status s = file_->read(off, rh, got); !s.ok()) return s;
    if (got != kRecordHeaderBytes) break;
    const std::uint32_t length = get_u32_le(rh.data());
    const std::uint32_t expected_crc = get_u32_le(rh.data() + 4);
    if (length == 0 || length > options_.max_record_bytes) break;
    if (off + kRecordHeaderBytes + length > size) break;
    payload.resize(length);
    if (Status s = file_->read(off + kRecordHeaderBytes, payload, got); !s.ok()) return s;
    if (got != length) break;
    std::uint32_t crc = crc32c(std::span<const std::byte>(rh.data(), 4));
    crc = crc32c(crc, payload);
    if (crc != expected_crc) break;
    records_.push_back(RecordRef{off, length});
    off += kRecordHeaderBytes + length;
  }
  end_ = off;
  dropped_tail_bytes_ = size - off;
  if (dropped_tail_bytes_ != 0) {
    if (Status s = file_->truncate(end_); !s.ok()) return s;
    if (Status s = file_->sync(); !s.ok()) return s;
  }
  return Status();
}

Status Journal::append(std::span<const std::byte> payload) {
  if (!file_) return Status::io("journal closed: " + path_);
  if (failed_) return Status::io("journal failed; reopen to recover: " + path_);
  if (payload.empty()) return Status::invalid_argument("empty journal record");
  if (payload.size() > options_.max_record_bytes) {
    return Status::invalid_argument("journal record exceeds max_record_bytes");
  }
  const std::uint32_t length = static_cast<std::uint32_t>(payload.size());

  std::vector<std::byte> frame(kRecordHeaderBytes + payload.size());
  put_u32_le(frame.data(), length);
  std::uint32_t crc = crc32c(std::span<const std::byte>(frame.data(), 4));
  crc = crc32c(crc, payload);
  put_u32_le(frame.data() + 4, crc);
  std::memcpy(frame.data() + kRecordHeaderBytes, payload.data(), payload.size());

  if (Status s = file_->write(end_, frame); !s.ok()) {
    failed_ = true;
    return s;
  }
  if (!options_.unsafe_skip_sync) {
    if (Status s = file_->sync(); !s.ok()) {
      failed_ = true;
      return s;
    }
  }
  records_.push_back(RecordRef{end_, length});
  end_ += frame.size();
  return Status();
}

Result<std::vector<std::byte>> Journal::read(std::size_t index) const {
  if (!file_) return Status::io("journal closed: " + path_);
  if (index >= records_.size()) return Status::not_found("journal record index out of range");
  const RecordRef& ref = records_[index];
  std::vector<std::byte> out(ref.length);
  std::size_t got = 0;
  if (Status s = file_->read(ref.offset + kRecordHeaderBytes, out, got); !s.ok()) return s;
  if (got != ref.length) return Status::corrupt("journal record short read: " + path_);
  return out;
}

Status Journal::close() {
  if (!file_) return Status();
  auto f = std::move(file_);
  return f->close();
}

}  // namespace archivum
