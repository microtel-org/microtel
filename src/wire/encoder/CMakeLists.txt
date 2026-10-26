# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# src/wire/encoder/ — OtlpEncoder: the only TU that links upb headers.
# Produces the microtel_encoder static library consumed by the exporter.

add_library(microtel_encoder STATIC
    otlp_encoder.cpp
    trace_request_concat.cpp)

# The upb decoder behind the leaf receiver (ICP 0031 Decision 5, design §6.2):
# the only other upb user in the runtime, and only in a concentrator build.
if(MICROTEL_WITH_CONCENTRATOR)
    target_sources(microtel_encoder PRIVATE otlp_trace_decoder.cpp)
endif()

# microtel_api: TraceState::ToHeader(), for span and link trace_state (#451).
target_link_libraries(microtel_encoder
    PUBLIC  microtel_headers
    PRIVATE microtel_api microtel_upb_gen)

# otlp_encoder.cpp includes upb headers via relative paths rooted at
# third_party/upb and the generated accessors rooted at gen/.
target_include_directories(microtel_encoder
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/gen
        ${PROJECT_SOURCE_DIR}/third_party/upb)

target_compile_features(microtel_encoder PUBLIC cxx_std_20)

# -include first: the symbol-prefix header must be seen before any TU text, so
# this TU's references resolve against the renamed upb runtime.
# See third_party/upb/microtel_upb_rename.h and ICP 0020 Decision 4.
target_compile_options(microtel_encoder PRIVATE
    -include ${MICROTEL_UPB_RENAME_HEADER}
    -Wall -Wextra -Wpedantic ${MICROTEL_WERROR})
