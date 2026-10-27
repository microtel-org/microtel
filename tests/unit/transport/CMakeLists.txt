# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/transport/ (Track D).

microtel_add_test(
    NAME    connect_error_test
    SOURCES connect_error_test.cpp
    LIBS    microtel_transport)

target_include_directories(connect_error_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    epoll_reactor_test
    SOURCES epoll_reactor_test.cpp
    LIBS    microtel_transport)

target_include_directories(epoll_reactor_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    http2_transport_test
    SOURCES http2_transport_test.cpp
    LIBS    microtel_transport OpenSSL::SSL PkgConfig::NGHTTP2)

target_include_directories(http2_transport_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# OpenSSL is linked explicitly: microtel_transport keeps it PRIVATE, and this
# test drives the BIO through OpenSSL's own API.
microtel_add_test(
    NAME    nosignal_io_test
    SOURCES nosignal_io_test.cpp
    LIBS    microtel_transport OpenSSL::SSL OpenSSL::Crypto)

target_include_directories(nosignal_io_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
