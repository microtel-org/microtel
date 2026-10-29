# Committed benchmark snapshot

These files are one benchmark snapshot, committed so the numbers in the
root [`README.md`](../../README.md) can be checked from a clone. The build
doesn't produce them. A local `cd bench && ./bench.sh` writes to
`bench/results/`, which is gitignored, so the README used to link a path that
only existed for people who had run the harness themselves.

The snapshot was refreshed on 2026-10-28 for the v1.2.5 tag, `16b39fb`
(`source_commit` and `images_built_from` in `results.json`). It ran on the
same machine as the v1.2.4 snapshot (Ryzen 5 5600G, 6 cores / 12 threads,
`performance` governor, SMT on), and the environment block matches it field
for field: the same kernel, 7.2.11-200.fc44, and the same podman, 5.8.9, as
[`docs/releasing.md`](../releasing.md) §5 requires. The two snapshots are a
like-for-like comparison.

**The snapshot merges five interleaved runs.** Each run is the full
`hot-loop-traces` profile (four SUTs, 10 samples each). Each run of the v1.2.5
images was preceded by a run of the v1.2.4 images, alternating old, new, old,
new… five times, so host drift falls on both alike. Only the microtel images
were rebuilt; both sets use the v1.2.4 otelcpp images, whose code did not
change. `results.json` holds the 50 samples per SUT from the five v1.2.5 runs,
with each run's start time, load and sample counts in `interleaved_runs`.
`results.md` and `plots.html` are rendered from it by the driver's own
writers. Every run started below host load 0.5.

Three rounds were run again in full, both halves, because an SUT container
logged nothing and didn't report ready within the driver's 60 s. That
happened in five of 16 runs, to a different SUT each time, otelcpp ones
included, so it's the harness, not microtel
([#499](https://github.com/microtel-org/microtel/issues/499)). Every kept run
has all four SUTs.

## Summary

This is the table the root README's performance claims are read off.
Hot-loop traces, 10 000 spans per sample, 50 samples per SUT from five
interleaved runs, each SDK exporting to a local
blackhole sink over a container network, one host (Podman; AMD Ryzen 5 5600G,
`performance` governor, SMT on). Medians:

| Metric | microtel (HTTP) | microtel (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| Span emit p50 | **223 ns** | **220 ns** | 812 ns | 813 ns |
| Span emit p95 | **485 ns** | **472 ns** | 3 394 ns | 3 222 ns |
| Spans / sec | **1 424 706** | 1 024 210 | 738 912 | 771 072 |
| Flush p50 | 3.0 ms | 6.1 ms | 2.0 ms | 2.0 ms |
| Delivery rate | **100%** | **100%** | 93.7% | 94.8% |
| Wire bytes / span | 68.2 | 68.2 | 68.1 | 68.1 |

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
**13.4 MB** for opentelemetry-cpp with its OTLP/gRPC exporter (gRPC, protobuf,
Abseil, c-ares, re2 and the OTLP proto libraries) against **1.2 MB** for
microtel (with nghttp2, upb and toml++), on top of the same **12.0 MB** shared
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

**No change: the span-emit ratio is 3.6×.** Span emit p50 is 223 ns for
microtel against 812 ns for otelcpp-gRPC, 3.64×; the v1.2.4 snapshot measured
3.63×. In the same five interleaved rounds the v1.2.4 images measured 3.69×
(p50 222 ns against 820 ns). Medians of 50 samples, v1.2.4 code against
v1.2.5:

| SUT | p50 | p95 | Spans / sec | Flush p50 |
|---|---|---|---|---|
| microtel (HTTP) | 222 → 223 ns (+0.5%) | 482 → 485 ns (+0.5%) | +1.8% | −6.7% |
| microtel (gRPC) | 220 → 220 ns (0%) | 467 → 472 ns (+1.1%) | +2.6% | −4.4% |
| otelcpp (gRPC) | 820 → 812 ns (−1.0%) | 3 361 → 3 394 ns (+1.0%) | −2.1% | −1.1% |
| otelcpp (HTTP) | 792 → 813 ns (+2.6%) | 2 561 → 3 222 ns (+25.8%) | −9.2% | +5.3% |

The otelcpp rows are the noise floor: both sides ran the same image. Every
microtel change is well inside it. v1.2.5's exporter worker counts each batch
for flush tracking (#484), off the application thread; it doesn't show up in
emit time or in flush time.

**The benchmark binary size row is withdrawn.** It compared the benchmark
executables' file sizes, which were over 92% debug info and left out the
shared libraries each executable loads. [`sizes/library-sizes.md`](sizes/library-sizes.md) replaces it,
and #445 tracks the driver fix. The leaf's footprint is in
[`leaf-footprint.md`](leaf-footprint.md).

`realistic-request` and `compression` were not rerun for v1.2.4 or v1.2.5. Their last
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
| Profile | `hot-loop-traces` — 10 000 spans/sample, 10 samples per run × 5 interleaved runs, 1 000 warmup spans, blackhole sink |
| SUTs | `microtel`, `microtel-grpc`, `otelcpp-grpc`, `otelcpp-http` |
| Generated | 2026-10-28 (v1.2.5 tag, `16b39fb`; microtel images built from it, otelcpp images from v1.2.4) |
| Host | AMD Ryzen 5 5600G, 6 physical cores / 12 threads, Fedora, kernel 7.2.11-200.fc44, podman 5.8.9 |

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
retagging the `bench-sut-*` images between the old and new sets, and merged
by concatenating each SUT's samples in run order and rendering with
`driver.report.build_results`, `write_json`, `write_markdown` and
`driver.plots.write_plots`.

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
