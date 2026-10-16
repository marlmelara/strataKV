"""Draw the benchmark results that bench.py recorded.

This reads bench/results/results.json and writes PNGs beside it. It never runs
a benchmark itself, which is deliberate: the numbers in the README have to come
from a recorded run with a known command, machine, and date, so plotting and
measuring are separate steps. Re-running the plots can never change a number.

    python bench/plot.py

Pass --results / --output-dir to work on a different recorded run.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import matplotlib

# Chosen before pyplot is imported: CI has no display, and the default
# interactive backend fails there rather than falling back.
matplotlib.use("Agg")

# Imported after matplotlib.use above, which has to run before pyplot loads.
import matplotlib.pyplot as plt

# StrataKV first in every pair, so the eye reads the same order in every chart.
ENGINE_ORDER = ["stratakv", "sqlite"]
ENGINE_COLOR = {"stratakv": "#1f4e79", "sqlite": "#b45309"}
ENGINE_LABEL = {"stratakv": "StrataKV", "sqlite": "SQLite"}

WRITE_WORKLOADS = ["sequential_fill", "random_fill", "overwrite"]
READ_WORKLOADS = ["random_read_hit", "random_read_miss"]

PRETTY = {
    "sequential_fill": "sequential\nfill",
    "random_fill": "random\nfill",
    "overwrite": "overwrite",
    "random_read_hit": "random read\n(hit)",
    "random_read_miss": "random read\n(miss)",
}


def select(results: list[dict], **criteria: object) -> list[dict]:
    return [r for r in results if all(r[key] == value for key, value in criteria.items())]


def one(results: list[dict], **criteria: object) -> dict | None:
    matches = select(results, **criteria)
    if not matches:
        return None
    if len(matches) > 1:
        raise AssertionError(f"{len(matches)} rows matched {criteria}, expected one")
    return matches[0]


def grouped_bars(
    axes: plt.Axes,
    results: list[dict],
    workloads: list[str],
    durability: str,
    value_bytes: int,
    log_scale: bool,
) -> None:
    """One cluster of bars per workload, one bar per engine."""
    width = 0.38
    positions = range(len(workloads))

    for offset, engine in enumerate(ENGINE_ORDER):
        heights = []
        for workload in workloads:
            row = one(
                results,
                engine=engine,
                durability=durability,
                workload=workload,
                value_bytes=value_bytes,
            )
            heights.append(row["ops_per_second"] if row else 0.0)

        bars = axes.bar(
            [p + (offset - 0.5) * width for p in positions],
            heights,
            width,
            label=ENGINE_LABEL[engine],
            color=ENGINE_COLOR[engine],
        )
        axes.bar_label(bars, fmt="{:,.0f}", fontsize=7, padding=2)

    axes.set_xticks(list(positions))
    axes.set_xticklabels([PRETTY[w] for w in workloads], fontsize=8)
    axes.set_ylabel("operations / second")
    if log_scale:
        axes.set_yscale("log")
    # Headroom for the value labels, which otherwise collide with the frame.
    axes.margins(y=0.22)
    axes.grid(axis="y", alpha=0.25, linewidth=0.5)
    axes.set_axisbelow(True)


def plot_throughput(
    suite: dict, workloads: list[str], durability: str, title: str, subtitle: str, path: str
) -> None:
    value_sizes = sorted({r["value_bytes"] for r in suite["results"]})
    figure, axes_row = plt.subplots(
        1, len(value_sizes), figsize=(5.2 * len(value_sizes), 4.4), squeeze=False
    )

    # Reads span 100k-1.5M ops/s against SQLite's 100k; a linear axis would
    # flatten every write bar into the baseline, so only reads get log scale.
    log_scale = workloads == READ_WORKLOADS

    for column, value_bytes in enumerate(value_sizes):
        axes = axes_row[0][column]
        grouped_bars(axes, suite["results"], workloads, durability, value_bytes, log_scale)
        axes.set_title(f"{value_bytes}-byte values", fontsize=10)

    axes_row[0][0].legend(frameon=False, fontsize=9)
    figure.suptitle(title, fontsize=13, y=0.99)
    figure.text(0.5, 0.925, subtitle, ha="center", fontsize=8, color="#444444")
    figure.tight_layout(rect=(0, 0, 1, 0.91))
    figure.savefig(path, dpi=160)
    plt.close(figure)
    print(f"wrote {path}")


def plot_write_amplification(suite: dict, path: str) -> None:
    rows = suite.get("write_amplification") or []
    if not rows:
        print("no write-amplification rows in the results; skipping that chart")
        return

    rows = sorted(rows, key=lambda r: r["keys"])
    figure, axes = plt.subplots(figsize=(6.4, 4.4))

    axes.plot(
        [r["keys"] for r in rows],
        [r["write_amplification"] for r in rows],
        marker="o",
        color=ENGINE_COLOR["stratakv"],
    )
    for row in rows:
        # Table count is the point of the chart: amplification climbs because
        # compaction rewrites data, and it only compacts once tables exist.
        axes.annotate(
            f"{row['write_amplification']:.2f}x\n{row['sst_files']} "
            f"table{'' if row['sst_files'] == 1 else 's'}",
            (row["keys"], row["write_amplification"]),
            textcoords="offset points",
            xytext=(0, 10),
            ha="center",
            fontsize=7,
        )

    axes.axhline(1.0, color="#999999", linestyle="--", linewidth=0.8)
    axes.annotate(
        "1.0x = every byte written exactly once",
        (rows[0]["keys"], 1.0),
        textcoords="offset points",
        xytext=(4, 5),
        fontsize=7,
        color="#666666",
    )

    memtable = rows[0]["memtable_bytes"]
    axes.set_xlabel("keys written (random order, 100-byte values)")
    axes.set_ylabel("bytes written / bytes of user data")
    axes.set_ylim(bottom=0.0)
    axes.margins(y=0.25)
    axes.grid(alpha=0.25, linewidth=0.5)
    axes.set_axisbelow(True)
    figure.suptitle("Write amplification grows as the tree deepens", fontsize=12)
    figure.text(
        0.5,
        0.915,
        f"memtable {memtable:,} bytes; each compaction rewrites data it has already written",
        ha="center",
        fontsize=8,
        color="#444444",
    )
    figure.tight_layout(rect=(0, 0, 1, 0.9))
    figure.savefig(path, dpi=160)
    plt.close(figure)
    print(f"wrote {path}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", default="bench/results/results.json")
    parser.add_argument("--output-dir", default="bench/results")
    arguments = parser.parse_args(argv)

    if not os.path.exists(arguments.results):
        print(
            f"{arguments.results} does not exist. Run bench/bench.py first:\n"
            f"    python bench/bench.py",
            file=sys.stderr,
        )
        return 1

    with open(arguments.results) as handle:
        suite = json.load(handle)

    os.makedirs(arguments.output_dir, exist_ok=True)
    machine = suite.get("machine", {})
    stamp = f"{machine.get('cpu', 'unknown CPU')}, {machine.get('platform', '')}"

    plot_throughput(
        suite,
        WRITE_WORKLOADS,
        "sync",
        "Durable writes: every operation reaches the disk",
        f"StrataKV F_FULLFSYNC vs SQLite WAL synchronous=FULL fullfsync=ON  |  {stamp}",
        os.path.join(arguments.output_dir, "writes_durable.png"),
    )
    plot_throughput(
        suite,
        WRITE_WORKLOADS,
        "nosync",
        "Buffered writes: the operating system decides when to flush",
        f"StrataKV kNoSync vs SQLite WAL synchronous=NORMAL  |  {stamp}",
        os.path.join(arguments.output_dir, "writes_buffered.png"),
    )
    plot_throughput(
        suite,
        READ_WORKLOADS,
        "sync",
        "Reads (log scale): bloom filters decide the miss case",
        f"A miss can be answered without touching a table file  |  {stamp}",
        os.path.join(arguments.output_dir, "reads.png"),
    )
    plot_write_amplification(suite, os.path.join(arguments.output_dir, "write_amplification.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
