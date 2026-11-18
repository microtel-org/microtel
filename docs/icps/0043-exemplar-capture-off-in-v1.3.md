# ICP 0043: Exemplar capture off in v1.3

**Status:** Accepted — 2026-11-17. Milestone v1.3; tracking issue #544.
**Affected interfaces / docs:**
- `docs/metrics-design.md` §7: the `trace_based` default is deferred to #454, not removed
- [ICP 0025](0025-propagation-core.md) §3, "The exemplar payoff": the wiring stays, but production providers don't enable it (a note there points here)
- no public API change: nothing under `include/microtel/` changes
- in the implementing PR: `README.md` (status table, Known limitations)
- code: `src/sdk/sdk_provider.{hpp,cpp}` (a new `SdkProviderArgs` field, and a null span source to `SdkMeter` unless it is set)
- tests: `tests/integration/sdk/exemplar_wiring_test.cpp` (sets the field); a new test that a provider built by `SdkBuilder` captures nothing

**Affected tracks:** A (SDK), on the metric provider's wiring only. The encoder (F) is unchanged.

## Summary

Exemplars are not supported in v1.3. A provider built through the public API never captures them. The capture code stays compiled and tested, reached through an internal, test-only hook. The public option, and the `trace_based` default, come back with export in #454.

## Motivation

The storages capture an exemplar for every measurement made inside a sampled span, but the encoder never writes one (#454). Every sync measurement still pays for capture: a virtual `GetCurrentSpan()` call and a thread-local read on each `Add` / `Record`. Inside a sampled span it also pays for a clock read, a second map operation under the storage lock, and per-series allocations after each collection. All of it is thrown away. v1.3 makes metrics supported, and that promise should not cover a feature that does nothing visible.

## Decision

1. **Capture off.** `SdkProvider` passes a null `ICurrentSpanSource` to each `SdkMeter` unless the hook below is set. The storages already treat null as "no exemplars", so the capture code is unchanged. A measurement then pays one predictable null check. Log trace-correlation is unaffected: it uses the log pipeline's own span source (`m_log_pipeline->span_source`), not the meters'.
2. **An internal, test-only hook.** `SdkProviderArgs` (`src/sdk/sdk_provider.hpp`, not installed) gains `bool exemplar_capture_for_testing = false;`. `SdkBuilder` never sets it, so no public path reaches it. Tests that build `SdkProvider` directly (`exemplar_wiring_test.cpp`) set it, which keeps the capture path exercised end to end.
3. **No public option in v1.3.** No builder method, env var or TOML key. The public switch arrives with export (#454), together with `OTEL_METRICS_EXEMPLAR_FILTER` (#300), so that it is designed once with something to switch.
4. **`trace_based` is deferred, not removed.** `metrics-design.md` §7 keeps `trace_based` as the target default, matching OTel. It gains a status note: capture is off in v1.3, and `trace_based` becomes the default when export lands (#454).
5. **No-op surfaces stay no-ops.** The otel-cpp shim's context parameters on `Add` / `Record` stay ignored, and `OTEL_METRICS_EXEMPLAR_FILTER` stays unread.
6. **Outside the compatibility promise.** Exemplars are not part of what v1.3 promises. A later minor release that turns capture and export on by default (#454) is not a breaking change.

## What capture-off leaves behind

No per-series exemplar storage is allocated when the span source is null. Each of the four storages (`SumStorage`, `GaugeStorage`, `HistogramStorage`, `ExponentialHistogramStorage`) inserts into its `m_exemplars` map only inside `if (exemplar.has_value())`, and an exemplar is built only when the span source is non-null. What remains:

- one empty `std::unordered_map` member per storage, held inline;
- once per collection, not per measurement, a `find` per point on that empty map, then `clear()`.

Neither allocates. That was checked against libstdc++ with clang and GCC: a default-constructed `unordered_map` performs no allocation across construction, `find` and `clear`. Removing the per-collection `find` is not needed for this ICP.

## Migration

None needed. No exemplar has ever left the process, and no public setting controlled capture, so no application or backend sees a difference.

## Rationale & alternatives

- **Export in v1.3 (#454).** Deferred to "Metrics: additive features (after v1.3)" together with the reservoirs (#300), so that exemplar output is designed once rather than changed after v1.3.
- **A public experimental builder option now.** Rejected. A switch for a feature that exports nothing gives users nothing, and it would be public surface to keep or break when #454 designs the real one.
- **Compile the capture code out (`#if`).** Rejected. It would stop being built and tested, and export will need it.
- **Delete the capture code.** Rejected for the same reason.
