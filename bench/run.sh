#!/usr/bin/env bash
#
# Reproduce every benchmark number in the README, from a fresh clone, with one
# command:
#
#     ./bench/run.sh
#
# It builds the extension into a local virtual environment, runs the suite,
# and draws the charts. Nothing here depends on the machine it was first run
# on; the numbers will differ, and the recorded ones in docs/NUMBERS.md say
# which machine produced them.
#
# Takes roughly 20 minutes, most of it waiting for real disk flushes in the
# durable write workloads. Pass arguments through to shorten a smoke run:
#
#     ./bench/run.sh --operations 2000 --sync-operations 200 \
#         --write-amp-keys 20000 50000

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repository_root"

venv=".venv"
if [[ ! -x "$venv/bin/python" ]]; then
  echo "==> creating $venv"
  python3 -m venv "$venv"
fi
python="$venv/bin/python"

echo "==> installing StrataKV and the benchmark's dependencies"
# Quiet, because a compiler's output would bury the benchmark results.
"$python" -m pip install --quiet --upgrade pip
"$python" -m pip install --quiet -r requirements-bench.txt
# Always reinstall: the extension is compiled, so an older build silently
# benchmarks older engine code. This is the step that makes the run honest.
"$python" -m pip install --quiet --force-reinstall --no-deps .

echo "==> versions"
"$python" - <<'PY'
import sqlite3, sys
import stratakv
print(f"  StrataKV {stratakv.__version__}  Python {sys.version.split()[0]}  SQLite {sqlite3.sqlite_version}")
PY

echo "==> running the suite (this is the slow part)"
"$python" bench/bench.py "$@"

echo "==> drawing the charts"
"$python" bench/plot.py

echo
if [[ $# -eq 0 ]]; then
  echo "Results:  bench/results/results.json"
else
  # Arguments were passed, so --output may point somewhere else; say so rather
  # than print a path that may not be the one just written.
  echo "Results:  wherever --output pointed (default bench/results/results.json)"
fi
echo "Charts:   bench/results/*.png"
