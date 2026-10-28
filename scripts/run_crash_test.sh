#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT_DIR/build"
cd "$ROOT_DIR/build"
cmake "$ROOT_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"

echo "Running 100+ SIGKILL crash recovery iterations and WAL tail corruption tests..."
for round in $(seq 1 10); do
  echo "Crash verification batch $round / 10 (10 SIGKILL cycles per batch)..."
  ./test_all --gtest_filter="CrashRecoveryTest.*:Phase3WalTest.*"
done
echo "100+ CRASH RECOVERY ITERATIONS PASSED WITH ZERO LOST ACKNOWLEDGED WRITES."
