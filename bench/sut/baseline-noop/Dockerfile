# syntax=docker/dockerfile:1
# baseline-noop SUT: the emit-app with the no-op backend (BENCH_BACKEND=NOOP).
# Same control socket, workload threads and pacing loop as every other SUT, no
# SDK, nothing exported. The driver subtracts its CPU from the others'
# (bench/docs/methodology.md, "CPU"). Build context must be the repo root.
#
#   docker build -f bench/sut/baseline-noop/Dockerfile -t bench-sut-baseline-noop .

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        clang-18 \
        cmake \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18
ENV CXX=clang++-18
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

COPY bench/emit-app /src/emit-app

RUN cmake -S /src/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBENCH_BACKEND=NOOP \
    && cmake --build /build/emit-app

FROM ubuntu:24.04 AS runtime

COPY --from=build /build/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
