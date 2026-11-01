# syntax=docker/dockerfile:1
# Parity study (bench/parity/README.md): bench/sut/microtel/Dockerfile with the
# compiler, extra flags and transport as build arguments.
# Build context must be the repo root.
#
#   podman build -f bench/sut/parity/microtel.Dockerfile \
#       --build-arg CC_BIN=gcc --build-arg CXX_BIN=g++ --build-arg GRPC=OFF \
#       -t bench-sut-parity-microtel-http-gcc .
#
# Arguments:
#   CC_BIN/CXX_BIN  compiler for microtel and the emit-app.
#   EXTRA_FLAGS     appended to the C and C++ flags.
#   GRPC            ON: the microtel-grpc SUT (gRPC transport); OFF: OTLP/HTTP.

ARG CC_BIN=clang-18
ARG CXX_BIN=clang++-18
ARG EXTRA_FLAGS=
ARG GRPC=OFF
ARG INSTALL_PERF=false

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG CC_BIN
ARG CXX_BIN
ARG EXTRA_FLAGS
ARG GRPC

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates clang-18 g++ cmake ninja-build git pkg-config \
        libssl-dev libnghttp2-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        -DCMAKE_C_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DMICROTEL_BUILD_TESTS=OFF \
        -DMICROTEL_BUILD_BENCH=ON \
        -DBENCH_BACKEND=MICROTEL \
        -DBENCH_MICROTEL_GRPC=$GRPC \
    && cmake --build build --target emit_app

FROM ubuntu:24.04 AS runtime

ARG INSTALL_PERF

RUN apt-get update \
    && apt-get install -y --no-install-recommends libnghttp2-14 zlib1g libssl3 \
    && if [ "$INSTALL_PERF" = "true" ]; then \
        apt-get install -y --no-install-recommends linux-tools-generic \
        && PERF_BIN=$(find /usr/lib/linux-tools -name perf | head -1) \
        && ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/bench/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
