// CRC-32C (Castagnoli), reflected polynomial 0x82F63B78, as used by iSCSI,
// ext4, and Btrfs. Used for every page and journal record checksum.
//
// Stage 0 ships a portable table-driven implementation. A hardware path
// (SSE4.2 / ARMv8 CRC) can be added later behind the same function; the
// test vectors in tests/unit/crc32c_test.cpp pin the output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace archivum {

// Incremental form: pass the previous return value as `crc` to continue.
// Start with crc = 0 for a fresh checksum.
std::uint32_t crc32c(std::uint32_t crc, std::span<const std::byte> data);

inline std::uint32_t crc32c(std::span<const std::byte> data) { return crc32c(0, data); }

}  // namespace archivum
