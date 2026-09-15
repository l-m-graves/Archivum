#include "archivum/crc32c.h"

#include <cstring>
#include <string_view>
#include <vector>

#include "test.h"

using archivum::crc32c;

namespace {
std::vector<std::byte> bytes_of(std::string_view s) {
  std::vector<std::byte> out(s.size());
  std::memcpy(out.data(), s.data(), s.size());
  return out;
}
}  // namespace

ARCHIVUM_TEST(crc32c_check_value) {
  // The standard CRC-32C check value for the ASCII string "123456789".
  CHECK(crc32c(bytes_of("123456789")) == 0xE3069283u);
}

ARCHIVUM_TEST(crc32c_empty_is_zero) {
  std::vector<std::byte> none;
  CHECK(crc32c(none) == 0u);
}

ARCHIVUM_TEST(crc32c_known_vectors) {
  // 32 zero bytes and 32 0xFF bytes, from the iSCSI CRC32C test vectors (RFC 3720 B.4).
  std::vector<std::byte> zeros(32, std::byte{0});
  CHECK(crc32c(zeros) == 0x8A9136AAu);
  std::vector<std::byte> ones(32, std::byte{0xFF});
  CHECK(crc32c(ones) == 0x62A8AB43u);
  std::vector<std::byte> inc(32);
  for (std::size_t i = 0; i < inc.size(); ++i) inc[i] = static_cast<std::byte>(i);
  CHECK(crc32c(inc) == 0x46DD794Eu);
}

ARCHIVUM_TEST(crc32c_incremental_matches_whole) {
  const auto data = bytes_of("The quick brown fox jumps over the lazy dog");
  const std::uint32_t whole = crc32c(data);
  for (std::size_t split = 0; split <= data.size(); ++split) {
    std::uint32_t c = crc32c(std::span<const std::byte>(data.data(), split));
    c = crc32c(c, std::span<const std::byte>(data.data() + split, data.size() - split));
    CHECK_MSG(c == whole, "split at " << split);
  }
}

ARCHIVUM_TEST(crc32c_detects_single_bit_flip) {
  auto data = bytes_of("payroll record 0001");
  const std::uint32_t original = crc32c(data);
  for (std::size_t i = 0; i < data.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      data[i] ^= static_cast<std::byte>(1 << bit);
      CHECK(crc32c(data) != original);
      data[i] ^= static_cast<std::byte>(1 << bit);
    }
  }
}
