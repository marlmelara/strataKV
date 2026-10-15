# P2 — SSTables, flush, and the read path

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are in [GLOSSARY.md](../GLOSSARY.md) or the build spec's Glossary tab; new C++ syntax is in
[CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

P1 kept everything in memory, so a big database would eventually run out of it. P2 moves
data to disk.

When the memtable passes 4 MB it is set aside as **immutable**, a fresh one takes over, and
a **background thread** writes the old one out as an **SSTable** — a sorted, immutable file.
Each SSTable carries a **bloom filter**, a small bit array that answers "definitely not
here" or "maybe here", so a lookup for an absent key usually costs no disk read at all, and
an **index** so a lookup that must read costs exactly one.

Reads now walk three layers, newest first: the active memtable, the immutable one, then the
SSTables newest to oldest, stopping at the first match.

The rule that keeps this safe: a log is deleted **only after** the SSTable built from it is
durable. Until then the log is the only copy.

## Data flow

### Trace 1: a write that triggers a rotation

1. **`DBImpl::Write`** takes the lock and calls `MakeRoomForWrite`
   ([src/db_impl.cc](../../src/db_impl.cc)).
2. `MakeRoomForWrite` compares `mem_->ApproximateMemoryUsage()` with
   `options_.memtable_bytes`. Under the limit, it returns immediately and the write proceeds
   exactly as in P1.
3. Over the limit, it rotates. **A new log is created and synced first**, because from the
   moment the handover happens new writes go to the new log while the old log is still the
   only durable copy of the old memtable.
4. A **sequence watermark** is written into the new log: a record with zero operations whose
   only job is to carry the current sequence number.
5. `imm_ = mem_`, a fresh `mem_` takes over, and the background thread is signalled.
6. The write then appends to the *new* log and inserts into the *new* memtable.

If a second rotation would be needed while the first flush is still running,
`MakeRoomForWrite` **waits** instead. That is what bounds memory: at most two memtables
exist at any moment.

### Trace 2: the background flush

In `DBImpl::BackgroundLoop`:

1. Wait until `imm_` is non-null or the database is shutting down.
2. Take a copy of `imm_`, the list of logs that produced it, and a new file number.
3. **Release the lock** and call `WriteLevel0Table`. A flush reads and writes megabytes;
   holding the lock across it would stall every writer and reader.
4. `WriteLevel0Table` iterates the memtable in sorted order, feeding `SSTableBuilder`, then
   `Finish()` writes the filter, index, and footer and **syncs the file**.
5. `CrashPoint("flush_after_sst_write")`.
6. `SyncDir` — the file's *contents* were durable after step 4, but its **name** is not
   until the directory is synced.
7. Reacquire the lock, open a reader, and install it at the **front** of the L0 list, since
   it is the newest and must shadow the others.
8. `CrashPoint("flush_before_wal_delete")`.
9. **Only now** delete the logs, and sync the directory again.

### Trace 3: `Get` across three layers

1. Under the lock, copy four things and release it: the two memtable pointers, the L0 list,
   and `last_sequence_` as a **snapshot**.
2. Search `mem_`, then `imm_`. Either may answer definitively — including "deleted".
3. Then each SSTable, newest first. For each: check the bloom filter (**usually no disk
   read**), binary-search the index for the one block that could hold the key, read that
   block, scan it.
4. Stop at the first table that answers. A tombstone is an answer.

## Annotated real code

### 1. `BloomBuilder::Finish` — sizing the filter and setting the bits

From [src/bloom.cc](../../src/bloom.cc):

```cpp
  const auto probes =
      std::clamp(static_cast<int>(std::lround(static_cast<double>(bits_per_key_) * 0.69314718)),
                 1, kMaxProbes);

  const size_t requested_bits = hashes_.size() * static_cast<size_t>(bits_per_key_);
  const size_t bit_count = std::max(kMinBits, (requested_bits + 7) / 8 * 8);
```

- `bits_per_key_ * 0.69314718` — the optimal probe count is `ln(2) x bits-per-key`: about 7
  for 10 bits. More probes set too many bits and raise false positives; fewer give each
  lookup too little evidence.
- `std::clamp(value, low, high)` — Python's `max(low, min(high, value))`.
- `(requested_bits + 7) / 8 * 8` — integer arithmetic rounding up to a whole number of
  bytes. The `+ 7` before dividing is the standard trick for ceiling division.
- `std::max(kMinBits, ...)` — a floor of 64 bits. A handful of keys in a tiny array would
  have a false-positive rate near 100%, which is worse than having no filter.

The size cannot be chosen earlier, which is why the builder collects hashes first: it
depends on how many keys the table ends up holding, and that is only known after the last
one has been added.

```cpp
    const uint32_t delta = (hash >> 17) | (hash << 15);
    for (int probe = 0; probe < probes; ++probe) {
      const size_t bit = hash % bit_count;
      encoded[bit / 8] = ... | static_cast<uint8_t>(1U << (bit % 8));
      hash += delta;
    }
```

- `(hash >> 17) | (hash << 15)` — a bit rotation, producing a second hash-like value from
  the first very cheaply. This is **double hashing**: every probe is derived from one hash
  by repeatedly adding a rotation of it, instead of computing seven independent hashes.
- `bit / 8` and `bit % 8` — the filter is bytes, so find the byte and then the bit inside
  it. `1U << (bit % 8)` builds a mask with just that bit set, and `|` turns it on.
- `hash += delta` — `uint32_t` arithmetic wraps at 2^32 automatically, which is fine here.
  In Python you would need `& 0xFFFFFFFF`.

The probe count is appended to the encoded block, so changing `bloom_bits_per_key` later
cannot make already-written files unreadable.

### 2. `SSTableReader::Open` — treating the file as untrusted

```cpp
  if (magic != kTableMagic) {
    return Status::Corruption(path, "bad magic number: not an SSTable, or truncated");
  }
  if (crc32c::Value(std::string_view(footer).substr(0, kFooterChecksumPrefix)) != stored_crc) {
    return Status::Corruption(path, "footer checksum mismatch");
  }

  const uint64_t metadata_limit = size - kFooterSize;
  if (filter_offset > metadata_limit || filter_size > metadata_limit - filter_offset || ...) {
    return Status::Corruption(path, "footer points outside the file");
  }
```

- The footer is **fixed-size and last**, so one read from the end of the file locates
  everything else. That is the only reason a reader can open a table without scanning it.
- The **magic number** is checked before the checksum, because it gives a much clearer error
  for a file that is not an SSTable at all.
- The bounds check matters more than it looks. Those offsets came *from the file being
  read*, so they are untrusted input. Without the check, a corrupt offset would make the
  reader try to allocate or read an arbitrary amount — and note the form
  `size > limit - offset` rather than `offset + size > limit`, because the latter can
  overflow and wrap around to a small number that passes.
- A file whose footer never landed — exactly what a crash during a flush leaves — fails
  here, which is what lets recovery discard it safely.

### 3. `SSTableReader::Get` — one disk read, usually zero

```cpp
  if (!filter_.MayContain(user_key)) return Status::OK();

  const auto block = std::lower_bound(
      index_.begin(), index_.end(), lookup_key,
      [this](const IndexEntry& entry, std::string_view key) {
        return comparator_.Compare(entry.last_key, key) < 0;
      });
  if (block == index_.end()) return Status::OK();
```

- `*found` is a second output, separating "this table does not have the key, keep searching
  older files" from "this table says it is deleted, stop". Both would otherwise look like
  "no value", and conflating them is how a deleted key comes back to life.
- The filter check returns for most absent keys **without touching the disk** — the entire
  reason filters exist. In an LSM tree a key lives in one file but a lookup may consult
  many, so most consultations are for keys the file does not hold.
- `std::lower_bound` — binary search for the first index entry whose last key is not less
  than the lookup key. That block is the only one that could contain it. Python:
  `bisect.bisect_left`.
- `[this](...) { ... }` — a **lambda**, an anonymous function. `[this]` captures the
  enclosing object so the comparator member is reachable inside.
- `index_.end()` means the key sorts after everything in this table.

```cpp
    if (comparator_.Compare(key, lookup_key) >= 0) {
      if (ExtractUserKey(key) != user_key) return Status::OK();
      *found = true;
      if (ExtractValueType(key) == ValueType::kTypeDeletion) return Status::NotFound("key");
      value->assign(entry_value);
      return Status::OK();
    }
```

- Scan forward until reaching where the key would be. Because internal keys sort newest
  first within a user key, the first entry at or past the lookup key is the newest visible
  version.
- Landing on a *different* user key means this table simply does not hold it.
- A tombstone here stops the search for good: older tables must not be consulted.

### 4. `MakeRoomForWrite` — rotation, and why the new log comes first

```cpp
    if (imm_ != nullptr) {
      background_done_.wait(lock);
      continue;
    }

    const uint64_t number = next_file_number_++;
    std::unique_ptr<WritableFile> file;
    if (auto s = env_.NewWritableFile(WalFileName(path_, number), &file); !s.ok()) return s;
    auto rotated = std::make_unique<WalWriter>(std::move(file));
    if (auto s = WriteSequenceWatermark(*rotated); !s.ok()) return s;
    if (auto s = env_.SyncDir(path_); !s.ok()) return s;
```

- `background_done_.wait(lock)` — a **condition variable**. It atomically releases the lock
  and sleeps until another thread signals, then reacquires it. Without releasing the lock,
  the flush could never finish and this would deadlock. Python:
  `threading.Condition.wait()`.
- The `while (true) { ... continue; }` loop around it is not decoration: a thread woken from
  a condition variable must **re-check** the condition, because it can wake spuriously or
  lose a race to another writer.
- Waiting rather than rotating again is what bounds memory to two memtables.
- The new log is created, written to, and its directory synced **before** the handover.
  Reverse that and there is a window where writes have nowhere durable to go.

### 5. `WriteSequenceWatermark` — the one design problem the spec left open

```cpp
Status DBImpl::WriteSequenceWatermark(WalWriter& wal) {
  const std::string payload = EncodeRecordPayload(last_sequence_ + 1, 0, {});
  return wal.AddRecord(AsBytes(payload), /*sync=*/true);
}
```

Fourteen characters of logic, for a bug that would otherwise be very hard to find.

**The problem.** Flushing a memtable deletes its log. Flush *every* memtable and every log
is gone, replaced by a fresh empty one. On the next open, recovery replays that empty log,
learns nothing, and starts sequence numbering from zero again. New writes then get
sequence numbers *lower* than the data already sitting in the SSTables — so they sort as
older, and a `Get` returns the stale value. The database would silently serve wrong data.

**The fix.** Every newly created log begins with a record carrying the current sequence
number and zero operations. It costs one tiny record per log and recovers numbering exactly.

- `count = 0` — a legitimately empty batch. The decoder already handles it, since
  `IterateContents` with a count of zero and no operations is valid.
- Recovery computes `last_sequence_ = sequence - 1` from it, so the next write continues
  precisely where the previous one stopped.
- The alternative was reading every SSTable at open just to find the highest sequence
  number, which is O(data) work on every open. P3's MANIFEST records it properly and this
  goes away.

`FlushTest.SequenceNumbersContinueAcrossAFullFlushAndReopen` is the test that fails without
it.

## New C++ features

Appended to [CPP_NOTES.md](../CPP_NOTES.md): `std::condition_variable` and the re-check
loop, `std::thread` with a lambda, `std::lower_bound` with a custom comparator, lambdas and
captures, `std::clamp`, ceiling division with integer arithmetic, bit manipulation
(`<<`, `>>`, `|`, `%`), and overflow-safe bounds checks.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): background flush, data block, double hashing,
false positive, filter block, footer, immutable memtable, index block, L0, probe, rotation,
sequence watermark, and SSTable.

## Struggles

See the [P2 debrief](../DEBRIEF.md). The short version: the sequence watermark above was
the one real design hole, and two test failures were both mistakes in the *tests* rather
than the engine — which is its own lesson about how to read a red result.

## Checkpoint

The gate is suspended for this build; see [STUDY.md](../STUDY.md). This phase's five
conceptual questions are in the spec's "Checkpoint quizzes" tab, and five written against
this code are at the end of the [P2 debrief](../DEBRIEF.md).
