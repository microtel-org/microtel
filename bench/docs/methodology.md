# Benchmark methodology

## Drop counter reporting

### microtel SUT

Drop counters are read from `Provider::GetExporterHealth()` which returns a
`HealthSnapshot` containing one counter per `DropReason` enumerator
(`kDropReasonCount` total).  `spans_dropped_total` in the result JSON is the
sum over all of those counters.

### otelcpp-grpc and otelcpp-http SUTs

opentelemetry-cpp does not expose drop counters via a stable public API.
`spans_dropped_total` is reported as **0** for these SUTs regardless of actual
drops.  Comparisons of drop rates between microtel and otelcpp SUTs are
therefore not meaningful.  This will be revisited if otelcpp adds a comparable
API.

## Sink modes

### blackhole

The blackhole sink accepts all incoming OTLP spans and discards them
immediately, providing minimal processing overhead.  It exposes `/stats`
(spans received, bytes received) and `/reset` over HTTP on port 19080.

`bytes_received` is available in blackhole mode and is recorded with each
sample; the report derives wire bytes per span and wire throughput from it.
The driver does not compare it with the SUT's `bytes_sent`, and nothing in
the driver or CI gates on the two being equal.

### collector (otel-collector-contrib)

The collector sink runs the OpenTelemetry Collector with an OTLP receiver,
batch processor, and debug exporter.  Span counts are read by scraping the
collector's Prometheus endpoint on port 8888 using a **two-scrape bracket**:
`reset()` records the current values of `otelcol_receiver_accepted_spans`
and `otelcol_receiver_refused_spans`; `stats()` scrapes again and subtracts.

`bytes_received` is `null` in collector mode — the collector does not expose
received-bytes via its Prometheus metrics, so wire bytes per span is not
reported in collector mode.  Span counts are read in both modes and reported
as the delivery rate (received over emitted); nothing fails on them.

Collector mode has higher span-count variance than blackhole mode due to
Prometheus scrape timing and collector-internal batching.

## Run order and host noise

The driver runs the SUTs one after another in registry order (the profile's
`suts:` list picks which), each as one fresh container: warm-up, then the
profile's `samples`. It does not shuffle the order and has no calibration
run.

At start it captures the environment fingerprint and warns — but still runs —
if the CPU governor is not `performance`, the 1-minute load average is above
0.5, or SMT is on without `--allow-smt`. The warnings are kept in the report.

Releases control for host drift by interleaving runs by hand. The v1.2.4
snapshot merged five runs of the full profile, each run of the new images
preceded by a run of the outgoing snapshot's images on the same host (old,
new, old, new…), so drift falls on both alike.

## Bytes sent

`bytes_sent_total` in the per-sample results is always 0 from the SUT side;
it is populated by the blackhole sink's `/stats` endpoint after each run.
In collector mode the field remains 0 and `sink.bytes_received` is null.

## Latency measurement

Per-span latency is the wall-clock time from immediately before `EmitSpan()`
returns to immediately after, measured with `std::chrono::steady_clock`.  This
captures SDK hot-path overhead (span creation, attribute encoding, queue push)
but excludes network RTT and batch flush latency.  It is **not** end-to-end
export latency.

## CPU

The driver records the CPU each SUT container uses ([`driver/cpu.py`](../driver/cpu.py)).
The figures are added to `results.json` (each SUT's `cpu`) and to the Results
table in `results.md`. Nothing gates on them.

### Window

There is one window per SUT. It starts after the warm-up's flush and sink reset,
and ends after the last sample's flush. Export work is therefore included,
along with the driver's 0.5 s wait after each flush, when the SUT is idle
apart from its background threads' wake-ups. Snapshots are taken at both ends,
and every figure is the difference between them.

### Sources

- **Process total.** This is the container's cgroup v2 `cpu.stat`:
  `usage_usec`, `user_usec` and `system_usec`. The cgroup comes from podman's
  `State.CgroupPath`, or for docker from the SUT process's
  `/proc/<pid>/cgroup`. It covers everything in the container, which is only
  the emit-app, PID 1. It works rootless.
  - If `cpu.stat` can't be read, the driver falls back to the process's
    `/proc/<pid>/stat` `utime` and `stime`. These are in clock ticks, 10 ms
    at `USER_HZ` 100.
  - The fallback is not the sum of `/proc/<pid>/task/*/stat`. That sum
    misses threads that have exited, and the emitting threads have.
  - The `CPU source` row and `cpu.source` say which source was used.
- **Per thread.** For every live thread, the name comes from
  `/proc/<pid>/task/<tid>/stat`, and the run time in ns from `schedstat`.
  When `schedstat` is missing, the run time is `utime + stime` in ticks
  instead (`cpu.thread_source`).

### Emitting threads and the rest

The emit-app names its workload threads `emit` (`pthread_setname_np`). It
creates them for each `run` command and joins them before it replies, so they
have exited by the time either snapshot is taken. Their CPU is measured two
ways, and the report gives both:

- **Self-reported** (`cpu.emit_self`). Each workload thread reads its own
  `CLOCK_THREAD_CPUTIME_ID` (ns) and `getrusage(RUSAGE_THREAD)` (user and
  system, µs) when it starts and when it finishes, and the run reply carries
  the sum over threads (`emit_cpu_ns`, `emit_user_ns`, `emit_sys_ns`). This
  is the emitting-thread figure. It leaves out thread creation and exit,
  which run before the first reading and after the last.
- **By subtraction** (`cpu.per_span_ns.emit`), the cross-check. The process
  total minus the other threads'. The other threads are the SDK's exporter
  and I/O threads, the HTTP client's, and the emit-app's main thread, which
  runs the control socket and computes the percentiles. Anything else that
  starts and exits inside the window is counted as emitting, and so are
  thread creation and exit. `cpu.threads_exited` lists the threads alive at
  the first snapshot and gone at the second; their CPU after the first
  snapshot is counted as emitting too.

The two should agree to within the thread start and exit cost and the
accounting granularity. A larger gap means something unnamed ran and exited
inside the window.

The split works only for emit-app SUTs. `go-otel` and `rust-otel` have no
`emit` threads. Their work runs on long-lived runtime threads and shows up
under "other".

### Per span, cores, ratios

- **Per span.** CPU is divided by the spans delivered at the sink, summed over
  the samples. Dropped spans still cost the SUT CPU, so a SUT that drops is
  charged that cost per delivered span. Read it together with the delivery
  rate.
- **Cores.** Total CPU divided by the window's wall time.
- **÷ microtel.** Each figure divided by the `microtel` SUT's in the same run.
  The ratio is absent when microtel didn't run.

### Baseline

`baseline-noop` is the emit-app built with `BENCH_BACKEND=NOOP`. It has the
same control socket, workload threads, pacing loop, clock reads and histogram
as the other SUTs, but no SDK, and it exports nothing. So its CPU is divided
by the spans it emitted.

When it is in the same run, each SUT also gets two baseline-subtracted
figures:
- `total − baseline`: the SUT's ns per span minus the baseline's;
- `cores − baseline`: the SUT's cores minus the baseline's.

Cores subtraction assumes both ran at the same rate, so it means something
only at a fixed rate. In hot-loop profiles the baseline loop runs much faster
than any SDK.

### Fixed-rate profiles

`fixed-rate-1k` and `fixed-rate-10k` run the realistic-request workload: a
parent and two children per request, on 4 threads. Each is one 60 s run after
a warm-up of about 1 s.

| Profile | Requests/s | Per thread | Spans/s, nominal |
|---|---:|---:|---:|
| `fixed-rate-1k` | 332 | 83 | 996 |
| `fixed-rate-10k` | 3,332 | 833 | 9,996 |

The emit-app divides the rate by the thread count, rounding down.

Each workload thread paces to absolute deadlines: iteration n starts no
earlier than start + n / rate (`emit-app/include/pacer.hpp`). A late wake-up
(wake-up latency, plus up to the 50 µs default timer slack) delays one
iteration, and the following ones catch up, so the achieved rate matches the
target. The threads' starts are staggered: thread i of n starts at
start + i × interval / n (`StaggeredStart`), so their iterations interleave
evenly instead of all waking at the same instant and contending on the SDK's
locks together. The `Spans/s, target → achieved` row gives both, from each sample's
`span_rate_target` and `span_rate_achieved`.

### Caveats

- **Accounting granularity.** `usage_usec` comes from the scheduler's
  ns-resolution run time. The kernel splits it into user and system by tick
  sampling, then scales the split to the total, so the user/system split is
  approximate over short windows. The `/proc` fallback is 10 ms ticks.
- **SMT.** CPU time is time on a logical CPU. When the sibling hyperthread is
  busy (the sink, the driver or another SUT thread), the same work takes more
  ns. Nothing is pinned.
- **The pacing loop's own cost.** At a fixed rate, each request costs clock
  reads, a `nanosleep` and a timer wake-up. For a cheap SDK these are a large
  part of the total, which is why the baseline is subtracted. The subtraction
  assumes the two costs simply add.
- **Low rates cost more per span.** At a fixed rate each workload thread
  sleeps between requests (12 ms per thread at `fixed-rate-1k`), and the core
  may idle in a deep C-state that loses its L1/L2 contents. Each request then
  starts cold: on one run the microtel SUT executed the same user-space
  instructions per span at 1k and 10k spans/s but took about 3.9× the cycles,
  with about 4× the cache misses. The baseline shows the same effect. So
  ns/span at 1k is mostly a wake-up cost, not a per-span cost, and does not
  scale down to the hot-loop figure.
  Before the threads' starts were staggered they woke in lockstep and
  contended on the SDK's locks: on this host that added about 1.8 µs/span to
  microtel at 1k spans/s (5 rounds each way), and nothing measurable to
  opentelemetry-cpp, whose span buffer is lock-free.
  Low-rate figures therefore depend on which deep C-states are enabled: with
  them disabled (`processor.max_cstate=1`, `idle=poll`, or a state's
  `disable` flag) they should fall (not measured here), and two hosts compare only with the same
  configuration. `results.json` records it (`environment.cpu_idle_driver`,
  `cpu_idle_states` with each state's disable flag, and the kernel command
  line's `kernel_idle_options`), and the Environment table shows it.
- **Not counted.** The sink, which runs in its own container. The container
  engine (`conmon`, rootless networking). Kernel network work done outside
  the SUT's own threads, for example softirqs that run while another task is
  scheduled.
- **Single run.** A driver run gives each SUT one window, with no spread.
  Comparisons need repeated, interleaved runs: see "Interleaved rounds".

### Interleaved rounds

`python3 -m driver.rounds` ([`driver/rounds.py`](../driver/rounds.py)) runs
one profile for several SUTs in rounds, one driver run per SUT per round:

```bash
cd bench
python3 -m driver.rounds --profile fixed-rate-1k \
    --sut microtel,otelcpp-http,baseline-noop --rounds 5 --out results/<dir>
```

- **Order.** Every round runs every SUT, the order rotated by one each round,
  so no SUT always runs first or after the same neighbour.
- **Load gate.** Before each run it waits until the 1-minute load average is
  below `--max-load` (0.5). After `--load-timeout` (300 s) it records the
  load and runs anyway. `summary.md` lists the load and the wait before every
  run.
- **Per round.** The baseline is subtracted from the other SUTs of the same
  round, and the ratio to microtel is taken within the round, so a round
  where the whole host ran slow moves both sides of the ratio.
- **Summary.** `summary.json` and `summary.md` give the median and range
  (min–max) over rounds for ns/span and cores, raw and minus the baseline,
  the emitting-thread figures, the ratios and the achieved rate. Each run's
  own `results.json` is kept under `round<r>/<sut>/`.

The runs never build images. Build them first (a plain driver run does), or
pass `--build` to build every listed SUT's image before the first round.
