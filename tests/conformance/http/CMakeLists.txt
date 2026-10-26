# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# OTLP/HTTP-protobuf against a real collector.
#
# One binary per theme rather than one per directory: a conformance binary is
# bounded by a ctest TIMEOUT that has to cover a flush plus a poll of the
# collector's output file, and a single binary would make that budget the sum
# of every theme's worst case.

microtel_add_conformance_test(
    NAME    conformance_http_basic_test
    SOURCES basic_export_test.cpp
    LIBS    microtel_sdk microtel_api)

microtel_add_conformance_test(
    NAME    conformance_http_batching_test
    SOURCES batching_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_http_tls_test
    SOURCES tls_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_http_auth_test
    SOURCES auth_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_http_plaintext_gap_test
    SOURCES plaintext_gap_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_http_logs_test
    SOURCES logs_test.cpp
    LIBS    microtel_sdk microtel_api)

microtel_add_conformance_test(
    NAME    conformance_http_metrics_test
    SOURCES metrics_test.cpp
    LIBS    microtel_sdk)
