#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>

#include "arena.h"
#include "internal_key.h"

namespace stratakv {

// A sorted structure supporting one writer and any number of lock-free readers.
//
// A skip list is a sorted linked list where each node also has a random number
// of "express lane" pointers that skip ahead. A search starts in the highest
// lane and drops a level whenever the next node would overshoot, giving roughly
// O(log n) lookups like a balanced tree, but far simpler to make safe for
// concurrent readers.
//
// The concurrency contract, which the whole engine depends on:
//   - At most ONE thread calls Insert at a time (the caller holds the DB's
//     write lock).
//   - Any number of threads may read concurrently with that writer, holding no
//     lock at all.
//
// This works because a node is fully built before it is published with a single
// release store, and readers reach nodes only through acquire loads. Without
// that pairing a reader on another core could see the pointer to a node before
// seeing the node's contents and read garbage -- a bug that may never appear on
// x86 but shows up on ARM, which reorders more aggressively.
class SkipList {
 private:
  // Declared before Iterator, which holds a Node*. Defined further down.
  struct Node;

 public:
  SkipList(const InternalKeyComparator& comparator, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Copies key and value into the arena and links in the new node.
  // The caller must hold the write lock.
  void Insert(std::string_view key, std::string_view value);

  // Reads take no lock at all.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }

    void SeekToFirst() { node_ = list_->head_->next[0].load(std::memory_order_acquire); }

    // Positions at the first entry >= target in comparator order.
    void Seek(std::string_view target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    void Next() { node_ = node_->next[0].load(std::memory_order_acquire); }

    std::string_view key() const { return node_->key; }
    std::string_view value() const { return node_->value; }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  static constexpr int kMaxHeight = 12;
  // One node in four grows an extra level, so most nodes are short and a few
  // are tall express lanes.
  static constexpr uint32_t kBranching = 4;

  // Trivially destructible on purpose: the arena frees its blocks without
  // running destructors, so a std::string member here would leak its buffer.
  struct Node {
    std::string_view key;
    std::string_view value;
    int height;
    // An array of `height` pointers, allocated in the arena alongside the node.
    // Kept as a pointer rather than a trailing [1] array, because indexing past
    // the end of a one-element array is undefined behavior that UBSan rightly
    // rejects.
    std::atomic<Node*>* next;
  };

  Node* NewNode(std::string_view key, std::string_view value, int height);
  int RandomHeight();
  uint32_t NextRandom();

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  // True when `key` sorts strictly after `node`.
  bool KeyIsAfterNode(std::string_view key, Node* node) const {
    return node != nullptr && comparator_->Compare(node->key, key) < 0;
  }

  // The first node >= key. If `prev` is non-null, fills it with the last node
  // before the insertion point at each level: the nodes to be rewired.
  Node* FindGreaterOrEqual(std::string_view key, Node** prev) const;

  const InternalKeyComparator* comparator_;
  Arena* arena_;
  Node* head_;

  // Only the writer stores to this, so relaxed is enough. A reader that sees a
  // stale, smaller height simply starts its search lower down, which is still
  // correct; one that sees the new height finds either nullptr or a fully
  // published node through an acquire load.
  std::atomic<int> max_height_{1};

  // Used only by the single writer, so it needs no synchronization.
  uint32_t random_state_ = 0x2545F491;
};

inline SkipList::SkipList(const InternalKeyComparator& comparator, Arena* arena)
    : comparator_(&comparator), arena_(arena), head_(NewNode({}, {}, kMaxHeight)) {}

inline SkipList::Node* SkipList::NewNode(std::string_view key, std::string_view value, int height) {
  char* node_memory = arena_->Allocate(sizeof(Node), alignof(Node));
  char* next_memory = arena_->Allocate(sizeof(std::atomic<Node*>) * static_cast<size_t>(height),
                                       alignof(std::atomic<Node*>));

  for (int level = 0; level < height; ++level) {
    new (next_memory + static_cast<size_t>(level) * sizeof(std::atomic<Node*>))
        std::atomic<Node*>(nullptr);
  }

  return new (node_memory)
      Node{key, value, height, reinterpret_cast<std::atomic<Node*>*>(next_memory)};
}

inline uint32_t SkipList::NextRandom() {
  // xorshift32: small, fast, and good enough to pick node heights.
  random_state_ ^= random_state_ << 13;
  random_state_ ^= random_state_ >> 17;
  random_state_ ^= random_state_ << 5;
  return random_state_;
}

inline int SkipList::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && (NextRandom() % kBranching) == 0) {
    ++height;
  }
  return height;
}

inline SkipList::Node* SkipList::FindGreaterOrEqual(std::string_view key, Node** prev) const {
  Node* node = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = node->next[level].load(std::memory_order_acquire);
    if (KeyIsAfterNode(key, next)) {
      node = next;  // Keep walking right at this level.
      continue;
    }
    if (prev != nullptr) prev[level] = node;
    if (level == 0) return next;
    --level;  // Drop a level: the next node here would overshoot.
  }
}

inline void SkipList::Insert(std::string_view key, std::string_view value) {
  std::array<Node*, kMaxHeight> prev{};
  FindGreaterOrEqual(key, prev.data());

  const int height = RandomHeight();
  const int current_height = GetMaxHeight();
  if (height > current_height) {
    for (int level = current_height; level < height; ++level) {
      prev[static_cast<size_t>(level)] = head_;
    }
    // Raised before the node is linked in. A reader that sees the new height
    // reads head_->next[level], which is still nullptr until the loop below
    // publishes the node.
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* node = NewNode(arena_->CopyBytes(key), arena_->CopyBytes(value), height);
  for (int level = 0; level < height; ++level) {
    Node* predecessor = prev[static_cast<size_t>(level)];
    // No reader can see `node` yet, so relaxed is enough for its own pointer.
    node->next[level].store(predecessor->next[level].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    // Publish. Release means any reader that sees `node` also sees its key,
    // value, and forward pointers.
    predecessor->next[level].store(node, std::memory_order_release);
  }
}

}  // namespace stratakv
