# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Builds ../setidentity_allocs.cpp against an installed opentelemetry-cpp
# (bench/sut/parity/otelcpp-dev.Dockerfile).
cmake_minimum_required(VERSION 3.16)
project(setidentity_allocs CXX)
find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS otlp_recordable)
add_executable(setidentity_allocs ../setidentity_allocs.cpp)
target_link_libraries(setidentity_allocs PRIVATE opentelemetry-cpp::otlp_recordable)
