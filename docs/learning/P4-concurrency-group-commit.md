# P4 — Concurrency and group commit

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are in [GLOSSARY.md](../GLOSSARY.md) or the build spec's Glossary tab; new C++ syntax is in
[CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

Until now a write took the database's lock, appended to the log, and `fsync`ed — alone.
Eight threads writing meant eight `fsync`s, one after another, and `fsync` is the slowest
thing the engine does.

**Group commit** fixes that. Writers queue up; the one at the front becomes the **leader**.
It merges the batches waiting behind it into a single log record and issues **one `fsync`
for all of them**, then hands each waiting thread its result. An `fsync` costs about the
same for 100 bytes as for 100 KB, so sharing one multiplies throughput — and the busier the
database gets, the larger the groups become, so it speeds up under load rather than
degrading.

Measured here: **4.7× faster** with 8 writers, and a 60-second run with 8 writers and 8
readers is clean under ThreadSanitizer.

Postgres, MySQL, and Kafka all use this idea.

## Data flow

### Trace: three threads calling `Put` at once

1. Each thread builds a `Writer` on **its own stack** — the batch, whether it needs a sync, a
   `done` flag, a `status`, and a condition variable — and pushes a *pointer* to it onto
   `writers_` ([src/db_impl.cc](../../src/db_impl.cc)).
2. Each then waits until either `done` is set or it reaches the front of the queue. Thread A
   is first, so it wakes as the **leader**; B and C sleep.
3. A calls `MakeRoomForWrite`, rotating the memtable if it is full.
4. A calls `BuildBatchGroup`, which walks the queue from the front and appends B's and C's
   operations to its own, stopping at a size cap or at a writer whose durability it cannot
   satisfy.
5. A takes one sequence number for the whole group; the operations get consecutive numbers
   from it.
6. **A releases the lock** and does the slow part: one append, one `fsync`. Meanwhile D and E
   arrive and queue up — they will be the *next* group.
7. A inserts every operation into the memtable. It is still the only writer, which is exactly
   the single-writer contract the skip list requires.
8. A retakes the lock, advances `last_sequence_`, then pops B and C off the queue, sets their
   `status` and `done`, and wakes them. Each returns A's result as its own.
9. A wakes whoever is now at the front, who becomes the next leader.

The thing to sit with: **B's write was performed by A's thread.** B never touched the log.
That is why every writer must receive the right status, and why a batch must be applied
exactly once — the correctness tests for this phase matter more than the speed test.

## Annotated real code

### 1. Queueing and waiting

```cpp
  Writer self;
  self.batch = batch;
  self.sync = options_.sync_policy == SyncPolicy::kSyncEveryWrite && options.sync;

  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&self);
  self.cv.wait(lock, [this, &self] { return self.done || &self == writers_.front(); });
  if (self.done) return self.status;
```

- `Writer self;` — a plain local, on **this thread's stack**. Other threads reach it only
  through the pointer in the queue, and it stays alive because this thread is blocked inside
  this function the whole time. No allocation, no ownership question.
- `std::unique_lock` rather than `std::lock_guard`, because the lock has to be released and
  retaken in the middle of the function. A `lock_guard` cannot do that.
- `cv.wait(lock, predicate)` — atomically releases the lock and sleeps; on waking, the lock
  is held again. The predicate is re-checked each time, which is required: a condition
  variable may wake spuriously. Python: `cond.wait_for(lambda: ...)`.
- Two ways to wake: `self.done` means a leader already did our work, so just return its
  status. Otherwise we are at the front and it is our turn to lead.

### 2. Merging the queue

```cpp
  for (auto it = std::next(writers_.begin()); it != writers_.end(); ++it) {
    Writer* candidate = *it;

    if (candidate->sync && !*sync) break;

    if (operations->size() + candidate->batch->Contents().size() > limit) break;

    operations->append(candidate->batch->Contents());
    *count += candidate->batch->Count();
    last = candidate;
  }
```

- `std::next(writers_.begin())` — start at the second element, since the first is the leader
  whose batch is already in.
- `if (candidate->sync && !*sync) break;` — the subtle one. A follower that asked for a sync
  must not be folded into a group that is not going to sync: that would quietly give it a
  weaker guarantee than it requested. The group stops there, and that writer becomes the next
  leader and gets its own `fsync`. The reverse is fine — a writer that did not ask for a sync
  may be swept into one that does, since stronger durability harms nobody.
- The size cap stops one enormous batch at the front from making everyone behind it wait for
  megabytes of I/O. When the leader's own batch is small the cap is tightened, so a short
  write is not delayed by a group that grew behind it.
- `last` is returned so the leader knows exactly how far down the queue to wake.

### 3. Doing the I/O without the lock

```cpp
    WalWriter* wal = wal_.get();
    MemTable* memtable = mem_.get();
    const std::string payload = EncodeRecordPayload(sequence, count, operations);

    lock.unlock();
    status = wal->AddRecord(AsBytes(payload), group_sync);

    if (status.ok()) {
      MemTableInserter inserter(sequence, memtable);
      status = WriteBatch::IterateContents(operations, count, inserter);
    }
    lock.lock();
```

- The two raw pointers are captured **while the lock is held**, so the unlocked section
  touches no shared state. Only a leader ever changes `wal_` or `mem_`, and right now that
  leader is us — but reading them under the lock makes that argument unnecessary rather than
  merely true.
- `lock.unlock()` before the append is the entire optimization. The `fsync` is milliseconds;
  the queue fills behind us during it, and those writers become the next group. **Throughput
  rises with load.**
- Inserting into the memtable without the lock is safe because the skip list permits one
  writer plus any number of lock-free readers, and the queue guarantees there is exactly one
  leader at a time.
- The durability order is unchanged from P1: the record is synced *before* anything becomes
  visible in memory.

### 4. Handing out the results

```cpp
  while (true) {
    Writer* front = writers_.front();
    writers_.pop_front();
    if (front != &self) {
      front->status = status;
      front->done = true;
      front->cv.notify_one();
    }
    if (front == last) break;
  }

  if (!writers_.empty()) writers_.front()->cv.notify_one();
```

- Each writer in the group is popped, given the shared result, marked `done`, and woken. It
  then returns that status from its own `Write` call.
- `if (front != &self)` — we must not mark ourselves done or notify ourselves; we are about
  to return normally.
- The final `notify_one` wakes whoever is now at the front so they become the next leader.
  Forget this line and the queue stalls until the next arriving writer happens to wake it.
- Writers are woken with `notify_one` on their *own* condition variable rather than one
  shared `notify_all`, so a wake-up goes to exactly the thread it concerns instead of
  stampeding every waiter.

**Compared to the learning guide (snippet 8).** The shape is the same. The real code adds
the sync-compatibility rule, the group size caps, a `MakeRoomForWrite` that can fail before
a group is even built, and an `enable_group_commit` option — which exists only so the
speed-up can be *measured* against a baseline rather than asserted.

## New C++ features

Appended to [CPP_NOTES.md](../CPP_NOTES.md): `std::deque`, condition variables per waiter,
pointers to stack objects shared across threads, `std::next`, `notify_one` versus
`notify_all`, and `std::chrono` for timing.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): group commit, leader/follower, fault injection,
and throughput.

## Struggles

See the [P4 debrief](../DEBRIEF.md).

## Checkpoint

The gate is suspended for this build; see [STUDY.md](../STUDY.md). This phase's five
conceptual questions are in the spec's "Checkpoint quizzes" tab, and five written against
this code are at the end of the [P4 debrief](../DEBRIEF.md).
