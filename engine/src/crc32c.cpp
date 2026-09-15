#include "archivum/crc32c.h"

#include <array>

namespace archivum {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (kPolynomial ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

}  // namespace

std::uint32_t crc32c(std::uint32_t crc, std::span<const std::byte> data) {
  std::uint32_t c = ~crc;
  for (std::byte b : data) {
    c = kTable[(c ^ std::to_integer<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace archivum
