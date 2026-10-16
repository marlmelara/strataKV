#include "sstable_builder.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "bloom.h"
#include "crc32c.h"
#include "internal_key.h"

namespace stratakv {
namespace {

std::span<const uint8_t> AsBytes(std::string_view text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

}  // namespace

SSTableBuilder::SSTableBuilder(std::unique_ptr<WritableFile> file, const Options& options)
    : file_(std::move(file)),
      block_bytes_(options.block_bytes),
      filter_(options.bloom_bits_per_key) {}

Status SSTableBuilder::AppendRaw(std::string_view data) {
  if (auto s = file_->Append(AsBytes(data)); !s.ok()) return s;
  offset_ += data.size();
  return Status::OK();
}

Status SSTableBuilder::Add(std::string_view internal_key, std::string_view value) {
  if (finished_) return Status::InvalidArgument("sstable", "Add after Finish");
  if (internal_key.size() < kInternalKeyTrailerSize) {
    return Status::InvalidArgument("sstable", "key is too short to be an internal key");
  }
  if (entry_count_ > 0 && comparator_.Compare(internal_key, last_key_) <= 0) {
    // Not a defensive nicety: binary search over the index and the scan within
    // a block both assume sorted order, so an out-of-order key would produce
    // silently unfindable entries.
    return Status::InvalidArgument("sstable", "keys must be added in increasing order");
  }

  // The filter indexes USER keys, because a lookup knows the user key but not
  // which sequence number it is looking for.
  filter_.Add(ExtractUserKey(internal_key));

  PutLengthPrefixed(&block_, internal_key);
  PutLengthPrefixed(&block_, value);

  last_key_.assign(internal_key);
  block_last_key_.assign(internal_key);
  if (entry_count_ == 0) smallest_key_.assign(internal_key);
  largest_key_.assign(internal_key);
  ++entry_count_;

  // Blocks are cut *after* crossing the target, so one entry is never split
  // across two blocks. An entry larger than the block size gets a block of its
  // own.
  if (block_.size() >= block_bytes_) return FlushDataBlock();
  return Status::OK();
}

Status SSTableBuilder::FlushDataBlock() {
  if (block_.empty()) return Status::OK();

  const uint64_t block_offset = offset_;
  const auto block_size = static_cast<uint32_t>(block_.size());
  if (auto s = AppendRaw(block_); !s.ok()) return s;

  // The checksum covers exactly the entry bytes, so a reader can verify a block
  // from the block alone without needing anything else about the file. This is
  // what makes corruption *inside* a block detectable rather than silently
  // returned to the caller as a value.
  std::string trailer;
  PutFixed32(&trailer, crc32c::Value(std::string_view(block_)));
  if (auto s = AppendRaw(trailer); !s.ok()) return s;

  // The recorded size excludes the checksum: it is the number of entry bytes,
  // and the trailer is part of the framing.
  index_.push_back(IndexEntry{block_last_key_, block_offset, block_size});
  block_.clear();
  block_last_key_.clear();
  return Status::OK();
}

Status SSTableBuilder::Finish() {
  if (finished_) return Status::InvalidArgument("sstable", "Finish called twice");
  finished_ = true;

  if (auto s = FlushDataBlock(); !s.ok()) return s;

  // Filter block.
  const std::string filter = filter_.Finish();
  const uint64_t filter_offset = offset_;
  if (auto s = AppendRaw(filter); !s.ok()) return s;
  const uint64_t filter_size = filter.size();

  // Index block: one entry per data block, holding that block's last key.
  std::string index;
  for (const IndexEntry& entry : index_) {
    PutLengthPrefixed(&index, entry.last_key);
    PutFixed64(&index, entry.offset);
    PutFixed32(&index, entry.size);
  }
  const uint64_t index_offset = offset_;
  if (auto s = AppendRaw(index); !s.ok()) return s;
  const uint64_t index_size = index.size();

  // Footer: fixed size and last, so a reader finds it by seeking to the end.
  std::string footer;
  PutFixed64(&footer, filter_offset);
  PutFixed64(&footer, filter_size);
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index_size);
  PutFixed64(&footer, kTableMagic);
  const uint32_t crc = crc32c::Value(std::string_view(footer));
  PutFixed32(&footer, crc);
  footer.resize(kFooterSize, '\0');  // Pad to the fixed width.
  if (auto s = AppendRaw(footer); !s.ok()) return s;

  // The table is not durable until this returns, and nothing may treat it as
  // existing before then.
  if (auto s = file_->Sync(); !s.ok()) return s;
  return file_->Close();
}

}  // namespace stratakv
