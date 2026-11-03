# opentelemetry-cpp v1.30.0 against v1.19.0 and microtel (2026-11-03)

## SUTs

| SUT | Image built at | Compiler (`DW_AT_producer`, all CUs) | Protobuf, as linked |
|---|---|---|---|
| `parity-microtel-http-clang` | `1eae506` | Ubuntu clang 18.1.3 (76 CUs) | none: vendored upb, 67 `microtel_upb_*` symbols; no `google::protobuf` symbols |
| `parity-otelcpp-http-clang-q16k` (v1.19.0) | `d58009a` (`78574d9` after the branch history rewrite; same code) | Ubuntu clang 18.1.3 (66 CUs) | Ubuntu `libprotobuf32t64` 3.21.12-8.2ubuntu0.4; `/emit_app` resolves `libprotobuf.so.32` → `/usr/lib/x86_64-linux-gnu/libprotobuf.so.32.0.12`, owned by that package |
| `parity-otelcpp130-http-clang-q16k` (v1.30.0) | `4bf0d37` | Ubuntu clang 18.1.3 (83 CUs) | the same package and library as v1.19.0 |

The v1.30.0 image is `otelcpp.Dockerfile` with `OTELCPP_VERSION=v1.30.0`. It
has the same `ubuntu:24.04` base, compiler, flags and Ubuntu protobuf as
v1.19.0. Its two builds (the second with `--no-cache`) are identical in all 48
compared sections ([`builds/`](builds/)).

The first build failed in the emit-app's CMake step. v1.30.0 renamed the
package's CMake components (`trace` became `sdk`, and so on) but kept the
target names. The fix is the emit-app's existing opt-in
`BENCH_OTELCPP_ALL_COMPONENTS`, passed as `ALL_COMPONENTS=ON` for the v1.30.0
variant only. There were no source changes.

Both opentelemetry-cpp SUTs run with `EMIT_OTELCPP_BSP_MAX_QUEUE_SIZE=16384`.
microtel runs with its default queue of 8,192.

## Results

**Timing.** `run_rounds.sh`, 10 interleaved rounds × 10 samples × 10,000 spans
([`timing/`](timing/)). Governor performance, boost 0, single memory channel.
No failed rounds and 0 drops. The sink received 1,000,000 of 1,000,000 spans
from each SUT. Ratios: median of the ten per-round ratios against microtel in
the same round, with the range.

**Allocations.** `count_mallocs.py`, 0 and 5 attributes, two runs each,
identical ([`mallocs/`](mallocs/)). Both opentelemetry-cpp SUTs ran with the
16,384 queue and dropped nothing.

| SUT | p50, ns (IQR) | p95, ns | p50 ÷ microtel | p95 ÷ microtel | Calling-thread allocs/span, 0 attrs | 5 attrs | Background allocs/span, 0 attrs | 5 attrs |
|---|---:|---:|---|---|---:|---:|---:|---:|
| microtel | 219 (210–232) | 480 | 1 | 1 | 2 | 11 | 0.09 | 0.11 |
| otelcpp v1.19.0 | 772 (758–792) | 1846 | 3.51 (3.34–3.67) | 3.84 (3.81–3.93) | 14 | 47 | 1.11 | 1.11 |
| otelcpp v1.30.0 | 722 (684–749) | 1838 | 3.30 (3.02–3.43) | 3.81 (3.71–3.98) | 10 | 43 | 5.30 | 31.30 |

v1.30.0 ÷ v1.19.0, per round: p50 0.94 (0.89–0.97), p95 0.99 (0.97–1.02).

The calling-thread counts are whole numbers. Each is N + 0.0001 or 0.0002 per
span, which is the worker thread's start-up. In step 3 (2026-11-02), v1.19.0
measured 14.25 and 47.57 with its default 2,048-span queue. With nothing
dropped it is exactly 14 and 47, so the fraction came from the drop path's
warning messages.

Observed, not investigated:
- **Background allocations.** v1.30.0 makes 4 fewer allocations per span on
  the calling thread, but many more on background threads: 5.3 against 1.1
  per span with no attributes, and 31.3 against 1.1 with 5.
- **Flush time.** Its median flush took 5.3 ms against 1.9 ms.
- **Throughput.** The driver's spans/s, which includes the flush, is 735,113
  against 974,658.

Profiled below.

## Background allocations and flush time, profiled

**Cause.** opentelemetry-cpp #4035 (`91805271`, first released in v1.27.0)
changed how `OtlpRecordableUtils::PopulateRequest` puts each span into the
export request:
- before: `*scope_spans->add_spans() = std::move(input_span->span());`
- after: `scope_spans->add_spans()->CopyFrom(otlp_recordable->span());`

The new line carries the comment "The recordable span can only be copied here
since the request message is Arena allocated". It is still on main.

On Ubuntu's protobuf 3.21.12 the request is not arena-backed in the way that
comment assumes. `OtlpHttpExporter::Export` creates the request with
`Arena::Create<ExportTraceServiceRequest>(arena)`. In 3.21.12 that places the
message's memory on the arena but constructs it as `T()`, without the arena
(`google/protobuf/arena.h:702-716`). So the request and everything added to it
are plain heap messages:
- **v1.19.0:** the move between two arena-less messages is a swap;
- **v1.30.0:** `CopyFrom` copies every string field onto the heap.

protobuf 35.1's `Arena::Create<T>` does construct messages on the arena (the
`is_arena_constructable` branch), which is why main with the bundled 35.1.0
shows only 1.24 background allocations per span (`results/2026-11-02/otelcpp-main/`).
Other protobuf versions were not checked.

**Call sites.** From [`malloc_stacks.py`](../../../malloc_stacks.py), every
allocation with its stack, 10,000 spans ([`stacks/`](stacks/)):
- **v1.30.0, 0 attributes:** `Span::MergeImpl` under `PopulateRequest`, 4.0
  per span (3.0 `ArenaStringPtr::Set` creating a heap `std::string`, 1.0 for
  the 16-byte `trace_id`'s buffer).
- **v1.30.0, 5 attributes:** about 5.2 more per attribute (31.3 against 5.3),
  5 of them in `KeyValue` and `AnyValue` `MergeImpl`.
- **Both versions:** `CreateMaybeMessage<Span>`, 1.0 per span.

In v1.30.0 the sampler's totals match `count_mallocs.py` exactly (10.0002 and
5.3017 per span). In v1.19.0 the sampler's slowdown of the emitting thread
makes its batch processor export smaller batches: about 443 requests, where
v1.30.0 under the same sampler made 20. Its background totals under the sampler (8.7) are per-request overhead,
not what an unslowed run does (1.11, confirmed with a 3 s settle before the
snapshot). v1.30.0 does not show this effect.

**Experiment.** v1.30.0 with that one line changed back to the move
([`patches/otelcpp130-populate-move.patch`](../../../../sut/parity/patches/otelcpp130-populate-move.patch),
built twice, identical in 48 sections). Allocations: two runs each,
identical.

| | v1.19.0 | v1.30.0 | v1.30.0 + move |
|---|---:|---:|---:|
| Background allocations per span, 0 attributes | 1.11 | 5.30 | **1.30** |
| Background allocations per span, 5 attributes | 1.09 | 31.30 | **1.30** |
| Calling-thread allocations per span, 0 / 5 attributes | 14 / 47 | 10 / 43 | 10 / 43 |
| Flush, median of 100 samples, ms (IQR) | 1.78 (1.50–2.14) | 5.12 (4.90–5.31) | **3.17** (3.02–3.37) |
| Flush ÷ v1.30.0, per round | | 1 | 0.62 (0.57–0.69) |
| Span emit p50 ÷ v1.30.0, per round | | 1 | 1.00 (0.96–1.04) |
| Spans/s (driver, includes the flush) ÷ v1.30.0, per round | | 1 | 1.21 (1.15–1.23) |

The timing columns come from 10 interleaved rounds of the three SUTs, queue
16,384, 0 drops, 100% delivered ([`timing-move/`](timing-move/)).

**Export-thread CPU.** [`perf_threads.py`](../../../perf_threads.py) over a
`perf_profile.py` run, 3,000,000 spans each, `cycles:u`, background threads
only ([`perf/`](perf/)). Cycles per span:

| Stage | v1.19.0 | v1.30.0 | v1.30.0 + move |
|---|---:|---:|---:|
| build request: copy spans (`MergeImpl`) | 3 | **498** | 0 |
| build request: other | 392 | 238 | 226 |
| serialize request | 251 | 149 | 242 |
| destroy request | 691 | 654 | 652 |
| HTTP client (curl) | 154 | 335 | 338 |
| batch processor | 47 | 355 | 205 |
| other | 32 | 26 | 23 |
| **total** | **1,571** | **2,255** | **1,686** |

Process kernel time over the same runs: 0.37 s, 0.88 s and 0.50 s.

**Reading.** The copy is v1.30.0's extra background allocations, all of them,
and about 58% of its extra flush time (1.95 of 3.34 ms). The rest of the flush
gap, and the 115 cycles per span left after the move, are in the HTTP client
(libcurl) and in `BatchSpanProcessor::Export`. Those samples' innermost frames
are in Ubuntu's stripped libc and libcurl, so this profile cannot name them;
they were not investigated further. The calling thread is unaffected by the
copy: span p50 is the same with and without it.
