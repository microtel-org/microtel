# ICP 0048: A meter's identity includes its schema URL

**Status:** Accepted — 2026-11-27. Milestone v1.3; tracking issue #456. The decision was made in review of #557.
**Affected interfaces / docs:**
- `include/microtel/provider.hpp`: the Doxygen of `Provider::GetMeter` (**public API contract**; the signature is unchanged)
- `docs/metrics-design.md` §8
- code: `src/sdk/sdk_provider.{hpp,cpp}` (the meter cache key), `src/sdk/metric_producer.{hpp,cpp}` (scope grouping in `AddStream`)
- tests: `tests/unit/sdk/sdk_provider_meter_test.cpp`, `tests/unit/sdk/metric_producer_test.cpp`

**Affected tracks:** A (SDK). The encoder is unchanged: it already writes each scope's own `schema_url`.

## Summary

`Provider::GetMeter` caches meters by `(name, version, schema_url)`, not `(name, version)`, so two schema URLs give two meters and two `ScopeMetrics`.

## Motivation

Since M12, `GetMeter`'s Doxygen has promised that `schema_url` does not affect caching. #557 exported the URL, which made the promise observable: the first call for a `(name, version)` fixed the URL, and a later call with another URL silently got the first one's meter. The OTel spec treats the URL as part of a meter's identity:

- Metrics API, [Get a Meter](https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/metrics/api.md#get-a-meter): "The term *identical* applied to Meters describes instances where all parameters are equal. The term *distinct* applied to Meters describes instances where at least one parameter has a different value." `schema_url` is one of those parameters.
- [Instrumentation scope](https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/common/instrumentation-scope.md): "The instrumentation scope is defined by the (name,version,schema_url,attributes) tuple".
- Metrics SDK, [Meter](https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/metrics/sdk.md#meter): "Distinct meters MUST be treated as separate namespaces for the purposes of detecting duplicate instrument registrations."

## Decision

1. **The cache key is `(name, version, schema_url)`.** A call that differs from an earlier one only in `schema_url` returns a new meter. An empty URL is a value like any other: `GetMeter("x", "1")` and `GetMeter("x", "1", "https://…")` are distinct.
2. **The producer groups streams by the same triple.** `MetricProducer::AddStream` matches a scope on name, version and schema URL, so each meter exports its own `ScopeMetrics` with its own `schema_url`.
3. **Each meter is its own instrument namespace**, as the SDK spec requires. An instrument created under the same name in two such meters gives two independent streams.
4. **Scope attributes are out of scope.** `GetMeter` has no `attributes` parameter. Adding one would extend the key the same way.
5. **The Doxygen** for `GetMeter` changes from "`schema_url` does not affect caching" to "the same `(name, version, schema_url)` returns the cached instance".

## Migration

- **Applications that pass different schema URLs for one `(name, version)`:** they now get one meter, and one `ScopeMetrics`, per URL, each carrying its own URL. Before #557 every URL was dropped; between #557 and this change the first one won. Metrics are experimental until v1.3, so this is outside any compatibility promise.
- **Applications that always pass the same URL, or none:** no change.
- **The otel-cpp shim** passes `schema_url` through unchanged and caches nothing itself, so it follows the same rule.

## Rationale & alternatives

- **Keep `(name, version)` and let the first call win.** Rejected: a later call's URL is silently ignored, and the spec calls the two meters distinct.
- **Keep `(name, version)` and let the latest URL win.** Rejected: a meter's exported scope would change under instruments that already exist.
- **Include the URL in the cache key but merge the scopes in the producer.** Rejected: two meters would then share one `ScopeMetrics`, and only one URL could be exported.
