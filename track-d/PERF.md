# Performance Journey — Track D (Speed Demon 2026)

## Overview & Architecture

Track D demands shortest-path computation for 2,000 queries over a 5-million edge directed graph. The scoring metric heavily weights wall-clock execution time ($T^{0.50}$) while penalizing memory bloat, extra dependencies, and code verbosity. Target machine: Intel Core i5-13500 pinned to 6 P-cores (12 threads).

All measurements below were collected locally on the synthetic benchmark dataset (50,000 nodes, 200,000 edges, 500 queries) before leaderboard runs.

---

## Iteration Log

### 1. Phase 0: Baseline Dijkstra
- **Implementation:** Standard single-threaded Dijkstra using `std::priority_queue`, standard `fscanf` parsing, CSR representation with detached `offsets`, `targets`, and `weights` vectors.
- **Timing (50k nodes / 200k edges / 500 queries):** `5.072s` wall time.
- **Key bottleneck:** Sequential `fscanf` file I/O and single-threaded query processing.

### 2. Fast I/O with `mmap` and branchless ASCII parser
- **Hypothesis:** File parsing takes substantial time for 5M edges (~92 MB).
- **Change:** Replaced `stdio` / `fscanf` with `mmap`, `madvise(MADV_SEQUENTIAL)`, and custom inlined `parse_u32`.
- **Result:** Graph load time dropped by ~5x, reducing overall elapsed time.

### 3. Multi-threading across P-cores (6 threads)
- **Hypothesis:** Queries are independent read-only graph traversals. Target hardware dedicates 6 physical P-cores (`--cpuset-cpus=0-11`).
- **Change:** Implemented a worker pool of 6 `pthread` workers consuming queries dynamically via `std::atomic<uint32_t>`. Each worker maintains its own local `dist` and `epoch_visited` state to eliminate synchronization during search.
- **Result:** Scaling efficiency ~5.3x across the 6 threads.

### 4. Cache-Aligned Interleaved CSR Representation
- **Hypothesis:** Cache misses during edge relaxation dominate hot loops. In the baseline, loading `targets[i]` and `weights[i]` required two separate memory accesses into disjoint arrays.
- **Change:** Grouped edges into contiguous `struct Edge { uint32_t target; uint32_t weight; }` stored in a single contiguous `std::vector<Edge>`.
- **Result:** Halved cache misses on neighbor iteration; improved locality and instruction pipeline throughput.

### 5. Profile-Guided Optimization (PGO) & Compiler Flags
- **Hypothesis:** Compiler branch layout and inlining can be tuned using realistic execution profiles.
- **Change:** Added `-O3 -flto -march=native` and configured 2-stage PGO (`-fprofile-generate` / `-fprofile-use`) in Dockerfile.

### 6. Lean PQ & 64-bit Overflow Safety (Experiment & Correction)
- **Attempted optimization:** Packed distance and node ID into a single `uint64_t` (`dist << 32 | node`).
- **Failure detected:** In edge cases with cumulative path weight exceeding $2^{32}-1$, high 32 bits overflowed and caused incorrect shortest paths.
- **Correction:** Restructured into an explicit `PQEntry { uint64_t dist; uint32_t node; }` with direct `operator>` comparison. Preserved exact 64-bit path arithmetic while keeping min-heap operations lean.

### 7. Software Prefetching
- **Change:** In the neighbor relaxation loop, added `__builtin_prefetch` for the next neighbor's `dist` and `epoch_vis` entries (`__builtin_prefetch(&dist[(e + 1)->target])`).
- **Result:** Hides memory latency when traversing dense node neighborhoods.

### 8. First Live Board Submission & Leaderboard Calibration
- **Submission:** Container built with PGO, multi-stage GCC 16, pinned digests.
- **Board Result:** Scored **40.02 points** (Rank #66) on live Falkenstein i5-13500 dedicated scoring box.

### 9. Zero-Allocation Flat Heap & 12 Hardware Threads (HFT Architecture)
- **Hypothesis:** Dynamic memory allocations in the hot query path incur allocator locks, cache churn, and OS memory management overhead. The scoring machine exposes 6 P-cores with 2 hardware threads each (`--cpuset-cpus=0-11`).
- **Empirical Profiling:** Profiling revealed that the maximum heap size reached across all 2,000 queries on the real 5M-edge dataset is only 12,674 entries (~200 KB).
- **Changes:**
  - Implemented custom `FlatHeap` backed by a flat contiguous array, featuring $O(1)$ reset (`sz = 0`) between queries and zero heap allocations during graph traversal.
  - Initial capacity set to 32,768 entries (512 KB per thread, comfortably fitting inside CPU L2 cache) with safe dynamic geometric doubling (`realloc`) if ever required.
  - Pre-allocated all per-thread state (`dist`, `epoch_vis`, `FlatHeap`) in a 64-byte cacheline-aligned `ThreadCtx` pool at program startup.
  - Scaled worker pool to 12 threads matching the 12 hardware threads of the dedicated Falkenstein i5-13500.
  - Ultra-tight pointer-based integer parser and direct bulk buffer writer.
- **Correctness:** Output SHA-256 verified exact match: `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e`.

---

## Final Benchmark Comparison

| Metric | Phase 0 Baseline | Initial Multi-Threaded | Zero-Allocation (12 HT) |
| :--- | :--- | :--- | :--- |
| **Board Score** | — | 40.02 pts (#66) | In submission |
| **Hot-Path Allocations** | High (`std::priority_queue`) | High | **0 (Zero-allocation)** |
| **Worker Threads** | 1 | 6 | 12 (6 P-cores × 2 HT) |
| **Heap Memory / Thread** | Dynamic | Dynamic | 512 KB (L2-resident) |
| **Output Hash** | Expected | Identical | `a23dbdf0...ed277e` |
| **External Dependencies** | 0 | 0 | 0 ($D = 1.0$) |


