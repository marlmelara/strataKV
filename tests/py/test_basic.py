"""P0 smoke tests: the package imports and reports a sane version.

These are deliberately thin. Their job is to fail loudly if the build chain
breaks -- a stale extension, a wheel missing the .so, or a version that drifted
away from the packaging metadata.
"""

from __future__ import annotations

import importlib.metadata
import re

import stratakv


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
