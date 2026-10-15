# C++ notes

C++ features used in the real code that are not already covered in the build spec's "C++
primer" tab. Each phase appends the features it introduced.

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
