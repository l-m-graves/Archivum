// On-disk formats of the database file and the write-ahead log.
// Normative description in docs/page-format.md. All integers little-endian.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "archivum/status.h"

namespace archivum::engine {

using PageNo = std::uint64_t;  // page 0 is the database header
using FrameNo = std::uint64_t;  // 1-based index of a frame in the WAL; 0 = none

constexpr std::uint32_t kMinPageSize = 512;
constexpr std::uint32_t kMaxPageSize = 65536;
constexpr std::uint32_t kDefaultPageSize = 4096;
constexpr std::uint32_t kPageTrailerBytes = 4;  // CRC32C of the rest of the page

// Database header, stored at the start of page 0. Page 0 also carries the
// page trailer like every other page.
struct DbHeader {
  static constexpr std::array<std::byte, 8> kMagic = {
      std::byte{'A'}, std::byte{'R'}, std::byte{'C'}, std::byte{'H'},
      std::byte{'V'}, std::byte{'D'}, std::byte{'B'}, std::byte{'1'}};
  static constexpr std::uint32_t kFormatVersion = 1;
  static constexpr std::size_t kEncodedBytes = 64;

  std::uint32_t page_size = kDefaultPageSize;
  std::uint32_t format_version = kFormatVersion;
  std::uint64_t page_count = 1;      // pages in the logical database, header included
  PageNo freelist_head = 0;          // 0: no free pages
  std::uint64_t freelist_count = 0;
  std::uint64_t change_counter = 0;  // incremented by every committed write transaction
  std::array<std::byte, 16> db_id{};  // random; the WAL carries the same id

  void encode(std::span<std::byte> page) const;
  static Result<DbHeader> decode(std::span<const std::byte> page);
};

// Free pages are linked through their first bytes.
struct FreePage {
  static constexpr std::array<std::byte, 8> kMagic = {
      std::byte{'A'}, std::byte{'R'}, std::byte{'C'}, std::byte{'H'},
      std::byte{'F'}, std::byte{'R'}, std::byte{'E'}, std::byte{'E'}};
  static constexpr std::size_t kEncodedBytes = 16;
  PageNo next = 0;

  void encode(std::span<std::byte> page) const;
  static Result<FreePage> decode(std::span<const std::byte> page);
};

// WAL file header.
struct WalHeader {
  static constexpr std::array<std::byte, 8> kMagic = {
      std::byte{'A'}, std::byte{'R'}, std::byte{'C'}, std::byte{'H'},
      std::byte{'W'}, std::byte{'A'}, std::byte{'L'}, std::byte{'1'}};
  static constexpr std::size_t kEncodedBytes = 48;

  std::uint32_t page_size = kDefaultPageSize;
  std::uint32_t salt1 = 0;  // fresh random values every time the WAL is reset
  std::uint32_t salt2 = 0;
  std::array<std::byte, 16> db_id{};

  void encode(std::span<std::byte> out) const;  // out.size() >= kEncodedBytes
  static Result<WalHeader> decode(std::span<const std::byte> in);
  // The header's own checksum, which seeds the frame chain.
  std::uint32_t chain_seed() const;
};

// WAL frame header, followed by page_size bytes of page content.
//
// Frame checksums are chained: each frame's checksum covers the previous
// frame's checksum (the header's for the first frame), its own header, and
// its page. A frame left over from an earlier, failed transaction cannot be
// mistaken for the continuation of a later one, because its chain value
// was computed against a different predecessor.
struct WalFrameHeader {
  static constexpr std::size_t kEncodedBytes = 32;

  PageNo page_no = 0;
  std::uint64_t db_size_after_commit = 0;  // non-zero only on the last frame of a transaction
  std::uint32_t salt1 = 0;
  std::uint32_t salt2 = 0;
  std::uint32_t checksum = 0;  // chained CRC32C, see above

  // Encodes the header with the checksum computed over `page`, chained from
  // `previous` (the preceding frame's checksum, or the WAL header's).
  void encode(std::span<std::byte> out, std::span<const std::byte> page, std::uint32_t previous);
  // Decodes and verifies against `page` and `previous`. Corrupt on mismatch.
  static Result<WalFrameHeader> decode(std::span<const std::byte> in, std::span<const std::byte> page,
                                       std::uint32_t previous);
};

// Page trailer helpers: every page's last 4 bytes are the CRC32C of the
// bytes before them.
void seal_page(std::span<std::byte> page);
bool page_checksum_ok(std::span<const std::byte> page);

void put_u32(std::byte* out, std::uint32_t v);
void put_u64(std::byte* out, std::uint64_t v);
std::uint32_t get_u32(const std::byte* in);
std::uint64_t get_u64(const std::byte* in);

}  // namespace archivum::engine
