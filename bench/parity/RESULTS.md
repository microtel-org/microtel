# microtel vs opentelemetry-cpp: where the span-cost gap comes from

**Status: draft, on the local `bench/otelcpp-parity` branch. Not reviewed, not for publication.**
Measured 2026-11-02; revised after review the same day (queue rerun, CPU per span, `SetIdentity` detail), then extended with opentelemetry-cpp main and #4580 ([section 4](#4-opentelemetry-cpp-main-and-pr-4580)). Data in [`results/2026-11-02/`](results/2026-11-02/).

## Summary

1. **Toolchain vs design: almost all design** (high confidence for OTLP/HTTP). Both binaries are Clang 18.1.3 with identical flags and no hardening (checked from `DW_AT_producer` and the binaries). opentelemetry-cpp still takes 3.66× microtel's span time (793 vs 216 ns p50), the same ratio as the published snapshot, and 3.58× (776 vs 216 ns) when its queue is large enough not to drop spans. The only hardened code in either path is Ubuntu's `libprotobuf.so.32`, which is 4.5% of opentelemetry-cpp's span time. The GCC and hardening variants were not run this time.
2. **Protobuf on the calling thread: 46–65% of the gap** (medium-high). Building the protobuf span in `OtlpRecordable` is 266 ns of opentelemetry-cpp's span time in the perf profile, which is 46% of the 577 ns gap. Swapping in the SDK's own SpanData recordable removes 366 of the 560 ns gap (65%), with both variants at a 16,384-span queue and confirmed drop-free. The swap uses the same `BatchSpanProcessor` and options and changes only the exporter. It also takes away export-thread work, so 65% is an upper bound.
3. **Most of that protobuf cost is malloc** (high). opentelemetry-cpp makes 14.2 heap allocations per span on the calling thread, against microtel's 2.0, and 47.6 against 11.0 with 5 attributes. Its biggest single cost is `OtlpRecordable::SetIdentity`, at 26% of span time (21 points of it allocation), and it makes 5 allocations per root span. Across all threads, opentelemetry-cpp uses **1.8× microtel's CPU per span** (1,420 vs 780 ns, user + kernel, with the profiling harness included in both).
4. **Footprint, gRPC vs protobuf: not measured in this run.** Part 5 was skipped by request.
5. **What could change in opentelemetry-cpp under C++14:** the design, inside its own SDK and exporter. That means recording into its existing SpanData-style recordable and building the protobuf message on the export thread (medium confidence). Smaller fixes in `OtlpRecordable` come first. Two small `SetIdentity` changes take a root span from 5 to 3 allocations on v1.19.0 (measured, high confidence), but only from 4 to 3 on current main, and nothing on top of open PR #4580. That PR implements the arena option of upstream issue open-telemetry/opentelemetry-cpp#4558 and takes a minimal span from 9 to 5 calling-thread allocations ([section 4](#4-opentelemetry-cpp-main-and-pr-4580)). Porting microtel's own exporter is a separate, larger option; see the [appendix](#appendix-porting-microtels-exporter-to-c14).

## Host and conditions

| | |
|---|---|
| CPU | AMD Ryzen 5 5600G, 6 cores / 12 threads, SMT on |
| Memory | **1 × 16 GB DIMM, single channel, JEDEC** (a faulty second DIMM was removed; see below) |
| Governor / boost | `performance` on all 12 CPUs / boost off (`/sys/devices/system/cpu/cpufreq/boost` = 0) |
| Kernel / podman | 7.2.9-201.fc44.x86_64 / 5.8.9 |
| Load at each SUT start | 0.32–0.48 (1-min average); Tctl 39–45 °C |

**Host history.** Before these runs, this host crashed compilers repeatedly. A stress-ng `--vm --verify` screen found bit errors in RAM. The bad DIMM was removed. Then:
- **memtest86+ on the remaining DIMM: 1 full pass, 0 errors**, as reported by the operator. That is below the 2-pass clearance originally asked for.
- **stress-ng: a clean 30-minute screen** after the reboot with the single DIMM. It was stress-ng 0.22.01, run from this study's session on 2026-11-02, 16:03–16:33 local time, as `stress-ng --cpu 12 --vm 4 --vm-bytes 75% --verify --timeout 30m --metrics-brief`. Result: 16 of 16 stressors passed (12 cpu, 4 vm), 0 failed, Tctl peak 75.6 °C. Log: [`results/2026-11-02/host/stress-ng.txt`](results/2026-11-02/host/stress-ng.txt), with temperatures in `stress-ng-tctl.txt`.

Everything below was built and measured after that. Each image was built twice from scratch (`--no-cache`), and the two builds were compared section by section. All were identical: 24 sections for microtel, 48 for each opentelemetry-cpp image.

**Absolute latencies are not comparable with the published snapshot.** There are two reasons:
- this host now runs one memory channel instead of two;
- cpufreq boost is off here. The published snapshot's `results.json` records the governor but not boost, so its boost state is unknown.

The ratio held, at 3.66× here against 3.65× published (813 / 223 ns).

## SUTs

| Name | What | Compiler (`DW_AT_producer`, all CUs) | Flags | Hardening scan of `/emit_app` |
|---|---|---|---|---|
| microtel-http | microtel master (`src/`, `include/` identical to 82aa378), OTLP/HTTP | Ubuntu clang 18.1.3 (76 CUs) | `-O2 -g -fno-omit-frame-pointer` + Release (`-O3 -DNDEBUG`), C++20 | 0 `__stack_chk_fail` calls, 6 `endbr64`, 0 `_chk` imports |
| otelcpp-http | opentelemetry-cpp v1.19.0 (`ced79860`) from source, OTLP/HTTP, published-snapshot config | Ubuntu clang 18.1.3 (66 CUs) | same, C++17 | same |
| otelcpp-http, in-memory | as above, but `InMemorySpanExporter`, so the SDK's SpanData recordable replaces `OtlpRecordable` | Ubuntu clang 18.1.3 (64 CUs) | same, C++17 | same |

All three images use the same emit-app harness and the `ubuntu:24.04` packages. opentelemetry-cpp's HTTP exporter links Ubuntu's `libprotobuf32t64` 3.21.12 and `libcurl4`. The Dockerfiles are [`../sut/parity/`](../sut/parity/), and the builds use [`build.sh`](build.sh) with `PARITY_DOUBLE=1`.

Two sets of opentelemetry-cpp images were used:
- **Steps 1–3:** images built at `1eae506` (HTTP) and `eabff04` (in-memory).
- **The [queue rerun](#1b-rerun-with-a-queue-that-does-not-overflow):** both rebuilt at `d58009a`, which adds a drop counter and an opt-in queue size to the bench backend and makes no other change.

**Same processor in both opentelemetry-cpp variants.** The in-memory variant differs only in its exporter: `InMemorySpanExporterFactory::Create` at `bench/emit-app/src/backend_otelcpp.cpp:254`, against `OtlpHttpExporterFactory::Create` at `:258`. Both then go through the same `BatchSpanProcessorFactory::Create` with the same `BatchSpanProcessorOptions` (`:270-279`). These are opentelemetry-cpp's defaults: a queue of 2,048 spans (`batch_span_processor_options.h:26`), export batches of 512 and a 5 s delay. The sampler and harness are the same too.

**microtel's processor** in the benchmark also runs with its defaults (`include/microtel/sdk_builder.hpp:61-64`, `:114-115`):
- a queue of 8,192 records, export batches of 512 and a 5 s delay;
- `DropPolicy::DropNewest`;
- caps of 16 MiB in total and 64 KiB per record.

When the queue is full, `Enqueue` → `MakeRoomFor` discards the incoming record and increments a `QueueFull` diagnostic counter (`src/sdk/batch_span_processor.cpp:185-192`). It does not log or do I/O on the calling thread. In every run here, microtel dropped nothing and delivered 100% of its spans.

## 1. Span emit time

The `hot-loop-traces` profile ran 10 samples × 10,000 spans per SUT per round against the blackhole sink. There were 5 rounds, each starting with a different SUT and starting only when the 1-minute load average was below 0.5 ([`run_rounds.sh`](run_rounds.sh)). [`merge.py`](merge.py) takes the median of the 50 per-sample p50 values. No SUT failed and no round was dropped.

| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | p50 ÷ microtel |
|---|---:|---:|---:|---:|
| microtel-http | 50 (5 rounds) | **216** (210–230) | 478 | 1.00× |
| otelcpp-http, SpanData recordable | 50 (5 rounds) | **411** (407–415) | 1,789 | 1.90× |
| otelcpp-http, published config | 50 (5 rounds) | **793** (777–811) | 2,786 | 3.66× |

The gap is 577 ns. Removing the protobuf recordable closes 382 ns of it. In this run, the published-config SUT dropped spans when its queue filled (see below), and the in-memory variant did not. The rerun below removes that difference.

### 1b. Rerun with a queue that does not overflow

The run was the same as above: 5 rounds, the same rotation, and microtel as the reference. opentelemetry-cpp's `BatchSpanProcessorOptions::max_queue_size` was raised from 2,048 to 16,384 through `EMIT_OTELCPP_BSP_MAX_QUEUE_SIZE` (`backend_otelcpp.cpp:274`), in both variants.

Drops are now counted. opentelemetry-cpp has no drop counter, so the bench installs a log handler that counts its `"BatchSpanProcessor queue is full - dropping span."` messages and forwards every message to the previous handler. The counter was checked against the sink first: in one round with the default queue, it counted 2,199 drops, exactly the 2,199 spans the sink did not receive ([`drop-counter-check.md`](results/2026-11-02/queue16k/drop-counter-check.md)). The same check showed the default-queue in-memory variant dropping nothing.

| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | p50 ÷ microtel | Drops | Delivered |
|---|---:|---:|---:|---:|---:|---|
| microtel-http | 50 (5 rounds) | **216** (210–227) | 477 | 1.00× | 0 | 500,000 / 500,000 (100%) at the sink |
| otelcpp-http, SpanData recordable, queue 16,384 | 50 (5 rounds) | **410** (408–414) | 1,805 | 1.89× | 0 | no sink traffic by design; 0 counted drops |
| otelcpp-http, published config, queue 16,384 | 50 (5 rounds) | **776** (752–788) | 1,848 | 3.58× | 0 | 500,000 / 500,000 (100%) at the sink |

With no drops on either side, the gap is 560 ns, and the SpanData recordable closes 366 ns of it (65%). The larger queue barely moves the p50 (793 → 776 ns) but **cuts opentelemetry-cpp's p95 by a third (2,786 → 1,848 ns)**. In the default configuration, the drop path's per-span log `write()` makes up much of the tail. The published snapshot uses that default configuration, so its opentelemetry-cpp p95 includes the cost.

Data: [`results/2026-11-02/queue16k/`](results/2026-11-02/queue16k/).

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

### Whole-process CPU per span

This is the SUT process's `utime` and `stime` (`/proc/1/stat` in the container), covering all threads, including exited ones. They were read before and after the profiled cycles (10 ms tick resolution) and divided by the spans emitted in between. The window covers the runs and their flushes. It includes the profiling harness in both SUTs: the emit loop with its two `steady_clock` reads and histogram per span, the control socket, and the flush. The sink runs in a separate container and is not included. These runs used the default queues, so opentelemetry-cpp dropped about 2% of its spans; they are counted as processed, because the calling thread did the full work for them.

| SUT | Spans | User, ns/span | Kernel, ns/span | Total, ns/span | Wall, ns/span |
|---|---:|---:|---:|---:|---:|
| microtel-http | 30,000,000 | 568 | 212 | **780** | 731 |
| otelcpp-http | 20,000,000 | 1,200 | 220 | **1,420** | 1,065 |
| ratio | | 2.1× | 1.0× | **1.82×** | 1.46× |

opentelemetry-cpp uses about 1.8× the CPU per span across all threads. The ratio is smaller than the 3.6× calling-thread ratio because the harness adds the same fixed cost to both, and because microtel spends more of its total in the kernel, likely the network writes of its exporter's transport. Total CPU exceeds wall time because the export threads run in parallel with the calling thread.

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

### `OtlpRecordable::SetIdentity`, field by field

The code is the same in all three versions checked. `trace_id` and `span_id` are always set, `parent_span_id` only when the parent is valid, and **`trace_state` is set unconditionally, including when the trace state is empty**:
- v1.19.0: `exporters/otlp/src/otlp_recordable.cc:43-56`, `set_trace_state` at `:55`;
- v1.30.0 (`c7b64ec`) and `main` at `0a5bb0b` (2026-11-01): `:42-55`, `set_trace_state` at `:54`.

Measured with [`setidentity_allocs.cpp`](setidentity_allocs.cpp), which interposes `malloc`. It was built in the same image's build stage, against the same opentelemetry-cpp v1.19.0 and Ubuntu protobuf 3.21.12. Two runs gave identical counts ([`result.txt`](results/2026-11-02/setidentity/result.txt)).

| Call | Allocations |
|---|---:|
| `SetIdentity`, root span (no parent) | **5** |
| `SetIdentity`, child span | **6** |
| `set_trace_id` (16 bytes) | 3 |
| `set_span_id` (8 bytes) | 1 |
| `set_parent_span_id` (8 bytes) | 1 |
| `TraceState::ToHeader()` on an empty trace state | 0 |
| `set_trace_state("")` | 1 |

Where the allocations come from:
- **Every string field's first set** allocates a heap `std::string` object, because the recordable's message has no arena.
- **`trace_id`** also needs a heap buffer, since 16 bytes is longer than libstdc++'s 15-byte inline limit, and it takes a third allocation. The generated `set_trace_id(const char*, size_t)` (`trace.pb.h:2536-2540`) calls `ArenaStringPtr::SetBytes(ConstStringParam, Arena*)` (protobuf 3.21.12 `arenastring.h:416-418`), which copies the value.
- **`trace_state`** costs one allocation even when empty, for the string object.

**A small upstream fix exists, in two parts, measured in the same test:**
- Skip `set_trace_state` when `span_context.trace_state()->Empty()`: 1 → 0 allocations (`otlp_recordable.cc:55` in v1.19.0, `:54` in v1.30.0 and main).
- Fill `trace_id` in place with `span_.mutable_trace_id()->assign(data, 16)`: 3 → 2 allocations (`:46-47` in v1.19.0, `:45-46` in v1.30.0 and main). `span_id` and `parent_span_id` fit the inline buffer and are already 1 each.

Together they take a root span from 5 to 3 allocations and a child span from 6 to 4, which is 2 of the 14.25 per span measured above. Both changes are plain C++ that compiles under C++14.

No upstream PR making these changes was found; open PR #4580 rewrites the same lines for an arena, and with it the changes save nothing ([section 4](#4-opentelemetry-cpp-main-and-pr-4580)). The closest issue is open-telemetry/opentelemetry-cpp#4558 (open, `triage/accepted`, 2026-09-11), *"OtlpRecordable: about 5 heap allocations per attribute, then a deep copy at export"*. It covers the per-attribute cost on `main` and asks whether to arena-back the recordable or adopt its message into the request. An arena-backed recordable would remove these per-field allocations too. This table is v1.19.0 with protobuf 3.21.12; on main with protobuf 35.1 the counts differ (section 4).

## 4. opentelemetry-cpp main and PR #4580

A follow-up on 2026-11-02 measured the `SetIdentity` changes against current opentelemetry-cpp, together with open-telemetry/opentelemetry-cpp#4580. That PR, open with changes requested, allocates the recordable's message on an arena owned by the recordable, with a 512-byte block stored inside it (option A of #4558). microtel is not involved. Full data: [`results/2026-11-02/otelcpp-main/`](results/2026-11-02/otelcpp-main/).

| Variant | Commit | What |
|---|---|---|
| a | `0a5bb0b` | `main` |
| b | `53cd5bb` | a + both `SetIdentity` changes |
| c | `5e5b283` | a merged with the #4580 head `5aa9922` (conflicts with #4734 resolved) |
| d | `3012c13` | c + both `SetIdentity` changes |

**Build.** Upstream's own dev container (`.devcontainer/Dockerfile.dev` at `0a5bb0b`), so the dependencies are the versions in its `third_party_release`:
- clang 22.1.8;
- protobuf 35.1.0 and abseil 20250512.1, both static.

The flags are the same as the other SUTs. Each variant was built twice, and the two builds are identical in all 74 compared sections.

**Method.** Same harness and counter as steps 1 and 3, with the queue at 16,384 for both, so nothing drops. Allocation counts: two runs each, identical.

| Variant | `SetIdentity` allocations, root / child | Calling-thread allocations per span, 0 attributes | 5 attributes | p50 ÷ a | p95 ÷ a |
|---|---:|---:|---:|---|---|
| a | 4 / 5 | 9 | 36 | 1 | 1 |
| b | 3 / 4 | 8 | 35 | 0.82 (0.78–0.94) | 0.94 |
| c | 1 / 1 | **5** | **11** | **0.74 (0.68–0.88)** | **0.66 (0.64–0.80)** |
| d | 1 / 1 | 5 | 11 | 0.76 (0.72–0.90) | 0.66 |

Each ratio is the median of the per-round ratios against a in the same round, with the range in brackets. c's ratios come from a 10-round rerun of a and c alone on 2026-11-03, with 100 samples each. b's and d's come from the first 5-round run of all four variants. In that run c was 0.76 (0.74–0.98) for p50 and 0.71 for p95. In the rerun, a's p50 was 601 ns and c's 444 ns.

- **#4580 reproduces its author's 9 → 5 allocations for a minimal span** on this harness. With 5 attributes the count goes from 36 to 11.
- **The `SetIdentity` changes save nothing measurable on top of #4580.** Allocations are identical, and d ÷ c is 0.98 for p50 (range 0.92–1.01).
- **On protobuf 35.1 without #4580, only the `Empty()` check saves anything.** `mutable_trace_id()->assign` costs the same 2 allocations as `set_trace_id`. So the 5 → 3 measured on v1.19.0 with protobuf 3.21.12 becomes 4 → 3 here.
- **Wire output is unchanged.** The serialized `Span` bytes after `SetIdentity` are identical in a, b, c and d, for root and child spans, with empty and non-empty trace state.
- **Caveat on b's p50.** a's per-sample p50s were unusually spread out (424–708 ns), and its fastest samples match the other variants. b's 0.82 is more than one allocation should cost, and it was not profiled.
- **Not measured:** memory per recordable (#4580 reports 216 → 752 bytes) and export-thread CPU.

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

## What could change in opentelemetry-cpp under C++14

In order of size, smallest first:

1. **Two `SetIdentity` changes**: skip an empty `trace_state`, and fill `trace_id` in place. That takes a root span from 5 to 3 allocations on v1.19.0, and from 4 to 3 on main ([details above](#otlprecordablesetidentity-field-by-field)). With #4580 applied, it saves nothing ([section 4](#4-opentelemetry-cpp-main-and-pr-4580)).
2. **Fewer allocations in the rest of `OtlpRecordable`.** That means an arena-backed recordable message, or adopting the message into the export request instead of deep-copying it. This is the subject of upstream open-telemetry/opentelemetry-cpp#4558, and of open PR #4580 (arena): 9 → 5 calling-thread allocations for a minimal span and p50 0.74× on main (10 rounds) ([section 4](#4-opentelemetry-cpp-main-and-pr-4580)). With 5 attributes, the recordable is the biggest part of opentelemetry-cpp's 47.6 allocations per span.
3. **The design change: no protobuf on the calling thread.** Record into a plain, SpanData-style recordable, which opentelemetry-cpp already has, and build the protobuf message on the export thread when a batch is serialized. Here, swapping the recordable alone removed 65% of the span-time gap, an upper bound (step 1b). It needs a recordable-to-protobuf conversion at export time inside the OTLP exporter, and no new dependency.

All three are ordinary C++14 inside opentelemetry-cpp's SDK and OTLP exporter. They need no C++17 or C++20 features and nothing from microtel.

Porting microtel's own upb + nghttp2 exporter is a separate and much larger option; see the appendix.

## Not measured

- **The rest of part 1:** the GCC 13 and hardening variants, opentelemetry-cpp v1.30.0, and both libraries over OTLP/gRPC. Skipped by request. For the gRPC pair, the published snapshot already shows GCC-and-hardened (812 ns) and Clang-unhardened (813 ns) opentelemetry-cpp at the same p50.
- **Part 5:** the footprint split between gRPC and protobuf.
- **Kernel time on the calling thread** (`perf_event_paranoid=2`).
- **Virtual dispatch.** An indirect call's cost lands in its caller, so sampling cannot separate it.
- **Which call sites make opentelemetry-cpp's 14 allocations.** The counter is per thread, not per caller.
- **opentelemetry-cpp's drop count in steps 1–3.** The SDK exposes none. Steps 1–3 ran before the bench counted drops, so their only signal is the sink's delivery rate (95–99%). The queue rerun counts drops through the SDK's log, a counter that was checked against the sink.

## Reproducing

```bash
# Queue rerun (1b): same images, larger opentelemetry-cpp queue
bench/parity/run_rounds.sh <out> 5 parity-microtel-http-clang parity-otelcpp-http-clang-q16k parity-otelcpp-http-clang-inmem-q16k
# SetIdentity allocations: compile in the otelcpp image's otelcpp-build stage, e.g.
#   clang++-18 -std=c++17 -O2 -I/opt/otelcpp/include setidentity_allocs.cpp -o setid \
#     -Wl,--start-group /opt/otelcpp/lib/*.a -Wl,--end-group \
#     /build/otelcpp/libopentelemetry_proto.so -Wl,-rpath,/build/otelcpp -lprotobuf -lpthread

PARITY_DOUBLE=1 bench/parity/build.sh <log-dir> microtel-http-clang otelcpp-http-clang otelcpp-http-clang-inmem
bench/parity/run_rounds.sh <out> 5 parity-microtel-http-clang parity-otelcpp-http-clang parity-otelcpp-http-clang-inmem
python3 -I bench/parity/merge.py <out> parity-microtel-http-clang
python3 -I bench/parity/perf_profile.py bench-sut-parity-<variant>:perf <cycles> <dir> && python3 -I bench/parity/perf_buckets.py <dir>
cc -O2 -fPIC -shared -o malloc_count.so bench/parity/malloc_count.c   # in ubuntu:24.04
python3 -I bench/parity/count_mallocs.py bench-sut-parity-<variant> malloc_count.so <attrs> <out.json>
# Section 4: build upstream's dev image, then each variant commit twice inside it
podman build -f .devcontainer/Dockerfile.dev -t otelcpp-dev:0a5bb0b <opentelemetry-cpp clone>
bench/parity/build_otelcpp_dev.sh <log-dir> localhost/otelcpp-dev:0a5bb0b <clone> a=<commit> b=<commit> c=<commit> d=<commit>
bench/parity/run_rounds.sh <out> 5 parity-otelcpp-main-a parity-otelcpp-main-b parity-otelcpp-main-c parity-otelcpp-main-d
python3 -I bench/parity/count_mallocs.py bench-sut-parity-otelcpp-main-<v> malloc_count.so <attrs> <out.json> --env EMIT_OTELCPP_BSP_MAX_QUEUE_SIZE=16384
podman run --rm --entrypoint /setidentity_allocs bench-sut-parity-otelcpp-main-<v> --dump
```

## Appendix: porting microtel's exporter to C++14

This is the alternative to changing opentelemetry-cpp's own exporter: bringing microtel's upb + nghttp2 transport, wire and exporter code to opentelemetry-cpp's C++14 baseline.

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
- **Size:** about 1,200 edit sites across about 49 files and 8,700 code lines. Mechanical edits: 1–2 weeks. Encoder rewrite against SpanData, plus integration with opentelemetry-cpp's exporter interface and tests: 2–4 weeks. In total, roughly 3–6 weeks (low confidence: nothing was compiled at C++14).
- **New dependencies:** nghttp2 and upb would be new for opentelemetry-cpp. Whether upb's headers compile in a C++14 translation unit is **unverified**.
