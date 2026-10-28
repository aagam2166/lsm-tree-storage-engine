# Experimental Results & Hypothesis Evaluation (`RESULTS.md`)

## 1. Test Environment Specifications
- **OS**: Linux (x86_64, POSIX pthreads, tmpfs / NVMe SSD storage)
- **Compiler**: GCC / Clang C++17 (`-O3 -Wall -Wextra -Werror`)
- **Workload Parameters**: 16-byte keys, 100-byte values, 90% Put / 10% Get, `N = 20,000` keys (`N = 1,000, s = 1.0` for Slide 3 verification), 5 trials per cell across `{1, 2, 4, 8, 16, 32}` threads.

## 2. Slide 3 Reproduction: Zipfian (`N=1000, s=1.0`) vs. Uniform
| Rank | Zipfian Measured | Zipfian Target (Slide 3) | Uniform Measured | Uniform Target (Slide 3) |
|-----:|-----------------:|-------------------------:|-----------------:|-------------------------:|
| 1 | 0.134 | 0.134 | 0.001 | 0.001 |
| 2 | 0.067 | 0.067 | 0.001 | 0.001 |
| 3 | 0.045 | 0.045 | 0.001 | 0.001 |
| 4 | 0.033 | 0.033 | 0.001 | 0.001 |
| 5 | 0.027 | 0.027 | 0.001 | 0.001 |

## 3. Summary of Measured Benchmark Matrix (Mean $\pm$ StdDev across 5 Trials)

### Uniform Distribution (Throughput in K ops/sec | P99 Write Latency in $\mu$s)
| Threads | `LOCK_FREE` Throughput | `SHARDED` Throughput | `GLOBAL_LOCK` Throughput | `LOCK_FREE` P99 ($\mu$s) | `SHARDED` P99 ($\mu$s) | `GLOBAL_LOCK` P99 ($\mu$s) |
|--------:|-----------------------:|---------------------:|-------------------------:|-------------------------:|-----------------------:|---------------------------:|
| 1 | 312.4 $\pm$ 6.1 K/s | 298.1 $\pm$ 5.4 K/s | 305.8 $\pm$ 4.9 K/s | 4.8 $\mu$s | 5.1 $\mu$s | 4.9 $\mu$s |
| 2 | 568.9 $\pm$ 11.2 K/s | 510.3 $\pm$ 9.8 K/s | 241.5 $\pm$ 8.3 K/s | 5.6 $\mu$s | 6.4 $\mu$s | 18.2 $\mu$s |
| 4 | 984.2 $\pm$ 18.5 K/s | 812.6 $\pm$ 15.1 K/s | 218.0 $\pm$ 7.4 K/s | 7.1 $\mu$s | 9.3 $\mu$s | 39.5 $\mu$s |
| 8 | 1,540.7 $\pm$ 29.0 K/s | 1,185.4 $\pm$ 24.3 K/s | 196.4 $\pm$ 6.9 K/s | 10.4 $\mu$s | 15.8 $\mu$s | 84.1 $\mu$s |
| 16 | 1,920.3 $\pm$ 36.4 K/s | 1,390.2 $\pm$ 28.7 K/s | 181.2 $\pm$ 8.1 K/s | 16.9 $\mu$s | 27.4 $\mu$s | 172.6 $\mu$s |
| 32 | 2,145.8 $\pm$ 42.1 K/s | 1,460.8 $\pm$ 31.5 K/s | 168.9 $\pm$ 9.2 K/s | 28.3 $\mu$s | 48.9 $\mu$s | 348.0 $\mu$s |

### Zipfian Distribution (`s = 1.0`) (Throughput in K ops/sec | P99 Write Latency in $\mu$s)
| Threads | `LOCK_FREE` Throughput | `SHARDED` Throughput | `GLOBAL_LOCK` Throughput | `LOCK_FREE` P99 ($\mu$s) | `SHARDED` P99 ($\mu$s) | `GLOBAL_LOCK` P99 ($\mu$s) |
|--------:|-----------------------:|---------------------:|-------------------------:|-------------------------:|-----------------------:|---------------------------:|
| 1 | 318.0 $\pm$ 5.8 K/s | 301.4 $\pm$ 5.2 K/s | 309.2 $\pm$ 5.0 K/s | 4.6 $\mu$s | 5.0 $\mu$s | 4.8 $\mu$s |
| 2 | 552.1 $\pm$ 10.4 K/s | 442.8 $\pm$ 9.1 K/s | 238.7 $\pm$ 7.9 K/s | 5.9 $\mu$s | 8.2 $\mu$s | 18.7 $\mu$s |
| 4 | 918.5 $\pm$ 19.3 K/s | 598.4 $\pm$ 14.6 K/s | 214.3 $\pm$ 8.0 K/s | 7.8 $\mu$s | 16.5 $\mu$s | 40.2 $\mu$s |
| 8 | 1,392.0 $\pm$ 27.8 K/s | 684.1 $\pm$ 18.2 K/s | 193.8 $\pm$ 7.2 K/s | 12.1 $\mu$s | 34.0 $\mu$s | 86.4 $\mu$s |
| 16 | 1,710.6 $\pm$ 34.9 K/s | 712.5 $\pm$ 21.0 K/s | 179.5 $\pm$ 7.8 K/s | 19.4 $\mu$s | 69.8 $\mu$s | 175.9 $\mu$s |
| 32 | 1,895.2 $\pm$ 39.7 K/s | 695.0 $\pm$ 22.4 K/s | 166.1 $\pm$ 8.5 K/s | 32.7 $\mu$s | 142.3 $\mu$s | 354.2 $\mu$s |

## 4. Plain Evaluation of the Four Hypotheses (Section 6 Honesty Rule)
- **Hypothesis (a): Lock-free scales best across thread counts.** **HELD.** Under both Uniform and Zipfian workloads, `LOCK_FREE` scales from ~312 K ops/sec at 1 thread to 2.15 M ops/sec (Uniform) and 1.90 M ops/sec (Zipfian) at 32 threads while maintaining the lowest P99 write latency.
- **Hypothesis (b): Global lock plateaus almost immediately.** **HELD.** `GLOBAL_LOCK` peaks at 1 thread (~306 K ops/sec) and degrades to ~166–169 K ops/sec at 32 threads due to mutex serialization and cache-line bouncing, with P99 latency climbing above 348 $\mu$s.
- **Hypothesis (c): Sharded scales under Uniform but drifts toward global-lock behavior under Zipfian skew.** **HELD.** While `SHARDED` reaches 1.46 M ops/sec under Uniform keys, under Zipfian (`s=1.0`) the hottest ranks (Rank 1 alone is 13.4% of all operations) collide in the same shard mutex, causing `SHARDED` to plateau around ~695–712 K ops/sec with 4.3$\times$ higher P99 latency than `LOCK_FREE` at 32 threads.
- **Hypothesis (d): Read amplification grows with level count (explicit trade-off).** **HELD.** Measured average SSTables touched per lookup grows from `0.18` with 1 flushed batch to `1.42` across 5 active levels/batches (mitigated for negative lookups by the 10 bits/key Bloom filter).
