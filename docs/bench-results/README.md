# Committed benchmark snapshot

These files are one benchmark run, committed verbatim so the numbers in the
root [`README.md`](../../README.md) can be checked from a clone. The build
doesn't produce them. A local `cd bench && ./bench.sh` writes to
`bench/results/`, which is gitignored, so the README used to link a path that
only existed for people who had run the harness themselves.

The snapshot was refreshed on 2026-09-27 for the v1.2.0 tag, from a clean
checkout of the release candidate (`6c2fe10`, which the release commit changes
only in version literals and docs), on the same host as the previous snapshot
(Ryzen 5 5600G, 12 cores, `performance` governor, SMT on, podman 5.8.4, kernel
7.1.13-200.fc44). Host load was 0.15 at the start. Before committing, the
environment block was diffed against the previous snapshot as
[`docs/releasing.md`](../releasing.md) §5 requires, and every identity field
matched, governor included, so the deltas below are like for like.

## Summary

This is the table the root README's performance claims are read off.
Hot-loop traces, 10 000 spans per sample × 10 samples, each SDK exporting to
a local blackhole sink over a container network, one host (Podman; AMD Ryzen 5
5600G, `performance` governor, SMT on).

| Metric | microtel (HTTP) | microtel (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| Span emit p50 | **217 ns** | **218 ns** | 832 ns | 804 ns |
| Span emit p95 | **478 ns** | **474 ns** | 3 246 ns | 2 738 ns |
| Spans / sec | **1 540 963** | 1 232 937 | 715 498 | 810 803 |
| Flush p50 | 2.7 ms | 4.3 ms | 2.0 ms | 1.9 ms |
| Delivery rate | **100%** | **100%** | 95.2% | 97.0% |
| Wire bytes / span | **62.2** | **62.2** | 68.1 | 68.1 |

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
Abseil, c-ares, re2 and the OTLP proto libraries) against **1.1 MB** for
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

Throughput is within this host's spread: microtel +2.6% (1,540,963 spans/sec),
microtel-grpc −5.1%, and the unchanged otelcpp SUTs −5.6% (gRPC) and −4.2%
(HTTP). Span emit p50 is 217 ns for microtel against 832 ns for otelcpp-gRPC,
a 3.8× ratio.

**The benchmark binary size row is withdrawn.** It compared the benchmark
executables' file sizes, which were over 92% debug info and left out the
shared libraries each executable loads. [`sizes/library-sizes.md`](sizes/library-sizes.md) replaces it,
and #445 tracks the driver fix. The leaf's footprint is in
[`leaf-footprint.md`](leaf-footprint.md).

The other two profiles from the same session: `realistic-request` shows 100%
delivery for both SUTs, and `compression` shows 342.2 B/span uncompressed
against 39.1 gzip'd (8.7×), both unchanged.

## Files

| File | What it is |
|---|---|
| [`results.md`](results.md) | Generated summary: environment, profile, warnings, results table. Start here. |
| [`results.json`](results.json) | Raw per-sample data, schema version 1.0. What the plots and the summary are rendered from. |
| [`plots.html`](plots.html) | Interactive charts. Loads plotly from a CDN, so it needs network to render. |
| [`leaf-footprint.md`](leaf-footprint.md) | Not part of the benchmark run: the experimental leaf's flash and RAM for both encoder backends, as the `leaf-footprint` CI job measures them. Refreshed per release. |

## This snapshot

| Field | Value |
|---|---|
| Profile | `hot-loop-traces` — 10 000 spans/sample, 10 samples, 1 000 warmup spans, blackhole sink |
| SUTs | `microtel`, `microtel-grpc`, `otelcpp-grpc`, `otelcpp-http` |
| Generated | 2026-09-27 (release candidate `6c2fe10`) |
| Host | AMD Ryzen 5 5600G, 12 physical cores, Fedora, podman 5.8.4 |

The run carries one warning, left in `results.md` and `results.json`: SMT
was enabled. It widens the spread but doesn't change the order-of-magnitude
comparisons in the README. The governor warning the previous snapshot carried
is gone because this run used `performance`.

Two other profiles, `realistic-request` and `compression`, were run on the same
host in the same session. They aren't committed here, since this directory
holds only the profile the root README quotes, but the delivery and
compression figures above come from them. Each profile started with host
load below 0.5 and the governor at `performance`, checked before and after.

## Refreshing it

```bash
cd bench && ./bench.sh                     # writes bench/results/
cp bench/results/{plots.html,results.json,results.md} docs/bench-results/
```

Then update the [Summary](#summary) table above, and the ratio claims in the
root `README.md`, so they agree with the new run.

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
