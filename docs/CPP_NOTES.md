# C++ notes

C++ features used in the real code, each explained next to the closest Python. Written for
someone fluent in Python and new to C++; each stage appends the features it introduced.

## P0

### `#pragma once`

The first line of every header. It tells the compiler to include this file only once per
translation unit, even if several files `#include` it. Without it, a class would be defined
twice and the compile would fail.

The older way to do this is an include guard (`#ifndef STRATAKV_STATUS_H_ ... #endif`).
`#pragma once` is not in the C++ standard, but every compiler the project targets supports
it, and it cannot get out of sync with the filename.

Python has no equivalent: `import` already caches modules.

### `[[nodiscard]]`

An attribute that makes the compiler warn when a return value is thrown away.

```cpp
class [[nodiscard]] Status { ... };

file->Sync();            // warning: ignoring return value
(void)file->Sync();      // explicitly ignored, no warning
```

Applied to the *class* rather than to individual functions, so every function anywhere that
returns a `Status` is covered automatically. This is what turns "every result must be
checked" from a rule people remember into a rule the compiler enforces.

### `constexpr` functions and compile-time data

`constexpr` means "this can be computed at compile time." The CRC32C table is built by a
`constexpr` function, so the 256 entries are baked into the binary as read-only data:

```cpp
constexpr std::array<uint32_t, 256> MakeTable() { ... }
constexpr std::array<uint32_t, 256> kTable = MakeTable();
```

There is no first-call cost and no static initialization order to reason about. Python's
closest idea is computing a module-level constant at import time, except here it happens
before the program ever runs.

### `std::array<T, N>`

A fixed-size array that knows its own length, unlike the raw `T[N]` inherited from C. Use
it whenever the size is known at compile time; `std::vector` is for sizes that are not.

Python closest: a `tuple` of fixed length, except mutable.

### `override` and `final`

```cpp
class PosixWritableFile final : public WritableFile {
  Status Sync() override { ... }
};
```

`override` says "this is meant to replace a virtual function in the base class." If the
signature does not actually match one, the compile fails — which catches a whole class of
silent bug where a typo creates a new function instead of overriding the intended one.

`final` on the class says nothing may inherit from it, which also lets the compiler skip
virtual dispatch when it can see the concrete type.

Python has no compile-time equivalent; `@typing.override` is checked by a type checker, not
the interpreter.

### Deleting copy operations

```cpp
WritableFile(const WritableFile&) = delete;
WritableFile& operator=(const WritableFile&) = delete;
```

`= delete` removes a function that C++ would otherwise generate for you. Copying a file
handle would mean two objects owning one file descriptor, and whichever was destroyed first
would close it out from under the other. Deleting the copy makes that a compile error
rather than a crash at runtime.

Python never copies on assignment, so the problem does not arise there.

### Function-local `static`

```cpp
Env& Env::Default() {
  static PosixEnv env;
  return env;
}
```

The object is constructed once, on the first call, and the C++ standard guarantees that
initialization is thread-safe even if several threads call at the same moment. This is the
standard way to write a singleton without a global that might be constructed in the wrong
order relative to other globals.

### Anonymous namespaces

```cpp
namespace {
Status PosixError(const std::string& path, int err) { ... }
}  // namespace
```

Everything inside is private to that one `.cc` file. It is the C++ equivalent of a leading
underscore in Python, except enforced: another file physically cannot refer to it, and the
linker will not see the name at all.

### Out-parameters with `std::unique_ptr`

```cpp
Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out);
```

The function returns a `Status` for success or failure, so the actual result is handed back
through a pointer argument. Ownership transfers to the caller: when their `unique_ptr` goes
out of scope, the file closes. Python would just return a tuple or raise.

### `reinterpret_cast`

```cpp
reinterpret_cast<const uint8_t*>(data.data())
```

"Treat this pointer as pointing to a different type." Used only to view text as raw bytes,
which is legal and well-defined for byte types. It is the sharpest of the casts and should
be rare; `static_cast` is for ordinary conversions.

Python closest: `bytes(s, "utf-8")`, though that copies while this does not.

### `struct ::stat info {};`

Two separate things worth noting. The leading `::` names the global (C) `stat` struct,
distinguishing it from the `stat()` function of the same name. The `{}` zero-initializes
every field, so there is no reading of uninitialized memory if a call fails partway.

### Trailing underscore on members

`fd_`, `path_`, `size_`, `sync_error_`. A convention, not a language feature: it makes it
obvious at a glance that a name is a member variable rather than a local or a parameter.
Python's equivalent is the explicit `self.`.

## P1

### `std::atomic<T>` and memory ordering

A value several threads may read and write without a lock.

```cpp
std::atomic<Node*> next;
next.store(node, std::memory_order_release);   // publish
Node* n = next.load(std::memory_order_acquire); // observe
```

The orderings are promises about *other* memory, not about the atomic itself:

- **release** on a store: everything this thread wrote before the store is visible to any
  thread that performs an acquire load of the same variable and sees this value.
- **acquire** on a load: everything the storing thread wrote before its release store is
  visible to this thread afterwards.
- **relaxed**: atomic, but with no ordering promise at all. Only safe when nothing else
  depends on the order, as when setting a brand-new node's own pointers before anyone can
  reach it.

Python has no equivalent, because the GIL makes the problem invisible.

### Placement `new`

Constructs an object in memory you already have, rather than allocating.

```cpp
char* memory = arena_->Allocate(sizeof(Node), alignof(Node));
auto* node = new (memory) Node{key, value, height, next_array};
```

`new (address) Type{...}` runs the constructor at that address and allocates nothing. It is
how an arena hands out typed objects. The matching `delete` is never called here: the
objects are trivially destructible and the whole arena is released at once.

### Abstract classes as callbacks

```cpp
class WriteBatch::Handler {
 public:
  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;
};
```

`= 0` makes a function *pure virtual*: it has no body and the class cannot be instantiated.
A subclass must supply one. This is C++'s interface, used here so the same decoder can feed
a live memtable, a recovery pass, or a test that just records what it saw.

Python's closest equivalent is an abstract base class, or simply passing a function.

### `std::mutex` and `std::lock_guard`

```cpp
const std::lock_guard<std::mutex> guard(mutex_);
```

The constructor locks and the destructor unlocks, so the lock is released on every exit
path including an early `return`. Python's `with lock:` is the same idea, except this needs
no special syntax. A `mutable` mutex is one a `const` method may still lock, since locking
is not a logical change to the object.

### `std::shared_ptr<T>`

Reference-counted shared ownership — the model every Python object already uses. The object
dies when the last pointer to it does. Used for the memtable so that from P2 a reader can
keep searching an old memtable while a flush installs a new one.

### `std::span<const uint8_t>`

A non-owning view of a contiguous byte range: a pointer and a length. Like a `memoryview`
over `bytes`. It makes "here are some bytes" a single parameter without implying who owns
them.

### Nested classes and forward declarations inside a class

`SkipList::Iterator` is a class declared inside another. It can reach the outer class's
private members, which is why an iterator can walk private nodes.

The trap: a nested class that holds a `Node*` must come *after* at least a declaration of
`Node`. Writing `struct Node* node_;` before `Node` exists does not error — it quietly
declares a new incomplete type. The fix is a forward declaration:

```cpp
class SkipList {
 private:
  struct Node;      // declared here...
 public:
  class Iterator { Node* node_ = nullptr; };
 private:
  struct Node { ... };  // ...defined here
};
```

Member function *bodies* written inside the class body are exempt, because they are
compiled as though the class were already complete.

### Destructors must not let exceptions escape

```cpp
DBImpl::~DBImpl() {
  try {
    (void)Close();
  } catch (...) {
  }
}
```

If an exception leaves a destructor while another exception is already propagating, the
program terminates. A destructor therefore swallows everything — and because it cannot
report a failure, anything whose errors matter needs an explicit method like `Close()` that
a caller can check. Python's `__del__` has the same problem and simply prints the error.

### `enum class Name : uint8_t`

Fixes the enum's underlying integer type. Without it the compiler picks `int`, four bytes,
which is wasteful inside a struct that exists in large numbers.

## P2

### `std::condition_variable`

Lets a thread sleep until another one signals it.

```cpp
background_done_.wait(lock);                                      // sleep
background_work_.wait(lock, [this] { return shutting_down_ || imm_ != nullptr; });
```

`wait` atomically releases the lock and sleeps, then reacquires it on waking. Releasing is
the essential part: the thread that will signal usually needs the same lock, so holding it
would deadlock.

**A woken thread must always re-check its condition**, because it can wake spuriously or
lose a race to another thread. The two-argument form does this for you — it is exactly
`while (!predicate()) wait(lock);`. The one-argument form must be wrapped in your own loop.

Python: `threading.Condition.wait()` and `wait_for(predicate)`.

### `std::thread` with a lambda

```cpp
background_thread_ = std::thread([this] { BackgroundLoop(); });
...
if (background_thread_.joinable()) background_thread_.join();
```

Starts a real OS thread, with no GIL, so it runs genuinely in parallel. `[this]` captures
the object so the new thread can call its methods.

A `std::thread` **must** be joined or detached before it is destroyed, or the program
terminates. That is why `Close()` joins it, and why the destructor calls `Close()`.

### Lambdas and captures

```cpp
[this](const IndexEntry& entry, std::string_view key) { return ...; }
```

An anonymous function. The brackets are the **capture list**: what the lambda can see from
the surrounding scope — `[this]` the enclosing object, `[&]` everything by reference, `[=]`
everything by copy, `[]` nothing. Python's `lambda` captures by reference implicitly and
has no equivalent control.

Capturing by reference is only safe while the referenced thing is alive, which is why a
lambda stored for later should capture by value.

### `std::lower_bound`

```cpp
std::lower_bound(index_.begin(), index_.end(), lookup_key, comparator);
```

Binary search over a sorted range for the first element not less than a value. Returns an
*iterator*, which equals `end()` when nothing qualifies — the way C++ says "not found".
Python: `bisect.bisect_left`.

The custom comparator takes `(element, value)` in that order and returns whether the
element sorts before the value. Getting the argument order backwards compiles and silently
searches wrongly.

### `std::clamp`

`std::clamp(value, low, high)` restricts a value to a range. Python:
`max(low, min(high, value))`.

### Ceiling division and bit manipulation

```cpp
(requested_bits + 7) / 8 * 8      // round up to a whole number of bytes
bits_[bit / 8] |= 1U << (bit % 8) // set one bit inside a byte array
hash += delta;                    // uint32_t wraps at 2^32 automatically
```

Integer division truncates, so `(n + 7) / 8` is the standard way to round up. `bit / 8`
finds the byte and `bit % 8` the bit inside it; `1U << n` builds a mask with just bit `n`
set, and `|=` turns it on without disturbing the others.

Unsigned overflow wraps around and is well defined. *Signed* overflow is undefined
behavior, which is why counters that may wrap are unsigned.

### Overflow-safe bounds checks

```cpp
if (filter_offset > metadata_limit || filter_size > metadata_limit - filter_offset) { ... }
```

Not `offset + size > limit`. That addition can overflow and wrap to a small number that
passes the check, which is a classic way for untrusted input to get past validation.
Subtracting from the limit instead cannot overflow, because the first test has already
established `offset <= limit`.

### `dynamic_cast`

```cpp
auto* impl = dynamic_cast<DBImpl*>(db.get());
```

A checked downcast from a base pointer to a derived one, returning `nullptr` if the object
is not actually that type. Used only in tests, to reach test-only observation points
through the public `DB` interface. It needs a virtual function in the base class to work.

## P3

### `std::priority_queue` as a min-heap

```cpp
using Heap = std::priority_queue<Iterator*, std::vector<Iterator*>, Greater>;
```

C++'s heap is a **max-heap** by default, so `top()` gives the largest element. Passing a
"greater than" comparison flips it into a min-heap and `top()` becomes the smallest. Python's
`heapq` is already a min-heap, so this inversion has no Python equivalent and is easy to get
backwards.

`top()` and `pop()` are separate calls: look at the smallest, then remove it. Python's
`heapq.heappop` does both at once.

### Function objects

```cpp
struct Greater {
  const InternalKeyComparator* comparator;
  bool operator()(const Iterator* a, const Iterator* b) const { ... }
};
```

Defining `operator()` makes a struct callable like a function. A lambda is really just a
compiler-generated version of this. The explicit struct is used where the type has to be
*named* — here in a `using` declaration — which a lambda's anonymous type cannot be.

### `std::multiset<T>`

A sorted collection that allows duplicates. `*begin()` is the smallest element, which is why
it suits tracking the oldest active read snapshot: several reads may share a sequence number,
and each must be released independently.

`erase(iterator)` removes one element; `erase(value)` would remove *all* copies of it, which
would release other reads' snapshots too.

Python has no direct equivalent; `sortedcontainers.SortedList` is the closest.

### `shared_ptr::use_count()`

Returns how many `shared_ptr`s own the object.

```cpp
return version.use_count() == 1;  // only this list holds it
```

Useful here because the retired-versions list is the only other owner, so a count of one
proves no reader is still searching that version. It is a blunt tool in general — the count
can change the instant after you read it — and is only safe under a lock, as it is here.

### The erase-remove idiom

```cpp
retired_.erase(std::remove_if(retired_.begin(), retired_.end(), predicate), retired_.end());
```

`std::remove_if` does not remove anything. It shuffles the elements you want to keep to the
front and returns an iterator to where the leftovers begin; `erase` then drops that tail.
Two steps because the algorithms cannot change a container's size, only its contents.

Python: `items = [x for x in items if not predicate(x)]`.

### `std::function<T(Args)>` as a parameter

```cpp
const std::function<uint64_t()>& allocate_file_number
```

Holds any callable with that signature — a lambda, a function pointer, a function object.
Taken by const reference to avoid a copy. It lets compaction ask for a new file number
without knowing anything about the version set that hands them out.

Python: a parameter annotated `Callable[[], int]`.

### Tagged binary encodings

```cpp
PutVarint32(destination, static_cast<uint32_t>(tag));
```

Each field is written as a tag followed by its payload, and the decoder switches on the tag.
The point is forward and backward compatibility: a later phase can add a field without
making existing MANIFESTs unreadable, because old readers see a tag they do not recognise
rather than data at an unexpected offset. Protocol Buffers works this way.

An unknown tag is treated as corruption here, which is the strict choice. Skipping unknown
tags requires each one to carry its own length.

### RAII guards for paired operations

```cpp
class ReadSnapshotGuard {
 public:
  ReadSnapshotGuard(DBImpl* db, uint64_t sequence) : db_(db), sequence_(sequence) {}
  ~ReadSnapshotGuard() { db_->UnregisterReadSnapshot(sequence_); }
};
```

The same idea as `std::lock_guard`, applied to anything that must be undone. `Get` has
several early returns, and a forgotten unregister on any one of them would leave a snapshot
registered forever — permanently stopping compaction from reclaiming anything above it. A
destructor cannot be forgotten.

This is the general lesson: whenever two operations must be paired, put the second one in a
destructor rather than trusting every exit path. Python's `contextlib.contextmanager` is the
closest equivalent.

## P4

### `std::deque<T>`

A double-ended queue: push and pop cheaply at either end, unlike `std::vector` which is only
cheap at the back. The writer queue needs `push_back` and `pop_front`, which is exactly what
a deque is for. Python: `collections.deque`.

### A condition variable per waiter

```cpp
struct Writer {
  ...
  std::condition_variable cv;
};
```

Each waiting writer carries its own condition variable, so a wake-up goes to exactly the
thread it concerns. One shared variable plus `notify_all` would wake every waiter so they
could all re-check their predicate and almost all go back to sleep — a *thundering herd*.
At eight writers it hardly matters; the habit matters.

`notify_one` wakes one waiter on that variable; `notify_all` wakes every waiter. With one
variable per writer there is only ever one waiter, so `notify_one` is both sufficient and
precise.

### Pointers to stack objects shared across threads

```cpp
Writer self;              // on this thread's stack
writers_.push_back(&self); // other threads will reach it through this pointer
```

This looks alarming and is safe, for one specific reason: the thread that owns `self` blocks
inside the same function for as long as the pointer is in the queue, so the object cannot go
out of scope while anyone can see it. No allocation, no shared ownership, no lifetime
question.

It is only safe because of that structure. The same pattern with a thread that returned
early — or with an `ASSERT` that returns from the middle of a function — would leave other
threads holding a pointer to a destroyed object. A test in this project had exactly that bug.

### `std::next(iterator)`

Returns the iterator one position along, without modifying the original. `std::next(begin())`
is "the second element" — clearer than `begin() + 1`, and it works on containers like
`std::deque` and `std::list` whose iterators do not support arithmetic.

### `std::chrono` for timing

```cpp
const auto started = std::chrono::steady_clock::now();
...
const auto elapsed = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - started).count();
```

`steady_clock` is the right clock for measuring a duration: it only moves forward and is
unaffected by the system clock being adjusted. `system_clock` is for telling the time, and
can jump backwards. Subtracting two time points gives a `duration`; wrapping it in
`duration<double>` and calling `.count()` gives seconds as a plain number.

`std::this_thread::sleep_for(std::chrono::seconds(60))` is Python's `time.sleep(60)`.

## P5

### `py::gil_scoped_release`

```cpp
const std::string owned_key = key;   // copy FIRST, while the GIL is held
{
  const py::gil_scoped_release no_gil;
  status = db.Get(options, owned_key, &value);
}
```

The **GIL** (global interpreter lock) lets only one thread run Python at a time. A C++
extension that holds it across a disk read blocks every other Python thread for the
duration; releasing it lets them run.

`gil_scoped_release` is RAII: it releases on construction and reacquires at the closing
brace, so an early return or a thrown exception cannot leave it released.

The ordering rule is absolute: **Python objects may only be touched while holding the GIL.**
Anything the released section needs must be copied into C++ memory first. Getting this
wrong usually works and occasionally corrupts memory, which is the worst failure mode there
is.

### `py::bytes` and conversions

`py::bytes` is a handle to a Python `bytes` object. Assigning it to a `std::string` copies
the data; constructing `py::bytes(value)` from a `std::string` copies the other way. Each
crossing copies, which is why very large values cost more through the bindings than in C++.

pybind11 refuses to convert a Python `str` to `py::bytes` and raises `TypeError`, which is
what makes rejecting string keys free rather than something to implement.

### `py::register_exception`

```cpp
const py::object base = py::register_exception<StrataKVError>(m, "StrataKVError");
py::register_exception<CorruptionError>(m, "CorruptionError", base.ptr());
```

Creates a real Python exception class and installs a translator, so throwing the C++ type
raises the Python one. The third argument is the Python base class, which is how an exception
hierarchy is mirrored across the boundary — one `except StrataKVError` then catches
everything.

Some C++ exceptions need no registration: pybind11 already maps `std::invalid_argument` to
`ValueError`, `std::out_of_range` to `IndexError`, and `std::runtime_error` to
`RuntimeError`.

### Raising an existing Python exception from C++

```cpp
PyErr_SetString(PyExc_OSError, message.c_str());
throw py::error_already_set();
```

To raise a *builtin*, set the Python error state with the C API and then throw
`error_already_set`, which tells pybind11 the error is already set rather than needing
translation. Newer pybind11 has a `py::set_error` helper; the two-line form works on every
version.

### `py::class_` members

```cpp
py::class_<PythonDB>(m, "DB")
    .def_static("open", &PythonDB::Open, py::arg("path"), py::arg("sync") = true)
    .def("get", &PythonDB::Get, py::arg("key"))
    .def("__contains__", &PythonDB::Contains, py::arg("key"))
    .def_property_readonly("closed", &PythonDB::closed);
```

- `def_static` — a `@staticmethod`/`@classmethod`-style factory.
- `py::arg("name") = value` — gives the parameter a name and a default, so
  `DB.open(path, sync=False)` works as a Python caller expects.
- Binding a dunder like `__contains__` makes the operator work: `key in db`.
- `def_property_readonly` — exposes a getter as an attribute rather than a method, so
  `db.closed` rather than `db.closed()`.
