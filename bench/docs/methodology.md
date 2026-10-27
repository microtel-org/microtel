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
