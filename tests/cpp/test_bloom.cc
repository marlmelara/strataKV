#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bloom.h"

namespace stratakv {
namespace {

std::string Key(int i) { return "key-" + std::to_string(i); }

std::string BuildFilter(int count, int bits_per_key = 10) {
  BloomBuilder builder(bits_per_key);
  for (int i = 0; i < count; ++i) builder.Add(Key(i));
  return builder.Finish();
}

TEST(Bloom, NeverHasFalseNegatives) {
  // The one guarantee that must hold absolutely: a key that was added must
  // never be reported absent. A false negative would lose data.
  constexpr int kKeys = 10000;
  const BloomFilter filter(BuildFilter(kKeys));

  for (int i = 0; i < kKeys; ++i) {
    EXPECT_TRUE(filter.MayContain(Key(i))) << "false negative for " << Key(i);
  }
}

// P2 acceptance: the false-positive rate must be between 0.5% and 2% on
// 100,000 absent keys at 10 bits per key.
TEST(Bloom, FalsePositiveRateIsAboutOnePercent) {
  constexpr int kPresent = 10000;
  constexpr int kAbsent = 100000;
  const BloomFilter filter(BuildFilter(kPresent));

  int false_positives = 0;
  for (int i = 0; i < kAbsent; ++i) {
    // Absent keys, drawn from a namespace that cannot collide with the present
    // ones.
    if (filter.MayContain("absent-" + std::to_string(i))) ++false_positives;
  }

  const double rate = static_cast<double>(false_positives) / kAbsent;
  // Printed so the number can be recorded in docs/NUMBERS.md rather than
  // guessed at: every figure in that file has to come from a real run.
  std::cout << "measured false-positive rate: " << rate * 100.0 << "% (" << false_positives
            << " of " << kAbsent << " absent keys, 10 bits/key, " << kPresent << " keys added)"
            << std::endl;

  EXPECT_GT(rate, 0.005) << "rate " << rate << " is suspiciously low: is the filter working?";
  EXPECT_LT(rate, 0.02) << "rate " << rate << " is too high for 10 bits per key";
}

TEST(Bloom, UsesSevenProbesForTenBitsPerKey) {
  // The optimal probe count is ln(2) x bits-per-key, which is 6.93 for 10 bits.
  const BloomFilter filter(BuildFilter(100));
  EXPECT_EQ(filter.probes(), 7);
}

TEST(Bloom, ProbeCountTracksBitsPerKey) {
  EXPECT_EQ(BloomFilter(BuildFilter(100, 1)).probes(), 1);
  EXPECT_EQ(BloomFilter(BuildFilter(100, 4)).probes(), 3);
  EXPECT_EQ(BloomFilter(BuildFilter(100, 10)).probes(), 7);
  EXPECT_EQ(BloomFilter(BuildFilter(100, 20)).probes(), 14);
}

// More bits per key must mean fewer false positives -- that is the whole
// trade-off being bought.
TEST(Bloom, MoreBitsPerKeyLowersTheFalsePositiveRate) {
  constexpr int kPresent = 5000;
  constexpr int kAbsent = 20000;

  const auto measure = [](int bits_per_key) {
    const BloomFilter filter(BuildFilter(kPresent, bits_per_key));
    int hits = 0;
    for (int i = 0; i < kAbsent; ++i) {
      if (filter.MayContain("absent-" + std::to_string(i))) ++hits;
    }
    return static_cast<double>(hits) / kAbsent;
  };

  const double rate_at_4 = measure(4);
  const double rate_at_10 = measure(10);
  const double rate_at_20 = measure(20);

  EXPECT_GT(rate_at_4, rate_at_10) << "4 bits should be worse than 10";
  EXPECT_GT(rate_at_10, rate_at_20) << "10 bits should be worse than 20";
  EXPECT_LT(rate_at_20, 0.001) << "20 bits per key should be well under 0.1%";
}

TEST(Bloom, HandlesAnEmptyFilter) {
  const BloomFilter filter(BuildFilter(0));
  // With no keys added, nothing should be reported present.
  EXPECT_FALSE(filter.MayContain("anything"));
}

TEST(Bloom, HandlesASingleKey) {
  BloomBuilder builder(10);
  builder.Add("only");
  const BloomFilter filter(builder.Finish());

  EXPECT_TRUE(filter.MayContain("only"));
  // Not a guarantee, but with a 64-bit minimum filter and one key, a handful of
  // specific misses should not all collide.
  int misses = 0;
  for (int i = 0; i < 100; ++i) {
    if (!filter.MayContain("other-" + std::to_string(i))) ++misses;
  }
  EXPECT_GT(misses, 50) << "a one-key filter should reject most other keys";
}

// A malformed or empty block must degrade to "maybe" for everything: that
// costs a disk read, never correctness.
TEST(Bloom, MalformedBlockAnswersMaybeForEverything) {
  for (const std::string encoded : {std::string(), std::string("x"), std::string("\xff\x00", 2)}) {
    const BloomFilter filter(encoded);
    EXPECT_TRUE(filter.MayContain("anything")) << "size " << encoded.size();
    EXPECT_TRUE(filter.MayContain("another"));
  }
}

TEST(Bloom, HandlesEmptyAndBinaryKeys) {
  BloomBuilder builder(10);
  builder.Add("");
  builder.Add(std::string("bin\0ary", 7));
  const BloomFilter filter(builder.Finish());

  EXPECT_TRUE(filter.MayContain(""));
  EXPECT_TRUE(filter.MayContain(std::string("bin\0ary", 7)));
}

TEST(Bloom, EncodedFilterSizeTracksBitsPerKey) {
  // 1000 keys at 10 bits each is about 1250 bytes, plus one byte of probe count.
  const std::string encoded = BuildFilter(1000, 10);
  EXPECT_GE(encoded.size(), 1250U);
  EXPECT_LE(encoded.size(), 1260U);
}

TEST(BloomHashFunction, SpreadsKeysAcrossTheOutputRange) {
  // A hash that clustered would make the filter lie far more often than the
  // theoretical rate predicts, so its evenness is worth checking directly.
  constexpr int kKeys = 20000;
  constexpr int kBuckets = 64;
  std::vector<int> buckets(kBuckets, 0);
  for (int i = 0; i < kKeys; ++i) {
    buckets[BloomHash(Key(i), 0xBC9F1D34) % kBuckets]++;
  }

  const int expected = kKeys / kBuckets;
  for (int bucket = 0; bucket < kBuckets; ++bucket) {
    EXPECT_GT(buckets[static_cast<size_t>(bucket)], expected / 2) << "bucket " << bucket;
    EXPECT_LT(buckets[static_cast<size_t>(bucket)], expected * 2) << "bucket " << bucket;
  }
}

TEST(BloomHashFunction, DiffersForSimilarKeysAndLengths) {
  std::set<uint32_t> seen;
  for (const char* key : {"", "a", "b", "ab", "ba", "abc", "abcd", "abcde", "abcdef"}) {
    seen.insert(BloomHash(key, 0xBC9F1D34));
  }
  EXPECT_EQ(seen.size(), 9U) << "short keys of different lengths collided";
}

}  // namespace
}  // namespace stratakv
