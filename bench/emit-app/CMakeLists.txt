cmake_minimum_required(VERSION 3.28)
project(emit_app CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Backend selection — passed by the SUT Dockerfile via -DBENCH_BACKEND=<name>.
# MICROTEL     : microtel SDK, OTLP/HTTP-protobuf
# OTELCPP_GRPC : opentelemetry-cpp SDK, OTLP/gRPC
# OTELCPP_HTTP : opentelemetry-cpp SDK, OTLP/HTTP-protobuf  (B1+)
set(BENCH_BACKEND "MICROTEL" CACHE STRING
    "Emit-app backend: MICROTEL | OTELCPP_GRPC | OTELCPP_HTTP")
set_property(CACHE BENCH_BACKEND PROPERTY
    STRINGS MICROTEL OTELCPP_GRPC OTELCPP_HTTP)

# Inherit compiler flags from the base Dockerfile environment variable.
# On a developer machine without the variable set, falls back to a sensible default.
if(DEFINED ENV{BENCH_CXXFLAGS})
    string(REPLACE " " ";" _bench_flags "$ENV{BENCH_CXXFLAGS}")
    add_compile_options(${_bench_flags})
else()
    add_compile_options(-O2 -g -fno-omit-frame-pointer)
endif()

add_executable(emit_app
    src/main.cpp
    src/histogram.cpp
    src/control_socket.cpp
    src/workload.cpp
)

target_compile_definitions(emit_app PRIVATE
    BENCH_BACKEND_${BENCH_BACKEND}=1)

target_include_directories(emit_app PRIVATE include)

if(BENCH_BACKEND STREQUAL "MICROTEL")
    # When built as a subdirectory of the microtel repo (MICROTEL_BUILD_BENCH=ON),
    # microtel_sdk is already defined in the parent scope — no find_package needed.
    # When built standalone (SUT Dockerfile stage 3), CMAKE_PREFIX_PATH points to
    # the installed microtel tree (issue #19 tracks adding install() targets).
    if(NOT TARGET microtel_sdk)
        find_package(microtel REQUIRED)
    endif()
    target_sources(emit_app PRIVATE src/backend_microtel.cpp)
    target_link_libraries(emit_app PRIVATE microtel_sdk)
    target_include_directories(emit_app PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src)

    option(BENCH_MICROTEL_GRPC "Build microtel backend with gRPC transport" OFF)
    if(BENCH_MICROTEL_GRPC)
        target_compile_definitions(emit_app PRIVATE BENCH_MICROTEL_GRPC=1)
    endif()

    # The leaf-fanin workload (bench/profiles/leaf-fanin.yaml): the backend
    # encodes leaf payloads with the C leaf library and ingests them through
    # the concentrator's LeafReceiver, so it needs both compiled in.
    option(BENCH_LEAF_FANIN "Build the microtel backend's leaf fan-in workload" OFF)
    if(BENCH_LEAF_FANIN)
        if(NOT TARGET microtel_leaf OR NOT MICROTEL_WITH_CONCENTRATOR)
            message(FATAL_ERROR "BENCH_LEAF_FANIN needs -DMICROTEL_BUILD_LEAF=ON "
                                "and -DMICROTEL_WITH_CONCENTRATOR=ON")
        endif()
        target_compile_definitions(emit_app PRIVATE BENCH_LEAF_FANIN=1)
        target_link_libraries(emit_app PRIVATE microtel_leaf)
    endif()

elseif(BENCH_BACKEND STREQUAL "OTELCPP_GRPC")
    find_package(opentelemetry-cpp REQUIRED COMPONENTS
        trace metrics logs otlp_grpc_exporter otlp_grpc_metrics_exporter
        otlp_grpc_log_record_exporter)
    target_sources(emit_app PRIVATE src/backend_otelcpp.cpp)
    target_link_libraries(emit_app PRIVATE
        opentelemetry-cpp::trace
        opentelemetry-cpp::metrics
        opentelemetry-cpp::logs
        opentelemetry-cpp::otlp_grpc_exporter
        opentelemetry-cpp::otlp_grpc_metrics_exporter
        opentelemetry-cpp::otlp_grpc_log_record_exporter)

elseif(BENCH_BACKEND STREQUAL "OTELCPP_HTTP")
    find_package(CURL REQUIRED)
    # opentelemetry-cpp main renamed the package components (trace -> sdk,
    # otlp_http_exporter -> exporters_otlp_http, ...) but kept the target
    # names; ON finds every built component, for building against main.
    option(BENCH_OTELCPP_ALL_COMPONENTS
        "find_package(opentelemetry-cpp) without naming components" OFF)
    if(BENCH_OTELCPP_ALL_COMPONENTS)
        find_package(opentelemetry-cpp REQUIRED)
    else()
        find_package(opentelemetry-cpp REQUIRED COMPONENTS
            trace metrics logs otlp_http_exporter otlp_http_metric_exporter
            otlp_http_log_record_exporter)
    endif()
    target_sources(emit_app PRIVATE src/backend_otelcpp.cpp)
    target_link_libraries(emit_app PRIVATE
        opentelemetry-cpp::trace
        opentelemetry-cpp::metrics
        opentelemetry-cpp::logs
        opentelemetry-cpp::otlp_http_exporter
        opentelemetry-cpp::otlp_http_metric_exporter
        opentelemetry-cpp::otlp_http_log_record_exporter)
    # Parity study (bench/parity): swap only the span exporter for the SDK's
    # in-memory one, keeping the OTLP/HTTP metrics and logs setup unchanged.
    option(BENCH_OTELCPP_INMEMORY_SPANS
        "Export spans to the SDK's in-memory exporter (SpanData), not OTLP" OFF)
    if(BENCH_OTELCPP_INMEMORY_SPANS)
        find_package(opentelemetry-cpp REQUIRED COMPONENTS in_memory_span_exporter)
        target_compile_definitions(emit_app PRIVATE BENCH_OTELCPP_INMEMORY_SPANS=1)
        target_link_libraries(emit_app PRIVATE opentelemetry-cpp::in_memory_span_exporter)
    endif()

else()
    message(FATAL_ERROR "Unknown BENCH_BACKEND: ${BENCH_BACKEND}. "
        "Valid values: MICROTEL | OTELCPP_GRPC | OTELCPP_HTTP")
endif()

# Histogram self-test — in-tree builds only. The standalone SUT Dockerfile
# builds have no test framework and never run ctest, so the guard keeps them
# byte-identical.
if(MICROTEL_BUILD_BENCH)
    add_executable(histogram_selftest
        test/histogram_selftest.cpp
        src/histogram.cpp
    )
    target_include_directories(histogram_selftest PRIVATE include)
    add_test(NAME bench_histogram_selftest COMMAND histogram_selftest)

    add_executable(workload_selftest
        test/workload_selftest.cpp
        src/workload.cpp
    )
    target_include_directories(workload_selftest PRIVATE include)
    add_test(NAME bench_workload_selftest COMMAND workload_selftest)
endif()
