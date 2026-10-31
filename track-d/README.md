# Track D: Shortest Paths on Directed Graphs

Solution for **Speed Demon 2026 — Track D**.

## Structure

```
track-d/
├── src/
│   └── main.cpp           # Complete optimized implementation
├── data/
│   ├── graph.txt          # Sample graph input
│   └── queries.txt        # Sample query input
├── out/
│   └── distances.txt      # Query results output
├── build/
│   └── run                # Native compiled binary
├── Dockerfile             # Multi-stage PGO build
├── PERF.md                # Performance narrative and benchmark log
└── .speed-demon.toml      # Track metadata
```

## How to Build & Run Locally

### Compile
```bash
mkdir -p build
g++ -O3 -std=c++17 -march=native -flto -pthread -o build/run src/main.cpp
```

### Run
```bash
./build/run data/graph.txt data/queries.txt out/distances.txt
```

If run without arguments, it defaults to the competition paths:
- `/data/graph.txt`
- `/data/queries.txt`
- `/out/distances.txt`

## Docker Build

The provided `Dockerfile` compiles with GCC 16, utilizing 2-stage Profile-Guided Optimization (PGO) and Link-Time Optimization (LTO) before deploying to a minimal runtime image:

```bash
docker build -t speeddemon/track-d .
docker run --rm \
  --cpuset-cpus="0-11" \
  -v /path/to/data:/data:ro \
  -v /path/to/out:/out:rw \
  speeddemon/track-d
```

