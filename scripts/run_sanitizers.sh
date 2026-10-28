#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== [1/3] Static Check: Zero Mutexes on Lock-Free Paths ==="
for f in \
  "$ROOT_DIR/src/memtable/arena.h" \
  "$ROOT_DIR/src/memtable/skiplist_lockfree.h" \
  "$ROOT_DIR/src/memtable/memtable_lockfree.cpp" \
  "$ROOT_DIR/src/wal/wal_writer.cpp" \
  "$ROOT_DIR/src/compaction/compaction_picker.cpp"; do
  if grep -E "std::mutex|lock_guard|unique_lock" "$f"; then
    echo "FAIL: Forbidden mutex found in $f"
    exit 1
  fi
done
echo "PASS: Zero mutexes on lock-free MemTable, WAL append, and compaction range-claim paths."

echo "=== [2/3] ThreadSanitizer (TSan) Build & Stress Suite ==="
mkdir -p "$ROOT_DIR/build_tsan"
cd "$ROOT_DIR/build_tsan"
cmake "$ROOT_DIR" -DLSM_ENABLE_TSAN=ON
cmake --build . -j"$(nproc)"
./test_all

echo "=== [3/3] AddressSanitizer + UndefinedBehaviorSanitizer (ASan/UBSan) ==="
mkdir -p "$ROOT_DIR/build_asan"
cd "$ROOT_DIR/build_asan"
cmake "$ROOT_DIR" -DLSM_ENABLE_ASAN=ON
cmake --build . -j"$(nproc)"
./test_all

echo "ALL SANITIZER SUITES PASSED WITH ZERO ERRORS."
