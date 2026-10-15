# P1 — WAL, memtable, and crash recovery

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are in [GLOSSARY.md](../GLOSSARY.md) or the build spec's Glossary tab; new C++ syntax is in
[CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

This is the phase where the project's central promise becomes real: **if `Put` returns OK,
that write survives a crash.**

A write is first appended to a **write-ahead log** — a file that only ever grows — and
`fsync`ed. Only then does it go into memory, and only then is the caller told OK. If the
process dies at any point before that `fsync` returns, the half-written record is thrown
away on restart, which is correct because nobody was ever promised it.

In memory, writes land in a **memtable**: a **skip list** that one thread can write while
any number of threads read it without taking a lock. Deletes do not erase anything; they
append a **tombstone** that shadows older versions.

On reopen, the log is replayed to rebuild the memtable exactly as it was.

## Data flow

### Trace 1: `Put("apple", "red")`

1. **`DBImpl::Put`** ([src/db_impl.cc](../../src/db_impl.cc)) wraps the single operation in a
   one-entry `WriteBatch` and calls `Write`. Every write goes through the batch path, so
   there is only one durability routine to get right.
2. **`DBImpl::Write`** takes `mutex_`, so writers are serialized — the skip list permits
   only one writer at a time.
3. It assigns the next **sequence number**, a counter that defines "newer".
4. It builds the record payload: `seq(8) | count(4) | operations`.
5. **`WalWriter::AddRecord`** ([src/wal_writer.cc](../../src/wal_writer.cc)) prepends
   `crc32c(4) | length(4) | type(1)`, appends the bytes, and — because the sync policy is
   `kSyncEveryWrite` — calls `Sync()`, which is `F_FULLFSYNC` on macOS.
   **This is the moment the write becomes durable.**
6. **`MemTableInserter`** adds each operation to the memtable, numbering them consecutively
   from the batch's base sequence.
7. `last_sequence_` advances and `Write` returns OK. **Now the write is acknowledged.**

Steps 5 and 6 are in that order for one reason: if the process dies between them, replay
rebuilds the memtable from the log. If they were reversed, a crash would leave the write
visible in memory but absent from disk — and the next open would silently lose it.

### Trace 2: `Get("apple")`

1. **`DBImpl::Get`** takes `mutex_` just long enough to copy two things: the memtable
   pointer and `last_sequence_` as a **snapshot**. Then it releases the lock. Holding it
   across the search would block every writer.
2. **`MemTable::Get`** ([src/memtable.cc](../../src/memtable.cc)) builds a lookup internal
   key of `("apple", snapshot, kValueTypeForSeek)` and seeks.
3. Because internal keys sort by user key ascending then trailer **descending**, the first
   entry found for `"apple"` is the newest one at or below the snapshot. Anything newer
   sorts *before* the seek target and is skipped.
4. If that entry is a `kTypeValue`, its value is the answer. If it is a tombstone, the
   answer is `NotFound` — even though older values sit right below it in the list.

### Trace 3: reopening after a crash

1. **`DBImpl::Recover`** takes the `LOCK` file first, so a second process fails fast rather
   than corrupting the first one's log.
2. **`WalReader::Replay`** ([src/wal_reader.cc](../../src/wal_reader.cc)) scans the entire
   log *before applying anything*, deciding how much of it is trustworthy.
3. Good records are replayed into a fresh memtable, restoring sequence numbers exactly.
4. A torn tail is truncated away, and the truncation is itself `fsync`ed — along with the
   directory — so the next append starts on a record boundary even if the process dies
   again immediately.
5. The log is reopened **for appending**, not recreated. In P1 the log is the only durable
   copy of the data, so replacing it would throw everything away.

## Annotated real code

### 1. `DBImpl::Write` — the ordering that is the whole guarantee

```cpp
  const bool sync = options_.sync_policy == SyncPolicy::kSyncEveryWrite && options.sync;

  crash::CrashPoint("write_before_wal");

  if (auto s = wal_->AddRecord(AsBytes(payload), sync); !s.ok()) return s;

  crash::CrashPoint("write_after_wal_sync");

  MemTableInserter inserter(sequence, mem_.get());
  if (auto s = batch->Iterate(inserter); !s.ok()) return s;

  last_sequence_ = inserter.next_sequence() - 1;
  return Status::OK();
```

- `options_.sync_policy == ... && options.sync` — the database's policy and the per-write
  option are combined with `&&`, so `WriteOptions::sync` can *relax* durability for one
  write but never strengthen it past what the database promised.
- `crash::CrashPoint(...)` — a no-op unless `STRATAKV_CRASH_AT` names it, in which case the
  process dies instantly. The two calls bracket the durability step, so a test can
  interrupt either side of it on purpose.
- `if (auto s = ...; !s.ok()) return s;` — declare and test in one statement. Python 3.8+:
  `if not (s := ...).ok(): return s`.
- `mem_.get()` — a raw pointer borrowed from the `shared_ptr`. The inserter does not own
  the memtable and must not outlive it, which it does not: it dies at the end of the
  function.
- `inserter.next_sequence() - 1` — the inserter numbered each operation consecutively, so
  this is the highest sequence actually used. Deriving it rather than recomputing
  `sequence + count - 1` means the two can never disagree.

### 2. `WalWriter::AddRecord` — one record, one checksum, one fsync

```cpp
  const uint32_t crc = crc32c::Extend(crc32c::Value(&type, 1), payload.data(), payload.size());
  EncodeFixed32(header.data(), crc);
  EncodeFixed32(header.data() + 4, static_cast<uint32_t>(payload.size()));
  header[8] = type;

  if (auto s = file_->Append({header.data(), header.size()}); !s.ok()) return s;
  if (auto s = file_->Append(payload); !s.ok()) return s;
  return sync ? file_->Sync() : Status::OK();
```

- `crc32c::Extend(crc32c::Value(&type, 1), ...)` — checksum the one type byte, then
  continue the same checksum over the payload. This avoids copying both into one buffer,
  and it works only because `Extend` is equivalent to checksumming the concatenation —
  a property P0 has a dedicated test for.
- `header.data()` — `std::array` carries its own length, so the append length and the
  array length cannot drift apart.
- Two appends, not one — the header and payload are separate buffers. A crash between them
  leaves a torn record, which recovery is built to expect.
- `return sync ? file_->Sync() : ...` — the sync lives here rather than at the call site,
  so no caller can forget it. The record is durable only when this returns OK.

### 3. `WalReader::Scan` — telling a torn tail from real corruption

This is the most interesting logic in the phase, and the part the teaching snippet
deliberately simplifies.

```cpp
    if (record.state == RecordState::kIncomplete) {
      break;                      // Bytes are missing: this is the end of the file.
    }

    if (record.state == RecordState::kBadChecksum) {
      if (!bad_seen) { bad_seen = true; first_bad_offset = offset; }
      offset = next_offset;       // Keep scanning: what follows decides what this was.
      continue;
    }

    if (bad_seen) {
      result->corruption = true;  // A good record AFTER a bad one.
      return Status::OK();
    }
```

- A record whose bytes are simply missing can only be at the end of the file, so it is a
  torn tail by definition.
- A record whose bytes are all present but whose checksum fails is **ambiguous**. It could
  be the last thing a crash was writing, or it could be damage in the middle of a file
  that was fully written. The code cannot tell yet, so it remembers the position and
  keeps going.
- **What follows is what decides.** If a later record checksums correctly, the file was
  complete and something damaged it afterwards — that is `Corruption`, and truncating it
  would throw away good data. If nothing valid follows, it was the tail, and truncating is
  correct.
- `return Status::OK()` with `corruption = true` — the *scan* succeeded; it is the caller
  that turns the finding into a `Corruption` status. Separating "the scan worked" from
  "the file is good" keeps I/O errors distinguishable from damage.

The scan is a separate pass from the replay on purpose. Applying records as they were read
would mean replaying records that sit *after* mid-file damage, which must instead be
refused outright.

**Compared to the learning guide (snippet 1).** The guide's `Replay` truncates at the first
bad record in a single pass, and its "what to understand" note says the real reader should
also detect valid records after a bad one. That note is the whole difference: this
implementation scans first, then applies, which is what makes the distinction possible.

### 4. `SkipList::Insert` — publishing a node safely

```cpp
  Node* node = NewNode(arena_->CopyBytes(key), arena_->CopyBytes(value), height);
  for (int level = 0; level < height; ++level) {
    Node* predecessor = prev[static_cast<size_t>(level)];
    node->next[level].store(predecessor->next[level].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    predecessor->next[level].store(node, std::memory_order_release);
  }
```

- `arena_->CopyBytes(key)` — the key and value are copied into the **arena**, so the
  caller's buffers can be reused or destroyed immediately.
- `node->next[level].store(..., relaxed)` — no reader can reach `node` yet, so there is
  nothing to synchronize with. Relaxed means "atomic, but with no ordering promises."
- `predecessor->next[level].store(node, release)` — **the publication.** A release store
  guarantees that any thread doing an **acquire** load of that same pointer also sees
  everything written before it: the node's key, value, and forward pointers.
- If this were `relaxed`, a reader on another core could see the pointer to `node` before
  seeing the node's contents, and read uninitialized memory. On x86 it might never fail;
  on ARM, including Apple Silicon, it would.

The matching acquire is in `FindGreaterOrEqual`:

```cpp
    Node* next = node->next[level].load(std::memory_order_acquire);
```

Nodes are never freed individually — the whole arena dies with the memtable — which is why
a reader can hold a raw `Node*` with no risk of it being freed underneath.

### 5. `MemTable::Get` — why a tombstone returns `true`

```cpp
  const std::string_view found = iterator.key();
  if (ExtractUserKey(found) != user_key) return false;  // Landed on a different key.

  switch (ExtractValueType(found)) {
    case ValueType::kTypeValue:
      value->assign(iterator.value());
      *status = Status::OK();
      return true;
    case ValueType::kTypeDeletion:
      *status = Status::NotFound("key");
      return true;
  }
```

- The return value answers "**did this table decide the question?**", not "was the key
  found?" Those are different, and conflating them is how a deleted key comes back to life.
- `return false` means "I know nothing about this key, keep searching older data." From P2
  that caller goes on to the immutable memtable, then L0 files, then L1.
- A tombstone returns `true` with a `NotFound` status: the table *did* decide, and the
  answer is that the key is gone. Returning `false` here would let an older value in a
  lower level show through.
- `value->assign(iterator.value())` — copies out of the arena, because the caller's string
  must stay valid after the memtable is gone.

## New C++ features

Appended to [CPP_NOTES.md](../CPP_NOTES.md): `std::atomic<T>` with explicit memory orders,
placement `new`, abstract interfaces as callbacks (`WriteBatch::Handler`), `std::mutex` with
`std::lock_guard`, `std::shared_ptr` for shared ownership, `std::span`, nested classes,
forward declarations inside a class, and why a destructor must swallow exceptions.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): bump allocator, group commit (previewed),
monotonic sequence number, placement new, publication, release/acquire pairing (expanded),
snapshot, torn write, trailer, and write amplification (previewed).

## Struggles

See the [P1 debrief](../DEBRIEF.md) for the bugs and the decisions. The short version: the
only genuinely hard design question in this phase was not writing the skip list — the
teaching snippet is accurate and translates directly — but deciding **what a failed
checksum means**, which is answered only by what comes after it in the file.

## Checkpoint

The gate is suspended for this build; see [STUDY.md](../STUDY.md). This phase's five
conceptual questions are in the spec's "Checkpoint quizzes" tab, and five written against
this code are at the end of the [P1 debrief](../DEBRIEF.md).
