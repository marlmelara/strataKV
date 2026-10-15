#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "crc32c.h"

namespace stratakv {
namespace {

std::span<const uint8_t> Bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Known answers for CRC-32C (Castagnoli). The "123456789" value is the one the
// build spec names as the acceptance test; the rest are the standard iSCSI
// (RFC 3720) vectors. These are the contract: if a future optimization of the
// table or the loop changes any of them, on-disk files written by an older
// build would stop verifying.
TEST(Crc32c, MatchesKnownAnswers) {
  EXPECT_EQ(crc32c::Value(std::string_view("")), 0x00000000U);
  EXPECT_EQ(crc32c::Value(std::string_view("a")), 0xC1D04330U);
  EXPECT_EQ(crc32c::Value(std::string_view("abc")), 0x364B3FB7U);
  EXPECT_EQ(crc32c::Value(std::string_view("123456789")), 0xE3069283U);
  EXPECT_EQ(crc32c::Value(std::string_view("hello world")), 0xC99465AAU);

  const std::vector<uint8_t> zeros(32, 0x00);
  EXPECT_EQ(crc32c::Value(zeros.data(), zeros.size()), 0x8A9136AAU);

  const std::vector<uint8_t> ones(32, 0xFF);
  EXPECT_EQ(crc32c::Value(ones.data(), ones.size()), 0x62A8AB43U);

  std::vector<uint8_t> ascending(32);
  for (size_t i = 0; i < ascending.size(); ++i) ascending[i] = static_cast<uint8_t>(i);
  EXPECT_EQ(crc32c::Value(ascending.data(), ascending.size()), 0x46DD794EU);

  std::vector<uint8_t> descending(32);
  for (size_t i = 0; i < descending.size(); ++i) {
    descending[i] = static_cast<uint8_t>(descending.size() - 1 - i);
  }
  EXPECT_EQ(crc32c::Value(descending.data(), descending.size()), 0x113FDB5CU);
}

// WalWriter::AddRecord checksums the one-byte record type, then continues the
// same checksum over the payload, without copying them into one buffer. That is
// only correct if Extend behaves exactly like checksumming the concatenation.
TEST(Crc32c, ExtendEqualsChecksumOfConcatenation) {
  const std::string head = "\x01";
  const std::string tail = "payload bytes";

  const uint32_t incremental = crc32c::Extend(crc32c::Value(Bytes(head)), Bytes(tail));
  EXPECT_EQ(incremental, crc32c::Value(std::string_view(head + tail)));
}

// Feeding the same bytes one at a time must give the same answer as one call,
// which is what makes a streaming checksum over a growing record possible.
TEST(Crc32c, ByteAtATimeMatchesOneCall) {
  const std::string data = "the quick brown fox jumps over the lazy dog";

  uint32_t streamed = 0;
  for (const char c : data) {
    const auto byte = static_cast<uint8_t>(c);
    streamed = crc32c::Extend(streamed, &byte, 1);
  }
  EXPECT_EQ(streamed, crc32c::Value(std::string_view(data)));
}

// The whole point: a torn or corrupted record must not checksum correctly.
TEST(Crc32c, DetectsEverySingleBitFlip) {
  std::string data = "a record that is about to be damaged";
  const uint32_t original = crc32c::Value(std::string_view(data));

  for (size_t byte = 0; byte < data.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      data[byte] = static_cast<char>(data[byte] ^ (1 << bit));
      EXPECT_NE(crc32c::Value(std::string_view(data)), original)
          << "flipping bit " << bit << " of byte " << byte << " did not change the checksum";
      data[byte] = static_cast<char>(data[byte] ^ (1 << bit));  // restore
    }
  }
}

// Truncation is the common crash shape, so losing trailing bytes must change
// the checksum even when the lost bytes are zeros.
TEST(Crc32c, DetectsTruncation) {
  const std::vector<uint8_t> full(64, 0x00);
  const uint32_t full_crc = crc32c::Value(full.data(), full.size());

  for (size_t shorter = 0; shorter < full.size(); ++shorter) {
    EXPECT_NE(crc32c::Value(full.data(), shorter), full_crc) << "length " << shorter;
  }
}

}  // namespace
}  // namespace stratakv
