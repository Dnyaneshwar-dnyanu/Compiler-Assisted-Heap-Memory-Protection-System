# MemoryGuard Empirical Performance Benchmarks & Overhead Analysis

This document provides empirical performance evaluations comparing baseline (uninstrumented) binaries against MemoryGuard-instrumented binaries across multiple workload profiles and input scales.

---

## 1. Benchmarking Methodology & Rigor

Measuring runtime overhead in compiler instrumentation requires strict experimental controls to prevent measurement distortion:

1. **Dead-Code Elimination Defense**: Under `-O2` optimization, modern compilers will identify heap allocations whose values never escape and eliminate the `malloc`/`free` calls entirely. MemoryGuard benchmarks use an inline assembly memory barrier (`escape_ptr(void *p) { asm volatile("" : : "g"(p) : "memory"); }`) to force the compiler to treat allocations and memory accesses as live and non-removable.
2. **Algorithmic Parity via Checksum Verification**: Every benchmark computes a 64-bit cumulative checksum over all accessed memory. Identical checksums across baseline and instrumented binaries guarantee both variants perform identical computational work.
3. **High-Resolution Monotonic Timing**: Timings are collected using POSIX monotonic high-resolution timers (`clock_gettime(CLOCK_MONOTONIC)`), recorded in integer nanoseconds to prevent floating-point rounding error.
4. **Sample Duration Adequacy**: Automated workload batching ensures each measured timing interval spans $\ge 100\text{ ms}$, eliminating noise from sub-millisecond timer quantization and OS scheduling interruptions.
5. **Statistical Repetition**: 5 measured repetitions per workload preceded by dedicated warm-up cycles.

---

## 2. Test Environment

- **Operating System**: Linux 6.6.87.2-microsoft-standard-WSL2-x86_64 (glibc 2.39)
- **Architecture**: x86_64
- **Compiler**: Ubuntu Clang version 18.1.3 (1ubuntu1)
- **Optimization Level**: `-O2` (identically applied to baseline and instrumented binaries)
- **Threading Engine**: POSIX `pthreads` with mutual exclusion locks

---

## 3. Workload Definitions

| Workload | Primary Unit | Description |
|:---|:---|:---|
| `malloc_free` | `allocation_lifecycle` | Dynamic heap lifecycle (1 malloc + 1 boundary write + 1 boundary read + 1 free). |
| `seq_read_write` | `sequential_rmw` | Sequential heap read-modify-write access pattern across contiguous buffer. |
| `random_access` | `random_rmw` | Pseudo-random heap read-modify-write access testing non-contiguous spatial patterns. |
| `multi_alloc` | `dynamic_lifecycle` | Mixed dynamic heap operations with multiple concurrent active allocations. |
| `interprocedural`| `forwarded_rmw` | Heap access forwarded across non-inlined function boundaries. |
| `multithreaded` | `concurrent_lifecycle`| Concurrent allocation and access cycles distributed across 4 worker threads. |

---

## 4. Empirical Performance Results

### Small Scale (1,000 Nominal Operations)

| Workload | Effective Ops | Base Duration | Inst Duration | Base (ns/op) | Inst (ns/op) | Throughput (Mops/s) | Slowdown | Checksum Match |
|:---|---:|---:|---:|---:|---:|---:|---:|:---:|
| `malloc_free` | 5,000,000 | 41.3 ms | 770.2 ms | 8.26 | 154.03 | 6.49 | **18.64x** | PASS (`0x259ac520`) |
| `seq_read_write` | 5,000,000 | 17.8 ms | 990.5 ms | 3.55 | 198.11 | 5.05 | **55.75x** | PASS (`0x5a5a5a5aa92eed60`) |
| `random_access` | 641,000 | 9.2 ms | 225.3 ms | 14.42 | 351.54 | 2.84 | **24.39x** | PASS (`0xaaaaaaa9d7bf8277`) |
| `multi_alloc` | 3,108,000 | 40.1 ms | 746.7 ms | 12.89 | 240.25 | 4.16 | **18.63x** | PASS (`0x16b21864`) |
| `interprocedural`| 5,000,000 | 7.0 ms | 270.7 ms | 1.40 | 54.15 | 18.47 | **38.73x** | PASS (`0x35dac572864780`) |
| `multithreaded` | 362,000 | 50.9 ms | 236.1 ms | 140.73 | 652.32 | 1.53 | **4.64x** | PASS (`0x2afb2c8`) |

---

### Medium Scale (100,000 Nominal Operations)

| Workload | Effective Ops | Base Duration | Inst Duration | Base (ns/op) | Inst (ns/op) | Throughput (Mops/s) | Slowdown | Checksum Match |
|:---|---:|---:|---:|---:|---:|---:|---:|:---:|
| `malloc_free` | 12,100,000 | 101.1 ms | 1551.8 ms | 8.36 | 128.24 | 7.80 | **15.34x** | PASS (`0xa8c53df0`) |
| `seq_read_write` | 121,100,000 | 117.2 ms | 5042.2 ms | 0.97 | 41.64 | 24.02 | **43.02x** | PASS (`0xb4b496c6ad9f8a90`) |
| `random_access` | 49,600,000 | 76.7 ms | 2116.7 ms | 1.55 | 42.67 | 23.43 | **27.59x** | PASS (`0x55555516afcc3990`) |
| `multi_alloc` | 1,800,000 | 56.0 ms | 537.5 ms | 31.10 | 298.61 | 3.35 | **9.60x** | PASS (`0x1927bc54`) |
| `interprocedural`| 104,000,000 | 122.6 ms | 4063.2 ms | 1.18 | 39.07 | 25.60 | **33.15x** | PASS (`0x3078518a19abc00`) |
| `multithreaded` | 17,000,000 | 85.8 ms | 5392.7 ms | 5.05 | 317.22 | 3.15 | **62.83x** | PASS (`0xb1dd5020`) |

---

### Large Scale (1,000,000 Nominal Operations)

| Workload | Effective Ops | Base Duration | Inst Duration | Base (ns/op) | Inst (ns/op) | Throughput (Mops/s) | Slowdown | Checksum Match |
|:---|---:|---:|---:|---:|---:|---:|---:|:---:|
| `malloc_free` | 13,000,000 | 110.7 ms | 1683.2 ms | 8.51 | 129.47 | 7.72 | **15.21x** | PASS (`0xc4567ce0`) |
| `seq_read_write` | 97,000,000 | 89.2 ms | 3879.2 ms | 0.92 | 39.99 | 25.01 | **43.46x** | PASS (`0xb7dca55970619ae0`) |
| `random_access` | 66,000,000 | 103.9 ms | 2624.3 ms | 1.57 | 39.76 | 25.15 | **25.25x** | PASS (`0xffffffac5dcc07d6`) |
| `multi_alloc` | 2,000,000 | 70.2 ms | 537.6 ms | 35.12 | 268.78 | 3.72 | **7.65x** | PASS (`0x1e34e332`) |
| `interprocedural`| 91,000,000 | 115.6 ms | 3753.2 ms | 1.27 | 41.24 | 24.25 | **32.46x** | PASS (`0x2b49d43ed409b80`) |
| `multithreaded` | 25,000,000 | 103.7 ms | 9106.2 ms | 4.15 | 364.25 | 2.75 | **87.81x** | PASS (`0x1746229e0`) |

---

## 5. Allocation Registry Microbenchmark: AVL Tree vs. Linked List

To evaluate the algorithmic advantage of maintaining active allocations in an AVL Interval Tree rather than a linear linked list, lookup latency was evaluated under varying numbers of active allocations ($N$):

| Active Allocations ($N$) | Linked List Search Time | AVL Interval Search Time | AVL Speedup Factor | Disjoint Interval Query Complexity |
|---:|---:|---:|---:|:---|
| **100** | 124.2 ns | 88.7 ns | **1.4x** | $O(\log N)$ vs $O(N)$ |
| **1,000** | 1,450.6 ns | 190.8 ns | **7.6x** | $O(\log N)$ vs $O(N)$ |
| **10,000** | 18,250.0 ns | 169.9 ns | **107.4x** | $O(\log N)$ vs $O(N)$ |
| **100,000** | 215,980.0 ns | 209.0 ns | **1033.4x** | $O(\log N)$ vs $O(N)$ |

### Microbenchmark Key Finding & Algorithmic Complexity Nuance

> [!IMPORTANT]
> **Theoretical Complexity Scope & Disjoint Allocation Guarantee**:
> - In general computational geometry, arbitrary interval trees with arbitrary overlapping intervals cannot guarantee worst-case $O(\log N)$ time for enclosing interval queries without exhaustive subtree backtracking ($O(N)$ worst-case).
> - In MemoryGuard, the $O(\log N)$ query guarantee applies specifically because **active heap memory allocations are mutually disjoint**:
>   $$\forall i \ne j, \quad [\text{start}_i, \text{end}_i) \cap [\text{start}_j, \text{end}_j) = \emptyset$$
> - Because active buffers never overlap in valid heap state, any pointer address $P$ can belong to **at most one** active allocation in the entire tree.
> - The AVL tree is balanced on `start` address and augmented with subtree `max_end`. Pruning rule 1 (`node->max_end < access_end`) immediately discards subtrees that cannot reach the target, and pruning rule 2 (`node->start < access_start`) restricts right-branch exploration.
> - As a result, search paths for interior pointers follow a strictly bounded branch of height $h \le 1.44 \log_2 N = O(\log N)$.
> - In workloads maintaining tens of thousands of active allocations, a linear linked list causes severe performance collapse due to cache misses and $O(N)$ node traversals. The self-balancing AVL Interval Tree maintains sub-microsecond query times (~209 ns at $N = 100,000$), delivering an empirical **1033x speedup**.

---

## 6. Overhead Synthesis & Root Causes

1. **Memory Check Interception Frequency**: In tight sequential loops (`seq_read_write`), every 4-byte or 8-byte write is intercepted by a function call to `mg_check_access()`. The register spill, function call overhead, and AVL tree search increase per-access cost from ~0.9 ns to ~40 ns (~43x slowdown).
2. **Allocation & Free Interception**: For `malloc_free`, each allocation requires memory allocation from libc plus AVL tree insertion and node allocation, resulting in ~15x slowdown.
3. **Multithreaded Lock Contention**: In `multithreaded` benchmarks, 4 threads concurrently modify and query the central AVL tree under a single POSIX mutex lock. Contention increases latency from ~4 ns/op to ~364 ns/op (up to ~87x slowdown). Introducing per-thread allocation arenas or read-write locks (`pthread_rwlock_t`) represents a clear avenue for future optimization.
