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

Not profiled.
