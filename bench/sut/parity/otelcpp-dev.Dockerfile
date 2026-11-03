# syntax=docker/dockerfile:1
# Parity study (bench/parity/README.md): opentelemetry-cpp built from a local
# source tree inside upstream's own dev container (.devcontainer/Dockerfile.dev),
# so protobuf, abseil and curl are the versions in upstream's
# third_party_release, as their CI builds them. Used to measure upstream
# branches and PRs (bench/parity/results/*/otelcpp-main/).
#
# Build context: the microtel repo root. The opentelemetry-cpp tree comes from
# a second context named otelsrc (a `git archive` of the variant's commit):
#
#   podman build -f bench/sut/parity/otelcpp-dev.Dockerfile \
#       --build-context otelsrc=/path/to/otelcpp-tree \
#       --build-arg DEV_IMAGE=localhost/otelcpp-dev:0a5bb0b \
#       -t bench-sut-parity-otelcpp-main-a .
#
# Same flags as the other parity SUTs (-O2 -g -fno-omit-frame-pointer, then
# CMake's Release -O3 -DNDEBUG), with the dev container's clang (22).

ARG DEV_IMAGE=localhost/otelcpp-dev:latest
ARG CXX_STD=17

FROM ${DEV_IMAGE} AS build

ARG CXX_STD

USER root
ENV BENCH_CXXFLAGS="-O2 -g -fno-omit-frame-pointer"
ENV THIRD_PARTY=/home/devuser/third-party/install

COPY --from=otelsrc . /src/opentelemetry-cpp

RUN cmake -S /src/opentelemetry-cpp -B /build/otelcpp \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_STANDARD="$CXX_STD" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DCMAKE_PREFIX_PATH="$THIRD_PARTY" \
        -DCMAKE_INSTALL_PREFIX=/opt/otelcpp \
        -DOTELCPP_BUILD_TESTING=OFF \
        -DOTELCPP_WITH_BENCHMARK=OFF \
        -DOTELCPP_WITH_EXAMPLES=OFF \
        -DOTELCPP_WITH_OTLP_HTTP=ON \
        -DOTELCPP_WITH_OTLP_GRPC=OFF \
    && cmake --build /build/otelcpp \
    && cmake --install /build/otelcpp

COPY bench/emit-app /src/emit-app

RUN cmake -S /src/emit-app -B /build/emit-app \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_STANDARD="$CXX_STD" \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBENCH_BACKEND=OTELCPP_HTTP \
        -DBENCH_OTELCPP_ALL_COMPONENTS=ON \
        -DCMAKE_PREFIX_PATH="/opt/otelcpp;$THIRD_PARTY" \
    && cmake --build /build/emit-app

COPY bench/parity/setidentity_allocs.cpp /src/setidentity/setidentity_allocs.cpp
COPY bench/parity/setidentity/CMakeLists.txt /src/setidentity/cmake/CMakeLists.txt

RUN cmake -S /src/setidentity/cmake -B /build/setidentity \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_FLAGS="${BENCH_CXXFLAGS}" \
        -DCMAKE_CXX_STANDARD="$CXX_STD" \
        -DCMAKE_PREFIX_PATH="/opt/otelcpp;$THIRD_PARTY" \
    && cmake --build /build/setidentity

FROM ubuntu:24.04 AS runtime

# protobuf and abseil are static in the dev container's third-party install;
# curl and zlib are shared, and curl links the system OpenSSL.
COPY --from=build /home/devuser/third-party/install/lib/ /tmp/third-party-lib/
RUN cp -a /tmp/third-party-lib/libcurl.so* /tmp/third-party-lib/libz.so* /usr/local/lib/ \
    && rm -rf /tmp/third-party-lib \
    && apt-get update \
    && apt-get install -y --no-install-recommends libssl3 ca-certificates \
    && ldconfig \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/emit-app/emit_app /emit_app
COPY --from=build /build/setidentity/setidentity_allocs /setidentity_allocs

EXPOSE 9090
ENTRYPOINT ["/emit_app"]
