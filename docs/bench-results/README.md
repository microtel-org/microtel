# Committed benchmark snapshot

These files are one benchmark snapshot, committed so the numbers in the
root [`README.md`](../../README.md) can be checked from a clone. The build
doesn't produce them. A local `cd bench && ./bench.sh` writes to
`bench/results/`, which is gitignored, so the README used to link a path that
only existed for people who had run the harness themselves.

The snapshot was measured on 2026-11-22: microtel **v1.2.6** against
opentelemetry-cpp **v1.30.0**. All four images were built from the v1.2.6 tag,
`59e5415` (`source_commit` and `images_built_from` in `results.json`), with
the opentelemetry-cpp SUTs moved from v1.19.0 to v1.30.0 (`OTELCPP_VERSION`
in `bench/sut/otelcpp-*/Dockerfile`; v1.30.0 renamed its CMake options and
package components, which `bench/emit-app/CMakeLists.txt` follows). Each image
was built twice, the second time with `--no-cache`, and the two builds were
identical section by section.

**This is a new baseline, not a like-for-like refresh.** It ran on the same
Ryzen 5 5600G as the v1.2.5 snapshot, but
[`docs/releasing.md`](../releasing.md) §5's same-environment rule does not
hold, for three reasons:
- **Memory:** a faulty DIMM was found and replaced after the v1.2.5 snapshot.
  The new DIMM passed memtest86+, and the host passed a 30-minute
  `stress-ng --vm --verify` screen before these builds.
- **Kernel:** 7.2.9-201.fc44, against 7.2.11-200.fc44. podman is the same, 5.8.9.
- **CPU frequency boost:** off (`/sys/devices/system/cpu/cpufreq/boost` = 0),
  with the `performance` governor. The environment block records the governor
  but not boost, so the v1.2.5 snapshot's boost state is unknown.

**The snapshot merges five runs.** Each run is the full `hot-loop-traces`
profile: four SUTs, 10 samples each. `results.json` holds the 50 samples per
SUT, with each run's start time, load and sample counts in `interleaved_runs`.
`results.md` and `plots.html` are rendered from it by the driver's own
writers. Every run started below host load 0.5.

**Only runs with all four SUTs are kept.** In 9 of the 14 runs started, an SUT
container logged nothing and didn't report ready within the driver's 60 s
([#499](https://github.com/microtel-org/microtel/issues/499)). It hit
microtel (HTTP) five times and otelcpp-http six times, and two runs lost both.
The five complete runs are kept whole.

## Summary

This is the table the root README's performance claims are read off.
Hot-loop traces, 10 000 spans per sample, 50 samples per SUT from five
runs, each SDK exporting to a local
blackhole sink over a container network, one host (Podman; AMD Ryzen 5 5600G,
`performance` governor, SMT on). Medians:

| Metric | microtel (HTTP) | microtel (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| Span emit p50 | **215 ns** | **210 ns** | 588 ns | 634 ns |
| Span emit p95 | **474 ns** | **447 ns** | 3 380 ns | 3 356 ns |
| Spans / sec | **1 439 519** | 1 038 099 | 688 475 | 718 739 |
| Flush p50 | 3.1 ms | 6.0 ms | 2.1 ms | 2.2 ms |
| Delivery rate | **100%** | **100%** | 88.1% | 87.7% |
| Wire bytes / span | 68.2 | 68.2 | 68.4 | 68.3 |

opentelemetry-cpp takes **about 3×** microtel's span emit time: 2.95× over
OTLP/HTTP (634 against 215 ns) and 2.80× over OTLP/gRPC (588 against 210 ns).
Per run, the HTTP ratio ranged 2.80–3.13× and the gRPC ratio 2.64–3.03×.

**Delivery rate** is what the sink received. opentelemetry-cpp runs with its
default `BatchSpanProcessor` queue of 2,048 spans, which overflows during a
10,000-span sample, and it drops the overflow. The harness has no drop counter
for opentelemetry-cpp, so `results.md`'s "Spans dropped" row reads 0 for it.

**Span emit** times one whole `EmitSpan()` call of the benchmark app
([`control_socket.cpp`](../../bench/emit-app/src/control_socket.cpp), the
`WorkerThread` loop). That is `StartSpan` and `End()` plus the harness's own
overhead: a virtual call, a relaxed counter increment and two
`steady_clock` reads. This profile sets no attributes. It is not the latency
of `StartSpan` alone, although the harness and [results.md](results.md) label
the row "StartSpan".

**Size** is measured separately, in [`sizes/library-sizes.md`](sizes/library-sizes.md):
the stripped benchmark executable plus every shared library it loads, Ubuntu
24.04 packages, split per library with bloaty. The telemetry stack is
**13.5 MB** for opentelemetry-cpp v1.30.0 with its OTLP/gRPC exporter (gRPC, protobuf,
Abseil, c-ares, re2 and the OTLP proto libraries) against **1.3 MB** for
microtel (measured on v1.2.6) (with nghttp2, upb and toml++), on top of the same **12.0 MB** shared
base of OpenSSL, zlib and the C/C++ runtime that both load.

The methodology is in [docs/bench-spec.md](../bench-spec.md), and
[plots.html](plots.html) has interactive plots and the raw
per-sample data. To reproduce with Docker or Podman:

```bash
cd bench && ./bench.sh              # hot-loop-traces profile
./bench.sh --profile hot-loop-metrics
./bench.sh --profile hot-loop-logs
./bench.sh --flamegraph             # adds per-SUT SVG flame graphs
```

The leaf's flash, RAM and stack on Cortex-M0+, Cortex-M4 and aarch64 are in
[leaf-footprint.md](leaf-footprint.md).

## Changes since the previous snapshot

**The span-emit ratio fell from 3.6× to about 3×.** The v1.2.5 snapshot
measured 223 ns for microtel against 813 ns for opentelemetry-cpp v1.19.0 over
OTLP/HTTP (3.6×), and 220 against 812 ns over OTLP/gRPC (3.7×). Most of the
change is opentelemetry-cpp:

- **opentelemetry-cpp v1.30.0 is faster than v1.19.0 on this path.** In
  separate interleaved rounds on this host the same day (OTLP/HTTP, both built
  with clang 18.1.3 against Ubuntu's protobuf 3.21.12, queues large enough not
  to drop), v1.30.0 took 0.81× v1.19.0's span time (per round 0.76–0.89×).
- **microtel moved little.** v1.2.6 leaves the span hot path of v1.2.5
  unchanged; its p50 went from 223 to 215 ns over HTTP and 220 to 210 ns over
  gRPC, which is the host change.
- **opentelemetry-cpp v1.30.0 drops more at its default queue:** 88% delivery,
  against 94–95% for v1.19.0.

Because the host changed too, no row here is a like-for-like delta against the
v1.2.5 snapshot.

**The benchmark binary size row is withdrawn.** It compared the benchmark
executables' file sizes, which were over 92% debug info and left out the
shared libraries each executable loads. [`sizes/library-sizes.md`](sizes/library-sizes.md) replaces it,
and #445 tracks the driver fix. The leaf's footprint is in
[`leaf-footprint.md`](leaf-footprint.md).

`realistic-request` and `compression` were not rerun for v1.2.4, v1.2.5 or this snapshot. Their last
figures, from 2026-09-27: `realistic-request` 100% delivery for both SUTs, and
`compression` 342.2 B/span uncompressed against 39.1 gzip'd (8.7×).

## Files

| File | What it is |
|---|---|
| [`results.md`](results.md) | Generated summary: environment, profile, warnings, results table. Start here. |
| [`results.json`](results.json) | Raw per-sample data, schema version 1.0. What the plots and the summary are rendered from. |
| [`plots.html`](plots.html) | Interactive charts. Loads plotly from a CDN, so it needs network to render. |
| [`leaf-footprint.md`](leaf-footprint.md) | Not part of the benchmark run: the experimental leaf's flash and RAM for both encoder backends, as the `leaf-footprint` CI job measures them. Refreshed for a release when `leaf/` changed. |

## This snapshot

| Field | Value |
|---|---|
| Profile | `hot-loop-traces` — 10 000 spans/sample, 10 samples per run × 5 runs, 1 000 warmup spans, blackhole sink |
| SUTs | `microtel`, `microtel-grpc`, `otelcpp-grpc`, `otelcpp-http` |
| Generated | 2026-11-22 (v1.2.6 tag, `59e5415`, with the opentelemetry-cpp SUTs at v1.30.0; all images built from it) |
| Versions | microtel v1.2.6; opentelemetry-cpp v1.30.0, built with clang 18.1.3 for the HTTP SUT and GCC 13.3 for the gRPC SUT (emit-app: clang 18.1.3 in all four) |
| Host | AMD Ryzen 5 5600G, 6 physical cores / 12 threads, Fedora, kernel 7.2.9-201.fc44, podman 5.8.9, `performance` governor, boost off |

The snapshot carries one warning, left in `results.md` and `results.json`:
SMT was enabled, which widens the spread.
`results.md`'s profile table shows the profile's 10 samples per run; the
summary rows are over all five runs.

## Refreshing it

```bash
cd bench && ./bench.sh                     # writes bench/results/
cp bench/results/{plots.html,results.json,results.md} docs/bench-results/
```

Then update the [Summary](#summary) table above, and the ratio claims in the
root `README.md`, so they agree with the new run.

For v1.2.4 and v1.2.5 the five runs were made with `./bench.sh --no-build --out <dir>`,
retagging the `bench-sut-*` images between the old and new sets. For this
snapshot they were made the same way from the v1.2.6 tree, all with one image
set. Each time the runs were merged by concatenating each SUT's samples in run
order and rendering with `driver.report.build_results`, `write_json`,
`write_markdown` and `driver.plots.write_plots`.

**Diff the `environment` block against the outgoing snapshot before you
commit** ([`docs/releasing.md`](../releasing.md) §5). A run from a different
machine makes every delta unreadable. The v1.1.0 attempt from CI's 4-core
`ubuntu-24.04` runner showed a ~45% throughput drop on SUTs whose code hadn't
changed; an unchanged SUT that moves is the giveaway. Until
[#277](https://github.com/microtel-org/microtel/issues/277) settles on a
reference host, refresh this directory from a local run on the host above,
not from a `benchmark.yml` artifact.

This snapshot is documentation only. The weekly perf-regression check reads a
separate file, `bench/baseline/results.json` (see
[`bench/baseline/README.md`](../../bench/baseline/README.md)).
