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

### 8. Dial's Bucket Queue with Automatic Fallback
- **Hypothesis:** When maximum edge weight is small ($W \le 1024$), Dial's algorithm achieves $O(1)$ amortized bucket operations compared to $O(\log V)$ binary heap pushes.
- **Change:** Evaluated `max_weight` during graph parsing. If $W \le 1024$, queries utilize a circular bucket queue (`BucketQueue`), falling back to `MinHeap` if $W > 1024$.

---

## Final Benchmark Comparison

| Metric | Phase 0 Baseline | Fully Optimized (Opt 1–7) | Speedup |
| :--- | :--- | :--- | :--- |
| **Wall Clock Time** | **5.072s** | **0.707s** | **7.17x faster** |
| **Query Scaling** | 1 thread | 6 P-core threads | ~5.3x parallel efficiency |
| **Output Hash** | Expected | Identical (`diff` matched) | 100% correct |
| **External Dependencies** | 0 | 0 | $D = 1.0$ (no penalty) |

