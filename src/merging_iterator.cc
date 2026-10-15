#include "merging_iterator.h"

#include <memory>
#include <queue>
#include <string_view>
#include <utility>
#include <vector>

#include "stratakv/status.h"

#include "internal_key.h"
#include "iterator.h"

namespace stratakv {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(const InternalKeyComparator& comparator,
                  std::vector<std::unique_ptr<Iterator>> inputs)
      : comparator_(&comparator), inputs_(std::move(inputs)), heap_(Greater{comparator_}) {}

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    // Rebuild the heap from scratch so SeekToFirst can be called more than once.
    heap_ = Heap(Greater{comparator_});
    current_ = nullptr;
    status_ = Status::OK();

    for (const std::unique_ptr<Iterator>& input : inputs_) {
      input->SeekToFirst();
      if (!input->status().ok()) {
        status_ = input->status();
        return;
      }
      if (input->Valid()) heap_.push(input.get());
    }
    Advance();
  }

  void Next() override {
    if (current_ == nullptr) return;

    // Advance the iterator just consumed and put it back if it has more.
    current_->Next();
    if (!current_->status().ok()) {
      status_ = current_->status();
      current_ = nullptr;
      return;
    }
    if (current_->Valid()) heap_.push(current_);
    Advance();
  }

  std::string_view key() const override { return current_->key(); }
  std::string_view value() const override { return current_->value(); }
  Status status() const override { return status_; }

 private:
  // std::priority_queue is a MAX-heap, so a "greater than" comparison turns it
  // into a min-heap and top() becomes the smallest key. Python's heapq is a
  // min-heap already.
  struct Greater {
    const InternalKeyComparator* comparator;
    bool operator()(const Iterator* a, const Iterator* b) const {
      return comparator->Compare(a->key(), b->key()) > 0;
    }
  };

  using Heap = std::priority_queue<Iterator*, std::vector<Iterator*>, Greater>;

  void Advance() {
    if (heap_.empty()) {
      current_ = nullptr;
      return;
    }
    // top() then pop(): in C++ these are separate calls, unlike Python's
    // heapq.heappop which does both.
    current_ = heap_.top();
    heap_.pop();
  }

  const InternalKeyComparator* comparator_;
  // The vector owns the iterators; the heap only points at them.
  std::vector<std::unique_ptr<Iterator>> inputs_;
  Heap heap_;
  Iterator* current_ = nullptr;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(const InternalKeyComparator& comparator,
                                             std::vector<std::unique_ptr<Iterator>> inputs) {
  return std::make_unique<MergingIterator>(comparator, std::move(inputs));
}

}  // namespace stratakv
