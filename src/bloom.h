#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A bloom filter: a compact bit array that answers "definitely not here" or
// "maybe here".
//
// This is what keeps reads fast as data grows. A key lives in exactly one file,
// but a point lookup may have to consult many, so most of those consultations
// are for keys the file does not contain. A filter answers those without
// touching the disk at all.
//
// "false" from MayContain is certain. "true" only means "maybe, go read the
// block" -- there are false positives but never false negatives.
namespace stratakv {

// A 32-bit hash, used by the filter and nothing else. Not cryptographic.
uint32_t BloomHash(std::string_view data, uint32_t seed);

// Accumulates key hashes, then sizes and builds the bit array in Finish().
//
// The size cannot be chosen earlier: it depends on how many keys the table ends
// up holding, which is only known once the last one has been added.
class BloomBuilder {
 public:
  explicit BloomBuilder(int bits_per_key);

  void Add(std::string_view user_key);

  // Number of keys added. Duplicates are counted, which only makes the filter
  // slightly larger than it needs to be.
  size_t KeyCount() const { return hashes_.size(); }

  // Returns the encoded filter block: the bit array followed by one byte
  // holding the number of probes, so a reader does not have to recompute it
  // from a bits-per-key setting that may since have changed.
  std::string Finish();

 private:
  int bits_per_key_;
  std::vector<uint32_t> hashes_;
};

// The read side, built from an encoded filter block.
class BloomFilter {
 public:
  // An empty or malformed block yields a filter that answers "maybe" for
  // everything, which is always safe: it costs a disk read, never correctness.
  explicit BloomFilter(std::string_view encoded);

  bool MayContain(std::string_view user_key) const;

  int probes() const { return probes_; }
  size_t bit_count() const { return bits_.size() * 8; }

 private:
  std::string bits_;
  int probes_ = 0;
  bool always_true_ = true;
};

}  // namespace stratakv
