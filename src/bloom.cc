#include "bloom.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "internal_key.h"

namespace stratakv {
namespace {

// The largest number of probes worth doing. Beyond this the filter is being
// asked to do something a bigger filter should do instead.
constexpr int kMaxProbes = 30;

// Minimum filter size. A handful of keys in a tiny bit array would have a
// false-positive rate near 100%, which is worse than having no filter.
constexpr size_t kMinBits = 64;

}  // namespace

// A Murmur-style mixing hash: cheap, and spreads keys evenly enough that the
// measured false-positive rate lands near the theoretical one. Evenness is the
// whole requirement here -- a hash that clusters would make the filter lie far
// more often than the math predicts.
uint32_t BloomHash(std::string_view data, uint32_t seed) {
  constexpr uint32_t kMultiplier = 0xC6A4A793;
  constexpr int kFinalShift = 24;

  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  size_t remaining = data.size();

  uint32_t hash = seed ^ (static_cast<uint32_t>(data.size()) * kMultiplier);

  // Four bytes at a time while there are at least four left.
  while (remaining >= 4) {
    hash += DecodeFixed32(bytes);
    hash *= kMultiplier;
    hash ^= hash >> 16;
    bytes += 4;
    remaining -= 4;
  }

  // Then whatever is left over, one byte at a time.
  switch (remaining) {
    case 3:
      hash += static_cast<uint32_t>(bytes[2]) << 16;
      [[fallthrough]];
    case 2:
      hash += static_cast<uint32_t>(bytes[1]) << 8;
      [[fallthrough]];
    case 1:
      hash += static_cast<uint32_t>(bytes[0]);
      hash *= kMultiplier;
      hash ^= hash >> kFinalShift;
      break;
    default:
      break;
  }
  return hash;
}

BloomBuilder::BloomBuilder(int bits_per_key) : bits_per_key_(std::max(1, bits_per_key)) {}

void BloomBuilder::Add(std::string_view user_key) {
  hashes_.push_back(BloomHash(user_key, 0xBC9F1D34));
}

std::string BloomBuilder::Finish() {
  // The optimal probe count is ln(2) x bits-per-key: about 7 for 10 bits. More
  // probes than that set too many bits and raise the false-positive rate;
  // fewer give each lookup too little evidence.
  const auto probes =
      std::clamp(static_cast<int>(std::lround(static_cast<double>(bits_per_key_) * 0.69314718)), 1,
                 kMaxProbes);

  const size_t requested_bits = hashes_.size() * static_cast<size_t>(bits_per_key_);
  const size_t bit_count = std::max(kMinBits, (requested_bits + 7) / 8 * 8);
  const size_t byte_count = bit_count / 8;

  std::string encoded(byte_count, '\0');
  for (const uint32_t base : hashes_) {
    uint32_t hash = base;
    // Double hashing: derive every probe from one hash by repeatedly adding a
    // rotation of it, instead of computing k independent hashes.
    const uint32_t delta = (hash >> 17) | (hash << 15);
    for (int probe = 0; probe < probes; ++probe) {
      const size_t bit = hash % bit_count;
      encoded[bit / 8] = static_cast<char>(static_cast<uint8_t>(encoded[bit / 8]) |
                                           static_cast<uint8_t>(1U << (bit % 8)));
      hash += delta;  // uint32_t arithmetic wraps on overflow, which is fine here.
    }
  }

  // The probe count travels with the filter, so changing bits_per_key later
  // cannot make old files unreadable.
  encoded.push_back(static_cast<char>(static_cast<uint8_t>(probes)));
  return encoded;
}

BloomFilter::BloomFilter(std::string_view encoded) {
  if (encoded.size() < 2) return;  // Nothing usable: stay "maybe" for everything.

  const auto probes = static_cast<int>(static_cast<uint8_t>(encoded.back()));
  if (probes < 1 || probes > kMaxProbes) return;

  bits_.assign(encoded.data(), encoded.size() - 1);
  probes_ = probes;
  always_true_ = false;
}

bool BloomFilter::MayContain(std::string_view user_key) const {
  if (always_true_) return true;

  const size_t bit_count = bits_.size() * 8;
  uint32_t hash = BloomHash(user_key, 0xBC9F1D34);
  const uint32_t delta = (hash >> 17) | (hash << 15);

  for (int probe = 0; probe < probes_; ++probe) {
    const size_t bit = hash % bit_count;
    // One unset bit is proof the key was never added.
    if ((static_cast<uint8_t>(bits_[bit / 8]) & static_cast<uint8_t>(1U << (bit % 8))) == 0) {
      return false;
    }
    hash += delta;
  }
  return true;
}

}  // namespace stratakv
