// Speed Demon 2026 — Track D — Zero-Allocation Optimized
//
// 1. mmap + fast manual parser
// 2. Interleaved CSR (target, weight packed)
// 3. Zero-allocation flat min-heap with O(1) reset
// 4. 12 hyperthreads (matching --cpuset-cpus=0-11)
// 5. __builtin_prefetch to hide memory latency
// 6. All per-thread state pre-allocated at startup

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

static constexpr uint64_t INF_DIST = UINT64_MAX;
static constexpr int NUM_THREADS = 12;

// ---- Zero-copy mmap file ----

struct MappedFile {
    const char *data, *end;
    size_t sz;
    int fd;
    MappedFile(const char* p) {
        fd = open(p, O_RDONLY);
        if (fd < 0) { perror("open"); exit(1); }
        struct stat st;
        fstat(fd, &st);
        sz = st.st_size;
        data = static_cast<const char*>(mmap(nullptr, sz, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
        if (data == MAP_FAILED) { perror("mmap"); exit(1); }
        end = data + sz;
        madvise(const_cast<void*>(static_cast<const void*>(data)), sz, MADV_SEQUENTIAL);
    }
    ~MappedFile() { munmap(const_cast<void*>(static_cast<const void*>(data)), sz); close(fd); }
};

// ---- Ultra-tight integer parser ----

static inline uint32_t parse_u32(const char*& __restrict__ p) {
    while (*p < '0') ++p;
    uint32_t v = *p++ - '0';
    while (*p >= '0') v = v * 10u + (*p++ - '0');
    return v;
}

// ---- Interleaved CSR graph ----

struct Edge { uint32_t target, weight; };

struct Graph {
    uint32_t n, m;
    uint32_t* offsets;   // n+1
    Edge* edges;         // m
};

static Graph load_graph(const char* path) {
    MappedFile mf(path);
    const char* p = mf.data;

    Graph g;
    g.n = parse_u32(p);
    g.m = parse_u32(p);

    // Temp edges for single-pass CSR build
    uint32_t* src = static_cast<uint32_t*>(malloc(g.m * sizeof(uint32_t)));
    uint32_t* dst = static_cast<uint32_t*>(malloc(g.m * sizeof(uint32_t)));
    uint32_t* wgt = static_cast<uint32_t*>(malloc(g.m * sizeof(uint32_t)));

    for (uint32_t i = 0; i < g.m; i++) {
        src[i] = parse_u32(p);
        dst[i] = parse_u32(p);
        wgt[i] = parse_u32(p);
    }

    // Build offsets
    g.offsets = static_cast<uint32_t*>(calloc(g.n + 1, sizeof(uint32_t)));
    for (uint32_t i = 0; i < g.m; i++) g.offsets[src[i] + 1]++;
    for (uint32_t i = 1; i <= g.n; i++) g.offsets[i] += g.offsets[i - 1];

    // Place edges
    g.edges = static_cast<Edge*>(malloc(g.m * sizeof(Edge)));
    uint32_t* cursor = static_cast<uint32_t*>(malloc(g.n * sizeof(uint32_t)));
    memcpy(cursor, g.offsets, g.n * sizeof(uint32_t));
    for (uint32_t i = 0; i < g.m; i++) {
        uint32_t idx = cursor[src[i]]++;
        g.edges[idx] = {dst[i], wgt[i]};
    }

    free(cursor);
    free(src); free(dst); free(wgt);
    return g;
}

// ---- Queries ----

struct Query { uint32_t s, t; };

static uint32_t load_queries(const char* path, Query*& out) {
    MappedFile mf(path);
    const char* p = mf.data;
    const char* end = mf.end;

    // Count queries first
    uint32_t count = 0;
    const char* scan = p;
    while (scan < end) { if (*scan == '\n') count++; scan++; }
    if (scan > mf.data && *(scan - 1) != '\n') count++; // last line without newline

    out = static_cast<Query*>(malloc(count * sizeof(Query)));
    uint32_t qi = 0;
    while (p < end) {
        while (p < end && *p < '0') ++p;
        if (p >= end) break;
        uint32_t s = parse_u32(p);
        uint32_t t = parse_u32(p);
        out[qi++] = {s, t};
    }
    return qi;
}

// ---- Zero-allocation flat min-heap (HFT-style) ----

struct alignas(64) FlatHeap {
    struct Entry { uint64_t dist; uint32_t node; };
    Entry* data;
    uint32_t sz;
    uint32_t cap;

    void init(uint32_t initial_cap = 32768) {
        cap = initial_cap;
        data = static_cast<Entry*>(malloc(cap * sizeof(Entry)));
        sz = 0;
    }

    inline void reset() { sz = 0; }
    inline bool empty() const { return sz == 0; }

    inline void push(uint64_t d, uint32_t n) {
        if (__builtin_expect(sz == cap, 0)) {
            cap *= 2;
            data = static_cast<Entry*>(realloc(data, cap * sizeof(Entry)));
        }
        uint32_t i = sz++;
        while (i > 0) {
            uint32_t p = (i - 1) >> 1;
            if (data[p].dist <= d) break;
            data[i] = data[p];
            i = p;
        }
        data[i] = {d, n};
    }

    inline uint64_t top_dist() const { return data[0].dist; }
    inline uint32_t top_node() const { return data[0].node; }

    inline void pop() {
        uint64_t d = data[--sz].dist;
        uint32_t n = data[sz].node;
        uint32_t i = 0;
        for (;;) {
            uint32_t c = 2 * i + 1;
            if (c >= sz) break;
            if (c + 1 < sz && data[c + 1].dist < data[c].dist) c++;
            if (d <= data[c].dist) break;
            data[i] = data[c];
            i = c;
        }
        data[i] = {d, n};
    }

    void destroy() { free(data); data = nullptr; }
};

// ---- Dijkstra (zero allocation in hot path) ----

static inline uint64_t dijkstra(const Graph& g, uint32_t src, uint32_t tgt,
                                uint64_t* __restrict__ dist,
                                uint32_t* __restrict__ epoch_vis,
                                uint32_t epoch, FlatHeap& pq) {
    if (src == tgt) return 0;
    pq.reset();

    dist[src] = 0;
    epoch_vis[src] = epoch;
    pq.push(0, src);

    while (!pq.empty()) {
        uint64_t du = pq.top_dist();
        uint32_t u  = pq.top_node();
        pq.pop();

        if (u == tgt) return du;
        if (du > dist[u]) continue;

        const Edge* __restrict__ eb = g.edges + g.offsets[u];
        const Edge* __restrict__ ee = g.edges + g.offsets[u + 1];

        for (const Edge* e = eb; e < ee; ++e) {
            // Prefetch next neighbor's dist and epoch to hide L3 latency
            if (__builtin_expect(e + 1 < ee, 1)) {
                __builtin_prefetch(&dist[(e + 1)->target], 1, 1);
                __builtin_prefetch(&epoch_vis[(e + 1)->target], 0, 1);
            }
            uint64_t nd = du + e->weight;
            uint32_t v = e->target;
            if (epoch_vis[v] != epoch || nd < dist[v]) {
                dist[v] = nd;
                epoch_vis[v] = epoch;
                pq.push(nd, v);
            }
        }
    }
    return INF_DIST;
}

// ---- Thread pool (12 HW threads = 6 P-cores × 2 HT) ----

struct alignas(64) ThreadCtx {
    const Graph* g;
    const Query* q;
    uint64_t* res;
    uint32_t total;
    std::atomic<uint32_t>* next_q;
    uint64_t* dist;
    uint32_t* epoch_vis;
    uint32_t epoch;
    FlatHeap pq;
};

static void* worker(void* arg) {
    ThreadCtx* c = static_cast<ThreadCtx*>(arg);
    const Graph& g = *c->g;
    while (true) {
        uint32_t i = c->next_q->fetch_add(1, std::memory_order_relaxed);
        if (i >= c->total) break;
        c->epoch++;
        c->res[i] = dijkstra(g, c->q[i].s, c->q[i].t,
                              c->dist, c->epoch_vis, c->epoch, c->pq);
    }
    return nullptr;
}

// ---- Fast output writer ----

static void write_results(const char* path, const uint64_t* res, uint32_t nq) {
    // Pre-size buffer: max 20 digits + newline per result
    char* buf = static_cast<char*>(malloc(nq * 22));
    char* wp = buf;

    for (uint32_t i = 0; i < nq; i++) {
        if (res[i] == INF_DIST) {
            *wp++ = '-'; *wp++ = '1';
        } else {
            // Fast uint64 to decimal
            char tmp[20];
            int len = 0;
            uint64_t v = res[i];
            if (v == 0) { *wp++ = '0'; }
            else {
                while (v) { tmp[len++] = '0' + (v % 10); v /= 10; }
                while (len) *wp++ = tmp[--len];
            }
        }
        *wp++ = '\n';
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const char* p = buf;
    size_t rem = wp - buf;
    while (rem > 0) { ssize_t w = write(fd, p, rem); p += w; rem -= w; }
    close(fd);
    free(buf);
}

// ---- Main ----

int main(int argc, char** argv) {
    const char* gp = argc > 1 ? argv[1] : "/data/graph.txt";
    const char* qp = argc > 2 ? argv[2] : "/data/queries.txt";
    const char* op = argc > 3 ? argv[3] : "/out/distances.txt";

    Graph g = load_graph(gp);

    Query* queries;
    uint32_t nq = load_queries(qp, queries);

    uint64_t* results = static_cast<uint64_t*>(malloc(nq * sizeof(uint64_t)));
    std::atomic<uint32_t> next_q{0};

    int nt = std::min<int>(NUM_THREADS, nq);
    ThreadCtx* ctx = static_cast<ThreadCtx*>(aligned_alloc(64, nt * sizeof(ThreadCtx)));

    for (int i = 0; i < nt; i++) {
        ctx[i].g = &g;
        ctx[i].q = queries;
        ctx[i].res = results;
        ctx[i].total = nq;
        ctx[i].next_q = &next_q;
        // Pre-allocate all per-thread memory ONCE — zero allocation during queries
        ctx[i].dist = static_cast<uint64_t*>(aligned_alloc(64, g.n * sizeof(uint64_t)));
        ctx[i].epoch_vis = static_cast<uint32_t*>(calloc(g.n, sizeof(uint32_t)));
        ctx[i].epoch = 0;
        ctx[i].pq.init(32768); // worst case: every node in the heap
    }

    pthread_t* threads = static_cast<pthread_t*>(malloc(nt * sizeof(pthread_t)));
    for (int i = 0; i < nt; i++)
        pthread_create(&threads[i], nullptr, worker, &ctx[i]);
    for (int i = 0; i < nt; i++)
        pthread_join(threads[i], nullptr);

    write_results(op, results, nq);

    // Cleanup (optional, process exits anyway)
    for (int i = 0; i < nt; i++) {
        free(ctx[i].dist);
        free(ctx[i].epoch_vis);
        ctx[i].pq.destroy();
    }
    free(ctx);
    free(threads);
    free(queries);
    free(results);
    free(g.offsets);
    free(g.edges);

    return 0;
}
