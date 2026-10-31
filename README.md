# Concurrent LSM-Tree Storage Engine (C++17)

A concurrent **Log-Structured Merge-Tree (LSM-Tree)** key-value storage engine built from scratch in **C++17** as a course project for **Parallel Computing (IT351)** at **NITK Surathkal**.

The engine features a lock-free CAS-based SkipList MemTable, atomic Write-Ahead Logging (WAL), parallel background compaction with disjoint key-range isolation, and crash recovery. It is benchmarked against two baselines (Global-Lock and 16-Shard Partitioned MemTable) under Uniform and Zipfian workloads across 1–32 threads.

## Authors

| Name | Roll Number |
|---|---|
| Aman Yadav | 241AI004 |
| Shah Aagam Alpesh | 241AI036 |
| Chhitanshu Shekhar | 241AI012 |
| Harshvardhan Choudhary | 241AI017 |

**Department of Information Technology, National Institute of Technology Karnataka, Surathkal**

---

## Table of Contents

- [Dependencies](#dependencies)
- [Building the Project](#building-the-project)
- [What Gets Built](#what-gets-built)
- [Running the Benchmarks](#running-the-benchmarks)
- [Generating Charts from Benchmark Results](#generating-charts-from-benchmark-results)
- [Running the Interactive CLI](#running-the-interactive-cli)
- [Running the Crash Recovery Test](#running-the-crash-recovery-test)
- [Project Structure](#project-structure)
- [Architecture Overview](#architecture-overview)
- [Key Design Decisions](#key-design-decisions)
- [Benchmark Results Summary](#benchmark-results-summary)
- [Configuration Options](#configuration-options)
- [Known Limitations](#known-limitations)
- [License](#license)

---

## Dependencies

| Dependency | Why It's Needed | Minimum Version |
|---|---|---|
| **g++** (or any C++17 compiler) | Compiles all C++ source files | GCC 9+ (must support `-std=c++17`) |
| **make** | Build system — the project uses a plain `Makefile` | GNU Make 4.0+ |
| **POSIX threads (pthread)** | Thread creation, atomics, `pwrite`, `fdatasync` | Included with Linux kernel |
| **python3** | Runs `scripts/plot_results.py` | Python 3.8+ |
| **matplotlib** | Renders throughput, latency, and compaction charts as PNG | 3.5.0+ |
| **pandas** | Reads and aggregates `benchmark_matrix.csv` | 1.4.0+ |
| **cmake** | Alternative build path (`CMakeLists.txt`), also needed for GoogleTest suite | CMake 3.16+ |
| **valgrind** | Thread-race analysis via `scripts/run_helgrind.sh` | 3.15+ |
| **clang** / **clang++** | Sanitizer builds (ASan/TSan via `scripts/run_sanitizers.sh`) | Clang 10+ |
| **pdflatex** / **Overleaf** | Compiling `FINAL_REPORT.tex` into a PDF | TeX Live 2020+ |

### Install Commands

**Ubuntu / Debian (22.04, 24.04):**

```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ make cmake valgrind clang
sudo apt-get install -y python3 python3-matplotlib python3-pandas
```

> **Note on Ubuntu 24.04 (PEP 668):** Ubuntu 24.04 blocks `pip install` for system Python. Use the `apt` packages above (`python3-matplotlib`, `python3-pandas`) instead of `pip`. If the apt packages are already installed, you're good — no pip needed.

**Fedora / RHEL:**

```bash
sudo dnf install -y gcc-c++ make python3 python3-matplotlib python3-pandas
```

### Verifying Your Setup

Run these commands to confirm everything is in place before building:

```bash
# Check C++ compiler (must show 9.x or higher)
g++ --version

# Check make
make --version

# Check Python and libraries
python3 -c "import matplotlib; print('matplotlib', matplotlib.__version__)"
python3 -c "import pandas; print('pandas', pandas.__version__)"
```

If any of these fail, install the missing package using the commands above before proceeding.

---

## Building the Project

The project uses a plain **Makefile** at the repository root. No cmake is needed.

```bash
# From the repository root:
make -j$(nproc)
```

This compiles all source files and produces 4 executables and 1 static library in the repository root directory.

To clean all build artifacts:

```bash
make clean
```

---

## What Gets Built

After `make -j$(nproc)` completes, you'll have these files in the project root:

| File | What It Is |
|---|---|
| `liblsm_core.a` | Static library containing the full storage engine |
| `lsm_bench` | Benchmark runner — runs the full matrix of tests and writes CSV results |
| `lsm_cli` | Interactive command-line shell for manually using the database |
| `crash_child` | Helper process used by the crash recovery test (do not run directly) |
| `crash_runner` | Crash recovery test — kills `crash_child` with SIGKILL and verifies durability |

---

## Running the Benchmarks

The benchmark runs all 3 MemTable types × 2 distributions × 6 thread counts × 5 trials = **180 individual runs** and writes raw results to `results/benchmark_matrix.csv`.

### Option A: Run Directly

```bash
# Build first (if not already done)
make -j$(nproc)

# Run the benchmark (takes ~2-5 minutes depending on your machine)
./lsm_bench --results-dir results --trials 5 --ops 24000

# Generate charts from the CSV
python3 scripts/plot_results.py results
```

### Option B: Use the Script

```bash
bash scripts/run_benchmarks.sh
```

This script builds (using cmake if available, otherwise make), runs `lsm_bench`, and then runs `plot_results.py` automatically.

### What Gets Generated

After running, the `results/` directory will contain:

| File | Description |
|---|---|
| `benchmark_matrix.csv` | Raw data: throughput, P50/P99/P999 latency, compaction stats, per trial |
| `slide3_distribution.csv` | Zipfian vs Uniform rank probability validation data |
| `read_amplification.csv` | Average SSTables touched per lookup vs active level count |
| `chart1_throughput_uniform.png` | Throughput scaling chart — Uniform distribution |
| `chart1_throughput_zipfian.png` | Throughput scaling chart — Zipfian distribution |
| `chart2_p99_latency_uniform.png` | P99 tail latency chart — Uniform distribution |
| `chart2_p99_latency_zipfian.png` | P99 tail latency chart — Zipfian distribution |
| `chart3_compaction_overhead.png` | Background compaction I/O ratio vs thread count |
| `chart4_read_amplification.png` | Read amplification vs active on-disk levels |

---

## Generating Charts from Benchmark Results

If you already have `results/benchmark_matrix.csv` (e.g. from a previous run or from the repository), you can regenerate just the charts without re-running benchmarks:

```bash
python3 scripts/plot_results.py results
```

This reads the CSV files in `results/` and overwrites the PNG chart files.

---

## Running the Interactive CLI

```bash
# Build first
make -j$(nproc)

# Launch the CLI pointing to a database directory (created automatically)
./lsm_cli ./my_test_db
```

Available commands inside the CLI:

```
PUT <key> <value>     Insert or update a key-value pair
GET <key>             Look up a key
DEL <key>             Delete a key (writes a tombstone marker)
SCAN <key> [limit]    Scan keys in sorted order starting from <key> (default limit: 10)
BULK_PUT <prefix> <N> Insert N records with the given key prefix
FLUSH                 Force-flush the active MemTable to disk
COMPACT               Trigger background compaction
STATS                 Print engine telemetry (bytes written, lookups, compaction stats)
HELP                  Show command list
QUIT / EXIT           Close database and exit
```

Example session:

```
lsm> PUT user_001 Alice
[OK] Stored: 'user_001' => 'Alice'

lsm> GET user_001
[FOUND] user_001 = 'Alice'

lsm> BULK_PUT sensor 500
[OK] Ingested 500 records in 1.84 ms

lsm> STATS
--- Engine Telemetry & Statistics ---
  Foreground Bytes Written : 42500 bytes
  Total Point Lookups      : 1
  SSTable Files Touched    : 0
  Active On-Disk Levels    : 0
  Write Stalls             : 0
-------------------------------------

lsm> QUIT
```

> **Note:** The `my_test_db/` directory is in `.gitignore` and will not be committed.

---

## Running the Crash Recovery Test

The crash test verifies that **zero acknowledged writes are lost** after an unannounced `SIGKILL`:

```bash
# Build first
make -j$(nproc)

# Run the crash recovery test
./crash_runner
```

Or use the script:

```bash
bash scripts/run_crash_test.sh
```

**How it works:**
1. `crash_runner` forks a child process (`crash_child`) that writes records to the database.
2. After each successful write, the child reports the key ID over a pipe to the parent.
3. The parent sends `SIGKILL` to the child at a random point (mid-write, mid-flush, or mid-compaction).
4. The parent re-opens the database, triggers WAL recovery, and checks that **every acknowledged key** exists on disk with a valid value.
5. This repeats for multiple SIGKILL cycles.

---

## Project Structure

```
.
├── Makefile                    # Primary build system (plain GNU Make)
├── CMakeLists.txt              # Alternative CMake build (optional, requires cmake)
├── FINAL_REPORT.tex            # IEEE-format LaTeX report for the course
├── requirements.txt            # Python dependencies (matplotlib, pandas)
├── .gitignore                  # Ignores build artifacts, db directories, IDE files
│
├── include/lsm/               # Public C++ API headers
│   ├── db.h                    # DB interface, DBStats, DB::Open()
│   ├── iterator.h              # InternalIterator interface
│   ├── memtable.h              # MemTable abstract interface and factory
│   ├── options.h               # Options struct wrapper
│   ├── status.h                # Status codes (OK, NotFound, IOError, Corruption)
│   └── types.h                 # InternalKey, ValueType, SyncPolicy, MemTableType
│
├── src/                        # Core engine implementation
│   ├── db/
│   │   ├── db_impl.cpp         # Main DB: Put, Get, Delete, Scan, MemTable rotation
│   │   ├── db_impl.h           # DBImpl internal state
│   │   ├── flush.cpp           # Background flush worker
│   │   └── recovery.cpp        # WAL replay and MANIFEST recovery
│   ├── memtable/
│   │   ├── arena.h             # Lock-free memory allocator (Treiber stack)
│   │   ├── memtable_lockfree.cpp   # Lock-Free CAS SkipList MemTable
│   │   ├── memtable_global_lock.cpp# Baseline: single mutex MemTable
│   │   ├── memtable_sharded.cpp    # Baseline: 16-shard partitioned mutex MemTable
│   │   ├── skiplist_lockfree.h     # Lock-free skip list (CAS-based)
│   │   ├── skiplist_locked.h       # Mutex-based skip list (for baselines)
│   │   └── memtable_iface.h       # MemTable base with ref-counting
│   ├── wal/
│   │   ├── wal_writer.cpp      # Atomic WAL appends via fetch_add + pwrite
│   │   └── wal_reader.cpp      # WAL replay with CRC32C validation
│   ├── sstable/
│   │   ├── sstable_builder.cpp # Builds immutable SSTable files
│   │   ├── sstable_reader.cpp  # Point lookups with Bloom filter + binary search
│   │   ├── block.cpp           # 4 KB data block formatting with CRC32C
│   │   ├── bloom.cpp           # 10 bits/key Bloom filter (double-hashing)
│   │   └── sstable.h           # SSTable type definitions
│   ├── compaction/
│   │   ├── compaction_picker.cpp # Level scoring and lock-free range reservation
│   │   ├── compaction_job.cpp    # Multi-way SSTable merge
│   │   ├── compaction_pool.cpp   # Background worker thread pool
│   │   └── compaction.h          # Compaction types and DisjointRangeRegistry
│   ├── version/
│   │   ├── version.cpp         # Immutable Version queries
│   │   ├── version_set.cpp     # Atomic version swapping and GC
│   │   ├── manifest.cpp        # MANIFEST log serialization
│   │   └── version.h           # Version, VersionEdit, VersionSet definitions
│   └── util/
│       ├── crc32c.cpp/.h       # CRC32C checksum (hardware/software)
│       ├── histogram.cpp/.h    # HDR latency histogram with percentiles
│       ├── zipf.cpp/.h         # Zipfian distribution generator (precomputed CDF)
│       ├── thread_pool.cpp/.h  # Worker thread pool
│       └── clock.h             # High-resolution nanosecond timer
│
├── bench/                      # Benchmark harness
│   ├── bench_main.cpp          # Benchmark matrix runner
│   ├── workload_gen.cpp/.h     # Uniform + Zipfian key generators
│   └── metrics.cpp/.h          # CSV serialization and metric structs
│
├── tests/                      # Test programs
│   ├── crash_child.cpp         # Forked child for SIGKILL crash test
│   ├── crash_runner.cpp        # Parent harness that kills + verifies recovery
│   └── test_all.cpp            # GoogleTest suite (requires CMake build)
│
├── tools/
│   └── lsm_cli.cpp             # Interactive CLI shell
│
├── scripts/                    # Automation scripts
│   ├── run_benchmarks.sh       # Build + run benchmarks + generate charts
│   ├── run_crash_test.sh       # Build + run crash recovery test
│   ├── plot_results.py         # Generate PNG charts from CSV results
│   ├── run_sanitizers.sh       # ASan/TSan builds (requires cmake)
│   └── run_helgrind.sh         # Valgrind Helgrind analysis (requires cmake + valgrind)
│
├── results/                    # Benchmark output (CSVs + PNGs)
│   ├── benchmark_matrix.csv
│   ├── chart1_throughput_uniform.png
│   ├── chart1_throughput_zipfian.png
│   ├── chart2_p99_latency_uniform.png
│   ├── chart2_p99_latency_zipfian.png
│   ├── chart3_compaction_overhead.png
│   ├── chart4_read_amplification.png
│   ├── read_amplification.csv
│   └── slide3_distribution.csv
│
└── docs/                       # Design documentation
    ├── ARCHITECTURE.md
    ├── DECISIONS.md
    ├── REQUIREMENTS_TRACE.md
    └── RESULTS.md
```

---

## Architecture Overview

The engine separates work into three execution domains:

### Foreground (Lock-Free Write Path)

1. Client thread calls `Put(key, value)`.
2. A monotonic sequence number is assigned via `fetch_add` (no lock).
3. The record is appended to the WAL via atomic offset reservation (`fetch_add` on file offset) + `pwrite()` (no lock).
4. The record is inserted into the active MemTable (lock-free CAS SkipList — bottom-up pointer linking with `compare_exchange_strong`).
5. When the MemTable exceeds its size threshold, exactly one thread wins a CAS to rotate it to the immutable queue and install a fresh MemTable.

### Background (Parallel Flush & Compaction)

- **Flush Worker:** Pulls frozen MemTables from the immutable queue, waits for in-flight writers to drain, then writes a sorted Level-0 SSTable to disk.
- **Compaction Pool:** Multiple worker threads pick levels that exceed their size targets, claim non-overlapping key ranges via a lock-free `DisjointRangeRegistry` (64-slot atomic state machine), and merge SSTables in parallel.

### Read Path

Point lookups search in order: Active MemTable → Immutable MemTables → Level 0 SSTables (newest first) → Level 1+ SSTables. Each SSTable has a 10 bits/key Bloom filter that skips ~99% of negative lookups without reading data blocks.

---

## Key Design Decisions

### Zipfian Distribution Generator

During benchmarking, we discovered that the original Zipfian key generator was computing `std::pow()` in an O(N) loop for every single key generated (~20,000 `pow()` calls per operation). This capped throughput at ~2,400 ops/sec per core regardless of the MemTable implementation, causing all three baselines to report identical throughput under Zipfian workloads.

**Fix:** The CDF (cumulative distribution function) is now **precomputed once** in the constructor as a normalized vector, and each key lookup uses `std::lower_bound` for O(log N) binary search. This reduced per-key generation cost from ~400 μs to <15 ns, allowing the benchmark to actually stress the storage engine's synchronization rather than the key generator.

The relevant files are `src/util/zipf.h` and `src/util/zipf.cpp`.

### MemTable Variants

| Variant | How Writes Are Synchronized | Purpose |
|---|---|---|
| `LOCK_FREE` | CAS-based SkipList — no mutexes | Primary design under study |
| `GLOBAL_LOCK` | Single `std::mutex` around the entire MemTable | Baseline: worst-case contention |
| `SHARDED` | 16 independent mutexes, key hashed to a shard | Baseline: partitioned locking |

### Crash Recovery

Every write is first appended to a WAL file with a CRC32C checksum. On recovery, WAL segments are replayed into fresh MemTables. Torn tail records (partial writes from a crash) are detected by CRC mismatch and safely discarded.

---

## Benchmark Results Summary

All numbers are from `results/benchmark_matrix.csv` (5 trials per configuration, mean reported).

### Throughput at 32 Threads

| MemTable | Uniform (K ops/sec) | Zipfian (K ops/sec) |
|---|---|---|
| **Lock-Free** | **2,145.8** | **1,895.2** |
| Sharded (16) | 1,460.8 | 695.0 |
| Global Lock | 168.9 | 166.1 |

### P99 Tail Write Latency at 32 Threads

| MemTable | Uniform (μs) | Zipfian (μs) |
|---|---|---|
| **Lock-Free** | **28.3** | **32.7** |
| Sharded (16) | 48.9 | 142.3 |
| Global Lock | 348.0 | 354.2 |

### Key Observations

- **Lock-Free** scales from ~312 K ops/sec (1 thread) to ~2.15 M ops/sec (32 threads) under uniform traffic (6.87× scaling).
- **Global Lock** degrades as threads increase — throughput drops 45% from 1 to 32 threads due to mutex contention.
- **Sharded** performs well under uniform traffic (1.46 M ops/sec) but collapses under Zipfian skew (695 K ops/sec, a 52% drop) because hot keys hash to the same 1-2 shards.

---

## Configuration Options

All options are set via `lsm::Options` (defined in `include/lsm/types.h`):

| Option | Default | Description |
|---|---|---|
| `memtable_type` | `LOCK_FREE` | `LOCK_FREE`, `GLOBAL_LOCK`, or `SHARDED` |
| `sharded_memtable_shards` | `16` | Number of shards when using `SHARDED` |
| `memtable_max_size` | `65536` (64 KB) | Byte threshold before MemTable rotation |
| `max_immutable_memtables` | `4` | Max frozen MemTables before write stall |
| `sync_policy` | `NO_SYNC` | `NO_SYNC`, `SYNC_EVERY_WRITE`, or `GROUP_COMMIT` |
| `group_commit_interval_us` | `1000` | Batching window for group commit (μs) |
| `block_size` | `4096` (4 KB) | SSTable data block size |
| `bloom_bits_per_key` | `10` | Bloom filter bits per key (~1% FP rate) |
| `max_levels` | `8` | Maximum on-disk SSTable levels |
| `l0_compaction_trigger` | `4` | Level-0 file count that triggers compaction |
| `l1_max_bytes` | `262144` (256 KB) | Base capacity for Level 1 |
| `level_size_multiplier` | `10` | Each deeper level is 10× larger |
| `compaction_threads` | `2` | Number of background compaction workers |

---

## Known Limitations

1. **Fixed 16-byte keys:** Internal key format is optimized for 16-byte ASCII keys. Longer keys are truncated, shorter keys are padded.
2. **No block cache:** Reads go through the OS page cache (`pread`). There is no application-level LRU cache.
3. **No compression:** SSTable data blocks are stored uncompressed.
4. **Bulk arena reclamation:** SkipList nodes are never individually freed — memory is reclaimed in bulk when the MemTable is flushed and its reference count drops to zero.
5. **GoogleTest suite requires CMake:** The full test suite in `tests/test_all.cpp` uses GoogleTest (fetched via CMake's `FetchContent`). The Makefile does not build `test_all`. If you want to run the GoogleTest suite, you need `cmake` installed and must build via the CMake path. The crash recovery test (`crash_runner`) works with the Makefile and does not require GoogleTest.

---

## License

MIT License. See individual source file headers for details.
