# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/exporter/ (M3 close).

microtel_add_test(
    NAME    otlp_exporter_test
    SOURCES otlp_exporter_test.cpp
    LIBS    microtel_exporter)

target_include_directories(otlp_exporter_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(otlp_exporter_test PRIVATE
    -Wno-missing-field-initializers)

microtel_add_test(
    NAME    retry_policy_test
    SOURCES retry_policy_test.cpp
    LIBS    microtel_exporter)

target_include_directories(retry_policy_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    otlp_metric_exporter_test
    SOURCES otlp_metric_exporter_test.cpp
    LIBS    microtel_exporter)

target_include_directories(otlp_metric_exporter_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(otlp_metric_exporter_test PRIVATE
    -Wno-missing-field-initializers)

# M14 L4.3 — OtlpLogExporter: encode via ILogEncoder + send via IWireCodec.
microtel_add_test(
    NAME    otlp_log_exporter_test
    SOURCES otlp_log_exporter_test.cpp
    LIBS    microtel_exporter)

target_include_directories(otlp_log_exporter_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(otlp_log_exporter_test PRIVATE
    -Wno-missing-field-initializers)

# Issue #311 — RetryEngine backoff schedule, first retry included.
microtel_add_test(
    NAME    retry_engine_test
    SOURCES retry_engine_test.cpp
    LIBS    microtel_exporter)

target_include_directories(retry_engine_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(retry_engine_test PRIVATE
    -Wno-missing-field-initializers)

microtel_add_test(
    NAME    flush_tracker_test
    SOURCES flush_tracker_test.cpp
    LIBS    microtel_exporter)

target_include_directories(flush_tracker_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src)
