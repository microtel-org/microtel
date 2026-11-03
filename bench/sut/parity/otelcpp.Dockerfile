# syntax=docker/dockerfile:1
# Parity study (bench/parity/README.md): one parameterised opentelemetry-cpp
# SUT, so every toolchain variant differs from the published otelcpp-http /
# otelcpp-grpc images only in the build arguments below.
# Build context must be the repo root.
#
#   podman build -f bench/sut/parity/otelcpp.Dockerfile \
#       --build-arg OTELCPP_VERSION=v1.19.0 --build-arg EXPORTER=http \
#       --build-arg SDK_CC=clang-18 --build-arg SDK_CXX=clang++-18 \
#       -t bench-sut-parity-otelcpp-http-clang .
#
# Arguments:
#   OTELCPP_VERSION  opentelemetry-cpp tag.
#   EXPORTER         http | grpc.
#   SDK_CC/SDK_CXX   compiler for opentelemetry-cpp (and protobuf, if built).
#   EMIT_CC/EMIT_CXX compiler for the emit-app; defaults to the SDK's.
#   EXTRA_FLAGS      appended to C and C++ flags of every build here, e.g. the
#                    flags that turn Ubuntu GCC's default hardening off.
#   CXX_STD          C++ standard for opentelemetry-cpp and the emit-app.
#   PROTOBUF_TAG     empty: Ubuntu's libprotobuf32t64. A protobuf tag (v21.12 is
#                    3.21.12, Ubuntu's version): built here with BENCH flags.
#   INMEMORY         ON: spans go to the SDK's in-memory exporter (EXPORTER=http).
#   OTELCPP_PATCH    a file in bench/sut/parity/patches/, applied to the
#                    opentelemetry-cpp tree before building (experiments only).
#   ALL_COMPONENTS   ON: the emit-app finds opentelemetry-cpp without naming
#                    components (v1.30.0 renamed them; the targets are unchanged).

ARG OTELCPP_VERSION=v1.19.0
ARG EXPORTER=http
ARG SDK_CC=clang-18
ARG SDK_CXX=clang++-18
ARG EMIT_CC=
ARG EMIT_CXX=
ARG EXTRA_FLAGS=
ARG CXX_STD=17
ARG PROTOBUF_TAG=
ARG INMEMORY=OFF
ARG ALL_COMPONENTS=OFF
ARG OTELCPP_PATCH=
ARG INSTALL_PERF=false

FROM ubuntu:24.04 AS base

ARG DEBIAN_FRONTEND=noninteractive
ARG EXPORTER
ARG PROTOBUF_TAG

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates clang-18 g++ cmake ninja-build git curl pkg-config \
        libssl-dev libcurl4-openssl-dev zlib1g-dev \
    && if [ -z "$PROTOBUF_TAG" ]; then \
        apt-get install -y --no-install-recommends libprotobuf-dev protobuf-compiler; \
    fi \
    && if [ "$EXPORTER" = "grpc" ]; then \
        apt-get install -y --no-install-recommends \
            libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc; \
    fi \
    && rm -rf /var/lib/apt/lists/*

# The same base flags as every published SUT (bench/sut/*/Dockerfile); CMake's
# Release type appends -O3 -DNDEBUG.
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"

FROM base AS protobuf-build

ARG PROTOBUF_TAG
ARG SDK_CC
ARG SDK_CXX
ARG EXTRA_FLAGS

RUN mkdir -p /opt/protobuf/lib \
    && if [ -n "$PROTOBUF_TAG" ]; then \
        git clone --depth 1 --branch "$PROTOBUF_TAG" \
            https://github.com/protocolbuffers/protobuf.git /src/protobuf \
        && cmake -S /src/protobuf -B /build/protobuf -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$SDK_CC" -DCMAKE_CXX_COMPILER="$SDK_CXX" \
            -DCMAKE_C_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
            -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
            -DCMAKE_INSTALL_PREFIX=/opt/protobuf \
            -DCMAKE_INSTALL_LIBDIR=lib \
            -Dprotobuf_BUILD_TESTS=OFF \
            -Dprotobuf_BUILD_SHARED_LIBS=ON \
        && cmake --build /build/protobuf \
        && cmake --install /build/protobuf; \
    fi

FROM protobuf-build AS otelcpp-build

ARG OTELCPP_VERSION
ARG EXPORTER
ARG SDK_CC
ARG SDK_CXX
ARG EXTRA_FLAGS
ARG CXX_STD

ARG OTELCPP_PATCH

RUN git clone --depth 1 --branch ${OTELCPP_VERSION} \
        https://github.com/open-telemetry/opentelemetry-cpp.git \
        /src/opentelemetry-cpp

COPY bench/sut/parity/patches /src/patches
RUN if [ -n "$OTELCPP_PATCH" ]; then \
        git -C /src/opentelemetry-cpp apply --verbose "/src/patches/$OTELCPP_PATCH"; \
    fi

# Both the pre-1.30 option names and v1.30's OTELCPP_* ones are passed; each
# release ignores the names it does not know.
RUN GRPC=OFF; HTTP=ON; \
    if [ "$EXPORTER" = "grpc" ]; then GRPC=ON; HTTP=OFF; fi; \
    cmake -S /src/opentelemetry-cpp -B /build/otelcpp \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$SDK_CC" -DCMAKE_CXX_COMPILER="$SDK_CXX" \
        -DCMAKE_C_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
        -DCMAKE_CXX_STANDARD="$CXX_STD" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_PREFIX_PATH=/opt/protobuf \
        -DCMAKE_INSTALL_PREFIX=/opt/otelcpp \
        -DBUILD_TESTING=OFF -DOTELCPP_BUILD_TESTING=OFF \
        -DWITH_BENCHMARK=OFF -DOTELCPP_WITH_BENCHMARK=OFF \
        -DWITH_EXAMPLES=OFF -DOTELCPP_WITH_EXAMPLES=OFF \
        -DWITH_OTLP_GRPC=$GRPC -DOTELCPP_WITH_OTLP_GRPC=$GRPC \
        -DWITH_OTLP_HTTP=$HTTP -DOTELCPP_WITH_OTLP_HTTP=$HTTP \
        -DWITH_ABSEIL=OFF \
        -DOPENTELEMETRY_BUILD_SHARED_LIBS=OFF \
    && cmake --build /build/otelcpp \
    && cmake --install /build/otelcpp

FROM otelcpp-build AS emit-build

ARG EXPORTER
ARG SDK_CC
ARG SDK_CXX
ARG EMIT_CC
ARG EMIT_CXX
ARG EXTRA_FLAGS
ARG CXX_STD
ARG INMEMORY
ARG ALL_COMPONENTS

COPY bench/emit-app /src/emit-app

RUN BACKEND=OTELCPP_HTTP; \
    if [ "$EXPORTER" = "grpc" ]; then BACKEND=OTELCPP_GRPC; fi; \
    cmake -S /src/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="${EMIT_CC:-$SDK_CC}" -DCMAKE_CXX_COMPILER="${EMIT_CXX:-$SDK_CXX}" \
        -DCMAKE_C_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS} ${EXTRA_FLAGS}" \
        -DCMAKE_CXX_STANDARD="$CXX_STD" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBENCH_BACKEND=$BACKEND \
        -DBENCH_OTELCPP_INMEMORY_SPANS=$INMEMORY \
        -DBENCH_OTELCPP_ALL_COMPONENTS=$ALL_COMPONENTS \
        -DCMAKE_PREFIX_PATH="/opt/otelcpp;/opt/protobuf" \
    && cmake --build /build/emit-app

FROM ubuntu:24.04 AS runtime

ARG EXPORTER
ARG PROTOBUF_TAG
ARG INSTALL_PERF

COPY --from=emit-build /opt/otelcpp/lib/ /tmp/otelcpp-lib/
COPY --from=emit-build /opt/protobuf/lib/ /tmp/protobuf-lib/

RUN cp -a /tmp/otelcpp-lib/*.so* /usr/local/lib/ 2>/dev/null; \
    cp -a /tmp/protobuf-lib/*.so* /usr/local/lib/ 2>/dev/null; \
    rm -rf /tmp/otelcpp-lib /tmp/protobuf-lib; \
    apt-get update \
    && apt-get install -y --no-install-recommends libssl3 libcurl4 \
    && if [ -z "$PROTOBUF_TAG" ]; then \
        apt-get install -y --no-install-recommends libprotobuf32t64; \
    fi \
    && if [ "$EXPORTER" = "grpc" ]; then \
        apt-get install -y --no-install-recommends \
            libgrpc++1.51t64 libprotobuf32t64 libcares2 libre2-10 libabsl20220623t64; \
    fi \
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
