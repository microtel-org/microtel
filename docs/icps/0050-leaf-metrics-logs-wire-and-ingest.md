# ICP 0050: Leaf metrics and logs — wire contract and concentrator ingest

**Status:** Draft — 2026-11-27. Milestone "Leaf: metrics and logs (after v1.3)"; issues #542, #543. Builds on [ICP 0049](0049-leaf-metrics-logs-scope-and-build-selection.md).
**Affected interfaces / docs:**
- `docs/leaf-concentrator-design.md` §6.4 (**reversed**: leaf metrics are cumulative since boot, not delta), §3.8 (new reserved attributes), §3.4 (validation per signal)
- [ICP 0036](0036-custom-export-transport.md) Decision 3 (**lifted**: metrics and logs payloads may be passed to `LeafReceiver::Ingest`)
- `include/microtel/leaf_receiver.hpp` (**public API**, [ICP 0034](0034-leaf-receiver-api.md)): `IngestRequest::signal`; per-signal counts in `IngestResult` and `LeafReceiverStats`
- `docs/configuration.md` §3.14 (new `[concentrator]` limits), `docs/error-model.md` §3 (units of the existing `leaf_payload_*` reasons)
- new internal decoder interfaces for metrics and logs under `include/microtel/internal/` (additive)

**Affected tracks:** the leaf (C), the SDK's concentrator (A), the wire decoder (F).

## Summary

Leaf metrics are sent cumulative since boot, with a start time the concentrator fixes once per leaf boot; metrics and logs payloads carry a sequence number so the concentrator can count lost payloads; and `LeafReceiver::Ingest` takes metrics and logs payloads, forwarding them as received.

## Motivation

§6.4 required delta leaf metrics so the concentrator would hold no per-series state. Over an unreliable link (UDP, BLE) a lost delta payload loses its increments for good, and nothing notices: the counter under-reports from then on (#542 question 9). The two §6.4 objections to cumulative can both be met without per-series state at the concentrator, as the design shows. ICP 0036 Decision 3 forbade metrics and logs payloads because the concentrator could not ingest them; this ICP is what lets it.

## Decision

1. **Cumulative since boot, not delta.** Every leaf Sum point (counter, up-down counter) and Histogram point is `AGGREGATION_TEMPORALITY_CUMULATIVE`, its value counted from the leaf's metrics attach. A lost payload is made up by the next one. §6.4 is reversed.
2. **A stable start time, cached per leaf boot.** A point's `start_time_unix_nano` is the attach time. The concentrator maps it to Unix time **once per (leaf, boot)** and caches the result in the existing leaf-table entry (§4.5), so the start does not jitter with link latency and a backend sees one series, not a new one per payload. A new boot (a changed `microtel.leaf.boot_id`, or a sequence number that goes backwards) re-maps it, which a backend reads as a reset. The concentrator keeps **no per-series state**; the cache is per leaf, bounded by `max_leaves`.
3. **A sequence number on metrics and logs payloads.** `microtel.leaf.seq` counts successful encodes per signal since boot, from 1. The concentrator keeps the last value per (leaf, signal) in the leaf-table entry and counts the gap in `LeafReceiverStats::payloads_lost`. Trace payloads do not carry it, so trace bytes are unchanged.
4. **Wire contract version 2 for the new payloads.** Metrics and logs payloads carry `microtel.leaf.proto = 2` and `microtel.leaf.signal` (1 = metrics, 2 = logs). Trace payloads keep version 1 and their bytes. A v1.3 concentrator rejects a version-2 payload as `Malformed`, which is the intended behaviour.
5. **`Ingest` takes every signal.** `IngestRequest` gains `LeafSignal signal = LeafSignal::Traces`, so existing callers are unchanged. A payload whose `microtel.leaf.signal` disagrees with `signal` is `Malformed`. ICP 0036 Decision 3's prohibition is lifted.
6. **Forward as received.** The concentrator applies the leaf's Resource (§4.4) and time correction (§5) to each point or record and forwards it; it never re-aggregates. Log records enter the `BatchLogRecordProcessor` queue, as decoded spans enter the span queue. Metric points enter a bounded pass-through queue that the periodic reader drains on its next collection, one `ResourceMetrics` per leaf. Leaf points keep their cumulative temporality whatever the Provider's `TemporalityPreference`.
7. **Limits and counts.** New `[concentrator]` limits `max_metric_points_per_payload` (512), `max_log_records_per_payload` (512) and `max_queued_leaf_metric_points` (8192). Payload-level failures reuse `leaf_payload_malformed` / `_too_large` / `leaf_unknown` (unit: payloads, any signal); a full pass-through queue counts `queue_full` (unit: points). No new `DropReason`. `IngestResult` and `LeafReceiverStats` gain per-signal accepted and dropped counts, `payloads_lost` and `leaf_restarts`.

## Migration

- **Trace-only leaves and concentrators:** nothing changes on the wire or in the API.
- **Applications that bridge metrics or logs from an `ExportTransport` (ICP 0036) to a concentrator:** set `IngestRequest::signal`. A full node's payloads have no `microtel.leaf.*` keys and are accepted from a sender configured `unix`, as traces are.
- **Backends:** leaf counters and histograms arrive cumulative, with a start time that changes only when the leaf reboots.

## Rationale & alternatives

- **Delta plus a sequence number (§6.4 as it stands).** Loss becomes visible but is never repaired, and up-down counters arrive as non-monotonic deltas, which many backends handle badly. It does let the leaf free a series after each encode, which matters only for high-cardinality leaves; the leaf's series table is sized at build time for fixed, low-cardinality fleet metrics. Rejected in favour of 1–3; the sequence number is kept anyway, because traces and logs cannot heal and loss should be visible.
- **Cumulative on the leaf, converted to delta at the concentrator.** Rejected: it needs the previous value of every series, which §4.5 exists to prevent.
- **Recompute the start time on every payload.** Rejected: the §6.4 objection, a new series per payload in two of the three time modes.
- **A one-byte signal prefix instead of `microtel.leaf.signal`.** Rejected: the leaf does not frame, and the payload stays plain OTLP.
- **A sequence number on trace payloads too.** Deferred: it would change trace bytes, which ICP 0049 keeps fixed. It can be added later behind version 2.
