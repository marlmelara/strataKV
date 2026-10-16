# Study guide

The way back into this project after it is built.

The checkpoint quizzes were **deliberately deferred** during the build to save time. Nothing
was skipped in the writing, only in the testing: every phase ships a plain-English
walkthrough of its real code, a debrief, five quiz questions with answers, and additions to
the C++ notes and glossary. This file is the index that ties them together and the place to
track progress through them.

## Why bother, if the code already works

The bar for putting StrataKV on a resume is not "it builds." It is in the build spec's
"Before StrataKV goes on the resume" section:

- [ ] All 20 final-exam questions answered out loud, without notes
- [ ] All 10 learning-guide snippets explained against the real code
- [ ] Tests, fuzzer, and benchmarks run personally from a fresh clone
- [ ] Every resume number traced to a line in [NUMBERS.md](NUMBERS.md)
- [ ] A 60-second walkthrough ready: what it is, how crash safety is proven, one trade-off
      you would change with more time

Deferring the quizzes moved when that happens. It did not lower the bar. An interviewer who
asks "why does macOS need `F_FULLFSYNC`?" is not checking whether the code compiles.

## How to use this

Work one phase at a time, in order, and for each one:

1. **Read the learning guide** for that phase in [learning/](learning/). It opens with a
   summary under 150 words, then traces one operation end to end through the real files, then
   annotates the three to five functions that matter line by line, with the closest Python
   next to each.
2. **Open the real code alongside it** and confirm the guide still matches. If a later phase
   changed a function, the guide for the phase that introduced it may be behind — that
   mismatch is itself worth understanding.
3. **Read that phase's debrief section** in [DEBRIEF.md](DEBRIEF.md): what was built, the
   decisions the spec did not dictate and the alternatives that lost, and the bugs that took
   more than one attempt. The bugs are the interview stories.
4. **Answer the quiz questions out loud before reading the answers.** Two sets per phase:
   five conceptual ones in `documentation/Checkpoint quizzes.pdf`, and five written against
   this repository's actual code at the end of each debrief section. Out loud matters —
   recognizing an answer and being able to say it are different skills.
5. **Mark the phase below** once you can explain it without the guide open.

Then finish with the 20-question final exam in `documentation/Build spec.pdf`, which
escalates the way a real interview does: fundamentals, durability, concurrency, trade-offs,
then Python and testing.

## Progress

| Phase | What it covers | Learning guide | Quiz questions | Studied |
| --- | --- | --- | --- | --- |
| P0 | Build system, `Status`, `Options`, `Env`, CRC32C | [P0-scaffold-and-ci.md](learning/P0-scaffold-and-ci.md) | Spec quiz tab + [debrief](DEBRIEF.md#five-questions) | [ ] |
| P1 | WAL, memtable, skip list, crash recovery | [P1-wal-memtable-recovery.md](learning/P1-wal-memtable-recovery.md) | Spec quiz tab + [debrief](DEBRIEF.md) | [ ] |
| P2 | SSTables, bloom filters, flush, read path | [P2-sstables-flush-read-path.md](learning/P2-sstables-flush-read-path.md) | Spec quiz tab + [debrief](DEBRIEF.md) | [ ] |
| P3 | MANIFEST, versions, compaction, merging iterator | [P3-manifest-versions-compaction.md](learning/P3-manifest-versions-compaction.md) | Spec quiz tab + [debrief](DEBRIEF.md) | [ ] |
| P4 | Concurrency and group commit | [P4-concurrency-group-commit.md](learning/P4-concurrency-group-commit.md) | Spec quiz tab + [debrief](DEBRIEF.md) | [ ] |
| P5 | Python package, property tests, crash fuzzer | [P5-python-package-fuzzer.md](learning/P5-python-package-fuzzer.md) | Spec quiz tab + [debrief](DEBRIEF.md) | [ ] |
| P6 | Benchmarks, README, polish | _pending_ | | [ ] |

## Reference material

Read these when a term or a language feature is unfamiliar rather than front to back.

| File | What it holds |
| --- | --- |
| [CPP_NOTES.md](CPP_NOTES.md) | Every C++ feature used in the real code that is not in the spec's primer, explained next to Python. Grows each phase |
| [GLOSSARY.md](GLOSSARY.md) | Storage, systems, and concurrency terms as they come up. Grows each phase |
| [DESIGN.md](DESIGN.md) | On-disk formats and the durability invariants, with the reasoning |
| [NUMBERS.md](NUMBERS.md) | Every measured number and the command that produced it |
| `documentation/C++ primer.pdf` | The baseline C++ tour written for a Python developer |
| `documentation/Glossary.pdf` | The baseline term list |
| `documentation/Code walkthroughs.pdf` | Fifteen teaching versions of the key code blocks, annotated line by line |
| `documentation/Build spec.pdf` | The spec, the ten core snippets, and the 20-question final exam |

## The shortest version, if time is short

If there is only an hour before an interview, these are the five things that carry most of
what gets asked:

1. **Trace a `Put` end to end**, and be precise about where "acknowledged" happens — after
   the `fsync` returns, not after the append.
2. **Explain why a torn WAL tail is not an error**, and how you tell it apart from mid-file
   corruption.
3. **Explain the four-step atomic file replacement** — write temp, sync temp, rename, sync
   directory — and what breaks if you drop step 2 or step 4.
4. **Explain how readers search the skip list with no lock**, and what the release store
   guarantees.
5. **Explain group commit**, and why throughput rises as more threads write.

Those five are in the final exam, in [DESIGN.md](DESIGN.md), and in the P1/P3/P4 learning
guides.
