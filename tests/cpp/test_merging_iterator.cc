#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/status.h"

#include "internal_key.h"
#include "iterator.h"
#include "merging_iterator.h"

namespace stratakv {
namespace {

std::string Internal(std::string_view user_key, uint64_t sequence,
                     ValueType type = ValueType::kTypeValue) {
  std::string out;
  AppendInternalKey(&out, user_key, sequence, type);
  return out;
}

// An in-memory iterator over a pre-sorted list, so the merge can be tested
// without building SSTables.
class VectorIterator final : public Iterator {
 public:
  explicit VectorIterator(std::vector<std::pair<std::string, std::string>> entries)
      : entries_(std::move(entries)) {}

  bool Valid() const override { return index_ < entries_.size(); }
  void SeekToFirst() override { index_ = 0; }
  void Next() override { ++index_; }
  std::string_view key() const override { return entries_[index_].first; }
  std::string_view value() const override { return entries_[index_].second; }
  Status status() const override { return Status::OK(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t index_ = 0;
};

std::unique_ptr<Iterator> Make(std::vector<std::pair<std::string, std::string>> entries) {
  return std::make_unique<VectorIterator>(std::move(entries));
}

// Drains a merged iterator into a readable list of "user_key@seq=value".
std::vector<std::string> Drain(Iterator& iterator) {
  std::vector<std::string> out;
  for (iterator.SeekToFirst(); iterator.Valid(); iterator.Next()) {
    out.push_back(std::string(ExtractUserKey(iterator.key())) + "@" +
                  std::to_string(ExtractSequence(iterator.key())) + "=" +
                  std::string(iterator.value()));
  }
  return out;
}

TEST(MergingIterator, NoInputsYieldsNothing) {
  const InternalKeyComparator comparator;
  auto merged = NewMergingIterator(comparator, {});
  merged->SeekToFirst();
  EXPECT_FALSE(merged->Valid());
}

TEST(MergingIterator, EmptyInputsYieldNothing) {
  const InternalKeyComparator comparator;
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make({}));
  inputs.push_back(Make({}));
  auto merged = NewMergingIterator(comparator, std::move(inputs));
  merged->SeekToFirst();
  EXPECT_FALSE(merged->Valid());
}

TEST(MergingIterator, OneInputPassesStraightThrough) {
  const InternalKeyComparator comparator;
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make({{Internal("a", 1), "1"}, {Internal("b", 2), "2"}}));
  auto merged = NewMergingIterator(comparator, std::move(inputs));
  EXPECT_EQ(Drain(*merged), (std::vector<std::string>{"a@1=1", "b@2=2"}));
}

TEST(MergingIterator, InterleavesSeveralInputsInSortedOrder) {
  const InternalKeyComparator comparator;
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make({{Internal("a", 1), "a1"}, {Internal("d", 1), "d1"}}));
  inputs.push_back(Make({{Internal("b", 1), "b1"}, {Internal("e", 1), "e1"}}));
  inputs.push_back(Make({{Internal("c", 1), "c1"}, {Internal("f", 1), "f1"}}));

  auto merged = NewMergingIterator(comparator, std::move(inputs));
  EXPECT_EQ(Drain(*merged),
            (std::vector<std::string>{"a@1=a1", "b@1=b1", "c@1=c1", "d@1=d1", "e@1=e1", "f@1=f1"}));
}

// The property compaction depends on: all versions of one user key arrive
// together, newest first, however they were spread across the inputs.
TEST(MergingIterator, VersionsOfOneKeyArriveTogetherNewestFirst) {
  const InternalKeyComparator comparator;
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make({{Internal("k", 1), "oldest"}}));
  inputs.push_back(Make({{Internal("k", 9), "newest"}}));
  inputs.push_back(Make({{Internal("k", 5), "middle"}}));

  auto merged = NewMergingIterator(comparator, std::move(inputs));
  EXPECT_EQ(Drain(*merged), (std::vector<std::string>{"k@9=newest", "k@5=middle", "k@1=oldest"}));
}

TEST(MergingIterator, HandlesInputsOfVeryDifferentLengths) {
  const InternalKeyComparator comparator;
  std::vector<std::pair<std::string, std::string>> many;
  for (int i = 0; i < 500; ++i) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "k%04d", i * 2);
    many.emplace_back(Internal(buffer, 1), buffer);
  }
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make(std::move(many)));
  inputs.push_back(Make({{Internal("k0501", 1), "odd"}}));

  auto merged = NewMergingIterator(comparator, std::move(inputs));
  const std::vector<std::string> drained = Drain(*merged);
  EXPECT_EQ(drained.size(), 501U);

  // And the whole stream must be sorted.
  std::vector<std::string> keys;
  for (const std::string& entry : drained) keys.push_back(entry.substr(0, entry.find('@')));
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST(MergingIterator, SeekToFirstCanBeCalledAgain) {
  const InternalKeyComparator comparator;
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make({{Internal("a", 1), "1"}}));
  inputs.push_back(Make({{Internal("b", 1), "2"}}));

  auto merged = NewMergingIterator(comparator, std::move(inputs));
  const std::vector<std::string> first = Drain(*merged);
  const std::vector<std::string> second = Drain(*merged);
  EXPECT_EQ(first, second) << "restarting the merge must reproduce the same stream";
}

TEST(MergingIterator, PropagatesAnInputFailure) {
  class FailingIterator final : public Iterator {
   public:
    bool Valid() const override { return false; }
    void SeekToFirst() override {}
    void Next() override {}
    std::string_view key() const override { return {}; }
    std::string_view value() const override { return {}; }
    Status status() const override { return Status::Corruption("input", "deliberate"); }
  };

  const InternalKeyComparator comparator;
  std::vector<std::unique_ptr<Iterator>> inputs;
  inputs.push_back(Make({{Internal("a", 1), "1"}}));
  inputs.push_back(std::make_unique<FailingIterator>());

  auto merged = NewMergingIterator(comparator, std::move(inputs));
  merged->SeekToFirst();
  EXPECT_TRUE(merged->status().IsCorruption())
      << "a broken input must not be silently skipped: that would look like missing data";
}

}  // namespace
}  // namespace stratakv
