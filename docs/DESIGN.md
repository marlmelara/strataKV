# Design

The binding design decisions live in the build spec; this file records the on-disk formats
and the reasoning in the repository itself, so the code and its justification travel
together.

## Shape of the engine

Writes go to a write-ahead log first, then into memory. Memory is periodically flushed to
immutable sorted files on disk, and a background thread merges those files. Reads check
the newest data first and stop at the first match.

The WAL is the only thing a write must reach before it is acknowledged. Everything after
that — memtable rotation, flush, compaction — happens in the background.

- **Write path:** `Put(k, v)` → assign sequence number → append record to WAL → `fsync`
  (per sync policy) → insert into the active memtable → acknowledge.
- **Read path:** active memtable → immutable memtable → L0 files newest to oldest (they
  may overlap) → the single L1 file whose key range covers the key (L1 files never
  overlap). Each SSTable checks its bloom filter before touching disk.
- **Compaction:** at 4 L0 files, merge all L0 files plus overlapping L1 files into new L1
  files, dropping overwritten versions and dropping tombstones, because L1 is the bottom
  level.

## Internal key

Every stored key is `user_key | seq (7 bytes) | type (1 byte, PUT or DELETE)`.

Ordering is user key ascending, then sequence number descending, so the newest version of
a key is always found first. A `Get` seeks to `(key, current_seq)` and takes the first
entry with a matching user key; if that entry is a DELETE, the answer is "not found" even
when older PUTs sit below it. Deletes are just newer entries, called tombstones.

## WAL record format

Little-endian throughout.

```
+-----------+-----------+-----------+------------------------------+
| crc32c(4) | length(4) | type(1)   | payload (length bytes)       |
+-----------+-----------+-----------+------------------------------+
payload = seq(8) | count(4) | { op(1) | klen(varint) | key | vlen(varint) | value } * count
```

The checksum covers the type byte plus the payload. One record is one atomic `WriteBatch`.

On recovery, records are replayed until the first record that is incomplete or fails its
checksum, and the file is truncated there. **A torn final record is normal after a crash
and is never an error, because it was never acknowledged.** A valid record appearing
*after* a bad one is a different matter: that is mid-file corruption and must be reported
as `Corruption`, not silently truncated.

## SSTable format

```
[data block 0][data block 1]...[data block N]   ~4 KB each, sorted internal keys
[filter block]                                  bloom filter over all user keys, 10 bits/key
[index block]                                   last key of each data block -> (offset, size)
[footer, 48 bytes]                              filter (offset,size) | index (offset,size) | magic | crc32c
```

Data blocks use a plain `klen | key | vlen | value` encoding. Prefix compression and
restart points are a stretch goal, not required.

## MANIFEST and CURRENT

The MANIFEST is an append-only log of version edits (file added to level, file removed,
next file number, last sequence number), using the same record framing as the WAL. CURRENT
holds the name of the active MANIFEST and is replaced atomically.

On open: read CURRENT → replay MANIFEST → replay any WAL newer than the last flushed
sequence number.

## Directory layout

```
mydb/
  CURRENT            -> "MANIFEST-000005"
  MANIFEST-000005
  000007.log         active WAL
  000004.sst         L0
  000006.sst         L1
  LOCK               flock() so two processes cannot open the same DB
```

## Durability rules the code must never break

These are the invariants that the whole project's central claim rests on. Each one has a
comment at its implementation site explaining why the order matters.

1. **A write is acknowledged only after its WAL record is `fsync`ed.** Not after the
   append — after the sync.
2. **Replacing a file atomically takes four steps, in order:** write a temp file, `fsync`
   the temp file, `rename` it over the target, then `fsync` the directory. Skip step 2 and
   a crash can leave the target pointing at an empty file, because the rename reached disk
   before the contents did. Skip step 4 and the rename itself can vanish.
3. **The old WAL is deleted only after the MANIFEST records the new SSTable.** The WAL is
   the only durable copy until that point. Reverse the order and a crash in between loses
   the data.
4. **On macOS, `fsync` is not enough.** It hands data to the drive but does not ask the
   drive to flush its own volatile cache, so `Env::Sync` uses `fcntl(fd, F_FULLFSYNC)`.
5. **A failed `fsync` is permanent, not retryable.** On Linux the kernel may drop the
   dirty pages and clear the error, so a second `fsync` can report success while the data
   is gone — the defect Postgres named "fsyncgate" in 2018. `PosixWritableFile` remembers
   a sync failure and returns it from every later call, so the engine can never acknowledge
   a write on top of a sync that failed.
6. **Tombstones may only be dropped at the bottom level.** A tombstone hides older PUTs in
   lower levels; drop it higher up and those values come back to life.

## Decisions made in P0 that the spec did not dictate

| Decision | Chose | Alternative | Why it won |
| --- | --- | --- | --- |
| Error representation | `StatusCode` enum + owned `std::string` message | LevelDB's packed `const char*` | Readable for a learner; the allocation only happens on the failure path, and an OK `Status` allocates nothing |
| Enforcing checked results | `[[nodiscard]]` on the `Status` class | Convention and code review | The documented cost of status returns is "every result must be checked", so the compiler should enforce it |
| Lock lifetime | RAII: destroying a `FileLock` unlocks | LevelDB's explicit `Env::UnlockFile` | A lock cannot be leaked by an early return; it also demonstrates RAII, which the engine relies on everywhere |
| `fsync` failure handling | Sticky: remembered and returned forever | Return the error once, let the caller retry | A retry can falsely succeed after the kernel discards the error (fsyncgate) |
| CRC32C table | `constexpr` table built at compile time | Runtime initialization on first use | No static initialization order to reason about, and no first-call cost |
| `Env` interface | Abstract base + `PosixEnv`, reached via `Env::Default()` | One concrete class | Leaves room for a fake Env in later phases without reworking call sites |
