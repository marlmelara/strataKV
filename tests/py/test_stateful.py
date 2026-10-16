"""Model-based tests: StrataKV against a plain dict.

Hypothesis calls the rules below in random orders with random arguments, many
times over, and checks after each one that the engine and the dict still agree.
This finds bugs no hand-written test thinks to look for, because nobody chose
the sequence.

Two details make it find more than it otherwise would:

* Keys are short, so collisions are likely and overwrites and deletes of
  existing keys actually get exercised rather than mostly missing.
* The memtable is tiny, so rotations, flushes and compactions happen constantly
  inside a single example instead of only in dedicated tests.

When an assertion fails after a few hundred random steps, Hypothesis shrinks the
sequence to the shortest one that still fails -- usually three or four steps.
A three-step reproduction of a recovery bug is debuggable; a four-hundred-step
one is not.
"""

from __future__ import annotations

import os
import shutil
import tempfile

import stratakv
from hypothesis import HealthCheck, settings
from hypothesis import strategies as st
from hypothesis.stateful import RuleBasedStateMachine, invariant, precondition, rule

# The acceptance criterion is 10,000 sequences. CI runs fewer on every push and
# the full run is recorded in docs/NUMBERS.md.
MAX_EXAMPLES = int(os.environ.get("STRATAKV_HYPOTHESIS_EXAMPLES", "300"))

# Short keys: collisions are the point.
KEYS = st.binary(min_size=0, max_size=8)
VALUES = st.binary(max_size=64)

# Two sizes, because they buy different things.
#
# The arena hands out 4 KiB blocks and a memtable's reported usage counts
# allocated blocks, so a 4 KiB limit is crossed by the first write and the engine
# rotates and flushes on nearly every one. That is excellent coverage of the
# flush and recovery paths and it is 42x slower per sequence -- measured at
# 894 ms against 21 ms. Running ten thousand sequences that way would take
# nearly two hours.
#
# So the long run uses a memtable big enough that most sequences stay in memory,
# and a second, shorter run keeps the flush-on-every-write coverage.
ROOMY_MEMTABLE = 64 * 1024
TINY_MEMTABLE = 4 * 1024


class KeyValueMachine(RuleBasedStateMachine):
    """Drives the engine and a dict in lockstep and insists they agree."""

    # Overridden by the flush-heavy subclass below.
    memtable_bytes = ROOMY_MEMTABLE

    def __init__(self) -> None:
        super().__init__()
        self.directory = tempfile.mkdtemp()
        self.path = os.path.join(self.directory, "mydb")
        self.db = stratakv.DB.open(self.path, sync=False, memtable_bytes=self.memtable_bytes)
        self.model: dict[bytes, bytes] = {}

    @rule(key=KEYS, value=VALUES)
    def put(self, key: bytes, value: bytes) -> None:
        self.db.put(key, value)
        self.model[key] = value

    @rule(key=KEYS)
    def delete(self, key: bytes) -> None:
        self.db.delete(key)
        self.model.pop(key, None)

    @rule(key=KEYS)
    def get(self, key: bytes) -> None:
        assert self.db.get(key) == self.model.get(key), key

    @rule(key=KEYS)
    def contains(self, key: bytes) -> None:
        assert (key in self.db) == (key in self.model), key

    @rule(operations=st.lists(st.tuples(KEYS, st.one_of(VALUES, st.none())), max_size=8))
    def write_batch(self, operations: list[tuple[bytes, bytes | None]]) -> None:
        self.db.write_batch(operations)
        # A batch applies in order, so later operations on the same key win --
        # exactly what updating the dict in the same order produces.
        for key, value in operations:
            if value is None:
                self.model.pop(key, None)
            else:
                self.model[key] = value

    @rule()
    def reopen(self) -> None:
        """Close and reopen mid-sequence.

        Every acknowledged write must survive this, so recovery runs constantly
        rather than only in tests written to exercise it.
        """
        self.db.close()
        self.db = stratakv.DB.open(self.path, sync=False, memtable_bytes=self.memtable_bytes)

    @precondition(lambda self: len(self.model) > 0)
    @rule()
    def check_everything(self) -> None:
        """Verify the whole model, not just one key."""
        for key, value in self.model.items():
            assert self.db.get(key) == value, key

    @invariant()
    def never_closed(self) -> None:
        assert not self.db.closed

    def teardown(self) -> None:
        # One last full comparison after whatever sequence was generated, so a
        # discrepancy cannot hide behind the rules that happened to be chosen.
        try:
            for key, value in self.model.items():
                assert self.db.get(key) == value, key
            self.db.close()
            with stratakv.DB.open(self.path) as reopened:
                for key, value in self.model.items():
                    assert reopened.get(key) == value, key
        finally:
            shutil.rmtree(self.directory, ignore_errors=True)


class FlushHeavyKeyValueMachine(KeyValueMachine):
    """The same rules, with a memtable so small it flushes on nearly every write.

    This is where rotation, flush, log deletion and compaction get hammered by
    random operation sequences. It runs far fewer sequences than the machine
    above because each one costs about forty times as much.
    """

    memtable_bytes = TINY_MEMTABLE


_COMMON = {
    "stateful_step_count": 50,
    # Disk I/O; a per-step time limit would only produce flakes on a loaded
    # machine rather than finding anything.
    "deadline": None,
    "suppress_health_check": [HealthCheck.too_slow, HealthCheck.data_too_large],
}

KeyValueMachine.TestCase.settings = settings(max_examples=MAX_EXAMPLES, **_COMMON)
FlushHeavyKeyValueMachine.TestCase.settings = settings(
    max_examples=max(20, MAX_EXAMPLES // 20), **_COMMON
)

TestKeyValueMachine = KeyValueMachine.TestCase
TestFlushHeavyKeyValueMachine = FlushHeavyKeyValueMachine.TestCase
