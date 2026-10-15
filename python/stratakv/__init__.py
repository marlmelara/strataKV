"""StrataKV: a crash-safe LSM-tree key-value storage engine.

The engine is C++; this package is the Python face of it. In P0 it exposes only
the version, which proves the build chain works end to end. The ``DB`` class,
the context manager, and the ``MutableMapping`` interface arrive in P5.
"""

from __future__ import annotations

from ._stratakv import version as _version

__all__ = ["__version__", "version"]


def version() -> str:
    """Return the version of the compiled C++ engine.

    This reads the string baked into the extension module at compile time, so it
    reports the build that is actually loaded rather than whatever metadata pip
    recorded.
    """
    return _version()


__version__: str = _version()
