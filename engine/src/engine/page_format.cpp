#include "archivum/engine/page_format.h"

#include <cstring>

#include "archivum/crc32c.h"

namespace archivum::engine {

void put_u32(std::byte* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
void put_u64(std::byte* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
std::uint32_t get_u32(const std::byte* in) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(in[i]) << (8 * i);
  return v;
}
std::uint64_t get_u64(const std::byte* in) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= std::to_integer<std::uint64_t>(in[i]) << (8 * i);
  return v;
}

void seal_page(std::span<std::byte> page) {
  const std::size_t body = page.size() - kPageTrailerBytes;
  put_u32(page.data() + body, crc32c(page.first(body)));
}

bool page_checksum_ok(std::span<const std::byte> page) {
  if (page.size() < kPageTrailerBytes + 1) return false;
  const std::size_t body = page.size() - kPageTrailerBytes;
  return get_u32(page.data() + body) == crc32c(page.first(body));
}

// ---------------------------------------------------------------------------

void DbHeader::encode(std::span<std::byte> page) const {
  std::byte* p = page.data();
  std::memcpy(p, kMagic.data(), 8);
  put_u32(p + 8, page_size);
  put_u32(p + 12, format_version);
  put_u64(p + 16, page_count);
  put_u64(p + 24, freelist_head);
  put_u64(p + 32, freelist_count);
  put_u64(p + 40, change_counter);
  std::memcpy(p + 48, db_id.data(), 16);
}

Result<DbHeader> DbHeader::decode(std::span<const std::byte> page) {
  if (page.size() < kEncodedBytes) return Status::corrupt("database header too short");
  const std::byte* p = page.data();
  if (std::memcmp(p, kMagic.data(), 8) != 0) return Status::corrupt("database header magic mismatch");
  DbHeader h;
  h.page_size = get_u32(p + 8);
  h.format_version = get_u32(p + 12);
  h.page_count = get_u64(p + 16);
  h.freelist_head = get_u64(p + 24);
  h.freelist_count = get_u64(p + 32);
  h.change_counter = get_u64(p + 40);
  std::memcpy(h.db_id.data(), p + 48, 16);
  if (h.format_version != kFormatVersion) {
    return Status::unsupported("database format version " + std::to_string(h.format_version));
  }
  if (h.page_size < kMinPageSize || h.page_size > kMaxPageSize ||
      (h.page_size & (h.page_size - 1)) != 0) {
    return Status::corrupt("database header page size invalid");
  }
  if (h.page_count == 0) return Status::corrupt("database header page count zero");
  return h;
}

void FreePage::encode(std::span<std::byte> page) const {
  std::memcpy(page.data(), kMagic.data(), 8);
  put_u64(page.data() + 8, next);
}

Result<FreePage> FreePage::decode(std::span<const std::byte> page) {
  if (page.size() < kEncodedBytes || std::memcmp(page.data(), kMagic.data(), 8) != 0) {
    return Status::corrupt("free page magic mismatch");
  }
  FreePage f;
  f.next = get_u64(page.data() + 8);
  return f;
}

// ---------------------------------------------------------------------------

void WalHeader::encode(std::span<std::byte> out) const {
  std::byte* p = out.data();
  std::memset(p, 0, kEncodedBytes);
  std::memcpy(p, kMagic.data(), 8);
  put_u32(p + 8, page_size);
  put_u32(p + 12, salt1);
  put_u32(p + 16, salt2);
  std::memcpy(p + 20, db_id.data(), 16);
  put_u32(p + 44, crc32c(std::span<const std::byte>(p, 44)));
}

Result<WalHeader> WalHeader::decode(std::span<const std::byte> in) {
  if (in.size() < kEncodedBytes) return Status::corrupt("WAL header too short");
  const std::byte* p = in.data();
  if (std::memcmp(p, kMagic.data(), 8) != 0) return Status::corrupt("WAL header magic mismatch");
  if (get_u32(p + 44) != crc32c(std::span<const std::byte>(p, 44))) {
    return Status::corrupt("WAL header checksum mismatch");
  }
  WalHeader h;
  h.page_size = get_u32(p + 8);
  h.salt1 = get_u32(p + 12);
  h.salt2 = get_u32(p + 16);
  std::memcpy(h.db_id.data(), p + 20, 16);
  return h;
}

std::uint32_t WalHeader::chain_seed() const {
  std::array<std::byte, kEncodedBytes> buf{};
  encode(buf);
  return get_u32(buf.data() + 44);
}

namespace {
std::uint32_t chained_checksum(const std::byte* header_with_zero_checksum, std::span<const std::byte> page,
                               std::uint32_t previous) {
  std::array<std::byte, 4> prev{};
  put_u32(prev.data(), previous);
  std::uint32_t c = crc32c(std::span<const std::byte>(prev.data(), 4));
  c = crc32c(c, std::span<const std::byte>(header_with_zero_checksum, WalFrameHeader::kEncodedBytes));
  return crc32c(c, page);
}
}  // namespace

void WalFrameHeader::encode(std::span<std::byte> out, std::span<const std::byte> page,
                            std::uint32_t previous) {
  std::byte* p = out.data();
  std::memset(p, 0, kEncodedBytes);
  put_u64(p, page_no);
  put_u64(p + 8, db_size_after_commit);
  put_u32(p + 16, salt1);
  put_u32(p + 20, salt2);
  // checksum field (24..28) is zero while hashing
  checksum = chained_checksum(p, page, previous);
  put_u32(p + 24, checksum);
}

Result<WalFrameHeader> WalFrameHeader::decode(std::span<const std::byte> in,
                                              std::span<const std::byte> page, std::uint32_t previous) {
  if (in.size() < kEncodedBytes) return Status::corrupt("WAL frame header too short");
  std::array<std::byte, kEncodedBytes> copy{};
  std::memcpy(copy.data(), in.data(), kEncodedBytes);
  const std::uint32_t stored = get_u32(copy.data() + 24);
  std::memset(copy.data() + 24, 0, 4);
  const std::uint32_t c = chained_checksum(copy.data(), page, previous);
  if (c != stored) return Status::corrupt("WAL frame checksum mismatch");
  WalFrameHeader h;
  h.page_no = get_u64(copy.data());
  h.db_size_after_commit = get_u64(copy.data() + 8);
  h.salt1 = get_u32(copy.data() + 16);
  h.salt2 = get_u32(copy.data() + 20);
  h.checksum = stored;
  return h;
}

}  // namespace archivum::engine
