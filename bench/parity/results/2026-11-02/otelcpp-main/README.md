# opentelemetry-cpp main, #4580, and the SetIdentity changes (2026-11-02)

Four builds of opentelemetry-cpp, each built twice in upstream's dev container
(`.devcontainer/Dockerfile.dev` at `0a5bb0b`, clang 22.1.8, protobuf 35.1.0 and
abseil 20250512.1 from `third_party_release`, static) with the parity flags
(`-O2 -g -fno-omit-frame-pointer` + Release). Both builds of every variant are
identical in all 74 compared sections ([`builds/`](builds/)).

| Variant | Commit | What |
|---|---|---|
| a | `0a5bb0b` | main |
| b | `53cd5bb` | a + `SetIdentity`: skip `set_trace_state` when `Empty()`, `mutable_trace_id()->assign` |
| c | `5e5b283` | a merged with #4580 head `5aa9922` (conflicts with #4734 resolved: CHANGELOG, and the constructor moved to the .cc gets `attribute_value_depth_limit`) |
| d | `3012c13` | c + the same two `SetIdentity` changes, in `span_->` form |

Host: Ryzen 5 5600G, one 16 GB DIMM (single channel, JEDEC 2667), governor
performance, boost 0, SMT on. Absolute ns are specific to this host; compare
variants by ratio.

## SetIdentity allocations

[`setidentity_allocs.cpp`](../../../setidentity_allocs.cpp), two runs per
variant, identical ([`setidentity/`](setidentity/)).

| Variant | Root | Child | Root, trace state `vendor=abc,other=1` |
|---|---:|---:|---:|
| a | 4 | 5 | 5 |
| b | 3 | 4 | 5 |
| c | 1 | 1 | 2 |
| d | 1 | 1 | 2 |

On protobuf 35.1 with a heap `Span` (a), `set_trace_id` and
`mutable_trace_id()->assign` both cost 2, so the in-place assign saves nothing;
only the `Empty()` check does (1). The v1.19.0 + protobuf 3.21.12 numbers in
RESULTS.md (5/6, trace_id 3) do not carry over. Serialized `Span` bytes after
`SetIdentity` are identical in a, b, c and d, for empty and non-empty trace
state, root and child.

## Calling-thread allocations per span

`count_mallocs.py` with `malloc_count.so`, 10,000 spans after a 1,000-span
warmup, `EMIT_OTELCPP_BSP_MAX_QUEUE_SIZE=16384` (no drops), two runs each,
identical ([`mallocs/`](mallocs/)). The emit-app's 5 attributes are string
attributes, keys `bench.attr.N` (12 bytes), values 24 bytes.

| Variant | 0 attributes | 5 attributes | Background, 0 attributes |
|---|---:|---:|---:|
| a | 9 | 36 | 1.236 |
| b | 8 | 35 | 1.232 |
| c | 5 | 11 | 1.236 |
| d | 5 | 11 | 1.232 |

(Each count is N + 0.0002: two allocations per run from the worker thread's
start.)

## Span emit latency

`run_rounds.sh`, 5 interleaved rounds × 10 samples, queue 16384, no drops, no
failed rounds ([`timing/`](timing/)). Ratios are the median of the five
per-round ratios against the same round's a.

| Variant | p50, ns (IQR) | p95, ns | p50 ÷ a, per round | p95 ÷ a, per round |
|---|---:|---:|---|---|
| a | 574 (498–633) | 1806 | 1 | 1 |
| b | 467 (447–490) | 1692 | 0.82 (0.78–0.94) | 0.94 (0.91–0.96) |
| c | 445 (432–460) | 1221 | 0.76 (0.74–0.98) | 0.71 (0.58–0.78) |
| d | 438 (427–450) | 1125 | 0.76 (0.72–0.90) | 0.66 (0.57–0.69) |

d ÷ c per round: p50 0.98 (0.92–1.01), p95 0.86 (0.81–1.19). Both ranges
include 1, and d makes the same allocations as c: no measurable saving from the
`SetIdentity` changes on top of #4580.

Caveat on a and b: a's per-sample p50s spread from 424 to 708 ns (the other
variants 404–588), and its fastest samples match the others. b's 0.82 is more
than one allocation and an empty `ToHeader()` should cost; it has not been
profiled, so it may include a layout or allocator-state effect, not only the
change.

Not measured: memory per recordable (#4580 reports `sizeof(OtlpRecordable)`
216 → 752 bytes), export-thread CPU, other allocators.
