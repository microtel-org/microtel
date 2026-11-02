# microtel vs opentelemetry-cpp: where the span-cost gap comes from

**Status: draft, on the local `bench/otelcpp-parity` branch. Not reviewed, not for publication.**
Measured 2026-11-02. Data in [`results/2026-11-02/`](results/2026-11-02/).

## Summary

1. **Toolchain vs design: almost all design** (high confidence for OTLP/HTTP). Both binaries are Clang 18.1.3 with identical flags and no hardening (checked from `DW_AT_producer` and the binaries). opentelemetry-cpp still takes 3.66× microtel's span time (793 vs 216 ns p50), the same ratio as the published snapshot. The only hardened code in either path is Ubuntu's `libprotobuf.so.32`, which is 4.5% of opentelemetry-cpp's span time (the "libprotobuf code" column below). The GCC and hardening variants were not run this time.
2. **Protobuf on the calling thread: 46–66% of the 577 ns gap** (medium-high). Building the protobuf span in `OtlpRecordable` is 266 ns of opentelemetry-cpp's span time in the perf profile, which is 46% of the gap. Swapping in the SDK's own SpanData recordable removes 382 ns (66%). That swap also takes away export-thread work, so 66% is an upper bound.
3. **Most of that protobuf cost is malloc** (high). opentelemetry-cpp makes 14.2 heap allocations per span on the calling thread, against microtel's 2.0, and 47.6 against 11.0 with 5 attributes. Its single biggest cost is `OtlpRecordable::SetIdentity`, at 26% of span time (21 points of it allocation). The rest of the gap is allocation outside the recordable, `nostd::shared_ptr<TraceState>` copies (about 67 ns), clock reads and the processor.
4. **Footprint, gRPC vs protobuf: not measured in this run.** Part 5 was skipped by request.
5. **What could port under C++14:** the design change itself. That means recording into a plain record (opentelemetry-cpp already has SpanData) and building the wire message on the export thread, plus cutting the allocations in `OtlpRecordable`. All of that is C++14 work inside opentelemetry-cpp's exporter (medium confidence). Porting microtel's upb + nghttp2 exporter is a larger job: about 1,200 mechanical C++17/20 edits across about 8,700 lines, plus an encoder rewrite for SpanData. A rough estimate is 3–6 weeks (low confidence; nothing was compiled at C++14).

## Host and conditions

| | |
|---|---|
| CPU | AMD Ryzen 5 5600G, 6 cores / 12 threads, SMT on |
| Memory | **1 × 16 GB DIMM, single channel, JEDEC** (a faulty second DIMM was removed; see below) |
| Governor / boost | `performance` on all 12 CPUs / boost off (`cpufreq/boost` = 0) |
| Kernel / podman | 7.2.9-201.fc44.x86_64 / 5.8.9 |
| Load at each SUT start | 0.32–0.48 (1-min average); Tctl 39–45 °C |

**Host history.** Before these runs, this host crashed compilers repeatedly. A stress-ng `--vm --verify` screen found bit errors in RAM. The bad DIMM was removed, and the remaining one passed memtest86+ (2 passes) and a clean 30-minute stress-ng screen (16/16 stressors). Everything below was built and measured after that. Each image was built twice from scratch (`--no-cache`), and the two builds were compared section by section. All were identical: 24 sections for microtel, 48 for each opentelemetry-cpp image.

**Absolute latencies are not comparable with the published snapshot**: one memory channel instead of two. The ratio held, at 3.66× here against 3.65× published (813 / 223 ns).

## SUTs

| Name | What | Compiler (`DW_AT_producer`, all CUs) | Flags | Hardening scan of `/emit_app` |
|---|---|---|---|---|
| microtel-http | microtel master (`src/`, `include/` identical to 82aa378), OTLP/HTTP | Ubuntu clang 18.1.3 (76 CUs) | `-O2 -g -fno-omit-frame-pointer` + Release (`-O3 -DNDEBUG`), C++20 | 0 `__stack_chk_fail` calls, 6 `endbr64`, 0 `_chk` imports |
| otelcpp-http | opentelemetry-cpp v1.19.0 (`ced79860`) from source, OTLP/HTTP, published-snapshot config | Ubuntu clang 18.1.3 (66 CUs) | same, C++17 | same |
| otelcpp-http, in-memory | as above, but `InMemorySpanExporter`, so the SDK's SpanData recordable replaces `OtlpRecordable` | Ubuntu clang 18.1.3 (64 CUs) | same, C++17 | same |

All three images use the same emit-app harness and the `ubuntu:24.04` packages. opentelemetry-cpp's HTTP exporter links Ubuntu's `libprotobuf32t64` 3.21.12 and `libcurl4`. The Dockerfiles are [`../sut/parity/`](../sut/parity/), and the builds use [`build.sh`](build.sh) with `PARITY_DOUBLE=1`.

## 1. Span emit time

The `hot-loop-traces` profile ran 10 samples × 10,000 spans per SUT per round against the blackhole sink. There were 5 rounds, each starting with a different SUT and starting only when the 1-minute load average was below 0.5 ([`run_rounds.sh`](run_rounds.sh)). [`merge.py`](merge.py) takes the median of the 50 per-sample p50 values. No SUT failed and no round was dropped.

| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | p50 ÷ microtel |
|---|---:|---:|---:|---:|
| microtel-http | 50 (5 rounds) | **216** (210–230) | 478 | 1.00× |
| otelcpp-http, SpanData recordable | 50 (5 rounds) | **411** (407–415) | 1,789 | 1.90× |
| otelcpp-http, published config | 50 (5 rounds) | **793** (777–811) | 2,786 | 3.66× |

The gap is 577 ns. Removing the protobuf recordable closes 382 ns of it.

## 2. Where the time goes

Profiling method:
- perf ran **on the host** against the container's process: `perf record -e cycles:u -F 4999 --call-graph fp`, using host perf 7.2.13. Rootless podman's seccomp profile blocks `perf_event_open` inside the container, and `perf_event_paranoid=2` limits sampling to user space.
- The workload repeats exactly what one benchmark sample does: a 10,000-span run on one worker thread, then a flush. It ran 3,000 times for microtel (30M spans) and 2,000 times for opentelemetry-cpp (20M spans), giving about 200,000 samples each.
- p50 during profiling was 211 and 739 ns.

[`perf_buckets.py`](perf_buckets.py) keeps samples on the worker thread, weights them by cycles, and leaves out the harness (loop, timer, histogram). Each sample is assigned to one bucket:
- **libprotobuf:** any frame in libprotobuf, `OtlpRecordable` or other OTLP exporter code. Allocation done by the recordable counts here.
- **Otherwise:** the innermost frame below `EmitSpan` that matches a bucket's function-name rule.

The ns columns are percentages times the step 1 p50, so they are approximate.

| Bucket | microtel % | ≈ ns | otelcpp % | ≈ ns | Gap, ns |
|---|---:|---:|---:|---:|---:|
| libprotobuf (incl. under `OtlpRecordable`) | 0.0 | 0 | 33.5 | 266 | +266 |
| allocation (new/malloc/free) | 11.3 | 24 | 12.9 | 102 | +78 |
| atomics and locks | 3.0 | 6 | 2.2 | 17 | +11 |
| clock reads | 22.5 | 49 | 10.8 | 86 | +37 |
| sampler | 0.5 | 1 | 0.7 | 6 | +5 |
| processor enqueue | 23.6 | 51 | 9.7 | 77 | +26 |
| attribute storage | 0.9 | 2 | 0.6 | 5 | +3 |
| other | 38.1 | 82 | 29.7 | 236 | +154 |
| **total** | | **216** | | **793** | **577** |

Flame graphs of the worker thread: [microtel](results/2026-11-02/step2/microtel-http-clang.flamegraph.svg), [opentelemetry-cpp](results/2026-11-02/step2/otelcpp-http-clang.flamegraph.svg).

Inside opentelemetry-cpp's libprotobuf bucket:

| `OtlpRecordable` method | % of span time | of which allocation | libprotobuf code | recordable / generated code |
|---|---:|---:|---:|---:|
| `SetIdentity` | 26.2 | 21.3 | 2.9 | 2.1 |
| `SetName` | 5.1 | 2.9 | 1.4 | 0.8 |
| everything else | 2.1 | 0.2 | 0.2 | 1.7 |

`SetIdentity` (`otlp_recordable.cc:43-55`) sets three protobuf string fields: `trace_id`, `span_id` and `trace_state`. With no arena, each set heap-allocates. Counting allocation everywhere, inside and outside the recordable, it is about 37% of opentelemetry-cpp's span time (≈296 ns), against 11% (≈24 ns) for microtel.

The rest of the gap:
- **opentelemetry-cpp's "other"** is led by `nostd::shared_ptr<TraceState>` copy, move and destroy, at 8.4% (≈67 ns). Then come `Tracer::StartSpan`, `Span::Span`, a `std::map` walk (`_Rb_tree_increment`), `GetCurrentSpan` and ID generation.
- **microtel's "other"** is `StartSpanInternal` (17%), the `SdkSpan` constructor (6%), `mt19937_64` for IDs (4%) and the per-span scope copy (2%, #533).
- **microtel's enqueue** includes the per-span `std::deque` node allocation under the lock (#530).
- **Clock reads** are microtel's third-largest cost: 49 ns for its two `system_clock::now()` calls.

**opentelemetry-cpp drops spans silently in this benchmark.** Its `BatchSpanProcessor` queue holds 2,048 spans by default (`batch_span_processor_options.h:26`) and fills during the 10,000-span bursts. Every drop logs `"BatchSpanProcessor queue is full - dropping span."` with a `write()` on the calling thread (`batch_span_processor.cc:89-91`). That logging is about 2.8% of its span time, inside "processor enqueue". The emit-app cannot see these drops, so the driver reports 0 dropped while the sink receives 95–99% of the spans. The published snapshot has the same blind spot. The drops hardly move p50, but the log writes probably inflate opentelemetry-cpp's p95.

**Kernel time is not in the profile.** Over each whole process (all threads, during profiling), kernel time was 27% of CPU for microtel (6.4 of 23.4 s) and 15% for opentelemetry-cpp (4.4 of 28.4 s). Most of it is probably the exporters' network writes, but the calling thread's share was not measured.

## 3. Allocations per span

[`malloc_count.c`](malloc_count.c) is an `LD_PRELOAD` counter, built twice in `ubuntu:24.04` with GCC 13.3.0, with identical output. It counts `malloc`, `calloc`, `realloc` and the aligned allocators per thread. C++ `new` reaches `malloc` through libstdc++, so it is counted too. The images are unmodified.

[`count_mallocs.py`](count_mallocs.py) runs a 1,000-span warmup and a flush, takes a snapshot, runs 10,000 spans and a flush, then takes a second snapshot. The emit-app starts a fresh worker thread per run, so the calling-thread count is that worker's total. Background is every other thread's growth between the snapshots. Each count was run twice.

| Calling thread, allocations per span | 0 attributes | 5 attributes (12-char keys, 24-byte values) | Per added attribute |
|---|---:|---:|---:|
| microtel-http | **2.000** (2 runs: 20,001 / 20,001 total) | **11.000** (110,001 / 110,002) | 1.8 |
| otelcpp-http | **14.25** (14.24 / 14.25) | **47.57** (47.59 / 47.54) | 6.7 |

| Background threads, allocations per span | 0 attributes | 5 attributes |
|---|---:|---:|
| microtel-http | 0.09 | 0.11 |
| otelcpp-http | 1.05 | 0.97 |

microtel's counts match a reading of its source exactly:
- **The benchmark span:** the `SdkSpan` object (`sdk_tracer.cpp:156`) and one deque node (`batch_span_processor.cpp:167`). Issues #531 and #530 respectively.
- **Each of 5 attributes:** a copy of the 24-byte value (`span.hpp:78`, taken by value). The attribute vector also regrows 4 times in total (`sdk_span.cpp:204`, #532).

opentelemetry-cpp's count is not a whole number, most likely because each dropped span's warning formats a message, which allocates.

## Design difference, with source citations

**opentelemetry-cpp v1.19.0** (`ced79860`): the recordable holds a protobuf `Span` and writes into it on the calling thread as each field is set.

| What | Where |
|---|---|
| The recordable is a protobuf `Span` | `exporters/otlp/include/opentelemetry/exporters/otlp/otlp_recordable.h:84` |
| One heap-allocated recordable per span | `exporters/otlp/src/otlp_http_exporter.cc:137-140` |
| Created at span start | `sdk/src/trace/span.cc:60` |
| `SetName`, `SetIdentity`, `SetAttribute` write the proto | `otlp_recordable.cc:165-167`, `:43-55`, `:120-124` |
| Handed to the processor at `End` | `span.cc:221` → `sdk/src/trace/batch_span_processor.cc:82-89` |
| Moved into the request, then serialized, on the export thread | `otlp_recordable_utils.cc:119`; `otlp_http_client.cc:469` |
| No protobuf arena on this path | `Arena` appears only in `otlp_file_exporter.cc:74-83` |

v1.30.0 has the same design: `otlp_recordable.h:119`, `otlp_recordable.cc:148` and `:224`.

**microtel** (master 82aa378): spans are recorded into a plain struct, and upb runs only on the exporter's worker.

| What | Where |
|---|---|
| Plain record: `std::string` and `std::vector`, no protobuf | `include/microtel/internal/batch.hpp:32` |
| Recorded at start, `SetAttribute` and `End` | `src/sdk/sdk_span.cpp:154-158`, `:186-204`, `:286` |
| Enqueued under the processor mutex | `src/sdk/batch_span_processor.cpp:134-167` |
| Encoded on the exporter's own worker thread | `src/exporter/otlp_exporter.cpp:36`, `:289` → `src/wire/encoder/otlp_encoder.cpp:754` |
| upb confined to the encoder directory | `src/wire/encoder/` only |

## Porting to C++14

[`cxx_audit.py`](cxx_audit.py) scans source with comments stripped, so counts are of spellings. Full output: [`results/2026-11-02/cxx_audit.md`](results/2026-11-02/cxx_audit.md).

- **C++20 in transport, wire and exporter:**
  - 443 designated initializers;
  - 50 `std::span` (to `nostd::span`);
  - 13 ranges uses;
  - 3 `starts_with`.
  - No concepts, coroutines, `<bit>`, `<=>`, `std::format` or `jthread`.
- **C++17, the bulk:**
  - 118 `string_view` and 5 `variant`, which map onto `nostd`;
  - 113 `optional` and 80 `std::byte`, which have no `nostd` equivalent;
  - 204 attributes such as `[[nodiscard]]`;
  - 55 lock types relying on template-argument deduction;
  - 49 nested namespaces;
  - 13 `if constexpr` in one file.
- **Size:** about 1,200 edit sites across about 49 files and 8,700 code lines. Mechanical edits: 1–2 weeks. Encoder rewrite against SpanData, plus integration with opentelemetry-cpp's exporter interface and tests: 2–4 weeks.
- **New dependencies:** nghttp2 and upb would be new for opentelemetry-cpp. Whether upb's headers compile in a C++14 translation unit is **unverified**.

The cheaper port is the design idea, not the code:
- record into SpanData;
- build the wire message on the export thread;
- in the meantime, cut `OtlpRecordable`'s per-field heap allocations, for example with an arena or reused messages.

All of that is plain C++14.

## Not measured

- **The rest of part 1:** the GCC 13 and hardening variants, opentelemetry-cpp v1.30.0, and both libraries over OTLP/gRPC. Skipped by request. For the gRPC pair, the published snapshot already shows GCC-and-hardened (812 ns) and Clang-unhardened (813 ns) opentelemetry-cpp at the same p50.
- **Part 5:** the footprint split between gRPC and protobuf.
- **Kernel time on the calling thread** (`perf_event_paranoid=2`).
- **Virtual dispatch.** An indirect call's cost lands in its caller, so sampling cannot separate it.
- **Which call sites make opentelemetry-cpp's 14 allocations.** The counter is per thread, not per caller.
- **opentelemetry-cpp's drop count.** The SDK does not expose it; delivery rate is the only signal.

## Reproducing

```bash
PARITY_DOUBLE=1 bench/parity/build.sh <log-dir> microtel-http-clang otelcpp-http-clang otelcpp-http-clang-inmem
bench/parity/run_rounds.sh <out> 5 parity-microtel-http-clang parity-otelcpp-http-clang parity-otelcpp-http-clang-inmem
python3 -I bench/parity/merge.py <out> parity-microtel-http-clang
python3 -I bench/parity/perf_profile.py bench-sut-parity-<variant>:perf <cycles> <dir> && python3 -I bench/parity/perf_buckets.py <dir>
cc -O2 -fPIC -shared -o malloc_count.so bench/parity/malloc_count.c   # in ubuntu:24.04
python3 -I bench/parity/count_mallocs.py bench-sut-parity-<variant> malloc_count.so <attrs> <out.json>
```
