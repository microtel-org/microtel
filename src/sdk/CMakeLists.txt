# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/sdk/ — Track A. Resource resolution, samplers,
# BatchSpanProcessor / SimpleSpanProcessor, Provider lifecycle.
# See src/sdk/README.md.

add_library(microtel_sdk STATIC
    diagnostics_counters.cpp
    sampler_factories.cpp
    sampler_chains.cpp
    metric_attribute_set.cpp
    metric_sum_storage.cpp
    metric_gauge_storage.cpp
    metric_histogram_storage.cpp
    metric_exp_histogram_storage.cpp
    metric_producer.cpp
    synchronous_metric_reader.cpp
    periodic_exporting_metric_reader.cpp
    sdk_meter.cpp
    simple_span_processor.cpp
    noop_span.cpp
    sdk_span.cpp
    sdk_tracer.cpp
    sdk_logger.cpp
    simple_log_record_processor.cpp
    batch_log_record_processor.cpp
    batch_span_processor.cpp
    provider_registry.cpp
    sdk_provider.cpp
    sdk_builder.cpp
    resource_detectors.cpp
    resource_builder.cpp
    view_registry.cpp
    span_limits.cpp)

# The leaf receiver (docs/leaf-concentrator-design.md §3.1, §6.2). Its sources
# are left out entirely when the option is OFF; the definition lets
# sdk_provider.cpp and sdk_builder.cpp choose between it and the no-op.
if(MICROTEL_WITH_CONCENTRATOR)
    target_sources(microtel_sdk PRIVATE
        leaf_options.cpp
        leaf_receiver.cpp
        leaf_resource.cpp
        leaf_table.cpp
        leaf_time.cpp)
    target_compile_definitions(microtel_sdk PRIVATE MICROTEL_WITH_CONCENTRATOR=1)
endif()

# microtel_api is PUBLIC: it defines methods declared on public API types
# (TraceId::ToHex, SpanId::ToHex), so every SDK consumer needs them in its
# link closure.
target_link_libraries(microtel_sdk
    PUBLIC  microtel_headers
            microtel_api
    PRIVATE microtel_config
            microtel_exporter
            microtel_transport
            microtel_http_wire
            microtel_grpc_wire
            microtel_custom_wire
            microtel_encoder
            # sdk_builder.cpp includes transport/http2_transport.hpp, which
            # includes <nghttp2/nghttp2.h> and <openssl/ssl.h>.
            # microtel_transport links both PRIVATE, so their include paths
            # do not reach this target (#473).
            OpenSSL::SSL
            PkgConfig::NGHTTP2)

target_include_directories(microtel_sdk
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_features(microtel_sdk PUBLIC cxx_std_20)

target_compile_options(microtel_sdk PRIVATE
    -Wall -Wextra -Wpedantic ${MICROTEL_WERROR})
