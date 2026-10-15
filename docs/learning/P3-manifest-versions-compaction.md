# P3 — MANIFEST, versions, and compaction

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are in [GLOSSARY.md](../GLOSSARY.md) or the build spec's Glossary tab; new C++ syntax is in
[CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

P2 could write data to disk but never clean it up: overwritten values and deleted keys
stayed forever, and L0 grew without limit.

P3 adds **compaction**. Once four L0 files exist, they are merged with the L1 files whose
key ranges overlap them, and rewritten as fresh L1 files. During the merge, versions a
newer write has replaced are dropped and tombstones disappear. That is where space is
actually reclaimed.

The second half is bookkeeping. A **MANIFEST** records every change to the file set as an
appended edit, and **CURRENT** names the live MANIFEST, replaced atomically. This replaces
P2's scan of the directory with an authoritative record.

The piece that makes it safe to delete files while reads are running is a **Version**: an
immutable snapshot of the file set, held by `shared_ptr`. A read holds one, so its files
cannot be deleted underneath it.

## Data flow

### Trace 1: installing a change to the file set

Every change goes through `VersionSet::LogAndApply`
([src/version.cc](../../src/version.cc)):

1. The caller fills a `VersionEdit` — files added, files removed.
2. `FinalizeNewVersion` builds the resulting `Version`, **reusing the already-open readers**
   for files that survive and opening readers only for new ones.
3. The edit is appended to the MANIFEST and **synced**.
4. Only then does `current_` point at the new version.

That order is the guarantee. A crash between 3 and 4 leaves a MANIFEST that replays into
the new version. A crash before 3 leaves the old version intact and the new files
unreferenced, which recovery deletes as garbage. There is never a version referencing a
file the MANIFEST does not list.

### Trace 2: a compaction

In `DBImpl::RunCompaction`:

1. Under the lock, copy the current `Version` and compute the **smallest active snapshot**.
2. Release the lock. `CompactLevel0` ([src/compaction.cc](../../src/compaction.cc)) picks
   every L0 file plus the L1 files whose user-key ranges overlap them.
3. Build one iterator per input and merge them with a min-heap, so all versions of one user
   key arrive together, newest first.
4. Walk the merged stream, dropping what can never be read again, writing the rest into new
   L1 files split at ~8 MB — but only ever at a user-key boundary.
5. `CrashPoint("compaction_after_output")`: the outputs are durable but unreferenced.
6. `CrashPoint("compaction_before_manifest")`.
7. **Release the local Version reference**, then `LogAndApply` the swap and delete obsolete
   files.

Step 7's ordering is not decoration — see the struggles section.

### Trace 3: `Get`, and why compaction has to know about it

1. Under the lock: copy the memtable pointers, copy the current `Version`, take
   `last_sequence_` as the **snapshot**, and **register that snapshot**.
2. Release the lock and search: memtable, immutable memtable, then the version's L0 files
   newest-first, then the one L1 file whose range covers the key.
3. On exit, by any path, unregister the snapshot.

The registration is what stops a compaction from dropping a version this read still needs.

## Annotated real code

### 1. `SetCurrentFile` — the four-step dance

```cpp
  if (auto s = env.WriteFile(temporary, bare + "\n"); !s.ok()) return s;
  if (auto s = env.SyncFile(temporary); !s.ok()) return s;
  if (auto s = env.Rename(temporary, directory + "/CURRENT"); !s.ok()) return s;
  return env.SyncDir(directory);
```

Four steps, and every one matters:

1. **Write to a temporary file**, never to `CURRENT` itself, so a crash mid-write can only
   damage the temporary one.
2. **Sync it**, so the bytes are on disk before the file gets the real name. Skip this and a
   crash can leave `CURRENT` naming nothing, because the rename reached disk before the
   contents did.
3. **Rename**, which POSIX performs atomically: a reader sees the whole old file or the
   whole new one, never a mix.
4. **Sync the directory**, because a file's name lives in its directory, which is its own
   data on disk. Skip this and the rename itself can vanish.

This is the same dance databases, package managers, and text editors all use to save a file
safely. Python: write, `f.flush()`, `os.fsync(f.fileno())`, `os.replace()`, then `fsync` the
directory file descriptor.

### 2. The compaction drop rule — the hardest logic in the project

```cpp
    const bool newer_version_already_visible = last_sequence_for_key <= smallest_snapshot;

    const bool droppable_tombstone =
        type == ValueType::kTypeDeletion && sequence <= smallest_snapshot;

    const bool drop = newer_version_already_visible || droppable_tombstone;
    last_sequence_for_key = sequence;
```

`last_sequence_for_key` holds the sequence of the *previous* entry for this same user key,
and resets to the maximum at each new key. Because the merge delivers versions newest-first,
"previous" means "newer".

- **`newer_version_already_visible`**: a newer version of this key has already been written
  out, and it is visible at every snapshot that could still be read. So nothing can ever see
  this older one again, and it can go.
- **`droppable_tombstone`**: a tombstone exists only to hide older values. L1 is the bottom
  level and every overlapping L1 file is in this compaction, so there is nothing left below
  for it to hide — unless a live snapshot can still see past it.

**Why `smallest_snapshot` appears in both.** Drop a version a live read still needs and that
read breaks. Concretely: key `K` has version 10 (`A`) and version 20 (`B`). A read captures
snapshot 15 and starts searching. If compaction drops version 10 as shadowed, that read
seeks to `(K, 15)`, skips version 20 because 20 > 15, finds nothing below — and returns
**NotFound for a key that exists**. With `smallest_snapshot` at 15, processing version 20
leaves `last_sequence_for_key = 20`, and `20 <= 15` is false, so version 10 survives.

The matching tests are `KeepsAShadowedVersionThatALiveSnapshotStillNeeds` and
`KeepsATombstoneALiveSnapshotStillNeeds`.

**Compared to the learning guide (snippet 6).** The guide drops every entry after the first
for each user key, and drops tombstones at the bottom level. That is correct for a database
with no concurrent reads. The real code adds the snapshot condition, because reads here run
without a lock while compaction is in progress.

### 3. Splitting output only at a user-key boundary

```cpp
    if (new_user_key) {
      if (output.HasOpenFile() && output.CurrentSize() >= options.l1_file_bytes) {
        if (auto s = output.Finish(&outputs); !s.ok()) return s;
      }
```

The check happens when a **new user key** starts, never mid-key. If two versions of one key
landed in different L1 files, both files would claim that key, their ranges would overlap,
and the read path's binary search over L1 — which assumes no overlap — would silently miss
data.

`OutputSplitsOnlyAtUserKeyBoundaries` checks the resulting ranges are disjoint.

### 4. `Version` as a reference-counted snapshot

```cpp
    mem = mem_;
    imm = imm_;
    version = versions_->current();
    snapshot = versions_->last_sequence();
    active_read_snapshots_.insert(snapshot);
```

Copying a `shared_ptr` increments a reference count, so this read now **co-owns** the
version. Compaction can install a new one and delete the files it made obsolete, and this
read is unaffected: its version still holds open readers for the files it is using.

```cpp
void VersionSet::ForgetUnreferencedVersions() {
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [](const std::shared_ptr<const Version>& version) {
                                  return version.use_count() == 1;
                                }),
                 retired_.end());
}
```

`use_count() == 1` means the retired list holds the only pointer, so no reader can still be
searching it. Only then do its files become deletable.

This is **reference counting used as a concurrency tool**, and it is the same idea as MVCC
snapshots in Postgres: readers see a fixed version while writers move on. The alternative —
holding a lock across the disk reads — would block every writer for the duration.

`std::remove_if` plus `erase` is the **erase-remove idiom**: `remove_if` shuffles the keepers
to the front and returns where the leftovers start; `erase` then drops the tail. Python
would just build a new list with a comprehension.

### 5. The min-heap merge

```cpp
  struct Greater {
    const InternalKeyComparator* comparator;
    bool operator()(const Iterator* a, const Iterator* b) const {
      return comparator->Compare(a->key(), b->key()) > 0;
    }
  };

  using Heap = std::priority_queue<Iterator*, std::vector<Iterator*>, Greater>;
```

- `std::priority_queue` is a **max-heap** by default, so passing a "greater than" comparison
  flips it into a min-heap and `top()` becomes the smallest key. Python's `heapq` is already
  a min-heap.
- `operator()` makes `Greater` a **function object**: a struct callable like a function. It
  is used instead of a lambda here only because the type has to be named in the `using`
  declaration.
- The heap holds raw `Iterator*`; the `inputs_` vector owns the iterators. The heap just
  points at them.
- `top()` then `pop()` are separate calls in C++, unlike Python's `heapq.heappop` which does
  both.

The merge is why compaction has **write amplification**: every surviving entry is rewritten.

## New C++ features

Appended to [CPP_NOTES.md](../CPP_NOTES.md): `std::priority_queue` as a min-heap, function
objects, `std::multiset`, `shared_ptr::use_count`, the erase-remove idiom, `std::function`
as a parameter, tagged binary encodings, and RAII guards for paired operations.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): compaction input/output, CURRENT, MANIFEST,
MVCC, read amplification, smallest snapshot, space amplification, version, version edit,
and write amplification.

## Struggles

Three real bugs, all found by tests rather than by reading. See the
[P3 debrief](../DEBRIEF.md) for the full account; the one worth knowing here is that
`RemoveObsoleteFiles` deleted the MANIFEST it had just written, which destroyed the database
on the next open.

## Checkpoint

The gate is suspended for this build; see [STUDY.md](../STUDY.md). This phase's five
conceptual questions are in the spec's "Checkpoint quizzes" tab, and five written against
this code are at the end of the [P3 debrief](../DEBRIEF.md).
