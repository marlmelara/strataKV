#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace stratakv {

// A bump allocator: hands out pieces of large blocks and frees every block at
// once when it is destroyed.
//
// Skip-list nodes live here. Nothing is ever freed individually, which is
// precisely what makes lock-free reads safe: a reader can hold a raw pointer to
// a node with no risk of it being freed underneath, because nodes only die when
// the whole memtable does and no reader is left.
//
// Everything stored here must be trivially destructible, since freeing a block
// does not run destructors. That is why nodes hold string_views into
// arena-copied bytes rather than std::strings.
class Arena {
 public:
  Arena() = default;
  ~Arena() = default;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns `bytes` of uninitialized memory aligned to `alignment`, which must
  // be a power of two.
  char* Allocate(size_t bytes, size_t alignment);

  // Copies `data` into the arena and returns a view of the copy. The view stays
  // valid for the arena's lifetime.
  std::string_view CopyBytes(std::string_view data);

  // Bytes held by the arena's blocks. Used to decide when a memtable is full,
  // so it counts allocated capacity rather than bytes handed out.
  size_t MemoryUsage() const { return usage_; }

 private:
  static constexpr size_t kBlockSize = 4096;

  std::vector<std::vector<char>> blocks_;
  char* cursor_ = nullptr;
  size_t remaining_ = 0;
  size_t usage_ = 0;
};

}  // namespace stratakv
