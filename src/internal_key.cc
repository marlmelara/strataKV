#include "internal_key.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace stratakv {

void EncodeFixed32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
}

void EncodeFixed64(uint8_t* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

uint32_t DecodeFixed32(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

uint64_t DecodeFixed64(const uint8_t* src) {
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result |= static_cast<uint64_t>(src[i]) << (8 * i);
  }
  return result;
}

void PutFixed32(std::string* dst, uint32_t value) {
  std::array<uint8_t, 4> buffer{};
  EncodeFixed32(buffer.data(), value);
  dst->append(reinterpret_cast<const char*>(buffer.data()), buffer.size());
}

void PutFixed64(std::string* dst, uint64_t value) {
  std::array<uint8_t, 8> buffer{};
  EncodeFixed64(buffer.data(), value);
  dst->append(reinterpret_cast<const char*>(buffer.data()), buffer.size());
}

void PutVarint32(std::string* dst, uint32_t value) {
  while (value >= 0x80) {
    // Low 7 bits, with the high bit set to mean "another byte follows".
    dst->push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  dst->push_back(static_cast<char>(value));
}

bool GetVarint32(std::string_view* input, uint32_t* value) {
  uint32_t result = 0;
  // A 32-bit number needs at most five 7-bit groups; anything longer is corrupt.
  for (int shift = 0; shift <= 28 && !input->empty(); shift += 7) {
    const auto byte = static_cast<uint8_t>((*input)[0]);
    input->remove_prefix(1);
    result |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t length = 0;
  if (!GetVarint32(input, &length)) return false;
  if (input->size() < length) return false;  // Claims more bytes than exist.
  *result = input->substr(0, length);
  input->remove_prefix(length);
  return true;
}

uint64_t PackSequenceAndType(uint64_t sequence, ValueType type) {
  return (sequence << 8) | static_cast<uint64_t>(type);
}

void AppendInternalKey(std::string* dst, std::string_view user_key, uint64_t sequence,
                       ValueType type) {
  dst->append(user_key);
  PutFixed64(dst, PackSequenceAndType(sequence, type));
}

std::string_view ExtractUserKey(std::string_view internal_key) {
  return internal_key.substr(0, internal_key.size() - kInternalKeyTrailerSize);
}

uint64_t ExtractTrailer(std::string_view internal_key) {
  return DecodeFixed64(reinterpret_cast<const uint8_t*>(internal_key.data()) + internal_key.size() -
                       kInternalKeyTrailerSize);
}

ValueType ExtractValueType(std::string_view internal_key) {
  return static_cast<ValueType>(ExtractTrailer(internal_key) & 0xFF);
}

uint64_t ExtractSequence(std::string_view internal_key) {
  return ExtractTrailer(internal_key) >> 8;
}

// User key ascending, then trailer descending so the newest version sorts first.
int InternalKeyComparator::Compare(std::string_view a, std::string_view b) const {
  const std::string_view user_a = ExtractUserKey(a);
  const std::string_view user_b = ExtractUserKey(b);
  if (const int c = user_a.compare(user_b); c != 0) return c;

  // Reversed on purpose: a larger sequence number sorts earlier.
  const uint64_t trailer_a = ExtractTrailer(a);
  const uint64_t trailer_b = ExtractTrailer(b);
  if (trailer_a > trailer_b) return -1;
  if (trailer_a < trailer_b) return 1;
  return 0;
}

}  // namespace stratakv
