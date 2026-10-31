// Speed Demon 2026 — Track D
// Optimized Dijkstra with mmap parsing, interleaved CSR, 6-thread pool,
// epoch-based reset, lean PQ, and software prefetching.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <queue>
#include <limits>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

static constexpr uint64_t INF_DIST = std::numeric_limits<uint64_t>::max();
static constexpr int NUM_THREADS = 6;

// RAII memory-mapped file with sequential access hint
struct MappedFile {
    const char *data, *end;
    size_t sz;
    MappedFile(const char* p) {
        int fd = open(p, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "ERROR: open %s\n", p); exit(1); }
        struct stat st;
        fstat(fd, &st);
        sz = st.st_size;
        data = static_cast<const char*>(mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0));
        if (data == MAP_FAILED) { fprintf(stderr, "ERROR: mmap\n"); exit(1); }
        end = data + sz;
        madvise(const_cast<void*>(static_cast<const void*>(data)), sz, MADV_SEQUENTIAL);
        close(fd);
    }
    ~MappedFile() { munmap(const_cast<void*>(static_cast<const void*>(data)), sz); }
};

// Fast inline unsigned integer parser
static inline uint32_t parse_u32(const char*& p, const char* end) {
    while (p < end && static_cast<unsigned char>(*p - '0') > 9) ++p;
    uint32_t val = 0;
    while (p < end && static_cast<unsigned char>(*p - '0') <= 9)
        val = val * 10u + (*p++ - '0');
    return val;
}

// Interleaved CSR graph representation for high cache locality
struct Edge { uint32_t target, weight; };

struct Graph {
    uint32_t n = 0, m = 0;
    std::vector<uint32_t> offsets;
    std::vector<Edge> edges;
};

static Graph load_graph(const char* path) {
    MappedFile mf(path);
    const char *p = mf.data, *end = mf.end;
    Graph g;
    g.n = parse_u32(p, end);
    g.m = parse_u32(p, end);

    struct Raw { uint32_t u, v, w; };
    std::vector<Raw> raw(g.m);
    for (uint32_t i = 0; i < g.m; i++) {
        raw[i].u = parse_u32(p, end);
        raw[i].v = parse_u32(p, end);
        raw[i].w = parse_u32(p, end);
    }

    g.offsets.assign(static_cast<size_t>(g.n) + 1, 0);
    for (const auto& r : raw) g.offsets[r.u + 1]++;
    for (uint32_t i = 1; i <= g.n; i++) g.offsets[i] += g.offsets[i - 1];

    g.edges.resize(g.m);
    std::vector<uint32_t> cur(g.offsets.begin(), g.offsets.begin() + g.n);
    for (const auto& r : raw) g.edges[cur[r.u]++] = {r.v, r.w};
    return g;
}

// Queries loader
struct Query { uint32_t s, t; };

static std::vector<Query> load_queries(const char* path) {
    MappedFile mf(path);
    const char *p = mf.data, *end = mf.end;
    std::vector<Query> q;
    while (p < end) {
        while (p < end && static_cast<unsigned char>(*p - '0') > 9) ++p;
        if (p >= end) break;
        uint32_t s = parse_u32(p, end), t = parse_u32(p, end);
        q.push_back({s, t});
    }
    return q;
}

// Lean 64-bit priority queue entry
struct PQEntry {
    uint64_t dist;
    uint32_t node;
    bool operator>(const PQEntry& o) const { return dist > o.dist; }
};
using MinHeap = std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>>;

// Core Dijkstra with prefetching and epoch reset
static uint64_t dijkstra(const Graph& g, uint32_t src, uint32_t tgt,
                         std::vector<uint64_t>& dist, std::vector<uint32_t>& epoch_vis,
                         uint32_t epoch, MinHeap& pq) {
    if (src == tgt) return 0;
    while (!pq.empty()) pq.pop();

    dist[src] = 0;
    epoch_vis[src] = epoch;
    pq.push({0, src});

    while (!pq.empty()) {
        auto [du, u] = pq.top();
        pq.pop();

        if (u == tgt) return du;
        if (du > dist[u]) continue;

        const Edge* eb = g.edges.data() + g.offsets[u];
        const Edge* ee = g.edges.data() + g.offsets[u + 1];
        for (const Edge* e = eb; e < ee; ++e) {
            if (e + 1 < ee) {
                __builtin_prefetch(&dist[(e + 1)->target], 1, 1);
                __builtin_prefetch(&epoch_vis[(e + 1)->target], 0, 1);
            }
            uint64_t nd = du + static_cast<uint64_t>(e->weight);
            if (epoch_vis[e->target] != epoch || nd < dist[e->target]) {
                dist[e->target] = nd;
                epoch_vis[e->target] = epoch;
                pq.push({nd, e->target});
            }
        }
    }
    return INF_DIST;
}

// Thread context for 6 P-core worker threads
struct ThreadCtx {
    const Graph* g;
    const Query* q;
    uint64_t* res;
    uint32_t total;
    std::atomic<uint32_t>* next_q;
    std::vector<uint64_t> dist;
    std::vector<uint32_t> epoch_vis;
    uint32_t epoch;
    MinHeap pq;
};

static void* worker(void* arg) {
    auto* c = static_cast<ThreadCtx*>(arg);
    while (true) {
        uint32_t i = c->next_q->fetch_add(1, std::memory_order_relaxed);
        if (i >= c->total) break;
        c->epoch++;
        c->res[i] = dijkstra(*c->g, c->q[i].s, c->q[i].t, c->dist, c->epoch_vis, c->epoch, c->pq);
    }
    return nullptr;
}

// Bulk output writer
struct OutBuf {
    std::vector<char> buf;
    void append_u64(uint64_t val) {
        char tmp[24]; int len = 0;
        if (!val) { buf.push_back('0'); }
        else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; }
               while (len) buf.push_back(tmp[--len]); }
    }
    void write_to(const char* path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { fprintf(stderr, "ERROR: write %s\n", path); exit(1); }
        const char* p = buf.data(); size_t rem = buf.size();
        while (rem > 0) {
            ssize_t w = write(fd, p, rem);
            if (w <= 0) exit(1);
            p += w; rem -= w;
        }
        close(fd);
    }
};

int main(int argc, char** argv) {
    const char* gp = argc > 1 ? argv[1] : "/data/graph.txt";
    const char* qp = argc > 2 ? argv[2] : "/data/queries.txt";
    const char* op = argc > 3 ? argv[3] : "/out/distances.txt";

    Graph g = load_graph(gp);
    auto q = load_queries(qp);
    uint32_t nq = static_cast<uint32_t>(q.size());

    std::vector<uint64_t> res(nq);
    std::atomic<uint32_t> nq_idx{0};

    int nt = std::min<int>(NUM_THREADS, nq);
    std::vector<ThreadCtx> ctx(nt);
    for (int i = 0; i < nt; i++) {
        ctx[i].g = &g;
        ctx[i].q = q.data();
        ctx[i].res = res.data();
        ctx[i].total = nq;
        ctx[i].next_q = &nq_idx;
        ctx[i].dist.resize(g.n);
        ctx[i].epoch_vis.assign(g.n, 0);
        ctx[i].epoch = 0;
    }

    std::vector<pthread_t> th(nt);
    for (int i = 0; i < nt; i++) pthread_create(&th[i], nullptr, worker, &ctx[i]);
    for (int i = 0; i < nt; i++) pthread_join(th[i], nullptr);

    OutBuf out;
    out.buf.reserve(nq * 12);
    for (uint32_t i = 0; i < nq; i++) {
        if (res[i] == INF_DIST) { out.buf.push_back('-'); out.buf.push_back('1'); }
        else out.append_u64(res[i]);
        out.buf.push_back('\n');
    }
    out.write_to(op);
    return 0;
}
