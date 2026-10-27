# Committed benchmark snapshot

These files are one benchmark snapshot, committed so the numbers in the
root [`README.md`](../../README.md) can be checked from a clone. The build
doesn't produce them. A local `cd bench && ./bench.sh` writes to
`bench/results/`, which is gitignored, so the README used to link a path that
only existed for people who had run the harness themselves.

The snapshot was refreshed on 2026-10-27 for the v1.2.4 tag, from the
release commit `5bbc655` (`source_commit` in `results.json`). The images were
built from `84ac4c0` (`images_built_from`), the same commit before #462 was
rebased onto master; the rebase changed only docs and packaging metadata. It ran
on the same machine as the previous snapshot (Ryzen 5 5600G, 6 cores / 12
threads, `performance` governor, SMT on). The host's software did change: the
kernel went from 7.1.13-200.fc44 to 7.2.11-200.fc44 and podman from 5.8.4 to
5.8.9. Diffing the environment block, as [`docs/releasing.md`](../releasing.md)
§5 requires, showed those two fields, so the old snapshot's numbers are not a
like-for-like baseline. [Changes since the previous snapshot](#changes-since-the-previous-snapshot)
compares old and new code on today's host instead.

**The snapshot merges five interleaved runs.** Each run is the full
`hot-loop-traces` profile (four SUTs, 10 samples each). Each run of the v1.2.4
images was preceded by a run of the previous snapshot's 2026-09-27 images,
alternating old, new, old, new… five times, so host drift falls on both alike.
`results.json` holds the 50 samples per SUT from the five v1.2.4 runs, with
each run's start time, load and sample counts in `interleaved_runs`.
`results.md` and `plots.html` are rendered from it by the driver's own
writers. otelcpp-http has 40 samples, not 50: in run 4 its container didn't
report ready within the driver's 60 s and was skipped. Run 4 also started at
host load 0.68, above the driver's 0.5 warning threshold, and the warning is
kept.

## Summary

This is the table the root README's performance claims are read off.
Hot-loop traces, 10 000 spans per sample, 50 samples per SUT (40 for
otelcpp-http) from five interleaved runs, each SDK exporting to a local
blackhole sink over a container network, one host (Podman; AMD Ryzen 5 5600G,
`performance` governor, SMT on). Medians:

| Metric | microtel (HTTP) | microtel (gRPC) | otelcpp (gRPC) | otelcpp (HTTP) |
|---|---|---|---|---|
| Span emit p50 | **226 ns** | **220 ns** | 820 ns | 807 ns |
| Span emit p95 | **486 ns** | **471 ns** | 3 364 ns | 2 882 ns |
| Spans / sec | **1 386 999** | 1 013 479 | 739 338 | 807 350 |
| Flush p50 | 3.2 ms | 6.1 ms | 2.0 ms | 1.9 ms |
| Delivery rate | **100%** | **100%** | 94.2% | 96.3% |
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

**The span-emit ratio fell from 3.8× to 3.6×, and the change is the host's,
not the code's.** Span emit p50 is 226 ns for microtel against 820 ns for
otelcpp-gRPC, 3.63×; the previous snapshot measured 3.83×. Over the same five
interleaved rounds the previous snapshot's 2026-09-27 images measured 3.59×
on today's host (p50 227 ns against 816 ns). Old and new code are within 1.5%
of each other at p50 for every SUT, and within 1% at p95 for all but
otelcpp-http (−4.4%, an SUT whose code didn't change). The ratio moved with
the kernel and podman updates above.

**Throughput.** On today's host, old images against new, medians of 50
samples each: microtel 1,512,908 → 1,386,999 spans/sec (−8.3%), microtel-grpc
1,289,564 → 1,013,479 (−21.4%), otelcpp-grpc −1.0% and otelcpp-http +0.6%. The
emit loop takes the same time; the difference is all in the flush, which
spans/sec includes. microtel's spans now carry the OTLP `flags` field (#452),
as opentelemetry-cpp's already did, so wire bytes per span went from 62.2 to
68.2, and a 512-span batch from 31.8 KB to 34.9 KB per request. That crosses
32 KiB, and the bench sink's handling time steps up by ~80 µs for requests
above 32 KiB; cause under investigation
([#460](https://github.com/microtel-org/microtel/issues/460)). A pre-#452
build shows the same step once its batches pass 32 KiB. Over OTLP/HTTP there
is no step, and the flush grows with the extra bytes.

**The benchmark binary size row is withdrawn.** It compared the benchmark
executables' file sizes, which were over 92% debug info and left out the
shared libraries each executable loads. [`sizes/library-sizes.md`](sizes/library-sizes.md) replaces it,
and #445 tracks the driver fix. The leaf's footprint is in
[`leaf-footprint.md`](leaf-footprint.md).

`realistic-request` and `compression` were not rerun for v1.2.4. Their last
figures, from 2026-09-27: `realistic-request` 100% delivery for both SUTs, and
`compression` 342.2 B/span uncompressed against 39.1 gzip'd (8.7×).

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
| Profile | `hot-loop-traces` — 10 000 spans/sample, 10 samples per run × 5 interleaved runs, 1 000 warmup spans, blackhole sink |
| SUTs | `microtel`, `microtel-grpc`, `otelcpp-grpc`, `otelcpp-http` |
| Generated | 2026-10-27 (release commit `5bbc655`; images built from `84ac4c0`, its pre-rebase SHA) |
| Host | AMD Ryzen 5 5600G, 6 physical cores / 12 threads, Fedora, kernel 7.2.11-200.fc44, podman 5.8.9 |

The snapshot carries two warnings, left in `results.md` and `results.json`:
SMT was enabled, which widens the spread, and run 4 started at load 0.68.
`results.md`'s profile table shows the profile's 10 samples per run; the
summary rows are over all five runs.

## Refreshing it

```bash
cd bench && ./bench.sh                     # writes bench/results/
cp bench/results/{plots.html,results.json,results.md} docs/bench-results/
```

Then update the [Summary](#summary) table above, and the ratio claims in the
root `README.md`, so they agree with the new run.

For v1.2.4 the five runs were made with `./bench.sh --no-build --out <dir>`,
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
