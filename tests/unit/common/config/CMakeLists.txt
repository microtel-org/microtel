# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# M6-A unit tests: config layer (TOML parsing, env-var overlay, validation).

microtel_add_test(
    NAME    config_test
    SOURCES config_test.cpp
    LIBS    microtel_config)

target_include_directories(config_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

# The per-signal OTLP exporter variables: one origin, a path per signal, and a
# warning for each per-signal variable microtel does not implement.
microtel_add_test(
    NAME    signal_endpoint_test
    SOURCES signal_endpoint_test.cpp
    LIBS    microtel_config)

target_include_directories(signal_endpoint_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Issue #200 — the ON half of the MICROTEL_FORBID_INSECURE_TLS gate.
#
# The macro is compiled into microtel_config, so whether the check fires is
# fixed when that archive is built: a test linking the default build can only
# see the OFF behaviour. This target recompiles the single translation unit
# that carries the check with the macro defined. config_validator.cpp needs no
# link dependency of its own (headers plus libstdc++), so this stays a two-file
# binary rather than a second copy of the whole config library.
#
# It is registered with ctest by microtel_add_test like any other unit test, so
# the rejecting branch runs in a default CI job rather than only in a build
# nobody configures.
microtel_add_test(
    NAME    forbid_insecure_tls_test
    SOURCES forbid_insecure_tls_test.cpp
            ${PROJECT_SOURCE_DIR}/src/common/config/config_validator.cpp)

target_include_directories(forbid_insecure_tls_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(forbid_insecure_tls_test PRIVATE MICROTEL_FORBID_INSECURE_TLS=1)

# v1.2 — the concentrator's [concentrator] table, MICROTEL_CONCENTRATOR_*
# variables and the WithLeafReceiver merge (docs/leaf-concentrator-design.md
# §4.2-§4.3). Part of microtel_config in every build, so not gated on
# MICROTEL_WITH_CONCENTRATOR.
microtel_add_test(
    NAME    concentrator_config_test
    SOURCES concentrator_config_test.cpp
    LIBS    microtel_config)

target_include_directories(concentrator_config_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
