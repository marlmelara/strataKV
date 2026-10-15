#pragma once

#include <memory>
#include <vector>

#include "stratakv/status.h"

#include "internal_key.h"
#include "iterator.h"

namespace stratakv {

// Merges several sorted iterators into one sorted stream, using a min-heap.
//
// This is what compaction reads through: several input tables at once, emerging
// in a single sorted order so that all versions of one user key arrive together
// and newest-first.
//
// Takes ownership of its inputs.
std::unique_ptr<Iterator> NewMergingIterator(const InternalKeyComparator& comparator,
                                             std::vector<std::unique_ptr<Iterator>> inputs);

}  // namespace stratakv
