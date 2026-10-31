#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if command -v cmake >/dev/null 2>&1; then
  mkdir -p "$ROOT_DIR/build"
  cd "$ROOT_DIR/build"
  cmake "$ROOT_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build . -j"$(nproc)"
  if [ -f "./crash_runner" ]; then
    ./crash_runner
  elif [ -f "./test_all" ]; then
    ./test_all --gtest_filter="CrashRecoveryTest.*:Phase3WalTest.*"
  fi
else
  echo "CMake not detected; building directly with make..."
  make crash_child crash_runner -j"$(nproc)"
  ./crash_runner
fi
