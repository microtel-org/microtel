# microtel bench/: Benchmark Harness Spec

**Status:** Draft v0.2
**Companion to:** `microtel` core spec
**License (proposed):** Apache 2.0

---

## 1. Purpose

A benchmark harness that produces **fair, reproducible, environment-portable** comparisons between **microtel** and stock **opentelemetry-cpp** (with both gRPC and HTTP exporters). Users — internal teams, prospective adopters, skeptics — can clone the repo, run a single command, and get a credible report of how the two libraries perform **on their own hardware** for **their own workload shape**.

Two audiences, one harness:
- **microtel CI:** runs on every PR to detect performance regressions.
- **End users:** run on their environment to make an evidence-based adoption decision.

---

## 2. Design Principles

1. **Apples-to-apples.** Same workload code, same compiler flags, same hardware, same collector, same network path. The *only* variable across runs is the OTel library.
2. **Reproducible.** Containerized by default with pinned versions (compiler, OS image, library SHA, collector version). Deterministic workload generation.
3. **Portable.** Runs on a developer laptop, a bare-metal server, or a constrained VM. No cloud-only assumptions, no proprietary dependencies.
4. **Honest.** Reports methodology, environment fingerprint, variance, and confidence intervals. Refuses to print a single "winner" number — always shows the distribution.
5. **Trivial to run.** `./bench.sh` from a clean clone, no manual setup. A single Markdown + JSON report drops into `results/`.
6. **Defensible.** Methodology is documented and reviewable. Anyone should be able to point at a number and trace exactly how it was produced.

---

## 3. Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         Bench Driver (Python)                    │
│  - Parses scenario config                                        │
│  - Spawns SUT and sink containers                                │
│  - Records environment fingerprint                               │
│  - Drives workload via control socket                            │
│  - Collects measurements, computes stats, writes report          │
└──────────────────────────────────────────────────────────────────┘
              │                      │                      │
              ▼                      ▼                      ▼
   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
   │   SUT-A:        │    │   SUT-B:        │    │   SUT-C:        │
   │   microtel      │    │   otel-cpp +    │    │   otel-cpp +    │
   │   (HTTP+pb)     │    │   gRPC exporter │    │   HTTP exporter │
   │                 │    │                 │    │                 │
   │   emit_app      │    │   emit_app      │    │   emit_app      │
   │   (same C++)    │    │   (same C++)    │    │   (same C++)    │
   └────────┬────────┘    └────────┬────────┘    └────────┬────────┘
            │                      │                      │
            └──────────────────────┼──────────────────────┘
                                   │
                                   ▼
                  ┌───────────────────────────────┐
                  │        Sink (selectable)      │
                  │  ┌─────────────────────────┐  │
                  │  │ blackhole-sink (default)│  │  ← minimal, low-variance
                  │  │  HTTP/2 + gRPC, parses, │  │
                  │  │  acks, counts bytes     │  │
                  │  └─────────────────────────┘  │
                  │  ┌─────────────────────────┐  │
                  │  │ otel-collector (optional)│ │  ← realistic, higher variance
                  │  │  pinned version         │  │
                  │  └─────────────────────────┘  │
                  └───────────────────────────────┘
```

### 3.1 The SUT (system-under-test)

Three SUT images, one per library/exporter combination. All share the **same workload shape** — the same profile YAML drives the same workload logic — with a **compile-time-selected backend**: `bench/emit-app/` contains backend-specific adapter files, one per library, selected at build time so the microtel and otel-cpp APIs can differ while the workload logic stays identical. Compiler, flags (`-O2 -g -fno-omit-frame-pointer`), and the rest of the toolchain are identical, enforced via a shared base Dockerfile.

The emit-app does three things:
1. Listens on a TCP socket (port 9090) for control messages from the driver (`start`, `stop`, `flush`, `report`). TCP is used rather than a Unix domain socket so the control channel works across Docker container boundaries without volume mounts.
2. Runs the configured workload profile.
3. Exposes a metrics endpoint (process-internal counters: items emitted, items dropped, queue high-water mark, wallclock spent in `StartSpan`/`End`, etc.).

### 3.2 The sink

Two sink modes:

- **`blackhole-sink` (default):** A tiny purpose-built server in Go (~500 LOC) that speaks both **HTTP/2 + protobuf** and **gRPC**, accepts OTLP, parses just enough to count items and bytes per signal, returns `200 OK` immediately, and exposes counters over a control socket. Eliminates collector-side variance — the cost we measure is purely the lib + transport.

- **`otel-collector`:** The real upstream `otel/opentelemetry-collector` at a pinned version, configured with a `debug` exporter (or `file` exporter for archival). Higher variance, but useful for end-to-end realism.

The driver picks the sink mode from config; default is `blackhole-sink` for low-noise comparisons, with `otel-collector` runs done separately.

### 3.3 The driver

Python 3.11+, single dependency: `docker` (or `podman`) on the host. No PyPI dependencies in the hot path — the driver itself is stdlib + a handful of vendored helpers, so it runs in air-gapped envs.

Responsibilities:
- Build SUT images (cached on a base layer hash).
- Capture **environment fingerprint** (CPU model, microcode, kernel, governor, hyperthreading, NUMA, container runtime, lib versions, compiler version, host load average at start). Embedded in every report.
- Pin SUT and sink containers to specific cores via `cpuset` (configurable; off by default).
- Run a configurable warm-up phase, then a measurement window.
- Record per-SUT measurements via control socket and `/proc/<pid>/status`.
- Repeat the run **N times** (default 5) with the order randomized across SUTs to wash out drift.
- Compute median, p25/p75, IQR, min/max. **Never reports just a single mean.**
- Emit `results.json`, `results.md`, optional `flamegraph-<sut>.svg`.

---

## 4. Workload Profiles

Profiles live in `bench/profiles/*.yaml`. v1 ships these:

### 4.1 `hot-loop-traces`
Tight loop emitting spans as fast as the lib will accept them. Measures **library overhead per span and steady-state max throughput.**

```yaml
profile: hot-loop-traces
duration_seconds: 60
warmup_seconds: 10
threads: 1                 # also runs configured at 4, 16
spans_per_iteration: 1
attributes_per_span: 5
attribute_value_mean_bytes: 24
events_per_span: 0
links_per_span: 0
target_rate_hz: max        # also runs at 1k, 10k, 100k
```

### 4.2 `realistic-request`
Simulated request handler: per "request," emit one span with two child spans, one log record, one histogram observation, one counter increment. Fixed RPS, sustained.

```yaml
profile: realistic-request
duration_seconds: 120
warmup_seconds: 20
threads: 4
target_rate_hz: 5000
attributes_per_span: 8
log_message_mean_bytes: 80
```

### 4.3 `cold-start`
Process starts → first byte received at sink. Measures **library init cost + first-export latency.** Run 50 times; report median + p95.

### 4.4 `bursty`
Idle baseline punctuated by 1-second bursts at 50k spans/sec every 10s for 5 minutes. Measures **queue overflow behavior, drop rate, and recovery.**

### 4.5 `large-attributes`
Spans with 20 attributes, mean 256 bytes each, including some 4 KB string attributes. Measures **encoding cost on big payloads.** This profile intentionally exercises the `attribute_value_length_limit` truncation path ([`memory-model.md`](memory-model.md) §7); truncation behavior and its effect on encoding cost are part of what is measured.

### 4.6 `binary-size` (static, no execution)
Build all three SUTs, measure stripped shared library size, statically-linked dep closure size, and `.text` / `.rodata` section sizes via `size` and `bloaty`. Pure static measurement; no runs.

### 4.7 `hot-loop-logs`
Tight loop of `Logger::Emit()` calls, each a minimal record (severity INFO, a short string body, no attributes), against the four microtel and opentelemetry-cpp SUTs over both OTLP/HTTP and OTLP/gRPC. Measures **per-record emit latency (p50/p95/p99), records/sec, delivery rate and wire bytes per record.** The profile sets `signal: logs`: the blackhole sink decodes `/v1/logs` and `LogsService.Export` and counts log records, and the driver computes delivery and bytes per record from that count. It runs against the blackhole sink only; the collector sink has no logs pipeline.

```yaml
profile: hot-loop-logs
signal: logs
workload:
  spans_per_sample: 10000   # log records per sample
env:
  EMIT_WORKLOAD: hot_loop_logs
```

Users add their own profile by dropping a YAML file into `bench/profiles/`.

---

## 5. Metrics Recorded

Per SUT, per run, per profile:

| Metric | Source | Units |
|---|---|---|
| Caller-thread CPU time on emit | `getrusage(RUSAGE_THREAD)` deltas, integrated over the measurement window | μs / 1k spans |
| Process CPU time | `getrusage(RUSAGE_SELF)` | seconds |
| Peak RSS | `/proc/self/status` `VmHWM`, polled every 100 ms | MB |
| Steady-state RSS (median) | same, median over window | MB |
| Throughput (sink-observed) | sink counters | items/sec |
| Items emitted | SUT counter | count |
| Items dropped — total | SUT counter (sum of all `DropReason` buckets via `IDiagnosticsSink`) | count |
| Items dropped — `QueueFull` | `RecordDrop(DropReason::QueueFull)` counter | count |
| Items dropped — `RecordTooLarge` | `RecordDrop(DropReason::RecordTooLarge)` counter | count |
| Items dropped — `SpanAttributeLimit` | `RecordDrop(DropReason::SpanAttributeLimit)` counter | count |
| Items dropped — `AttributeValueTruncated` | `RecordDrop(DropReason::AttributeValueTruncated)` counter | count |
| Drop rate | computed (total drops / items emitted) | % |
| StartSpan overhead | per-call wallclock histogram in SUT | ns p50/p95/p99 |
| Span lifecycle overhead | StartSpan→End wallclock | ns p50/p95/p99 |
| Encoding cost per batch | timing around protobuf serialize | μs |
| Wire bytes per signal | sink-observed, gzip on/off | bytes/span |
| Cold-start to first export | driver-observed | ms |
| Binary size (stripped) | static measurement | bytes |
| Dep closure size | `ldd` + file sizes | bytes |

All histograms ship as raw HDR-histogram data alongside the summary, so users can re-derive any percentile.

---

## 6. Reporting

### 6.1 `results.json`
Full machine-readable record: environment fingerprint, profile config, every metric, every percentile, every run's raw data. Schema versioned. Designed to feed downstream dashboards or regression-detection tooling.

### 6.2 `results.md`
Human-readable report. Layout:

1. **Environment fingerprint** (CPU, kernel, lib SHAs, etc.) — first, so the reader knows what they're looking at.
2. **TL;DR table.** Median values, side by side, with relative deltas. Includes the IQR so deltas inside the noise band are visible as such.
3. **Per-profile sections.** Detailed stats, with notes on anomalies (e.g., "SUT-B saw 0.3% drop rate; SUT-A saw 0%").
4. **Methodology footer.** What was measured, how, what was held constant, what wasn't.

Example TL;DR row format. **The values are illustrative placeholders, not
measurements**; measured figures are in [`docs/bench-results/`](bench-results/README.md),
and sizes in [`docs/bench-results/sizes/`](bench-results/sizes/library-sizes.md).

```
| Metric                       | microtel        | otel-cpp+gRPC   | otel-cpp+HTTP   | Δ (microtel vs gRPC) |
|------------------------------|-----------------|-----------------|-----------------|----------------------|
| StartSpan p50 (ns)           |  82  [76–89]    | 148  [140–161]  | 121  [115–129]  | -45%                 |
| Peak RSS (MB)                |  18.4           |  62.1           |  41.7           | -70%                 |
| Wire bytes/span (gzip off)   | 214             | 214             | 214             |  0%                  |
| Binary size stripped (KB)    | 1380            | 4920            | 4220            | -72%                 |
```

Every cell with a square-bracket range is `[p25–p75]`. Cells without ranges are deterministic measurements (sizes, byte counts on identical payloads).

### 6.3 Optional plots
`results.html` with Plotly charts (latency CDFs, RSS over time, throughput over time). Off by default to keep the harness dependency-light; enable with `--with-plots`.

### 6.4 Optional flame graphs
`--flamegraph <sut>` runs `perf record` against the named SUT during the measurement window and renders a flame graph SVG via Brendan Gregg's scripts (vendored). Requires `perf` on the host and the `SYS_ADMIN` capability for the container. Off by default.

---

## 7. Reproducibility Guarantees

- **Pinned versions.** Every external dep (compiler image, OS base, otel-cpp release tag, microtel commit, collector release) is pinned in `bench/versions.lock`. Updates are a deliberate PR.
- **Deterministic workload.** Workload generation uses a seeded PRNG; the same seed + profile yields the same sequence of spans/metrics/logs.
- **Order randomization.** SUT-run order is shuffled per repetition to wash out time-ordered drift.
- **Cold cache, warm process.** Each repetition starts a fresh SUT process, runs `warmup_seconds` of traffic that is discarded, then measures.
- **Environment guards.** Driver refuses to run (with a warning override flag) if it detects:
  - CPU governor is not `performance`
  - System load average > 0.5 at start
  - SMT/hyperthreading is enabled and `--allow-smt` not passed
  - Available memory below a threshold
- **Known-noise floor.** A `null-vs-null` calibration run (same SUT vs itself across two containers) is included in the harness; it reports the noise floor of the measurement environment so users can tell the difference between "real win" and "in the noise."

---

## 8. Extensibility

- **Adding a library.** Drop a new SUT Dockerfile in `bench/sut/<name>/`, add it to `bench/sut/registry.yaml`, rebuild. The harness is library-agnostic by design — `otel-rust`, `otel-go`, or future microtel siblings can be benchmarked side-by-side. The library doesn't need to be C++.
- **Adding a profile.** Drop a YAML in `bench/profiles/`. No driver changes.
- **Adding a metric.** Two changes: SUT counter exposure + driver schema update. Backward compatible — old reports stay readable.
- **CI integration.** A `--regression-check baseline.json` mode compares the current run against a stored baseline and exits non-zero on regression beyond a configurable threshold (default 5% on key metrics). Drop-in for Jenkins, GitHub Actions, GitLab CI.

---

## 9. Project Layout

Lives inside the main `microtel` repo at `bench/`, built behind
`cmake -DMICROTEL_BUILD_BENCH=ON`. Benchmarks are atomically versioned with
the library; the directory can be extracted to its own repo later if needed
(`git filter-repo --subdirectory-filter bench/`). See ICP 0005.

```
microtel/
└── bench/
    ├── README.md                       (one-page run instructions)
    ├── bench.sh                        (entry point)
    ├── CMakeLists.txt                  (included when MICROTEL_BUILD_BENCH=ON)
    ├── driver/                         (Python orchestration)
    │   ├── __main__.py
    │   ├── env_fingerprint.py
    │   ├── runner.py
    │   ├── stats.py
    │   └── report.py
    ├── emit-app/                       (workload, compile-time-selected backend)
    │   ├── CMakeLists.txt
    │   ├── src/main.cpp
    │   ├── src/workload_*.cpp
    │   ├── src/control_socket.cpp
    │   ├── src/backend_microtel.cpp    (microtel adapter)
    │   ├── src/backend_otelcpp.cpp     (otel-cpp adapter)
    │   └── include/backend.hpp         (common interface)
    ├── sut/
    │   ├── microtel/Dockerfile
    │   ├── otelcpp-grpc/Dockerfile
    │   ├── otelcpp-http/Dockerfile
    │   ├── base.Dockerfile
    │   └── registry.yaml
    ├── sink/
    │   ├── blackhole/                  (Go, ~500 LOC)
    │   └── collector/config.yaml
    ├── profiles/
    │   ├── hot-loop-traces.yaml
    │   ├── realistic-request.yaml
    │   ├── cold-start.yaml
    │   ├── bursty.yaml
    │   ├── large-attributes.yaml
    │   └── binary-size.yaml
    ├── versions.lock
    ├── docs/
    │   ├── methodology.md
    │   ├── interpreting-results.md
    │   └── adding-a-library.md
    └── results/                        (gitignored; user output lands here)
```

The CI workflow (`.github/workflows/benchmark.yml`) is a separate scheduled
run; it does not block PR merges.

---

## 10. Roadmap

| Milestone | Scope |
|---|---|
| **B0** | Driver scaffolding, env fingerprint, blackhole sink, microtel + otel-cpp-gRPC SUTs, `hot-loop-traces` profile only, JSON output. |
| **B1** | All v1 profiles, Markdown report, otel-cpp-HTTP SUT. |
| **B2** | otel-collector sink mode, regression-check mode, CI templates. |
| **B3** | Flamegraph integration, Plotly plots, Rust/Go SUT examples. |
| **B4** | Public publish: blog post + reproducible results on a known reference machine, inviting third-party validation. |

---

## 11. Anti-Goals (Things The Harness Will *Not* Do)

- **Pick a winner for the user.** The report shows the data; the user makes the call. We never print "microtel wins" headlines.
- **Hide variance.** Single-number summaries without IQR/CIs are forbidden in the report template.
- **Compare across machines.** Every report is single-machine. Cross-machine comparison is the user's job and the methodology doc says so explicitly.
- **Synthesize numbers.** No "scaled" or "normalized" metrics that aren't directly measured.
- **Run as root by default.** Optional capabilities (perf, cpuset) require explicit opt-in flags.

---

## 12. Open Questions

1. **Default sink:** blackhole or collector? Leaning blackhole for primary comparison (lower noise), collector available as `--sink=collector`. Confirm.
2. **Should the harness include otel-cpp+nghttp2 hybrid?** I.e., otel-cpp's HTTP exporter but built against nghttp2 instead of its current HTTP client. Would isolate "is the win from the transport, or from the SDK rewrite, or both?" Worthwhile but adds maintenance burden. Defer to v2?
3. **Languages beyond C++.** Stock OTel Python, Go, Rust comparisons would be informative but they're different implementations entirely — apples-to-oranges. Document carefully if included.
4. **Hosting reference results.** A public reference machine (GitHub-hosted runner? a known cloud VM type? a beefy local box?) for the canonical baseline numbers. Pick one, document it, run weekly.
5. **License of the workload code.** Apache 2.0 to match microtel and OTel. Confirmed?

---

## 13. References

- `docs/bench-spec.md` (this document); the performance targets are §14
- [HdrHistogram](https://github.com/HdrHistogram/HdrHistogram_c) — for percentile recording in the SUT
- [bloaty](https://github.com/google/bloaty) — for binary size analysis
- [Brendan Gregg, "Systems Performance" Ch. 12](https://www.brendangregg.com/sysperfbook.html) — methodology references
- [OpenTelemetry Collector](https://github.com/open-telemetry/opentelemetry-collector) — pinned for collector-mode sink

---

## 14. Performance Targets

The targets the harness exists to check. Measured against `opentelemetry-cpp` with both its OTLP/gRPC and OTLP/HTTP exporters on identical workloads, same hardware, OTel collector pinned to the same version. Workload: traces only, 100k spans/sec sustained, 200-byte average span, 5 attributes per span. Full methodology in §1–§7 of this document.

§14.1–§14.4 name the metrics the targets are stated in; §5 is the table of what a run records, and §14.6 maps one onto the other rather than restating it.

### 14.1 Hot-path metrics

For trace SDKs, sampler behavior dominates production cost — most spans in production are unsampled and the unsampled path must be near-zero overhead. Benchmarks cover both:

- p50 / p95 / p99 nanoseconds for span creation, **sampled** and **unsampled**
- p50 / p95 / p99 for parent-sampled vs parent-not-sampled paths
- attributes added before the sampling decision vs after
- allocations per span on caller thread (sampled and unsampled)
- bytes allocated per span on caller thread
- caller-thread CPU cycles per span
- queue push contention under 1, 4, 16 application threads

### 14.2 Exporter metrics

- batches/sec sustained
- spans/sec sustained
- encoder CPU
- transport CPU
- queue depth under collector outage
- spans dropped under sustained outage
- reconnect behavior under `GOAWAY` and `RST_STREAM`

### 14.3 Footprint metrics

- stripped shared object size
- package install size
- loaded RSS after init
- RSS under steady export
- transitive dynamic dependencies via `lddtree`

### 14.4 Cold-start metric

Defined precisely: time from SDK initialization start to successful receipt of the first export response from a local collector, with DNS disabled, collector warmed, TLS mode specified, and batch delay forced to zero.

### 14.5 Footprint targets (v1, stretch)

Component-separated to keep claims defensible:

| Component | Target (stretch, pending prototype) |
|---|---|
| `libmicrotel-exporter.so` (stripped) | < 800 KB |
| `libmicrotel-sdk.so` (stripped, full v1 surface) | < 1.5 MB |
| Total transitive dynamic closure | < 3 MB |
| Python extension | measured separately |
| Control-plane component | excluded from core size target (deferred to v1.4) |

Benchmarks report both **dynamic-link** and **mostly-static** configurations. Dependency closure is measured with `lddtree` for dynamic and package artifact size for static. Realistic floors were to be set after M0 and M2; none has been set, so the table above is stretch.

| Metric | vs otel-cpp+gRPC | vs otel-cpp+HTTP |
|---|---|---|
| Caller-thread CPU per span | ≥ 30% lower | ≥ 15% lower |
| RSS, steady state | ≥ 50% lower | ≥ 25% lower |
| Wire bytes per span (gzip on) | within ±5% | within ±5% |
| Wire bytes per span (gzip off) | within ±2% | within ±2% |
| Library binary size (stripped, dyn) | ≥ 70% smaller | ≥ 50% smaller |
| Cold-start to first export | ≥ 40% faster | ≥ 20% faster |

The component table assumes shared libraries. microtel ships static archives
only (`libmicrotel_*.a`), so the per-library rows have no artifact to measure
as named; [`roadmap.md`](roadmap.md) §6 carries that
status and the footprint trajectory across releases.

### 14.6 Where §5 records each target metric

| Target metric | Recorded by |
|---|---|
| Span creation p50/p95/p99 (§14.1) | §5 "StartSpan overhead". The §4 profiles do not set a sampler, so the sampled / unsampled and parent-sampled splits have no row |
| Attributes before vs after sampling, allocations and bytes per span (§14.1) | no §5 row |
| Caller-thread CPU per span (§14.1, §14.5) | §5 "Caller-thread CPU time on emit" (CPU time, not cycles) |
| Queue push contention, 1 / 4 / 16 threads (§14.1) | `hot-loop-traces` (§4.1) runs at 1, 4 and 16 threads; contention itself has no row |
| Spans/sec (§14.2) | §5 "Throughput (sink-observed)" |
| Batches/sec, transport CPU, queue depth under outage, reconnect behaviour (§14.2) | no §5 row |
| Encoder CPU (§14.2) | §5 "Encoding cost per batch" |
| Spans dropped under outage (§14.2) | §5 drop counters; `bursty` (§4.4) overflows the queue but is not an outage |
| Stripped size, dependency closure (§14.3, §14.5) | §5 "Binary size (stripped)" and "Dep closure size"; `binary-size` (§4.6) |
| Package install size, RSS after init (§14.3) | no §5 row |
| RSS under steady export (§14.3, §14.5) | §5 "Steady-state RSS (median)" |
| Wire bytes per span (§14.5) | §5 "Wire bytes per signal", gzip on/off |
| Cold start (§14.4, §14.5) | §5 "Cold-start to first export"; `cold-start` (§4.3) measures process start to first byte at the sink, which is looser than the §14.4 definition |
