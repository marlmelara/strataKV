# P5 — Python package, property tests, and the crash fuzzer

Written for someone fluent in Python and new to C++ and storage engines. Terms in **bold**
are in [GLOSSARY.md](../GLOSSARY.md) or the build spec's Glossary tab; new C++ syntax is in
[CPP_NOTES.md](../CPP_NOTES.md).

## Plain-English summary

The engine becomes usable from Python, and then gets tested far harder than any
hand-written test could manage.

The binding layer exposes a `DB` class with `get`, `put`, `delete`, `write_batch` and
`close`, plus the mapping syntax — `db[k]`, `k in db`, `del db[k]` — and a context manager.
Two things in it matter beyond plumbing: the **GIL** is released around every engine call,
so Python threads actually overlap with disk I/O, and each `Status` becomes the exception a
Python caller would expect.

Then the testing. **Hypothesis** drives the engine and a plain `dict` through thousands of
random operation sequences and checks after every step that they still agree. And the
**crash fuzzer** `SIGKILL`s a writing process at random moments, reopens the database, and
insists every acknowledged write is still there.

That fuzzer is how the project's central claim stops being a claim.

## Data flow

### Trace: `db[b"apple"]` from Python to disk and back

1. `DB.__getitem__` in [python/stratakv/\_\_init\_\_.py](../../python/stratakv/__init__.py)
   calls `self._core.get(key)`.
2. That is a pybind11-generated function reaching `PythonDB::Get`
   ([python/bindings.cc](../../python/bindings.cc)).
3. **While still holding the GIL**, the Python `bytes` is copied into a `std::string`. It has
   to happen here: a Python object may not be touched without the GIL, and the GIL is about
   to be released.
4. `py::gil_scoped_release` releases it. Other Python threads now run.
5. `DB::Get` searches memtable, immutable memtable, then the version's L0 and L1 files.
6. The scope ends, the GIL is reacquired automatically (RAII again).
7. `NotFound` becomes `None`; any other failure becomes an exception; a value becomes
   `py::bytes`.
8. Back in the wrapper, `None` becomes a `KeyError` — because `db[k]` should behave like a
   dict, while `db.get(k)` returns `None` like `dict.get`.

### Trace: one fuzzer iteration

1. The parent spawns [tests/py/fuzz_writer.py](../../tests/py/fuzz_writer.py) as a child,
   sometimes with `STRATAKV_CRASH_AT` set to a named crash point.
2. The child opens the database with `sync=True` and writes keys in a loop, printing each
   one **after** its write returns and flushing immediately.
3. The parent sleeps a random 0.2–0.6 seconds, then calls `child.kill()` — `SIGKILL`, which
   runs no cleanup, no destructors, and flushes no buffers.
4. The parent reads what the child managed to print. **That set is exactly the acknowledged
   writes**, because the child printed each key only after being told the write succeeded.
5. The parent reopens the database and demands every one of those keys back with the right
   value, then writes once more to prove the database is still usable rather than merely
   readable.

## Annotated real code

### 1. Releasing the GIL, and why the copy comes first

```cpp
  py::object Get(const py::bytes& key) {
    const std::string owned_key = key;
    std::string value;
    stratakv::Status status;
    {
      const py::gil_scoped_release no_gil;
      status = Engine().Get(stratakv::ReadOptions{}, owned_key, &value);
    }
    if (status.IsNotFound()) return py::none();
    ThrowIfError(status);
    return py::bytes(value);
  }
```

- `const std::string owned_key = key;` — pybind11 converts `py::bytes` to `std::string`,
  **copying** the data. This line must come before the release: touching a Python object
  without the GIL is undefined behavior, and `key` is a Python object.
- `py::gil_scoped_release no_gil;` — releases the GIL for the rest of the braces and
  reacquires it at the closing brace. RAII again, so an early return or a thrown exception
  cannot leave it released.
- **Why it matters:** the **GIL** lets only one thread run Python at a time. A C++ extension
  that holds it during a disk read blocks every other Python thread for the duration. Release
  it and they run. `test_engine_calls_release_the_gil` checks this directly — a counter
  thread spins while the main thread does 300 synced writes, and if the GIL were held the
  counter would barely move.
- `IsNotFound` is handled separately and returns `None`. A missing key is not an error at
  this layer; the wrapper decides whether it is a `KeyError`.

### 2. Turning a Status into the right Python exception

```cpp
    case stratakv::StatusCode::kCorruption:
      throw CorruptionError(message);
    case stratakv::StatusCode::kIOError:
      PyErr_SetString(PyExc_OSError, message.c_str());
      throw py::error_already_set();
    case stratakv::StatusCode::kInvalidArgument:
      throw std::invalid_argument(message);
```

- `CorruptionError` is registered with `py::register_exception`, which creates a real Python
  class. It derives from `StrataKVError`, so one `except` clause catches everything the
  engine raises.
- An I/O failure becomes `OSError` — the builtin a Python caller already catches for a failed
  file operation. Inventing a new type would only make it easier to miss.
- `std::invalid_argument` is one pybind11 already maps to `ValueError`, so no registration is
  needed.
- `PyErr_SetString` then `throw py::error_already_set()` is how C++ raises an already-set
  Python error. The portable spelling, rather than a newer helper, so the bindings build
  against older pybind11 too.

The pattern: **errors should arrive as the exception the caller would have guessed.** A
`CorruptionError` means the bytes were there and wrong; an `OSError` means they never
arrived. Collapsing both into `RuntimeError` would lose a distinction that decides what a
caller does next.

### 3. The model-based test

```python
    @rule(key=KEYS, value=VALUES)
    def put(self, key: bytes, value: bytes) -> None:
        self.db.put(key, value)
        self.model[key] = value

    @rule(key=KEYS)
    def get(self, key: bytes) -> None:
        assert self.db.get(key) == self.model.get(key), key

    @rule()
    def reopen(self) -> None:
        self.db.close()
        self.db = stratakv.DB.open(self.path, ...)
```

- `RuleBasedStateMachine` — Hypothesis calls these `@rule` methods in random orders with
  random arguments, thousands of times, checking assertions as it goes.
- `st.binary(min_size=0, max_size=8)` — a **strategy**, a recipe for random values. Keys are
  deliberately *short*: collisions become likely, so overwrites and deletes of existing keys
  actually get exercised instead of mostly missing.
- `self.model` is a plain `dict`, which is trivially correct. Every rule updates both, and
  `get` asserts they agree. **The dict is the specification.**
- `reopen` closes and reopens mid-sequence, so recovery runs constantly rather than only in
  tests written for it.
- **Shrinking** is what makes this practical. When an assertion fails after 300 random steps,
  Hypothesis searches for the shortest sequence that still fails — usually three or four
  steps — and prints it. A three-step reproduction of a recovery bug is debuggable; a
  three-hundred-step one is not.

### 4. The child that defines "acknowledged"

```python
            db.put(key.encode(), value.encode())

            # Printed only after put returned. This line IS the promise.
            sys.stdout.write(f"{key} {value}\n")
            sys.stdout.flush()
```

Three decisions, each of which the fuzzer's verdict depends on:

- **Printed after, not before.** The printed set is therefore exactly the writes the engine
  claimed to have made durable. Print first and the parent would demand writes that were
  never promised, and the fuzzer would report failures that are not failures.
- **Flushed every line.** `SIGKILL` runs no cleanup and flushes no buffers, so a buffered
  line would be lost in precisely the case that matters — and the parent would then *not*
  demand a write that really was promised, quietly weakening the check.
- **Each key written once per run.** If a key were written twice, a later *unacknowledged*
  write of it could legitimately be on disk after the crash, and the parent comparing against
  the earlier acknowledged value would report a failure that is not one.

### 5. Only acknowledged writes are checked

```python
        with stratakv.DB.open(directory) as db:
            for key, expected in acknowledged.items():
                actual = db.get(key.encode())
                if actual is None:
                    results.failures.append(f"[{label}] lost acknowledged write {key}")
```

A write that was in flight when the process died may or may not have survived, and **both
outcomes are correct** — the caller was never told it succeeded. Checking those would make
the fuzzer fail on perfectly legal behavior.

This is the precise meaning of the project's guarantee, and the reason it can be tested at
all: "acknowledged" is not a vague notion, it is "`put` returned".

Some iterations also arm a crash point, so the kill lands inside a flush or a compaction
rather than wherever the scheduler happened to be. Random kills find bugs probabilistically;
named crash points hit the dangerous windows deliberately. Using both is the strongest
answer to "how do you know recovery works?"

## New C++ features

Appended to [CPP_NOTES.md](../CPP_NOTES.md): `py::gil_scoped_release`, `py::bytes` and the
conversion rules, `py::register_exception`, raising a Python error from C++, and
`py::class_` with `def_static` and `def_property_readonly`.

## New terms

Appended to [GLOSSARY.md](../GLOSSARY.md): acknowledged write (sharpened), GIL, model-based
testing, shrinking, strategy, and SIGKILL.

## Struggles

See the [P5 debrief](../DEBRIEF.md). The short version: the first version of the stateful
test was forty-two times slower than it needed to be because of one badly chosen constant,
and the first version of the fuzzer verified nothing in half its iterations.

## Checkpoint

The gate is suspended for this build; see [STUDY.md](../STUDY.md). This phase's five
conceptual questions are in the spec's "Checkpoint quizzes" tab, and five written against
this code are at the end of the [P5 debrief](../DEBRIEF.md).
