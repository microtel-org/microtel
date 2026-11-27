# microtel Leaf Metrics and Logs Design

**Status:** Draft — 2026-11-27, for review. No code until it is signed off.
**Issues:** #542 (metrics), #543 (logs). Milestone "Leaf: metrics and logs (after v1.3)".
**ICPs:** [ICP 0049](icps/0049-leaf-metrics-logs-scope-and-build-selection.md) (order, milestone, compile-time selection), [ICP 0050](icps/0050-leaf-metrics-logs-wire-and-ingest.md) (wire contract, concentrator ingest).
**Extends:** [`leaf-concentrator-design.md`](leaf-concentrator-design.md), the signed-off trace design. Section references below without a file name (§1.4, §3.8, §4.5, §5, §6.4) are to that document.

This is the one design pass for both new leaf signals. It decides *what* and *why*; signatures are concrete enough to implement against, and the headers written in the first packet must match them. Every figure in §5 is an **estimate until measured**.

## Decisions already made

Recorded in ICP 0049, not re-decided here:

- One design pass; **metrics are implemented and released first, then logs** (reverses §1.3).
- A new unnumbered milestone, worked on straight after v1.3; v1.4 to v1.6 are not renumbered.
- Every signal, and every metric instrument kind, is selected at compile time by plain C defines; a disabled one has no API and is not linked; selecting nothing is a compile error; trace-only users see no change in API, bytes or footprint.

---

## 1. Shared core and per-signal code

### 1.1 What is shared and what is per signal

Today `leaf_core.c` (1,859 lines) holds everything. It splits along what a signal-free leaf still needs:

| Part | File | Compiled when |
|---|---|---|
| state, size guards, version, config validation, init / free | `leaf_core.c` | always |
| Resource and scope persistence, `kv` validation and packing | `leaf_core.c` | always |
| clocks: `now_ns`, `clock_sync`, the three time modes, encode time `E` | `leaf_time.c` | always |
| reserved attributes and the payload header (Resource, reserved keys, scope) shared by every request type | `leaf_payload.c` | always |
| encode driver: buffer and streaming sinks, size bounds | `leaf_payload.c` | always |
| chunk arena: slot table, chunk heap, compaction (§1.4 "Record buffer layout") | `leaf_arena.c` | traces or metrics |
| span API, events, status, id derivation (§1.6.1), trace batch cursors | `leaf_trace.c` | `MICROTEL_LEAF_TRACES` |
| instrument table, series table, aggregation, metrics cursors | `leaf_metrics.c` | `MICROTEL_LEAF_METRICS` |
| histogram buckets | `leaf_metrics_histogram.c` | `MICROTEL_LEAF_METRIC_HISTOGRAM` |
| log ring, log cursors | `leaf_logs.c` | `MICROTEL_LEAF_LOGS` |
| per backend: `KeyValue` / `AnyValue` / Resource / scope writers | `backend_<b>.c` | always |
| per backend, per signal | `backend_<b>_{trace,metrics,logs}.c` | that signal |
| nanopb generated descriptors, per signal | `gen_nanopb_{common,trace,metrics,logs}.c` (wrappers that `#include` the generated `.pb.c`) | that signal |

Notes:

- **Id derivation is trace-only.** With traces off, `random_bytes` is not required and the device key is not computed. `boot_id` stays in the core: it is mixed into ids when traces are on, and sent on metrics and logs payloads (§2.3).
- **Struct layouts do not depend on a switch.** `microtel_leaf_t` (`MICROTEL_LEAF_STATE_WORDS`, 32 words), `microtel_leaf_config_t` and `microtel_leaf_counters_t` keep every field in every build. Per-signal state lives at the head of that signal's own buffer (§1.2), so the 256-byte state does not grow.
- **The rule of §1.1** ("only one source file per backend includes that encoder's headers") widens to "only the backend's own files" (ICP 0049 Decision 4).
- **Static archives already select by object file.** A firmware that links `libmicrotel_leaf.a` pulls only the object files it references. The guarded wrappers make the same true for a firmware that compiles the sources directly. For the upb backend, the generated mini-tables come from the shared `microtel_upb_gen` archive (§2.1): unreferenced signals' tables are not pulled from the archive, and a source build needs `--gc-sections` or the per-signal file list in `leaf/README.md`.

### 1.2 Memory: one buffer per signal

Each signal gets its own caller-owned buffer, attached after init:

| Signal | Buffer | Attached by |
|---|---|---|
| traces | the record buffer of `microtel_leaf_init` (unchanged); also holds Resource and scope | `microtel_leaf_init` |
| metrics | the series table | `microtel_leaf_metrics_attach` |
| logs | the log ring | `microtel_leaf_logs_attach` |

With traces off, `microtel_leaf_init` still takes a record buffer, sized for the Resource and scope only.

Why separate buffers: a burst of logs or spans cannot evict metric series (whose state must persist, §2.2); each signal is sized on its own; and a disabled signal's attach function does not exist, so its memory cannot be passed by mistake.

*Alternative considered:* one shared record buffer with a record-type tag. One sizing knob, but signals compete for space, and metrics series that must live for the whole boot would fragment the span arena.

### 1.3 The configuration header

`leaf.h` includes `microtel/leaf_config.h` first. That header:

1. includes `MICROTEL_LEAF_CONFIG_FILE` if it is defined (`-DMICROTEL_LEAF_CONFIG_FILE='"my_leaf_config.h"'`);
2. applies the defaults in ICP 0049 Decision 3 to each switch not yet defined;
3. fails with `#error` on an invalid selection:
   - `#error "microtel-leaf: no signal selected: set MICROTEL_LEAF_TRACES, MICROTEL_LEAF_METRICS or MICROTEL_LEAF_LOGS to 1"`
   - metrics on and every kind off; a kind defined to `1` with metrics off (checked before defaults are applied); any switch not `0` or `1`;
4. defines `MICROTEL_LEAF_FEATURES`, a bit mask of what was selected, and `leaf.h` declares `uint32_t microtel_leaf_features(void)`, which returns the mask the library was built with, so firmware can check a header against a prebuilt archive at boot.

For a disabled function, `leaf.h` defines a function-like macro of the same name that expands to an undeclared identifier naming the switch:

```c
#if !MICROTEL_LEAF_METRIC_COUNTER
#define microtel_leaf_counter_add(...) \
    (microtel_leaf_disabled__set_MICROTEL_LEAF_METRIC_COUNTER_to_1)
#endif
```

A use of an undeclared identifier is an error in every C compiler (unlike an implicit function declaration, which GCC before 14 only warns about), and the message names the switch to set.

---

## 2. Metrics

### 2.1 API

```c
typedef enum microtel_leaf_metric_kind
{
    MICROTEL_LEAF_METRIC_KIND_COUNTER = 0,
    MICROTEL_LEAF_METRIC_KIND_UPDOWN_COUNTER = 1,
    MICROTEL_LEAF_METRIC_KIND_GAUGE = 2,
    MICROTEL_LEAF_METRIC_KIND_HISTOGRAM = 3
} microtel_leaf_metric_kind_t;

/* One instrument. The table is caller-owned, static storage, and borrowed for
 * the leaf's lifetime (see §2.2 question 1). */
typedef struct microtel_leaf_instrument_desc
{
    const char *name;
    size_t name_len;
    const char *unit;              /* may be NULL */
    size_t unit_len;
    microtel_leaf_metric_kind_t kind;
    microtel_leaf_value_type_t value_type; /* INT64, or DOUBLE for a gauge */
    const int64_t *bounds;         /* histogram: ascending explicit bounds */
    size_t bound_count;            /* histogram: at most 15 */
} microtel_leaf_instrument_desc_t;

typedef uint16_t microtel_leaf_instrument_t; /* index into the table */

typedef struct microtel_leaf_metrics_config
{
    uint32_t struct_size;
    const microtel_leaf_instrument_desc_t *instruments;
    size_t instrument_count;
    uint16_t max_attributes_per_point; /* 0 selects [4] */
} microtel_leaf_metrics_config_t;

microtel_leaf_status_t microtel_leaf_metrics_attach(microtel_leaf_t *leaf,
                                                    const microtel_leaf_metrics_config_t *config,
                                                    void *series_buffer, size_t series_buffer_size);

/* MICROTEL_LEAF_METRIC_COUNTER */
microtel_leaf_status_t microtel_leaf_counter_add(microtel_leaf_t *leaf, microtel_leaf_instrument_t inst,
                                                 int64_t increment,   /* >= 0 */
                                                 const microtel_leaf_kv_t *attrs, size_t attr_count);
/* MICROTEL_LEAF_METRIC_UPDOWN_COUNTER */
microtel_leaf_status_t microtel_leaf_updown_counter_add(microtel_leaf_t *leaf, microtel_leaf_instrument_t inst,
                                                        int64_t delta,
                                                        const microtel_leaf_kv_t *attrs, size_t attr_count);
/* MICROTEL_LEAF_METRIC_GAUGE */
microtel_leaf_status_t microtel_leaf_gauge_set_i64(microtel_leaf_t *leaf, microtel_leaf_instrument_t inst,
                                                   int64_t value,
                                                   const microtel_leaf_kv_t *attrs, size_t attr_count);
microtel_leaf_status_t microtel_leaf_gauge_set_f64(microtel_leaf_t *leaf, microtel_leaf_instrument_t inst,
                                                   double value,
                                                   const microtel_leaf_kv_t *attrs, size_t attr_count);
/* MICROTEL_LEAF_METRIC_HISTOGRAM */
microtel_leaf_status_t microtel_leaf_histogram_record(microtel_leaf_t *leaf, microtel_leaf_instrument_t inst,
                                                      int64_t value,   /* >= 0 */
                                                      const microtel_leaf_kv_t *attrs, size_t attr_count);

/* One ExportMetricsServiceRequest with every series. Series are not released
 * (they are cumulative, §2.2 question 9). */
microtel_leaf_status_t microtel_leaf_metrics_encode(microtel_leaf_t *leaf, uint8_t *out, size_t out_size,
                                                    size_t *written);
microtel_leaf_status_t microtel_leaf_metrics_encode_to(microtel_leaf_t *leaf,
                                                       int (*write)(void *, const uint8_t *, size_t),
                                                       void *write_ctx, size_t *written);
size_t microtel_leaf_metrics_encoded_size(const microtel_leaf_t *leaf);
```

The hot-path rule of §1.7 holds: a failed record call drops the measurement, counts it and returns an error the caller may ignore. `attach` rejects a descriptor whose kind is compiled out, a histogram with unsorted or too many bounds, and a `DOUBLE` value type on anything but a gauge (`MICROTEL_LEAF_ERR_ARG`).

### 2.2 #542's nine questions

**1. Aggregation on the leaf, or raw measurements?** *Answer: aggregate on the leaf*, in a series table in the metrics buffer. A series is (instrument, attribute set) and holds its aggregate: 8 bytes for a sum or gauge (plus a type byte for a gauge), `4 × (bounds + 1) + 4 + 8 + 16` for a histogram (bucket counts, count, sum, min, max), plus its packed attributes. Raw measurements are too heavy for slow links. **When the table is full**, a measurement for a new series is dropped and counted (`dropped_measurements`), exactly as a span is at start; existing series keep aggregating. There is no overflow series: an `otel.metric.overflow` series per instrument would cost a reserved slot per instrument on a leaf that is sized for a known set of series.
   - The instrument table is **borrowed, not copied**, a deliberate exception to §1.4's copy rule: it is configuration, written once as a `static const` array, and copying names into RAM would cost the scarcest resource for no safety gain. The header says so.

**2. Histograms.** *Answer: in scope as an optional kind* (`MICROTEL_LEAF_METRIC_HISTOGRAM`, default off). **Bucket boundaries are a build-time table per instrument**, an `int64_t` array in the descriptor, in flash, at most 15 bounds (16 buckets), checked at attach. Values and bounds are integers so a Cortex-M0+ (no FPU) does no soft-float arithmetic on the hot path: bucket search is integer comparison, and sum, min and max are `int64_t`, converted to `double` once per point at encode. Negative values are dropped and counted, as ICP 0047 does for the SDK. **Exponential histograms are excluded** from the leaf: they need `log2` and scale arithmetic in floating point and a downscale path, which is flash and soft-float the leaf cannot afford; a leaf that wants distribution data uses explicit buckets.

**3. Start time across restarts.** *Answer:* a point's `start_time_unix_nano` is the leaf's metrics attach time, on the leaf's clock, for every series. The concentrator maps it to Unix time once per (leaf, boot) and caches it (ICP 0050 Decision 2), per mode:
   - *concentrator-stamped:* the first payload of a boot fixes the mapping (its arrival time minus the leaf's encode-to-attach interval, or the arrival time if the leaf has no clock);
   - *sync-relative:* the leaf converts the attach time with the first `clock_sync` it gets and then **freezes** that Unix value, so a later re-sync cannot move it;
   - *boot-relative:* the anchor (§5.4) at first sight fixes it, and later anchor refinement does not move it.
   A new boot is a changed `microtel.leaf.boot_id`, or a sequence number lower than the last one seen (question 9). `boot_id` is therefore sent on metrics and logs payloads in every mode when it is non-zero, not only in boot-relative mode. Evicting a leaf-table entry (§4.5) loses the cached start; the next payload re-maps it, which a backend may read as a reset, so evictions are counted and `max_leaves` should exceed the fleet.

**4. Concentrator: forward or re-aggregate?** *Answer: forward as received* (ICP 0050 Decision 6). Re-aggregating across leaves loses device identity and needs per-series state.

**5. Cardinality.** *Answer:* on the leaf, cardinality is the series table, sized by the caller's buffer, with `max_attributes_per_point` (default 4) bounding each series. Overflow is `dropped_measurements`, reported on the next payload (§2.3) and summed into `LeafReceiverStats::leaf_reported_drops`. At the concentrator, `max_metric_points_per_payload` (default 512) bounds a payload, and over it the payload is `TooLarge`. There is no per-series limit at the concentrator because it holds no per-series state; the backend's own limits apply downstream.

**6. Footprint budget.** *Answer:* `MICROTEL_LEAF_METRICS`, off by default (ICP 0049), with the per-profile budgets in §5. Each implementation issue measures before it merges; the budgets are revisited once, after the metrics prototype is measured.

**7. Ordering with leaf logs.** *Decided: metrics first* (ICP 0049 Decision 1).

**8. Release.** The design starts now; code starts after v1.3, since it builds on #455 (ICP 0045) and the v1.3 wire fixes. The milestone ships as the next minor after v1.3 (see question Q1 in §8 on whether logs ship in the same minor).

**9. Loss on unreliable links.** Two candidates:

| | Delta + sequence number | Cumulative since boot |
|---|---|---|
| A lost payload | its increments are lost; the gap is counted | the next payload makes it up |
| Leaf state | one accumulator per series, freed after each encode | one accumulator per series, kept for the whole boot |
| Leaf memory | only the series touched since the last encode | every series seen since boot |
| Concentrator state | last sequence number per leaf | last sequence number and a cached start time per leaf |
| Up-down counters | non-monotonic deltas, which many backends handle badly (Prometheus needs `deltatocumulative`) | cumulative, which every backend handles |
| Start-time stability | windows may overlap or gap by the link latency; harmless | must be cached per leaf boot (question 3) |
| §6.4 | as decided | reversed |

   *Recommendation: cumulative since boot*, plus the sequence number. Cumulative heals, which is the property that matters on UDP and BLE; its cost is that series persist, and a leaf's series are a fixed, small set sized at build time. §6.4's two objections are met without per-series state: start-time jitter by caching the start per leaf boot (question 3), and the concentrator's memory by forwarding as received. The sequence number is cheap (4 bytes of state, about 6 on the wire) and is kept because logs cannot heal and their loss should be visible. It also detects a reboot when `boot_id` is not configured.
   - Gauges are unaffected: each payload carries the last value set, with the encode time as its timestamp, as the SDK reports a synchronous gauge.

### 2.3 Wire

One `ExportMetricsServiceRequest` with one `ResourceMetrics` and one `ScopeMetrics` (the leaf's scope, as for spans), one `Metric` per instrument that has at least one series, in table order; points in series-creation order. Counters are `Sum{is_monotonic=true}`, up-down counters `Sum{is_monotonic=false}`, both cumulative; gauges `Gauge`; histograms `Histogram`, cumulative, with `explicit_bounds` converted from the table. `description` is never sent (it costs link bytes on every payload); `unit` only when set. Integer values use `as_int`.

New reserved Resource attributes (ICP 0050), on metrics and logs payloads only, so trace payloads are byte-identical to v1.2:

| Key | When | Meaning |
|---|---|---|
| `microtel.leaf.proto` | always | `2` |
| `microtel.leaf.signal` | always | `1` metrics, `2` logs |
| `microtel.leaf.seq` | always | successful encodes of this signal since boot, from 1 |
| `microtel.leaf.boot_id` | when non-zero, every mode | as §3.8 |
| `microtel.leaf.dropped_measurements` | metrics, when non-zero | since the last successful encode |
| `microtel.leaf.dropped_logs`, `microtel.leaf.truncated_bodies` | logs, when non-zero | since the last successful encode |

The §2.3 byte-identity rules apply unchanged; packed repeated fields now occur (`bucket_counts`, `explicit_bounds`), and both backends must write them packed, which the golden vectors pin.

---

## 3. Logs

### 3.1 Record shape

| Field | Leaf API | On the wire |
|---|---|---|
| timestamp | read from `now_ns` at emit | `time_unix_nano`, corrected by the time mode (§5) |
| observed time | — | `observed_time_unix_nano`, set by the concentrator to the receipt time |
| severity | `microtel_leaf_severity_t`, the OTel numbers 1–24 | `severity_number`; `severity_text` is not sent |
| body | a string with a length | `body` as `AnyValue.string_value`; no structured bodies |
| attributes | `microtel_leaf_kv_t`, at most `max_attributes_per_log` (default 4) | `attributes` |
| trace correlation | only through `microtel_leaf_log_emit_in_span`, present when traces are compiled in | `trace_id`, `span_id`, copied at emit |

```c
typedef struct microtel_leaf_logs_config
{
    uint32_t struct_size;
    uint16_t max_body_bytes;         /* 0 selects [128] */
    uint16_t max_attributes_per_log; /* 0 selects [4] */
    uint8_t min_severity;            /* records below are discarded uncounted; 0 keeps all */
} microtel_leaf_logs_config_t;

microtel_leaf_status_t microtel_leaf_logs_attach(microtel_leaf_t *leaf, const microtel_leaf_logs_config_t *config,
                                                 void *log_buffer, size_t log_buffer_size);
microtel_leaf_status_t microtel_leaf_log_emit(microtel_leaf_t *leaf, microtel_leaf_severity_t severity,
                                              const char *body, size_t body_len,
                                              const microtel_leaf_kv_t *attrs, size_t attr_count);
/* MICROTEL_LEAF_TRACES && MICROTEL_LEAF_LOGS */
microtel_leaf_status_t microtel_leaf_log_emit_in_span(microtel_leaf_t *leaf, microtel_leaf_span_t span,
                                                      microtel_leaf_severity_t severity,
                                                      const char *body, size_t body_len,
                                                      const microtel_leaf_kv_t *attrs, size_t attr_count);
/* encode, encode_to, encoded_size: as for metrics, prefixed microtel_leaf_logs_ */
```

- A separate `_in_span` function keeps `microtel_leaf_log_emit`'s signature the same whether or not traces are compiled in, so switching traces off never breaks log call sites.
- The span's ids are **copied** at emit (24 bytes per record): the span may be encoded and released before the record is.
- The leaf has no "current span": it has no threads and no context (§1.1), so correlation is explicit.

### 3.2 Body size and the caller's buffer

- A body longer than `max_body_bytes` is **truncated on a UTF-8 boundary**: the cut moves back over continuation bytes (`10xxxxxx`), at most three, so the leaf never emits invalid UTF-8 (the concentrator rejects a payload with invalid UTF-8, §2.3). This is not validation; a body that was already invalid stays invalid. Each truncation counts `truncated_bodies`.
- `attach` rejects a buffer that cannot hold one record of the largest size the config allows, so a configuration that can never log fails at attach, not silently.

### 3.3 When the buffer is full

Records are immutable once emitted, so the log buffer is a simple append-only region: a record is appended, and a successful encode empties the buffer. No compaction is needed. **When a record does not fit, the new record is dropped** and counted in `dropped_logs`, as a span is refused at start. Severity filtering below `min_severity` is not a drop and is not counted.

*Alternative considered:* evict the oldest record (a ring), on the view that the latest logs matter most. It needs ring wrap-around in both backends' cursors and makes a payload's contents depend on timing; and a crash loses an MCU's RAM buffer either way. Left as a question (§8 Q6).

---

## 4. Concentrator

### 4.1 Ingest per signal

| | Metrics | Logs |
|---|---|---|
| decoder | `IOtlpMetricsDecoder` (new internal interface), upb, in `src/wire/encoder/` | `IOtlpLogsDecoder` (new), same |
| validation (whole payload, as §3.4) | proto 2, signal matches, one ResourceMetrics; from a leaf: Sum, Gauge and Histogram only, cumulative, `start <= time`, histogram bucket count = bounds + 1 | proto 2, signal matches; severity 0–24; `trace_id` empty or 16 bytes, `span_id` empty or 8; at least one record or a non-zero `dropped_logs` |
| Resource | §4.4, unchanged | same |
| time | `time_unix_nano` by the leaf's mode; `start_time_unix_nano` from the per-boot cache (§2.2 question 3) | `time_unix_nano` by the leaf's mode; `observed_time_unix_nano` = receipt |
| limits | the existing per-record attribute limits; `max_metric_points_per_payload` | the Provider's log limits (`LogAttributeLimit`); `max_log_records_per_payload` |
| into | a bounded pass-through queue (`max_queued_leaf_metric_points`, default 8192) that `MetricProducer` drains into one `MetricBatchHandle` per leaf Resource on the reader's next collection; never re-aggregated, no temporality applied | the `BatchLogRecordProcessor` queue, shared with in-process logs |
| when the signal is not configured on the Provider | `IngestStatus::Disabled` | `IngestStatus::Disabled` |

- From a sender configured `unix` (a full node, ICP 0036), any valid point type is forwarded, exponential histograms included; `Summary` is dropped and counted.
- The decode-arena factor (§3.7) is re-measured per schema against the new golden vectors and fuzz corpora, and pinned by each decoder's test.

### 4.2 Counters

- Payload-level failures reuse `leaf_payload_malformed`, `leaf_payload_too_large` and `leaf_unknown`, whose unit is payloads of any signal (`error-model.md` §3 says so).
- A full pass-through queue drops points, counted `queue_full` (unit: points); a full log queue, `queue_full` as today.
- `IngestResult` gains `metric_points_accepted`, `metric_points_dropped`, `log_records_accepted`, `log_records_dropped`.
- `LeafReceiverStats` gains `payloads_lost` (sequence gaps), `leaf_restarts` (boots seen), `metric_starts_remapped` (cache lost to eviction), and per-signal `payloads_accepted`.
- No new `DropReason`: payloads lost on the link were never microtel's to drop.

---

## 5. Footprint

**Estimates, not measurements.** Starting point: the v1.2 trace-only probe on a Cortex-M0+ with nanopb is 9,472 bytes of flash (core 5,074, backend 1,086, nanopb runtime 2,206, generated descriptors about 1,100; [`leaf-footprint.md`](bench-results/leaf-footprint.md)). Each profile's probe: one span with one attribute; three instruments with one series each (four with a 4-bound histogram); two log records of 32-byte bodies.

| Profile | Flash, nanopb, Cortex-M0+ (est.) | Flash, upb, Cortex-M4 (est.) | Caller RAM (est.) | Encode stack (est.) | **Proposed budget** (nanopb M0+ flash / caller RAM) |
|---|---:|---:|---:|---:|---|
| traces only | 9,500 | 14,400 | 512 | 2,600 | **≤ 9,700 / 512** — no more than 2 % over v1.2.6 |
| metrics only (counter, up-down, gauge) | 8,800 | 15,500 | 512 | 2,700 | **≤ 10,000 / 640** |
| metrics only, with histogram | 9,900 | 16,500 | 640 | 2,700 | **≤ 11,000 / 768** |
| logs only | 7,500 | 13,500 | 512 | 2,500 | **≤ 9,000 / 640** |
| all on, histogram included | 16,500 | 24,000 | 1,280 | 2,700 | **≤ 18,000 / 1,536** |

- Caller RAM is `microtel_leaf_t` (256 bytes) plus each signal's buffer as the probe sizes it.
- The < 15 KB nanopb target remains a **trace-only** target (roadmap §6; a gate from v2.1). "All on" is expected to exceed it, and its budget says so.
- Budgets are checked by the `leaf-footprint` job per profile and are **targets, not gates**, until v2.1, as the trace target is today. The trace-only budget is the exception: it is a gate from the first PR (issue 1 in §7), since ICP 0049 promises no change.

---

## 6. Test plan

**Four profiles, end to end, on every PR**, each with both backends:

| Check | traces only | metrics only (all kinds) | logs only | all on |
|---|:-:|:-:|:-:|:-:|
| leaf unit tests | ✓ | ✓ | ✓ | ✓ |
| golden vectors and byte identity (§7.2), `leaf_backend_diff_fuzz` per signal | ✓ | ✓ | ✓ | ✓ |
| ingest fuzz targets (`leaf_ingest_fuzz` and one per new decoder) in `fuzz.yml` and `corpus-check` | ✓ | ✓ | ✓ | ✓ |
| collector conformance: leaf → in-memory link → concentrator → pinned collector, both protocols, per backend | ✓ | ✓ | ✓ | ✓ |
| Cortex-M0+ and Cortex-M4 under QEMU (`leaf-target`) | ✓ | ✓ | ✓ | ✓ |
| footprint (`leaf-footprint`), per target and backend | ✓ | ✓ | ✓ | ✓ |

**Every other combination compiles**, in a new compile-only job (`leaf-config-matrix`): every non-empty signal set, and for each set with metrics every non-empty kind subset, each kind switched off on its own included. That is 3 + 4 × 15 = 63 configurations × 2 backends, built with `-std=c11 -pedantic-errors -Werror` on the host compiler and linked against a probe that calls every enabled function. A few minutes in total.

**Negative compile tests** (the build must fail, with the expected message): no signal; metrics with no kind; a kind on with metrics off; a switch set to `2`; a call to a disabled function (the undeclared-identifier message names the switch).

**Trace-only invariance**, from the first PR: the v1.2 golden vectors are unchanged; the trace-only build's global symbols are exactly v1.2.6's (`symbol-scan` against a committed list); the trace-only footprint is within its budget.

**Concentrator tests**, per signal: decoder unit tests and the arena cap; validation (every rule in §4.1 rejects); per-boot start caching (stable across 100 payloads in each time mode, re-mapped on a new `boot_id` and on a sequence regression, counted on eviction); sequence gaps (one lost payload counts one); a lost metrics payload repaired by the next (the collector sees the right cumulative value); the pass-through queue drains once per collection and drops and counts when full; logs share the log queue and its limits; a metrics or logs payload with the wrong `signal` is `Malformed`.

---

## 7. Implementation issues, in order

Each lands as its own PR with its acceptance tests and a footprint measurement; metrics (1–4) are released before logs (5–6).

1. **Shared-core split, traces made optional.** The file split of §1.1, `leaf_config.h`, the switches and their errors, the disabled-function macros, `microtel_leaf_features()`, the guarded nanopb wrappers, the `leaf-config-matrix` job and the negative compile tests, the four-profile plumbing in the footprint and target jobs. *Accepts when:* trace-only golden vectors, symbols and footprint unchanged; a no-signal build fails with the message; the matrix compiles.
2. **Metrics: counter, up-down counter, gauge.** Leaf API, series table, both backends, nanopb metrics generation, golden vectors, the new reserved attributes and sequence number, unit and QEMU tests, footprint for the metrics-only profile.
3. **Histogram.** The optional kind: integer bounds, bucket search, encode in both backends, golden vectors, footprint with and without it, the kind-off compile check.
4. **Concentrator metrics ingest.** `IngestRequest::signal`, the metrics decoder and validation, per-boot start cache, sequence tracking, the pass-through queue, stats, config keys, fuzz target, collector conformance per backend and protocol (including the lost-payload repair test).
5. **Logs.** Leaf API, log buffer, UTF-8-safe truncation, `_in_span`, both backends, nanopb logs generation, golden vectors, unit and QEMU tests, footprint for logs-only and all-on.
6. **Concentrator logs ingest.** The logs decoder and validation, observed time, the log queue, stats, fuzz target, collector conformance per backend and protocol.

---

## 8. Questions for review

These could not be settled in the design; each has a recommendation.

- **Q1. Release split.** Do metrics and logs ship in the same minor (the milestone's), or metrics in the next minor and logs in the one after? *Recommendation:* metrics in the next minor, logs in the following one, both in this milestone; it matches "metrics released first".
- **Q2. Reverse §6.4?** Cumulative since boot with a cached start (recommended, §2.2 question 9), or keep delta and add only the sequence number.
- **Q3. Borrow the instrument table** instead of copying it (§2.2 question 1), an exception to §1.4's copy rule. *Recommendation:* borrow.
- **Q4. Integer-only counters and histograms.** No `double` counter, up-down counter or histogram values, to keep soft-float off the hot path; `double` gauges only. *Recommendation:* yes for the first release; a `double` variant can be added later behind its own function.
- **Q5. Kind defaults.** Counter, up-down counter and gauge on, histogram off, when `MICROTEL_LEAF_METRICS=1`. *Recommendation:* as proposed.
- **Q6. Log buffer full:** drop the newest record (recommended, §3.3), or evict the oldest.
- **Q7. Self-describing payloads.** `microtel.leaf.signal` on the wire and `IngestRequest::signal`, with a mismatch rejected (recommended), or the explicit field alone (saves about 22 bytes per payload).
- **Q8. Budgets.** The proposed per-profile budgets in §5, including "all on" at 18 KB, over the trace-only 15 KB target.
- **Q9. upb source builds.** For the upb backend the "not linked" guarantee relies on static-archive object selection or `--gc-sections`, not on guarded wrappers (§1.1), since the upb generated tables are shared with the main project. Acceptable, or should the leaf build its own per-signal upb generated archives?
- **Q10. Leaf metrics stay cumulative** even when the concentrator's Provider uses `Delta` or `LowMemory` (ICP 0050 Decision 6). Acceptable, given that converting would need per-series state?
