# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# OTLP/gRPC against a real collector.
#
# microtel implements gRPC as a unary-RPC protocol layer over its own nghttp2
# transport, without linking the gRPC library (CLAUDE.md rule 13). Everything
# under tests/ can only check that layer against itself; these binaries are the
# first thing in the repo that checks it against a receiver written by the gRPC
# project.
#
# One binary per theme, mirroring http/: a conformance binary is bounded by a
# ctest TIMEOUT covering a flush plus a poll of the collector's output file, and
# a single binary would make that budget the sum of every theme's worst case.

microtel_add_conformance_test(
    NAME    conformance_grpc_basic_test
    SOURCES basic_export_test.cpp
    LIBS    microtel_sdk microtel_api)

microtel_add_conformance_test(
    NAME    conformance_grpc_batching_test
    SOURCES batching_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_grpc_tls_test
    SOURCES tls_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_grpc_auth_test
    SOURCES auth_test.cpp
    LIBS    microtel_sdk)

microtel_add_conformance_test(
    NAME    conformance_grpc_logs_test
    SOURCES logs_test.cpp
    LIBS    microtel_sdk microtel_api)

microtel_add_conformance_test(
    NAME    conformance_grpc_metrics_test
    SOURCES metrics_test.cpp
    LIBS    microtel_sdk)
