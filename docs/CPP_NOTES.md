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
