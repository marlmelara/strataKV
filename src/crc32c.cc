#include "crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace stratakv::crc32c {
namespace {

// The Castagnoli polynomial, bit-reversed. Reversed form lets the loop shift
// right and consume the low byte first, which matches little-endian layout.
constexpr uint32_t kPolynomial = 0x82F63B78U;

// One table entry per possible byte: the checksum contribution of that byte.
// constexpr means the 256 entries are computed by the compiler and live in
// read-only memory.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = ((c & 1U) != 0U) ? (kPolynomial ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t crc, const uint8_t* data, size_t n) {
  // The standard pre- and post-inversion. Inverting on the way in and out is
  // what makes leading zero bytes change the result, so a truncated record
  // full of zeros does not accidentally checksum correctly.
  uint32_t c = crc ^ 0xFFFFFFFFU;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t index = (c ^ static_cast<uint32_t>(data[i])) & 0xFFU;
    c = kTable[index] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

uint32_t Value(const uint8_t* data, size_t n) { return Extend(0, data, n); }

}  // namespace stratakv::crc32c
