# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Integration tests for src/transport/ (Track D).

microtel_add_integration_test(
    NAME    http2_connect_test
    SOURCES http2_connect_test.cpp
    LIBS    microtel_transport OpenSSL::SSL PkgConfig::NGHTTP2)

# microtel_grpc_wire is linked for the split-DATA-frame tests: the
# accumulation they exercise lives in the transport, but the assertion that it
# worked is the codec parsing the reassembled message (grpc-wire-protocol.md §2.3).
# microtel_http_wire is linked for the header-validation test (ICP 0042 §3).
microtel_add_integration_test(
    NAME    http2_send_test
    SOURCES http2_send_test.cpp
    LIBS    microtel_transport microtel_grpc_wire microtel_http_wire OpenSSL::SSL PkgConfig::NGHTTP2)

# OpenSSL is linked explicitly: microtel_transport keeps it PRIVATE, and this
# test builds its own TLS server rather than borrowing the transport's.
microtel_add_integration_test(
    NAME    http2_tls_connect_test
    SOURCES http2_tls_connect_test.cpp
    LIBS    microtel_transport OpenSSL::SSL OpenSSL::Crypto PkgConfig::NGHTTP2)
