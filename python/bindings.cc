// The compiled core of the Python package.
//
// Two things matter here beyond plumbing. The GIL is released around every
// engine call, so Python threads actually overlap with disk I/O instead of
// queueing behind it -- and because Python objects may only be touched while
// holding the GIL, every key and value is copied into C++ memory first.
//
// And a Status is turned into the exception a Python caller would expect:
// corruption into a dedicated type, an I/O failure into OSError, a bad argument
// into ValueError. A missing key is not an error at this layer; `get` returns
// None and the wrapper decides whether that is a KeyError.

#include <pybind11/pybind11.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/version.h"
#include "stratakv/write_batch.h"

#include "env.h"

namespace py = pybind11;

namespace {

// Base of the package's own exception hierarchy, so a caller can catch
// everything this engine raises with one except clause.
class StrataKVError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Raised when data on disk fails its checksum or does not decode. Separate from
// an I/O failure because it means something different: the bytes arrived and
// were wrong, rather than not arriving.
class CorruptionError : public StrataKVError {
 public:
  using StrataKVError::StrataKVError;
};

void ThrowIfError(const stratakv::Status& status) {
  if (status.ok()) return;

  const std::string message = status.ToString();
  switch (status.code()) {
    case stratakv::StatusCode::kOk:
      return;
    case stratakv::StatusCode::kCorruption:
      throw CorruptionError(message);
    case stratakv::StatusCode::kIOError:
      // OSError is what a Python caller already catches for a failed file
      // operation; inventing a new type would only make it easier to miss.
      PyErr_SetString(PyExc_OSError, message.c_str());
      throw py::error_already_set();
    case stratakv::StatusCode::kInvalidArgument:
      // pybind11 maps std::invalid_argument to ValueError.
      throw std::invalid_argument(message);
    case stratakv::StatusCode::kNotFound:
      // Not an error here: `get` reports it by returning None.
      throw std::invalid_argument(message);
  }
  throw StrataKVError(message);
}

// Holds the engine for Python. A separate type rather than binding
// stratakv::DB directly, so the GIL handling and the conversions live in one
// place and the Python object cannot outlive a half-open database.
class PythonDB {
 public:
  explicit PythonDB(std::unique_ptr<stratakv::DB> db) : db_(std::move(db)) {}

  static std::unique_ptr<PythonDB> Open(const std::string& path, bool create_if_missing,
                                        bool error_if_exists, bool sync, size_t memtable_bytes,
                                        bool enable_group_commit) {
    stratakv::Options options;
    options.create_if_missing = create_if_missing;
    options.error_if_exists = error_if_exists;
    options.sync_policy =
        sync ? stratakv::SyncPolicy::kSyncEveryWrite : stratakv::SyncPolicy::kNoSync;
    if (memtable_bytes > 0) options.memtable_bytes = memtable_bytes;
    options.enable_group_commit = enable_group_commit;

    std::unique_ptr<stratakv::DB> db;
    stratakv::Status status;
    {
      // Opening replays the log and opens every table: real I/O, so other
      // Python threads should keep running.
      const py::gil_scoped_release no_gil;
      status = stratakv::DB::Open(options, path, &db);
    }
    ThrowIfError(status);
    return std::make_unique<PythonDB>(std::move(db));
  }

  // Returns bytes, or None when the key is absent or deleted.
  py::object Get(const py::bytes& key) {
    // Copied while the GIL is held: a Python object may not be touched without
    // it, and it is about to be released.
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

  void Put(const py::bytes& key, const py::bytes& value) {
    const std::string owned_key = key;
    const std::string owned_value = value;
    stratakv::Status status;
    {
      const py::gil_scoped_release no_gil;
      status = Engine().Put(stratakv::WriteOptions{}, owned_key, owned_value);
    }
    ThrowIfError(status);
  }

  void Delete(const py::bytes& key) {
    const std::string owned_key = key;
    stratakv::Status status;
    {
      const py::gil_scoped_release no_gil;
      status = Engine().Delete(stratakv::WriteOptions{}, owned_key);
    }
    ThrowIfError(status);
  }

  // `operations` is an iterable of (key, value) pairs; a value of None means
  // delete. The whole batch is applied atomically: one log record, one
  // checksum, one fsync.
  void WriteBatch(const py::iterable& operations) {
    stratakv::WriteBatch batch;

    // Every conversion happens here, while the GIL is held, so the engine call
    // below touches no Python object.
    for (const py::handle item : operations) {
      py::sequence pair;
      try {
        pair = py::cast<py::sequence>(item);
      } catch (const py::cast_error&) {
        throw std::invalid_argument("each operation must be a (key, value) pair");
      }
      if (py::len(pair) != 2) {
        throw std::invalid_argument("each operation must be a (key, value) pair");
      }
      const std::string key = py::cast<py::bytes>(pair[0]);
      if (pair[1].is_none()) {
        batch.Delete(key);
      } else {
        batch.Put(key, py::cast<py::bytes>(pair[1]));
      }
    }

    stratakv::Status status;
    {
      const py::gil_scoped_release no_gil;
      status = Engine().Write(stratakv::WriteOptions{}, &batch);
    }
    ThrowIfError(status);
  }

  bool Contains(const py::bytes& key) { return !Get(key).is_none(); }

  void Close() {
    if (db_ == nullptr) return;
    stratakv::Status status;
    {
      const py::gil_scoped_release no_gil;
      status = db_->Close();
    }
    // Released only after a successful close, so a failed final sync is
    // reported rather than swallowed by the destructor.
    ThrowIfError(status);
    db_.reset();
  }

  bool closed() const { return db_ == nullptr; }

 private:
  stratakv::DB& Engine() {
    if (db_ == nullptr) throw std::invalid_argument("database is closed");
    return *db_;
  }

  std::unique_ptr<stratakv::DB> db_;
};

}  // namespace

PYBIND11_MODULE(_stratakv, m) {
  m.doc() = "Compiled core of StrataKV. Import the `stratakv` package instead.";

  const py::object base = py::register_exception<StrataKVError>(m, "StrataKVError");
  py::register_exception<CorruptionError>(m, "CorruptionError", base.ptr());

  m.def(
      "version", []() { return std::string(stratakv::VersionString()); },
      "Version of the compiled engine, taken from the CMake project version.");

  // Instrumentation for the benchmarks. Named plainly rather than hidden,
  // because a number in the README has to be reproducible by whoever reads it.
  m.def(
      "bytes_written", []() { return stratakv::Env::BytesWrittenForTesting(); },
      "Total bytes this process has written to disk, across all files.");
  m.def(
      "reset_bytes_written", []() { stratakv::Env::ResetBytesWrittenForTesting(); },
      "Zero the byte counter before a measurement.");
  m.def(
      "sync_count", []() { return stratakv::Env::SyncCountForTesting(); },
      "Number of durable syncs this process has performed.");
  m.def(
      "reset_sync_count", []() { stratakv::Env::ResetSyncCountForTesting(); },
      "Zero the sync counter before a measurement.");

  py::class_<PythonDB>(m, "DB")
      .def_static("open", &PythonDB::Open, py::arg("path"), py::arg("create_if_missing") = true,
                  py::arg("error_if_exists") = false, py::arg("sync") = true,
                  py::arg("memtable_bytes") = 0, py::arg("enable_group_commit") = true,
                  "Open or create the database at `path`.")
      .def("get", &PythonDB::Get, py::arg("key"), "Return the value, or None if absent.")
      .def("put", &PythonDB::Put, py::arg("key"), py::arg("value"))
      .def("delete", &PythonDB::Delete, py::arg("key"))
      .def("write_batch", &PythonDB::WriteBatch, py::arg("operations"),
           "Apply (key, value) pairs atomically; a value of None deletes.")
      .def("close", &PythonDB::Close)
      .def("__contains__", &PythonDB::Contains, py::arg("key"))
      .def_property_readonly("closed", &PythonDB::closed);

  m.attr("__version__") = std::string(stratakv::VersionString());
}
