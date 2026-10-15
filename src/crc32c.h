#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// CRC32C (Castagnoli), the checksum used by WAL records and the SSTable
// footer. It detects accidental corruption and torn writes; it is not a
// cryptographic hash and offers no protection against deliberate tampering.
//
// Hand-written so the engine has no checksum dependency. The table is built at
// compile time, so there is no initialization order to worry about.
namespace stratakv::crc32c {

// Checksum of `n` bytes starting at `data`.
uint32_t Value(const uint8_t* data, size_t n);

// Continue a checksum over more bytes. Extend(Value(a), b, n) equals the
// checksum of a followed by b, which is what lets a WAL record checksum its
// type byte and payload without copying them into one buffer first.
uint32_t Extend(uint32_t crc, const uint8_t* data, size_t n);

// Convenience overloads.
inline uint32_t Value(std::span<const uint8_t> data) { return Value(data.data(), data.size()); }

inline uint32_t Extend(uint32_t crc, std::span<const uint8_t> data) {
  return Extend(crc, data.data(), data.size());
}

inline uint32_t Value(std::string_view data) {
  return Value(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

}  // namespace stratakv::crc32c
