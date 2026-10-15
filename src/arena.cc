#include "arena.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace stratakv {
namespace {

// Bytes that must be skipped to bring `pointer` up to `alignment`.
size_t PaddingFor(const char* pointer, size_t alignment) {
  const auto address = reinterpret_cast<uintptr_t>(pointer);
  const size_t misaligned = address & (alignment - 1);
  return misaligned == 0 ? 0 : alignment - misaligned;
}

}  // namespace

char* Arena::Allocate(size_t bytes, size_t alignment) {
  size_t padding = PaddingFor(cursor_, alignment);

  if (remaining_ < padding + bytes) {
    // A fresh block. operator new[] returns memory aligned for any fundamental
    // type, but a caller may ask for more than that, so the padding is
    // recomputed below rather than assumed to be zero.
    const size_t block_bytes = std::max(kBlockSize, bytes + alignment);
    // Growing the outer vector moves the inner vectors, but a moved vector
    // keeps its heap buffer, so cursor_ stays valid.
    blocks_.emplace_back(block_bytes);
    usage_ += block_bytes;
    cursor_ = blocks_.back().data();
    remaining_ = block_bytes;
    padding = PaddingFor(cursor_, alignment);
  }

  char* const result = cursor_ + padding;
  cursor_ += padding + bytes;
  remaining_ -= padding + bytes;
  return result;
}

std::string_view Arena::CopyBytes(std::string_view data) {
  if (data.empty()) return {};
  char* destination = Allocate(data.size(), alignof(char));
  std::copy(data.begin(), data.end(), destination);
  return {destination, data.size()};
}

}  // namespace stratakv
