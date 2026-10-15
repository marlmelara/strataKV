#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "internal_key.h"

namespace stratakv {
namespace {

TEST(Fixed, RoundTripsLittleEndian) {
  uint8_t buffer[8];

  EncodeFixed32(buffer, 0x12345678);
  EXPECT_EQ(buffer[0], 0x78) << "least significant byte must come first";
  EXPECT_EQ(buffer[3], 0x12);
  EXPECT_EQ(DecodeFixed32(buffer), 0x12345678U);

  EncodeFixed64(buffer, 0x0123456789ABCDEFULL);
  EXPECT_EQ(buffer[0], 0xEF);
  EXPECT_EQ(buffer[7], 0x01);
  EXPECT_EQ(DecodeFixed64(buffer), 0x0123456789ABCDEFULL);
}

TEST(Fixed, HandlesExtremes) {
  uint8_t buffer[8];
  EncodeFixed32(buffer, 0);
  EXPECT_EQ(DecodeFixed32(buffer), 0U);
  EncodeFixed32(buffer, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(DecodeFixed32(buffer), std::numeric_limits<uint32_t>::max());
  EncodeFixed64(buffer, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(DecodeFixed64(buffer), std::numeric_limits<uint64_t>::max());
}

TEST(Varint, UsesFewerBytesForSmallNumbers) {
  std::string out;
  PutVarint32(&out, 1);
  EXPECT_EQ(out.size(), 1U);

  out.clear();
  PutVarint32(&out, 300);
  EXPECT_EQ(out.size(), 2U) << "300 should take two bytes, not four";
  EXPECT_EQ(static_cast<uint8_t>(out[0]), 0xAC);
  EXPECT_EQ(static_cast<uint8_t>(out[1]), 0x02);

  out.clear();
  PutVarint32(&out, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(out.size(), 5U) << "a 32-bit number needs at most five 7-bit groups";
}

TEST(Varint, RoundTripsAcrossBoundaries) {
  for (const uint32_t value : {0U, 1U, 127U, 128U, 129U, 16383U, 16384U, 300U, 1U << 21,
                               (1U << 28) - 1, 1U << 28, std::numeric_limits<uint32_t>::max()}) {
    std::string encoded;
    PutVarint32(&encoded, value);
    std::string_view input = encoded;
    uint32_t decoded = 0;
    ASSERT_TRUE(GetVarint32(&input, &decoded)) << value;
    EXPECT_EQ(decoded, value);
    EXPECT_TRUE(input.empty()) << "decoder must consume exactly the bytes it used";
  }
}

TEST(Varint, AdvancesTheCursorAndLeavesTheRest) {
  std::string encoded;
  PutVarint32(&encoded, 300);
  encoded += "trailing";

  std::string_view input = encoded;
  uint32_t decoded = 0;
  ASSERT_TRUE(GetVarint32(&input, &decoded));
  EXPECT_EQ(decoded, 300U);
  EXPECT_EQ(input, "trailing");
}

// Bytes read from disk are never trusted: a malformed varint must be rejected
// rather than producing a plausible-looking number.
TEST(Varint, RejectsTruncatedAndOverlongInput) {
  // Every byte says "more follows" but the input ends.
  const std::string truncated = "\x80\x80";
  std::string_view input = truncated;
  uint32_t decoded = 0;
  EXPECT_FALSE(GetVarint32(&input, &decoded));

  // Six continuation bytes: longer than any valid 32-bit varint.
  const std::string overlong = "\x80\x80\x80\x80\x80\x01";
  input = overlong;
  EXPECT_FALSE(GetVarint32(&input, &decoded));

  input = std::string_view{};
  EXPECT_FALSE(GetVarint32(&input, &decoded));
}

TEST(LengthPrefixed, RoundTripsIncludingEmptyAndBinary) {
  const std::string binary("a\0b\xFF", 4);
  std::string encoded;
  PutLengthPrefixed(&encoded, "first");
  PutLengthPrefixed(&encoded, "");
  PutLengthPrefixed(&encoded, binary);

  std::string_view input = encoded;
  std::string_view piece;
  ASSERT_TRUE(GetLengthPrefixed(&input, &piece));
  EXPECT_EQ(piece, "first");
  ASSERT_TRUE(GetLengthPrefixed(&input, &piece));
  EXPECT_EQ(piece, "");
  ASSERT_TRUE(GetLengthPrefixed(&input, &piece));
  EXPECT_EQ(piece, binary) << "values may contain zero bytes";
  EXPECT_TRUE(input.empty());
}

TEST(LengthPrefixed, RejectsALengthLongerThanTheData) {
  std::string encoded;
  PutVarint32(&encoded, 100);  // Claims 100 bytes...
  encoded += "short";          // ...but provides five.

  std::string_view input = encoded;
  std::string_view piece;
  EXPECT_FALSE(GetLengthPrefixed(&input, &piece));
}

TEST(InternalKey, PacksSequenceAboveTypeAndRoundTrips) {
  std::string key;
  AppendInternalKey(&key, "apple", 42, ValueType::kTypeValue);

  EXPECT_EQ(key.size(), 5U + kInternalKeyTrailerSize);
  EXPECT_EQ(ExtractUserKey(key), "apple");
  EXPECT_EQ(ExtractSequence(key), 42U);
  EXPECT_EQ(ExtractValueType(key), ValueType::kTypeValue);

  std::string tombstone;
  AppendInternalKey(&tombstone, "apple", 43, ValueType::kTypeDeletion);
  EXPECT_EQ(ExtractValueType(tombstone), ValueType::kTypeDeletion);
  EXPECT_EQ(ExtractSequence(tombstone), 43U);
}

TEST(InternalKey, HandlesTheLargestRepresentableSequence) {
  std::string key;
  AppendInternalKey(&key, "k", kMaxSequenceNumber, ValueType::kTypeValue);
  EXPECT_EQ(ExtractSequence(key), kMaxSequenceNumber);
}

TEST(InternalKey, SupportsEmptyAndBinaryUserKeys) {
  std::string empty_key;
  AppendInternalKey(&empty_key, "", 7, ValueType::kTypeValue);
  EXPECT_EQ(ExtractUserKey(empty_key), "");
  EXPECT_EQ(ExtractSequence(empty_key), 7U);

  const std::string binary("a\0b", 3);
  std::string binary_key;
  AppendInternalKey(&binary_key, binary, 8, ValueType::kTypeValue);
  EXPECT_EQ(ExtractUserKey(binary_key), binary);
}

// The ordering rule the whole read path depends on.
TEST(InternalKeyComparator, OrdersUserKeysAscending) {
  const InternalKeyComparator comparator;
  std::string a;
  std::string b;
  AppendInternalKey(&a, "apple", 1, ValueType::kTypeValue);
  AppendInternalKey(&b, "banana", 1, ValueType::kTypeValue);

  EXPECT_LT(comparator.Compare(a, b), 0);
  EXPECT_GT(comparator.Compare(b, a), 0);
  EXPECT_EQ(comparator.Compare(a, a), 0);
}

TEST(InternalKeyComparator, OrdersSequenceNumbersDescendingSoNewestIsFirst) {
  const InternalKeyComparator comparator;
  std::string older;
  std::string newer;
  AppendInternalKey(&older, "apple", 10, ValueType::kTypeValue);
  AppendInternalKey(&newer, "apple", 20, ValueType::kTypeValue);

  EXPECT_LT(comparator.Compare(newer, older), 0)
      << "a larger sequence number must sort earlier, so a Get finds it first";
}

// A Get seeks with kValueTypeForSeek, which must sort at or before any real
// entry carrying the same sequence number.
TEST(InternalKeyComparator, SeekTypeSortsFirstWithinOneSequence) {
  const InternalKeyComparator comparator;
  std::string seek;
  std::string deletion;
  AppendInternalKey(&seek, "apple", 10, kValueTypeForSeek);
  AppendInternalKey(&deletion, "apple", 10, ValueType::kTypeDeletion);

  EXPECT_LE(comparator.Compare(seek, deletion), 0);
}

TEST(InternalKeyComparator, ShorterUserKeySortsBeforeItsExtension) {
  const InternalKeyComparator comparator;
  std::string shorter;
  std::string longer;
  AppendInternalKey(&shorter, "app", 1, ValueType::kTypeValue);
  AppendInternalKey(&longer, "apple", 1, ValueType::kTypeValue);

  EXPECT_LT(comparator.Compare(shorter, longer), 0);
}

// The trailer must not leak into the user-key comparison: a user key whose
// bytes resemble a trailer still has to compare as a user key.
TEST(InternalKeyComparator, IgnoresTrailerBytesWhenComparingUserKeys) {
  const InternalKeyComparator comparator;
  const std::string tricky("key\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 11);
  std::string a;
  std::string b;
  AppendInternalKey(&a, tricky, 1, ValueType::kTypeValue);
  AppendInternalKey(&b, tricky, 2, ValueType::kTypeValue);

  EXPECT_EQ(ExtractUserKey(a), tricky);
  EXPECT_GT(comparator.Compare(a, b), 0) << "sequence 2 is newer, so it sorts first";
}

}  // namespace
}  // namespace stratakv
