#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT_DIR/build"
cd "$ROOT_DIR/build"
cmake "$ROOT_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"

./lsm_bench --results-dir "$ROOT_DIR/results" --trials 5 --ops 24000
python3 "$ROOT_DIR/scripts/plot_results.py" "$ROOT_DIR/results"
echo "All benchmark CSVs and charts generated in $ROOT_DIR/results/"
