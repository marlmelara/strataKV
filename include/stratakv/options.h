#pragma once

#include <cstddef>
#include <cstdint>

namespace stratakv {

// How hard a write works to reach the physical disk before Put returns.
//
// kSyncEveryWrite is the default because the project's central claim is that no
// acknowledged write is ever lost. Group commit (P4) shares one fsync across
// queued writers, which is what makes this policy affordable.
enum class SyncPolicy {
  kSyncEveryWrite = 0,
  kNoSync,
};

// Options fixed for the lifetime of an open database.
struct Options {
  // Rotate the active memtable once it holds this many bytes. 4 MB keeps the
  // skip list in cache and makes each flushed L0 file a convenient size.
  size_t memtable_bytes = 4u << 20;

  SyncPolicy sync_policy = SyncPolicy::kSyncEveryWrite;

  bool create_if_missing = true;
  bool error_if_exists = false;

  // --- Set in later phases, declared here so the shape of the engine is
  // --- visible from the start. ---

  // Target size of an SSTable data block (P2).
  size_t block_bytes = 4u << 10;
  // Bloom filter bits per key; 10 bits with 7 probes is about 1% false
  // positives (P2).
  int bloom_bits_per_key = 10;
  // Compact L0 into L1 once this many L0 files exist (P3).
  int l0_compaction_trigger = 4;
  // Split L1 output into files of about this size (P3).
  size_t l1_file_bytes = 8u << 20;
};

struct ReadOptions {
  // Verify block checksums on every read. Off trades safety for speed.
  bool verify_checksums = true;
};

struct WriteOptions {
  // Override the database's sync policy for this one write. Unset means follow
  // Options::sync_policy.
  bool sync = true;
};

}  // namespace stratakv
