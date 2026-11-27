# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Integration tests for src/sdk/ wiring that unit tests cannot observe.

microtel_add_integration_test(
    NAME    exporter_health_test
    SOURCES exporter_health_test.cpp
    LIBS    microtel_sdk)

microtel_add_integration_test(
    NAME    retry_budget_test
    SOURCES retry_budget_test.cpp
    LIBS    microtel_sdk)

# Issues #250 / #251: real exporter + real HTTP codec + real auth provider
# against a FakeTransport, so the blast radius of an auth failure is visible.
microtel_add_integration_test(
    NAME    auth_failure_export_test
    SOURCES auth_failure_export_test.cpp
    LIBS    microtel_exporter microtel_http_wire microtel_config)

# Same relaxation as tests/unit/exporter: SpanRecord is built field-by-field.
target_compile_options(auth_failure_export_test PRIVATE
    -Wno-missing-field-initializers)

# Issue #221 / ICP 0025 §3: the two ICurrentSpanSource seams — metrics
# exemplars and log trace-correlation — driven through a real SdkProvider.
microtel_add_integration_test(
    NAME    exemplar_wiring_test
    SOURCES exemplar_wiring_test.cpp
    LIBS    microtel_sdk)

# SdkProviderArgs and LogRecord are built with designated initializers that
# intentionally leave later fields at their defaults.
target_compile_options(exemplar_wiring_test PRIVATE
    -Wno-missing-field-initializers)

# v1.2 — the concentrator ingest path end to end, short of a network: leaf
# payloads through the real decoder, processor, exporter and encoder into a
# recording codec, decoded again with upb (docs/leaf-concentrator-design.md
# §3.6.1, §7.4).
if(MICROTEL_WITH_CONCENTRATOR)
    microtel_add_integration_test(
        NAME    leaf_ingest_test
        SOURCES leaf_ingest_test.cpp
        LIBS    microtel_sdk microtel_exporter microtel_encoder microtel_upb_gen)
    target_include_directories(leaf_ingest_test
        PRIVATE
            ${PROJECT_SOURCE_DIR}/gen
            ${PROJECT_SOURCE_DIR}/third_party/upb)
    target_compile_options(leaf_ingest_test PRIVATE
        -include ${MICROTEL_UPB_RENAME_HEADER}
        -Wno-missing-field-initializers)
    # The leaf's golden vectors, ingested as a leaf would send them (ICP 0036).
    target_compile_definitions(leaf_ingest_test PRIVATE
        MICROTEL_LEAF_VECTORS_DIR="${PROJECT_SOURCE_DIR}/tests/leaf/vectors")
endif()

# Issue #345: the real BatchSpanProcessor feeding the real OtlpExporter from
# many Resources — the exporter's queue must hold what one Resource's would.
microtel_add_integration_test(
    NAME    fanin_export_test
    SOURCES fanin_export_test.cpp
    LIBS    microtel_sdk microtel_exporter)
target_compile_options(fanin_export_test PRIVATE
    -Wno-missing-field-initializers)

# The per-signal OTLP endpoint variables on the wire: a real Provider's one
# loopback HTTP/2 connection to a server that records each request's :path
# (docs/configuration.md §3.3).
microtel_add_integration_test(
    NAME    per_signal_endpoint_test
    SOURCES per_signal_endpoint_test.cpp
    LIBS    microtel_sdk PkgConfig::NGHTTP2)
