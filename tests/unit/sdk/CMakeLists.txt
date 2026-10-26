# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for src/sdk/ (Track A). Mirrors src/sdk/ one-to-one per
# tests/unit/README.md.

# M3 — first production-code-driven test in microtel. Drives AlwaysOn
# sampler behaviour against the real production code in src/sdk/.
microtel_add_test(
    NAME    always_on_sampler_test
    SOURCES always_on_sampler_test.cpp
    LIBS    microtel_sdk)

# M12 — metric aggregation key (AttributeSet).
microtel_add_test(
    NAME    metric_attribute_set_test
    SOURCES metric_attribute_set_test.cpp
    LIBS    microtel_sdk)
# Reaches into src/sdk/metric_attribute_set.hpp directly (internal header).
target_include_directories(metric_attribute_set_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — Sum aggregation storage.
microtel_add_test(
    NAME    metric_sum_storage_test
    SOURCES metric_sum_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_sum_storage_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Data-point start / end timestamps across every storage (issue #448).
microtel_add_test(
    NAME    metric_timestamps_test
    SOURCES metric_timestamps_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_timestamps_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Observable sums under delta temporality (issue #450).
microtel_add_test(
    NAME    metric_observable_delta_test
    SOURCES metric_observable_delta_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_observable_delta_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — Gauge (last-value) aggregation storage.
microtel_add_test(
    NAME    metric_gauge_storage_test
    SOURCES metric_gauge_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_gauge_storage_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — explicit-bucket Histogram aggregation storage.
microtel_add_test(
    NAME    metric_histogram_storage_test
    SOURCES metric_histogram_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_histogram_storage_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — base-2 exponential Histogram aggregation storage.
microtel_add_test(
    NAME    metric_exp_histogram_storage_test
    SOURCES metric_exp_histogram_storage_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_exp_histogram_storage_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — MetricProducer: IMetricProducer impl that assembles MetricBatchHandles.
microtel_add_test(
    NAME    metric_producer_test
    SOURCES metric_producer_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_producer_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — Concrete IMetricStream implementations (one per storage kind).
microtel_add_test(
    NAME    metric_stream_impls_test
    SOURCES metric_stream_impls_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_stream_impls_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — Meter: instrument factory wrapping MetricProducer.
microtel_add_test(
    NAME    meter_test
    SOURCES meter_test.cpp
    LIBS    microtel_sdk)
target_include_directories(meter_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — Observable instruments (callback-driven streams + Meter factories).
microtel_add_test(
    NAME    metric_observable_instruments_test
    SOURCES metric_observable_instruments_test.cpp
    LIBS    microtel_sdk)
target_include_directories(metric_observable_instruments_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — PeriodicExportingMetricReader: background-thread push-model reader.
microtel_add_test(
    NAME    periodic_exporting_metric_reader_test
    SOURCES periodic_exporting_metric_reader_test.cpp
    LIBS    microtel_sdk)
target_include_directories(periodic_exporting_metric_reader_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — SynchronousMetricReader: collect + export pipeline (pull-on-demand).
microtel_add_test(
    NAME    synchronous_metric_reader_test
    SOURCES synchronous_metric_reader_test.cpp
    LIBS    microtel_sdk)
target_include_directories(synchronous_metric_reader_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    always_off_sampler_test
    SOURCES always_off_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    trace_id_ratio_sampler_test
    SOURCES trace_id_ratio_sampler_test.cpp
    LIBS    microtel_sdk)

microtel_add_test(
    NAME    parent_based_sampler_test
    SOURCES parent_based_sampler_test.cpp
    LIBS    microtel_sdk)

# v1.1 — rule combinators (attribute / name / kind) and the two chain
# composition modes. Uses tests/mocks/mock_sampler.hpp as the invocation probe
# for the short-circuit tests, hence tests/ on the include path.
microtel_add_test(
    NAME    sampler_chain_test
    SOURCES sampler_chain_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sampler_chain_test
    PRIVATE ${PROJECT_SOURCE_DIR}/tests)

# v1.1 — the no-allocation half of the sampler-chain contract. Replaces global
# operator new with a counting one, which is process-wide, so it needs its own
# binary; skipped under sanitizers for the same reason as
# noexcept_alloc_failure_test (asan/tsan install their own new/delete).
if(MICROTEL_SANITIZER STREQUAL "")
microtel_add_test(
    NAME    sampler_chain_alloc_test
    SOURCES sampler_chain_alloc_test.cpp
    LIBS    microtel_sdk)
endif()

microtel_add_test(
    NAME    simple_span_processor_test
    SOURCES simple_span_processor_test.cpp
    LIBS    microtel_sdk)
# Test reaches into src/sdk/simple_span_processor.hpp directly (it's an
# internal header, not a public-API surface). Add src/ to the include path.
target_include_directories(simple_span_processor_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    noop_span_test
    SOURCES noop_span_test.cpp
    LIBS    microtel_sdk)
target_include_directories(noop_span_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    sdk_span_test
    SOURCES sdk_span_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_span_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    sdk_tracer_test
    SOURCES sdk_tracer_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_tracer_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

microtel_add_test(
    NAME    batch_span_processor_test
    SOURCES batch_span_processor_test.cpp
    LIBS    microtel_sdk)
target_include_directories(batch_span_processor_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — SdkProvider::GetMeter: meter factory + scope caching.
microtel_add_test(
    NAME    sdk_provider_meter_test
    SOURCES sdk_provider_meter_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_provider_meter_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(sdk_provider_meter_test PRIVATE
    -Wno-missing-field-initializers)

# M14 L5 — SdkProvider::GetLogger (ICP 0012).
microtel_add_test(
    NAME    sdk_provider_logger_test
    SOURCES sdk_provider_logger_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_provider_logger_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(sdk_provider_logger_test PRIVATE
    -Wno-missing-field-initializers)

# M6-C: SdkBuilder::Build() + SdkProvider lifecycle.
microtel_add_test(
    NAME    sdk_builder_test
    SOURCES sdk_builder_test.cpp
    LIBS    microtel_sdk)

# ICP 0036: SdkBuilder::WithExportTransport through a real Provider.
microtel_add_test(
    NAME    export_transport_builder_test
    SOURCES export_transport_builder_test.cpp
    LIBS    microtel_sdk)
target_include_directories(export_transport_builder_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)
target_compile_options(export_transport_builder_test PRIVATE
    -Wno-missing-field-initializers)

# Issue #267: SdkBuilder::Build and Provider::SetBatchOptions share one
# BatchOptions validator — code, TOML and the setter over one table of cases.
microtel_add_test(
    NAME    batch_options_validation_test
    SOURCES batch_options_validation_test.cpp
    LIBS    microtel_sdk)

# Static request header names (ICP 0038): code, env and TOML reach one check.
microtel_add_test(
    NAME    header_validation_test
    SOURCES header_validation_test.cpp
    LIBS    microtel_sdk)

# SdkProvider::ForceFlush two-stage flush (processor → exporter).
# Concurrency regression: Provider is documented thread-safe; GetMeter/
# GetLogger raced ForceFlush/Shutdown on the lazily-built members. Meaningful
# under TSAN.
microtel_add_test(
    NAME    sdk_provider_race_test
    SOURCES sdk_provider_race_test.cpp
    LIBS    microtel_sdk)

target_include_directories(sdk_provider_race_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(sdk_provider_race_test PRIVATE
    -Wno-missing-field-initializers)

# v1.1 — the four hot-reload Provider setters (ICP 0026). Reaches into
# src/sdk/sdk_provider.hpp and src/common/internal_log.hpp directly, and uses
# tests/mocks for the pipelines it does not want to build for real.
microtel_add_test(
    NAME    provider_setters_test
    SOURCES provider_setters_test.cpp
    LIBS    microtel_sdk microtel_common)

target_include_directories(provider_setters_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(provider_setters_test PRIVATE
    -Wno-missing-field-initializers)

# v1.1 ships-when gate clause 2 (ICP 0024): the four setters racing span/log
# emission, the BSP/BLRP workers and metric collection. Only meaningful under
# -DMICROTEL_SANITIZER=tsan; elsewhere it exercises the paths without being
# able to observe a race. MICROTEL_HAMMER_SECONDS extends the default budget
# for the minutes-scale local run.
microtel_add_test(
    NAME    hot_reload_hammer_test
    SOURCES hot_reload_hammer_test.cpp
    LIBS    microtel_sdk microtel_common)

target_include_directories(hot_reload_hammer_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(hot_reload_hammer_test PRIVATE
    -Wno-missing-field-initializers)

# Concurrency regression: MetricProducer had no mutex; AddStream raced Collect.
microtel_add_test(
    NAME    metric_producer_race_test
    SOURCES metric_producer_race_test.cpp
    LIBS    microtel_sdk)

target_include_directories(metric_producer_race_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(metric_producer_race_test PRIVATE
    -Wno-missing-field-initializers)

# Allocation-failure regression. Replaces global operator new process-wide, so
# it must be its own binary — arming the failure switch while another test's
# gtest machinery allocates would abort the run.
#
# Skipped under sanitizers: asan and tsan install their own global operator
# new/delete to track allocations, and a second replacement in the test binary
# either fails to link (tsan) or trips alloc-dealloc-mismatch (asan). The
# behaviour under test — surviving a throwing allocator — is orthogonal to what
# the sanitizers check, so the non-sanitized build is the right place for it.
if(MICROTEL_SANITIZER STREQUAL "")
microtel_add_test(
    NAME    noexcept_alloc_failure_test
    SOURCES noexcept_alloc_failure_test.cpp
    LIBS    microtel_sdk)

target_include_directories(noexcept_alloc_failure_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(noexcept_alloc_failure_test PRIVATE
    -Wno-missing-field-initializers)
endif()

# v1.1 — the multi-profile registry (ICP 0027): the slot array below the line,
# WithProfileName / GetProvider above it. Reaches into src/sdk/
# provider_registry.hpp and src/sdk/sdk_provider.hpp directly, and uses
# tests/mocks for the pipelines it does not want to build for real.
microtel_add_test(
    NAME    provider_registry_test
    SOURCES provider_registry_test.cpp
    LIBS    microtel_sdk)

target_include_directories(provider_registry_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(provider_registry_test PRIVATE
    -Wno-missing-field-initializers)

# v1.1 — the same registry under contention. Only meaningful under
# -DMICROTEL_SANITIZER=tsan; MICROTEL_HAMMER_SECONDS extends its budget for a
# longer local run.
microtel_add_test(
    NAME    provider_registry_race_test
    SOURCES provider_registry_race_test.cpp
    LIBS    microtel_sdk)

target_include_directories(provider_registry_race_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(provider_registry_race_test PRIVATE
    -Wno-missing-field-initializers)

# threading-model.md §7 fork semantics. Actually forks; Linux-only, which
# matches the project's target platform.
microtel_add_test(
    NAME    fork_safety_test
    SOURCES fork_safety_test.cpp
    LIBS    microtel_sdk)

target_include_directories(fork_safety_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(fork_safety_test PRIVATE
    -Wno-missing-field-initializers)

microtel_add_test(
    NAME    sdk_provider_test
    SOURCES sdk_provider_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_provider_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests)

target_compile_options(sdk_provider_test PRIVATE
    -Wno-missing-field-initializers)

# M12 — ICP 0008 metric drop reasons + diagnostics name mapping.
microtel_add_test(
    NAME    drop_reason_test
    SOURCES drop_reason_test.cpp
    LIBS    microtel_sdk)
target_include_directories(drop_reason_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M12 — DiagnosticsCounters: production IDiagnosticsSink (atomic counters).
microtel_add_test(
    NAME    diagnostics_counters_test
    SOURCES diagnostics_counters_test.cpp
    LIBS    microtel_sdk)
target_include_directories(diagnostics_counters_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# M14 — OTel Logs API surface (M14 increment 2).
microtel_add_test(
    NAME    log_api_test
    SOURCES log_api_test.cpp
    LIBS    microtel_sdk)
target_include_directories(log_api_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(log_api_test PRIVATE
    -Wno-missing-field-initializers)

# M13 — Per-view attribute_allowlist filtering (M13 increment 3).
microtel_add_test(
    NAME    attribute_allowlist_test
    SOURCES attribute_allowlist_test.cpp
    LIBS    microtel_sdk)
target_include_directories(attribute_allowlist_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(attribute_allowlist_test PRIVATE
    -Wno-missing-field-initializers)

# M13 — attribute_allowlist filtering for observable instruments (M13 increment 4).
microtel_add_test(
    NAME    observable_allowlist_test
    SOURCES observable_allowlist_test.cpp
    LIBS    microtel_sdk)
target_include_directories(observable_allowlist_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(observable_allowlist_test PRIVATE
    -Wno-missing-field-initializers)

# M14 L4.1 — SdkLogger: Emit routing, observed-time, correlation, attribute limit.
microtel_add_test(
    NAME    sdk_logger_test
    SOURCES sdk_logger_test.cpp
    LIBS    microtel_sdk)
target_include_directories(sdk_logger_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(sdk_logger_test PRIVATE
    -Wno-missing-field-initializers)

# M14 L4.2 — SimpleLogRecordProcessor: synchronous passthrough.
microtel_add_test(
    NAME    simple_log_record_processor_test
    SOURCES simple_log_record_processor_test.cpp
    LIBS    microtel_sdk)
target_include_directories(simple_log_record_processor_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(simple_log_record_processor_test PRIVATE
    -Wno-missing-field-initializers)

# M14 L4.2 — BatchLogRecordProcessor: queue + worker + batch, scope grouping.
microtel_add_test(
    NAME    batch_log_record_processor_test
    SOURCES batch_log_record_processor_test.cpp
    LIBS    microtel_sdk)
target_include_directories(batch_log_record_processor_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(batch_log_record_processor_test PRIVATE
    -Wno-missing-field-initializers)

# M13 — ViewRegistry wiring into SdkMeter (M13 increment 2).
microtel_add_test(
    NAME    view_wiring_test
    SOURCES view_wiring_test.cpp
    LIBS    microtel_sdk)
target_include_directories(view_wiring_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(view_wiring_test PRIVATE
    -Wno-missing-field-initializers)

# M13 — ViewRegistry: selector matching for Views (M13 increment 1).
microtel_add_test(
    NAME    view_registry_test
    SOURCES view_registry_test.cpp
    LIBS    microtel_sdk)
target_include_directories(view_registry_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
# Partial designated initializers are intentional: tests verify that nullopt
# fields in InstrumentSelector and ViewTransform match any value.
target_compile_options(view_registry_test PRIVATE
    -Wno-missing-field-initializers)

# v1.1 — built-in resource detectors (process, host). Runs against fixture
# trees under a temp directory via the detectors' injected path root, so it
# never depends on the host's real /proc, /etc or /var.
microtel_add_test(
    NAME    resource_detectors_test
    SOURCES resource_detectors_test.cpp
    LIBS    microtel_sdk)

# v1.1 — the docs/configuration.md §3.2 composition order (defaults, detectors, env, user) and
# the strict/lenient detector policy. Reaches into src/sdk/resource_builder.hpp
# and src/common/config/config.hpp directly.
microtel_add_test(
    NAME    resource_builder_test
    SOURCES resource_builder_test.cpp
    LIBS    microtel_sdk)
target_include_directories(resource_builder_test
    PRIVATE ${PROJECT_SOURCE_DIR}/src)

# v1.2 — SdkBuilder::WithLeafReceiver (ICP 0034, docs/leaf-concentrator-design.md
# §4.3, §6.2). Built in both configurations of MICROTEL_WITH_CONCENTRATOR, and
# told which one it is so it can check the other half of the contract.
microtel_add_test(
    NAME    leaf_receiver_builder_test
    SOURCES leaf_receiver_builder_test.cpp
    LIBS    microtel_sdk)
target_compile_options(leaf_receiver_builder_test PRIVATE
    -Wno-missing-field-initializers)
if(MICROTEL_WITH_CONCENTRATOR)
    target_compile_definitions(leaf_receiver_builder_test PRIVATE MICROTEL_WITH_CONCENTRATOR=1)
endif()

# v1.2 — the concentrator ingest path: SdkLeafReceiver against the dumb decoder
# mock and the fake, and the leaf table under it. Only in a concentrator build.
if(MICROTEL_WITH_CONCENTRATOR)
    microtel_add_test(
        NAME    leaf_receiver_test
        SOURCES leaf_receiver_test.cpp
        LIBS    microtel_sdk)
    target_include_directories(leaf_receiver_test
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(leaf_receiver_test PRIVATE
        -Wno-missing-field-initializers)

    microtel_add_test(
        NAME    leaf_table_test
        SOURCES leaf_table_test.cpp
        LIBS    microtel_sdk)
    target_include_directories(leaf_table_test
        PRIVATE ${PROJECT_SOURCE_DIR}/src)

    microtel_add_test(
        NAME    leaf_time_test
        SOURCES leaf_time_test.cpp
        LIBS    microtel_sdk)
    target_include_directories(leaf_time_test
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()
