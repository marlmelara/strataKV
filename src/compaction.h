#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "stratakv/options.h"
#include "stratakv/status.h"

#include "env.h"
#include "internal_key.h"
#include "version.h"

namespace stratakv {

// What a compaction did, for tests and for the numbers that go in NUMBERS.md.
struct CompactionStats {
  uint64_t input_files = 0;
  uint64_t output_files = 0;
  uint64_t entries_read = 0;
  uint64_t entries_written = 0;
  uint64_t entries_dropped = 0;  // Shadowed versions and dropped tombstones.
  uint64_t bytes_written = 0;
};

// Merges every L0 file, plus the L1 files whose key ranges overlap them, into
// fresh non-overlapping L1 files.
//
// This is where space is actually reclaimed: overwritten versions and deleted
// keys stop existing. It is also where write amplification comes from, since
// every surviving entry is rewritten.
//
// `smallest_snapshot` is the oldest sequence number any live read might still
// need. A version at or below it may be dropped once a newer version visible at
// that snapshot has been emitted; anything above it must be kept, or a read in
// flight would stop being able to see the value it was promised.
//
// On success `edit` describes the swap -- every input removed, every output
// added at L1 -- ready to be handed to VersionSet::LogAndApply.
Status CompactLevel0(Env& env, const std::string& directory, const Options& options,
                     const Version& version, uint64_t smallest_snapshot,
                     const std::function<uint64_t()>& allocate_file_number, VersionEdit* edit,
                     CompactionStats* stats);

}  // namespace stratakv
