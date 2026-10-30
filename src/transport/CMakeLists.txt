# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Track D: nghttp2 transport.  EpollReactor first; Http2Transport follows.

add_library(microtel_transport STATIC
    connect_error.cpp
    connection_setup.cpp
    epoll_reactor.cpp
    http1_response_parser.cpp
    http1_transport.cpp
    http2_transport.cpp
    nosignal_io.cpp)

target_link_libraries(microtel_transport
    PUBLIC  microtel_headers
    PRIVATE microtel_common
            OpenSSL::SSL
            # The transport calls libcrypto directly (BIO_*, X509_*). FindOpenSSL
            # makes OpenSSL::SSL pull libcrypto in, but OpenSSL's own CMake config
            # (preferred by Conan, and by CMAKE_FIND_PACKAGE_PREFER_CONFIG) does
            # not for shared builds, and the link then fails.
            OpenSSL::Crypto
            PkgConfig::NGHTTP2)

target_include_directories(microtel_transport
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_features(microtel_transport PUBLIC cxx_std_20)
target_compile_options(microtel_transport PRIVATE
    -Wall -Wextra -Wpedantic ${MICROTEL_WERROR})
