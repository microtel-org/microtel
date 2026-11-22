# syntax=docker/dockerfile:1
# otelcpp-http SUT: builds opentelemetry-cpp (HTTP/protobuf exporter) via CMake
# FetchContent superbuild, then builds the emit-app against it.
# Build context must be the repo root.
#
#   docker build \
#       --build-arg OTELCPP_VERSION=v1.30.0 \
#       -f bench/sut/otelcpp-http/Dockerfile \
#       -t bench-sut-otelcpp-http .
#
# Layer caching:
#   base          OS + apt packages                              ~2-3 min
#   otelcpp-build otel-cpp + HTTP/protobuf superbuild deps       ~10-15 min cold
#                 (no gRPC/abseil/c-ares/re2)                   seconds with cache
#   emit-build    emit-app binary against installed otel-cpp     ~1-2 min
#   runtime       minimal image with binary only                 seconds
#
# Why no gRPC deps?  The HTTP exporter needs only protobuf + libcurl + openssl.
# Omitting abseil, c-ares, re2, and the gRPC library shaves ~15 min off cold
# builds compared to the otelcpp-grpc SUT.

ARG OTELCPP_VERSION=v1.30.0

# ---------------------------------------------------------------------------
# Stage 1: base — OS and toolchain
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS base

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-18 \
        cmake \
        ninja-build \
        git \
        curl \
        pkg-config \
        libssl-dev \
        libcurl4-openssl-dev \
        zlib1g-dev \
        libprotobuf-dev \
        protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-18
ENV CXX=clang++-18
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

# ---------------------------------------------------------------------------
# Stage 2: otelcpp-build — superbuild of otel-cpp with HTTP exporter only
# No gRPC, no abseil, no c-ares, no re2.
# Cached on OTELCPP_VERSION.
# ---------------------------------------------------------------------------
FROM base AS otelcpp-build

ARG OTELCPP_VERSION

RUN git clone --depth 1 --branch ${OTELCPP_VERSION} \
        https://github.com/open-telemetry/opentelemetry-cpp.git \
        /src/opentelemetry-cpp

RUN cmake -S /src/opentelemetry-cpp -B /build/otelcpp \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/otelcpp \
        -DOTELCPP_BUILD_TESTING=OFF \
        -DOTELCPP_WITH_BENCHMARK=OFF \
        -DOTELCPP_WITH_EXAMPLES=OFF \
        -DOTELCPP_WITH_OTLP_HTTP=ON \
        -DOTELCPP_WITH_OTLP_GRPC=OFF \
        -DWITH_ABSEIL=OFF \
        -DOPENTELEMETRY_BUILD_SHARED_LIBS=OFF \
    && cmake --build /build/otelcpp \
    && cmake --install /build/otelcpp

# ---------------------------------------------------------------------------
# Stage 3: emit-build — workload binary against installed otel-cpp
# ---------------------------------------------------------------------------
FROM otelcpp-build AS emit-build

COPY bench/emit-app /src/emit-app

RUN cmake -S /src/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBENCH_BACKEND=OTELCPP_HTTP \
        -DCMAKE_PREFIX_PATH=/opt/otelcpp \
    && cmake --build /build/emit-app

# ---------------------------------------------------------------------------
# Stage 4: runtime — minimal image with binary only
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ARG INSTALL_PERF=false

COPY --from=emit-build /opt/otelcpp/lib/libopentelemetry_proto.so /usr/local/lib/

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libssl3 \
        libcurl4 \
        libprotobuf32t64 \
    && if [ "$INSTALL_PERF" = "true" ]; then \
        apt-get install -y --no-install-recommends linux-tools-generic \
        && PERF_BIN=$(find /usr/lib/linux-tools -name perf | head -1) \
        && ln -sf "$PERF_BIN" /usr/local/bin/perf; \
    fi \
    && ldconfig \
    && rm -rf /var/lib/apt/lists/*
COPY --from=emit-build /build/emit-app/emit_app /emit_app

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
