# syntax=docker/dockerfile:1
# otelcpp-grpc SUT: builds opentelemetry-cpp (gRPC exporter) via CMake
# FetchContent superbuild, then builds the emit-app against it.
# Build context must be the repo root.
#
#   docker build \
#       --build-arg OTELCPP_VERSION=v1.30.0 \
#       -f bench/sut/otelcpp-grpc/Dockerfile \
#       -t bench-sut-otelcpp-grpc .
#
# Layer caching:
#   base          OS + apt packages (incl. system gRPC/protobuf)  ~2-3 min
#   otelcpp-build otel-cpp built against system gRPC/protobuf     ~5-10 min cold
#                                                                  seconds with cache
#   emit-build    emit-app binary against installed otel-cpp       ~1-2 min
#   runtime       minimal image with binary only                   seconds
#
# The otelcpp-build stage is cached as long as OTELCPP_VERSION and the base
# image are unchanged. Only emit-build rebuilds when emit-app source changes.
#
# gRPC and protobuf are taken from Ubuntu 24.04 system packages rather than
# built from source; otelcpp v1.30.0 builds and runs against the shipped versions
# (gRPC 1.51, protobuf 3.21.12).

ARG OTELCPP_VERSION=v1.30.0

# Flamegraph variant (B3): pass --build-arg INSTALL_PERF=true to add linux-tools-generic (perf)
# to the runtime image and tag as bench-sut-otelcpp-grpc:perf.
# Run with --cap-add=PERFMON (Linux >=5.8).  Normal images never install perf.
ARG INSTALL_PERF=false

# ---------------------------------------------------------------------------
# Stage 1: base — OS and toolchain
# Invalidated only when the ubuntu image or apt package list changes.
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS base

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-18 \
        g++ \
        cmake \
        ninja-build \
        git \
        curl \
        pkg-config \
        libssl-dev \
        zlib1g-dev \
        libgrpc++-dev \
        libprotobuf-dev \
        protobuf-compiler \
        protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

# clang-18 is used for the emit-app (what we measure).
# otelcpp itself is built with GCC to avoid a clang-18.1.3 compiler crash
# (SIGSEGV) in filtered_ordered_attribute_map.cpp (template-heavy otelcpp file).
# Both compilers produce libstdc++ ABI-compatible code on Ubuntu 24.04.
ENV CC=clang-18
ENV CXX=clang++-18
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

# ---------------------------------------------------------------------------
# Stage 2: otelcpp-build — build otel-cpp against system gRPC/protobuf
# Cached on OTELCPP_VERSION. Change the version pin → this stage re-runs.
# ---------------------------------------------------------------------------
FROM base AS otelcpp-build

ARG OTELCPP_VERSION

RUN git clone --depth 1 --branch ${OTELCPP_VERSION} \
        https://github.com/open-telemetry/opentelemetry-cpp.git \
        /src/opentelemetry-cpp

# Build otelcpp with GCC to work around a clang-18 crash in template-heavy files.
RUN cmake -S /src/opentelemetry-cpp -B /build/otelcpp \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/otelcpp \
        -DOTELCPP_BUILD_TESTING=OFF \
        -DOTELCPP_WITH_BENCHMARK=OFF \
        -DOTELCPP_WITH_EXAMPLES=OFF \
        -DOTELCPP_WITH_OTLP_GRPC=ON \
        -DOTELCPP_WITH_OTLP_HTTP=OFF \
        -DWITH_ABSEIL=OFF \
        -DOPENTELEMETRY_BUILD_SHARED_LIBS=OFF \
    && cmake --build /build/otelcpp \
    && cmake --install /build/otelcpp

# ---------------------------------------------------------------------------
# Stage 3: emit-build — workload binary against installed otel-cpp
# Only this stage rebuilds when emit-app source changes.
# ---------------------------------------------------------------------------
FROM otelcpp-build AS emit-build

COPY bench/emit-app /src/emit-app

RUN cmake -S /src/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBENCH_BACKEND=OTELCPP_GRPC \
        -DCMAKE_PREFIX_PATH=/opt/otelcpp \
    && cmake --build /build/emit-app

# ---------------------------------------------------------------------------
# Stage 4: runtime — minimal image with binary only
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

# Re-declare so this stage can read the global ARG.
ARG INSTALL_PERF=false

COPY --from=emit-build /opt/otelcpp/lib/libopentelemetry_proto.so /usr/local/lib/
COPY --from=emit-build /opt/otelcpp/lib/libopentelemetry_proto_grpc.so /usr/local/lib/

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libssl3 \
        libgrpc++1.51t64 \
        libprotobuf32t64 \
        libcares2 \
        libre2-10 \
        libabsl20220623t64 \
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
