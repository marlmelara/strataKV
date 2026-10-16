"""StrataKV: a crash-safe LSM-tree key-value storage engine.

The engine is C++; this package is the Python face of it. Keys and values are
arbitrary ``bytes``, including zero bytes.

The central guarantee: if a write returns, it survives a crash.

    >>> with stratakv.DB.open("/tmp/mydb") as db:
    ...     db[b"apple"] = b"red"
    ...     db[b"apple"]
    b'red'

Deliberately *not* a ``collections.abc.MutableMapping``
-------------------------------------------------------
The mapping methods that make sense are here -- ``db[k]``, ``db[k] = v``,
``del db[k]``, ``k in db`` -- but this class does not inherit from
``MutableMapping``, because that ABC requires ``__iter__`` and ``__len__`` and
this engine has no range scans yet. Inheriting and raising ``NotImplementedError``
from those would advertise an interface it cannot honor: ``dict(db)`` and
``len(db)`` would type-check and then fail at runtime. When range-scan iterators
arrive, ``__iter__`` and ``__len__`` can be added and the ABC claimed honestly.
"""

from __future__ import annotations

from collections.abc import Iterable
from types import TracebackType

from ._stratakv import DB as _CoreDB
from ._stratakv import CorruptionError, StrataKVError
from ._stratakv import version as _version

__all__ = ["DB", "CorruptionError", "StrataKVError", "__version__", "version"]

# One operation for ``write_batch``: a key, and either a value or None to delete.
Operation = tuple[bytes, bytes | None]


def version() -> str:
    """Return the version of the compiled C++ engine.

    This reads the string baked into the extension module at compile time, so it
    reports the build that is actually loaded rather than whatever metadata pip
    recorded.
    """
    return _version()


__version__: str = _version()


class DB:
    """An open StrataKV database."""

    __slots__ = ("_core",)

    def __init__(self, core: _CoreDB) -> None:
        """Wrap an already-open core handle. Use :meth:`open` instead."""
        self._core = core

    @classmethod
    def open(
        cls,
        path: str,
        *,
        create_if_missing: bool = True,
        error_if_exists: bool = False,
        sync: bool = True,
        memtable_bytes: int = 0,
        enable_group_commit: bool = True,
    ) -> DB:
        """Open or create the database at ``path``.

        Takes an exclusive lock, so a second open of the same directory fails
        rather than corrupting the first. Replays the write-ahead log, discarding
        any record that was not completely written.

        ``sync=True`` (the default) means every write reaches durable storage
        before it returns. ``sync=False`` is faster and gives up the engine's
        central guarantee; it is for bulk loading, not for data you care about.
        ``memtable_bytes=0`` means use the engine's default.
        """
        return cls(
            _CoreDB.open(
                path,
                create_if_missing=create_if_missing,
                error_if_exists=error_if_exists,
                sync=sync,
                memtable_bytes=memtable_bytes,
                enable_group_commit=enable_group_commit,
            )
        )

    # --- The engine's own operations ---

    def get(self, key: bytes, default: bytes | None = None) -> bytes | None:
        """Return the value for ``key``, or ``default`` if it is absent or deleted."""
        value = self._core.get(key)
        return default if value is None else value

    def put(self, key: bytes, value: bytes) -> None:
        """Store ``value`` under ``key``, replacing any previous value."""
        self._core.put(key, value)

    def delete(self, key: bytes) -> None:
        """Delete ``key``.

        Idempotent: deleting a key that is not there is not an error. Use
        ``del db[key]`` for dict semantics, which raises ``KeyError`` instead.
        """
        self._core.delete(key)

    def write_batch(self, operations: Iterable[Operation]) -> None:
        """Apply several operations atomically.

        Each operation is a ``(key, value)`` pair; a value of ``None`` deletes the
        key. The whole batch becomes one log record with one checksum, so after a
        crash it is replayed entirely or not at all.

            >>> db.write_batch([(b"a", b"1"), (b"b", None)])
        """
        self._core.write_batch(operations)

    def close(self) -> None:
        """Flush, release the lock, and close.

        Idempotent. Calling this explicitly is the only way to see an error from
        the final sync; the context manager does it for you.
        """
        self._core.close()

    @property
    def closed(self) -> bool:
        """True once the database has been closed."""
        return self._core.closed

    # --- Mapping behavior ---

    def __getitem__(self, key: bytes) -> bytes:
        value = self._core.get(key)
        if value is None:
            raise KeyError(key)
        return value

    def __setitem__(self, key: bytes, value: bytes) -> None:
        self._core.put(key, value)

    def __delitem__(self, key: bytes) -> None:
        # dict semantics, unlike delete(): deleting something absent is an error.
        # This costs a read before the write, which delete() does not.
        if self._core.get(key) is None:
            raise KeyError(key)
        self._core.delete(key)

    def __contains__(self, key: bytes) -> bool:
        return key in self._core

    # --- Context manager ---

    def __enter__(self) -> DB:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        # Closed even when the block raised: the lock must not outlive the block.
        self._core.close()

    def __repr__(self) -> str:
        return f"<stratakv.DB {'closed' if self.closed else 'open'}>"
