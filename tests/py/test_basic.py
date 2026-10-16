"""Unit tests for the Python package.

These cover the binding layer: conversions, exception mapping, the mapping
dunders, and that the GIL is actually released during engine calls. The
model-based tests in test_stateful.py cover engine behavior far more thoroughly
than anything hand-written here could.
"""

from __future__ import annotations

import importlib.metadata
import re
import threading

import pytest
import stratakv


@pytest.fixture
def db(tmp_path):
    """An open database in a fresh directory, closed afterwards."""
    with stratakv.DB.open(str(tmp_path / "mydb")) as handle:
        yield handle


# --- Version and packaging ---


def test_version_is_a_nonempty_string() -> None:
    assert isinstance(stratakv.version(), str)
    assert stratakv.version() != ""


def test_version_looks_like_a_release_number() -> None:
    assert re.fullmatch(r"\d+\.\d+\.\d+", stratakv.version()), stratakv.version()


def test_dunder_version_matches_the_function() -> None:
    assert stratakv.__version__ == stratakv.version()


def test_compiled_version_matches_packaging_metadata() -> None:
    """The CMake project version and the pyproject version are separate strings.

    If one is bumped without the other, this fails -- which is the only thing
    keeping them honest.
    """
    assert stratakv.version() == importlib.metadata.version("stratakv")


def test_extension_module_is_the_compiled_one() -> None:
    from stratakv import _stratakv

    assert _stratakv.__file__ is not None
    assert not _stratakv.__file__.endswith(".py")


# --- Basic operations ---


def test_put_then_get(db) -> None:
    db.put(b"apple", b"red")
    assert db.get(b"apple") == b"red"


def test_get_of_a_missing_key_returns_none(db) -> None:
    assert db.get(b"absent") is None


def test_get_accepts_a_default(db) -> None:
    assert db.get(b"absent", b"fallback") == b"fallback"
    db.put(b"present", b"value")
    assert db.get(b"present", b"fallback") == b"value"


def test_overwrite_returns_the_newest_value(db) -> None:
    db.put(b"k", b"first")
    db.put(b"k", b"second")
    assert db.get(b"k") == b"second"


def test_delete_hides_the_key(db) -> None:
    db.put(b"k", b"v")
    db.delete(b"k")
    assert db.get(b"k") is None


def test_delete_is_idempotent(db) -> None:
    """The engine's delete writes a tombstone; deleting nothing is not an error."""
    db.delete(b"never-there")
    db.delete(b"never-there")


def test_empty_and_binary_keys_and_values(db) -> None:
    cases = {
        b"": b"empty-key",
        b"k\x00ey": b"v\x00al\xff",
        b"empty-value": b"",
        bytes(range(256)): bytes(reversed(range(256))),
    }
    for key, value in cases.items():
        db.put(key, value)
    for key, value in cases.items():
        assert db.get(key) == value, key


def test_large_values_round_trip(db) -> None:
    value = bytes(range(256)) * 4096  # 1 MiB
    db.put(b"big", value)
    assert db.get(b"big") == value


# --- Mapping behavior ---


def test_setitem_and_getitem(db) -> None:
    db[b"k"] = b"v"
    assert db[b"k"] == b"v"


def test_getitem_raises_key_error_when_absent(db) -> None:
    with pytest.raises(KeyError):
        _ = db[b"absent"]


def test_delitem_raises_key_error_when_absent(db) -> None:
    """Unlike delete(), `del db[k]` follows dict semantics."""
    with pytest.raises(KeyError):
        del db[b"absent"]


def test_delitem_removes_a_present_key(db) -> None:
    db[b"k"] = b"v"
    del db[b"k"]
    assert db.get(b"k") is None


def test_contains(db) -> None:
    assert b"k" not in db
    db[b"k"] = b"v"
    assert b"k" in db
    del db[b"k"]
    assert b"k" not in db


def test_not_a_mutable_mapping() -> None:
    """Deliberate: the ABC requires __iter__ and __len__, which need range scans.

    Claiming the interface without them would make dict(db) and len(db)
    type-check and then fail at runtime.
    """
    from collections.abc import MutableMapping

    assert not issubclass(stratakv.DB, MutableMapping)
    assert not hasattr(stratakv.DB, "__iter__")
    assert not hasattr(stratakv.DB, "__len__")


# --- Batches ---


def test_write_batch_applies_every_operation(db) -> None:
    db.put(b"existing", b"old")
    db.write_batch([(b"a", b"1"), (b"b", b"2"), (b"existing", None)])

    assert db.get(b"a") == b"1"
    assert db.get(b"b") == b"2"
    assert db.get(b"existing") is None


def test_write_batch_accepts_any_iterable(db) -> None:
    db.write_batch(pair for pair in [(b"gen", b"1")])
    assert db.get(b"gen") == b"1"
    db.write_batch([[b"list-pair", b"2"]])
    assert db.get(b"list-pair") == b"2"


def test_write_batch_of_nothing_is_harmless(db) -> None:
    db.write_batch([])


def test_write_batch_later_operations_win(db) -> None:
    db.write_batch([(b"k", b"first"), (b"k", b"second")])
    assert db.get(b"k") == b"second"


@pytest.mark.parametrize(
    "operations",
    [
        [b"not-a-pair"],
        [(b"only-one",)],
        [(b"too", b"many", b"items")],
        [42],
    ],
)
def test_write_batch_rejects_malformed_operations(db, operations) -> None:
    with pytest.raises((ValueError, TypeError)):
        db.write_batch(operations)


# --- Exceptions and types ---


def test_string_keys_are_rejected(db) -> None:
    """Silently encoding str would be a trap: b"k" and "k" would collide."""
    with pytest.raises(TypeError):
        db.put("not-bytes", b"v")
    with pytest.raises(TypeError):
        db.put(b"k", "not-bytes")
    with pytest.raises(TypeError):
        db.get("not-bytes")


def test_operations_after_close_are_rejected(tmp_path) -> None:
    db = stratakv.DB.open(str(tmp_path / "mydb"))
    db.close()
    assert db.closed

    with pytest.raises(ValueError):
        db.put(b"k", b"v")
    with pytest.raises(ValueError):
        db.get(b"k")


def test_close_is_idempotent(tmp_path) -> None:
    db = stratakv.DB.open(str(tmp_path / "mydb"))
    db.close()
    db.close()


def test_closed_property_tracks_state(tmp_path) -> None:
    db = stratakv.DB.open(str(tmp_path / "mydb"))
    assert not db.closed
    assert "open" in repr(db)
    db.close()
    assert db.closed
    assert "closed" in repr(db)


def test_a_second_open_of_the_same_database_fails(tmp_path) -> None:
    """The LOCK file stops two handles writing to one directory."""
    path = str(tmp_path / "mydb")
    with stratakv.DB.open(path), pytest.raises(OSError):
        stratakv.DB.open(path)


def test_open_respects_create_if_missing(tmp_path) -> None:
    with pytest.raises(ValueError):
        stratakv.DB.open(str(tmp_path / "absent"), create_if_missing=False)


def test_open_respects_error_if_exists(tmp_path) -> None:
    path = str(tmp_path / "mydb")
    stratakv.DB.open(path).close()
    with pytest.raises(ValueError):
        stratakv.DB.open(path, error_if_exists=True)


def test_corruption_is_reported_as_corruption_error(tmp_path) -> None:
    """A damaged log must raise the dedicated type, not a generic error."""
    path = tmp_path / "mydb"
    with stratakv.DB.open(str(path)) as db:
        for i in range(200):
            db.put(f"key-{i:06d}".encode(), f"value-{i:06d}".encode())

    log = sorted(path.glob("*.log"))[-1]
    data = bytearray(log.read_bytes())
    assert len(data) > 100
    middle = len(data) // 2
    data[middle] ^= 0x01  # Flip one bit in the middle, so valid records follow it.
    log.write_bytes(bytes(data))

    with pytest.raises(stratakv.CorruptionError):
        stratakv.DB.open(str(path))


def test_corruption_error_is_a_strata_kv_error() -> None:
    """One except clause should be able to catch everything the engine raises."""
    assert issubclass(stratakv.CorruptionError, stratakv.StrataKVError)


# --- Durability and the context manager ---


def test_data_survives_close_and_reopen(tmp_path) -> None:
    path = str(tmp_path / "mydb")
    with stratakv.DB.open(path) as db:
        db.put(b"persisted", b"yes")
        db.delete(b"removed")
        db.put(b"removed", b"back")

    with stratakv.DB.open(path) as db:
        assert db.get(b"persisted") == b"yes"
        assert db.get(b"removed") == b"back"


def test_tombstones_survive_reopen(tmp_path) -> None:
    path = str(tmp_path / "mydb")
    with stratakv.DB.open(path) as db:
        db.put(b"k", b"v")
        db.delete(b"k")

    with stratakv.DB.open(path) as db:
        assert db.get(b"k") is None


def test_context_manager_closes_even_when_the_block_raises(tmp_path) -> None:
    """The lock must not outlive the block."""
    path = str(tmp_path / "mydb")
    db = None
    with pytest.raises(RuntimeError), stratakv.DB.open(path) as handle:
        db = handle
        raise RuntimeError("boom")
    assert db is not None and db.closed
    # Closed means reopenable.
    stratakv.DB.open(path).close()


def test_many_keys_round_trip(tmp_path) -> None:
    path = str(tmp_path / "mydb")
    count = 20000
    with stratakv.DB.open(path, sync=False) as db:
        db.write_batch([(f"k{i:06d}".encode(), f"v{i:06d}".encode()) for i in range(count)])

    with stratakv.DB.open(path) as db:
        for i in range(0, count, 97):
            assert db.get(f"k{i:06d}".encode()) == f"v{i:06d}".encode()


# --- The GIL ---


def test_engine_calls_release_the_gil(tmp_path) -> None:
    """Other Python threads must keep running during disk I/O.

    A counter thread spins while the main thread does hundreds of synced writes.
    If the GIL were held across the engine calls the counter could barely
    advance; the threshold is set a thousandfold below what it reaches in
    practice, so this is about whether the GIL is released at all rather than
    about timing.
    """
    counter = 0
    stop = threading.Event()

    def spin() -> None:
        nonlocal counter
        while not stop.is_set():
            counter += 1

    thread = threading.Thread(target=spin)
    thread.start()
    try:
        with stratakv.DB.open(str(tmp_path / "mydb")) as db:
            for i in range(300):
                db.put(f"k{i}".encode(), b"v" * 100)
    finally:
        stop.set()
        thread.join()

    assert counter > 1000, f"counter only reached {counter}: was the GIL held?"


def test_concurrent_writers_from_python_threads(tmp_path) -> None:
    """Group commit is exercised from Python, through the binding layer."""
    path = str(tmp_path / "mydb")
    writers = 8
    per_writer = 100
    errors: list[BaseException] = []

    with stratakv.DB.open(path) as db:

        def write(worker: int) -> None:
            try:
                for i in range(per_writer):
                    db.put(f"w{worker}-k{i:05d}".encode(), f"w{worker}-v{i:05d}".encode())
            except BaseException as error:
                errors.append(error)

        threads = [threading.Thread(target=write, args=(w,)) for w in range(writers)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        assert not errors, errors
        for worker in range(writers):
            for i in range(per_writer):
                key = f"w{worker}-k{i:05d}".encode()
                assert db.get(key) == f"w{worker}-v{i:05d}".encode(), key
