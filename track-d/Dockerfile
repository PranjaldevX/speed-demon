# Build stage — gcc 16 pinned by digest (from speeddemon.dev/allowlist.json)
FROM gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95 AS build

COPY src/ /src/
WORKDIR /src
RUN mkdir -p /app

# Phase 1: build instrumented binary for profile generation
RUN g++ -O3 -std=c++17 -march=native -pthread \
    -fprofile-generate=/tmp/pgo \
    -o /tmp/run_instr main.cpp

# Phase 2: generate profile on a small synthetic graph
RUN mkdir -p /tmp/profdata /tmp/profout && \
    printf '10 15\n0 1 4\n0 2 1\n1 3 1\n2 1 2\n2 3 5\n3 4 3\n4 5 7\n5 6 2\n6 7 1\n7 8 3\n8 9 5\n9 0 10\n1 5 20\n3 7 15\n6 2 8\n' > /tmp/profdata/graph.txt && \
    printf '0 9\n0 4\n1 0\n5 5\n3 8\n7 2\n0 0\n9 1\n2 7\n4 1\n' > /tmp/profdata/queries.txt && \
    /tmp/run_instr /tmp/profdata/graph.txt /tmp/profdata/queries.txt /tmp/profout/distances.txt

# Phase 3: build final binary using collected profile
RUN g++ -O3 -std=c++17 -march=native -flto -pthread \
    -fprofile-use=/tmp/pgo -fprofile-correction \
    -o /app/run main.cpp

# Runtime stage
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /app/run /app/run
ENTRYPOINT ["/app/run"]
