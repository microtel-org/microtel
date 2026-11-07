# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Compute statistics and write results.json + results.md."""

from __future__ import annotations

import dataclasses
import json
import statistics
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Statistics helpers
# ---------------------------------------------------------------------------

def _pct(values: list[float], p: float) -> float:
    """Return the p-th percentile (0..1) of a sorted or unsorted list."""
    if not values:
        return 0.0
    s = sorted(values)
    idx = (len(s) - 1) * p
    lo, hi = int(idx), min(int(idx) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (idx - lo)


def _stats(values: list[float]) -> dict[str, float]:
    if not values:
        return {"median": 0.0, "p25": 0.0, "p75": 0.0, "min": 0.0, "max": 0.0}
    return {
        "median": _pct(values, 0.5),
        "p25":    _pct(values, 0.25),
        "p75":    _pct(values, 0.75),
        "min":    min(values),
        "max":    max(values),
    }


def _received(sample: dict) -> int:
    """Items of the profile's signal the sink counted for one sample.

    items_received is spans for trace profiles and log records for logs
    profiles; results documents written before it existed fall back to
    sink.spans_received.
    """
    return sample.get("items_received", sample["sink"]["spans_received"])


def _drop_sum(samples: list[dict], key: str) -> int:
    return sum(s["spans_dropped"].get(key, 0) for s in samples)


# ---------------------------------------------------------------------------
# Build the results.json document
# ---------------------------------------------------------------------------

def build_results(
    profile_data: dict,
    env_data: dict,
    sut_results: list[dict],
    warnings: list[str],
) -> dict[str, Any]:
    """Assemble the full results.json document from collected data."""
    suts_out = []
    for sr in sut_results:
        samples = sr["samples"]
        suts_out.append({
            "name":                sr["name"],
            "library":             sr["library"],
            "transport":           sr["transport"],
            "library_version":     sr["library_version"],
            "library_build_flags": sr["library_build_flags"],
            "image_tag":           sr["image_tag"],
            "image_id":            sr["image_id"],
            "samples":             samples,
            "summary":             _summarize(samples),
            "cpu":                 sr.get("cpu"),
            "flamegraph_svg":      sr.get("flamegraph_svg"),
        })
    add_cpu_comparisons(suts_out)

    return {
        "schema_version": "1.0",
        "generated_at":   env_data["bench_started_at"],
        "warnings":       warnings,
        "profile":        profile_data,
        "environment":    env_data,
        "suts":           suts_out,
    }


_CPU_KEYS = ("total", "user", "system", "emit", "other")
_REFERENCE_SUT = "microtel"


def _ratio(value, ref):
    return value / ref if value is not None and ref else None


def add_cpu_comparisons(suts: list[dict]) -> None:
    """Add ratios to microtel and baseline-subtracted figures to each SUT's cpu.

    ratio_to_microtel divides each per-span figure and cores by the SUT named
    "microtel" in the same document. baseline_subtracted takes away the no-op
    baseline SUT's (library "none") total per span and cores, the cost of the
    harness's own loop; it is absent when either SUT is missing.
    """
    with_cpu = [s for s in suts if s.get("cpu") and s["cpu"].get("per_span_ns")]
    ref = next((s["cpu"] for s in with_cpu if s["name"] == _REFERENCE_SUT), None)
    base = next((s["cpu"] for s in with_cpu if s["library"] == "none"), None)
    for s in with_cpu:
        c = s["cpu"]
        if base is not None and s["library"] != "none":
            total = c["per_span_ns"]["total"]
            c["baseline_subtracted"] = {
                "baseline_sut": next(x["name"] for x in with_cpu if x["library"] == "none"),
                "total_ns_per_span": (total - base["per_span_ns"]["total"]
                                      if total is not None and base["per_span_ns"]["total"] is not None
                                      else None),
                "cores": (c["cores"] - base["cores"]
                          if c["cores"] is not None and base["cores"] is not None else None),
            }
    ref_sub = (ref or {}).get("baseline_subtracted", {})
    for s in with_cpu:
        if ref is None:
            break
        c = s["cpu"]
        ratios = {k: _ratio(c["per_span_ns"][k], ref["per_span_ns"][k]) for k in _CPU_KEYS}
        ratios["cores"] = _ratio(c["cores"], ref["cores"])
        sub = c.get("baseline_subtracted")
        if sub is not None and ref_sub:
            ratios["baseline_subtracted_total"] = _ratio(sub["total_ns_per_span"],
                                                         ref_sub["total_ns_per_span"])
            ratios["baseline_subtracted_cores"] = _ratio(sub["cores"], ref_sub["cores"])
        c["ratio_to_microtel"] = ratios


def _summarize(samples: list[dict]) -> dict[str, Any]:
    if not samples:
        return {}

    def _floats(key: str) -> list[float]:
        return [float(s[key]) for s in samples]

    drop_keys = ("queue_full", "record_too_large", "span_attribute_limit",
                 "attribute_value_truncated", "other", "total")

    # wire_bytes_per_span is only available in blackhole mode; None in collector mode.
    bytes_available = [s["sink"]["bytes_received"] for s in samples
                       if s["sink"]["bytes_received"] is not None]
    if bytes_available and len(bytes_available) == len(samples):
        wire_bytes = _stats([
            s["sink"]["bytes_received"] / max(_received(s), 1)
            for s in samples
        ])
    else:
        wire_bytes = None

    flush_vals = [float(s["flush_ns"]) for s in samples if s.get("flush_ns") is not None]
    tp_vals = [float(s["throughput_mbps"]) for s in samples
               if s.get("throughput_mbps") is not None]
    sps_vals = [float(s["spans_per_sec"]) for s in samples
                if s.get("spans_per_sec") is not None]

    return {
        "reps":                  len(samples),
        "spans_emitted_total":   sum(s["spans_emitted"] for s in samples),
        "spans_dropped":         {k: _drop_sum(samples, k) for k in drop_keys},
        "drop_rate_pct":         _drop_rate(samples),
        "delivery_rate_pct":     _delivery_rate_from_sink(samples),
        "latency_p50_ns":        _stats(_floats("latency_p50_ns")),
        "latency_p95_ns":        _stats(_floats("latency_p95_ns")),
        "latency_p99_ns":        _stats(_floats("latency_p99_ns")),
        "latency_min_ns":        _stats(_floats("latency_min_ns")),
        "latency_max_ns":        _stats(_floats("latency_max_ns")),
        "flush_ns":              _stats(flush_vals) if flush_vals else None,
        "spans_per_sec":         _stats(sps_vals) if sps_vals else None,
        "throughput_mbps":       _stats(tp_vals) if tp_vals else None,
        "wire_bytes_per_span":   wire_bytes,
    }


def _drop_rate(samples: list[dict]) -> float:
    # The SDK counts dropped *spans*, so the denominator is spans_expected —
    # spans actually sent — not spans_emitted, which counts workload
    # iterations and is 3x smaller on realistic-request.  Results documents
    # written before spans_expected existed fall back to it, exactly as
    # _delivery_rate_from_sink does.
    total_expected = sum(s.get("spans_expected", s["spans_emitted"]) for s in samples)
    total_dropped = sum(s["spans_dropped"]["total"] for s in samples)
    if total_expected == 0:
        return 0.0
    return round(total_dropped / total_expected * 100, 4)


def _delivery_rate_from_sink(samples: list[dict]):
    # If all per-sample delivery values are None the profile is non-trace (e.g. metrics)
    # and delivery is undefined — return None so callers can print N/A.
    if all(s.get("delivery_rate_pct") is None for s in samples):
        return None
    # spans_expected is the span count actually sent; spans_emitted counts
    # workload iterations, which is 3x smaller on realistic-request.  Results
    # documents written before spans_expected existed fall back to it.
    total_expected = sum(s.get("spans_expected", s["spans_emitted"]) for s in samples)
    total_received = sum(_received(s) for s in samples)
    if total_expected == 0:
        return 100.0
    return round(total_received / total_expected * 100, 4)


# ---------------------------------------------------------------------------
# Write outputs
# ---------------------------------------------------------------------------

def write_json(doc: dict, out_dir: Path, filename: str = "results.json") -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / filename
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    return path


def write_markdown(doc: dict, out_dir: Path, filename: str = "results.md") -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / filename
    path.write_text(_render_md(doc), encoding="utf-8")
    return path


def _fmt_ns(v) -> str:
    return f"{v:,.0f}" if v is not None else "—"


def _fmt_ratio(v) -> str:
    return f" ({v:.2f}×)" if v is not None else ""


def _cpu_rows(suts: list[dict]) -> list[str]:
    """Results-table rows for the CPU window, plus a note on source and window."""
    if not any(s.get("cpu") for s in suts):
        return []
    rows = []

    def row(label, cell):
        rows.append(f"| {label} | " + " | ".join(
            cell(s["cpu"]) if s.get("cpu") else "—" for s in suts) + " |")

    def per_span(key):
        return lambda c: (_fmt_ns(c["per_span_ns"][key])
                          + _fmt_ratio(c.get("ratio_to_microtel", {}).get(key)))

    row("CPU source", lambda c: c["source"].split()[0])
    row("CPU per span basis", lambda c: f"{c['per_span_basis']} ({c['spans']:,})")
    row("CPU ns/span, total (÷ microtel)", per_span("total"))
    row("CPU ns/span, user", per_span("user"))
    row("CPU ns/span, system", per_span("system"))
    row("CPU ns/span, emitting threads", per_span("emit"))
    row("CPU ns/span, other threads", per_span("other"))
    row("CPU cores used", lambda c: (f"{c['cores']:.3f}" if c["cores"] is not None else "—")
        + _fmt_ratio(c.get("ratio_to_microtel", {}).get("cores")))
    if any(s.get("cpu") and s["cpu"].get("baseline_subtracted") for s in suts):
        def sub(key, fmt, rkey):
            def cell(c):
                b = c.get("baseline_subtracted")
                if not b or b[key] is None:
                    return "—"
                return fmt(b[key]) + _fmt_ratio(c.get("ratio_to_microtel", {}).get(rkey))
            return cell
        row("CPU ns/span, total − baseline", sub("total_ns_per_span", _fmt_ns,
                                                  "baseline_subtracted_total"))
        row("CPU cores − baseline", sub("cores", lambda v: f"{v:.3f}", "baseline_subtracted_cores"))
    windows = sorted({f"{s['cpu']['window_ns'] / 1e9:.1f}" for s in suts if s.get("cpu")})
    rows += [
        "",
        "CPU: the SUT container's cgroup v2 `cpu.stat` (\"cgroup\"), or the process's "
        "`/proc/<pid>/stat` when that is unreadable (\"proc\"), over one window per SUT "
        f"({', '.join(windows)} s): from the end of warm-up to after the last sample's flush. "
        "Per span delivered at the sink (emitted, for the no-op baseline). Emitting threads are "
        "the process total minus the other threads' `/proc/<pid>/task/*/schedstat`. One run, "
        "no spread. See `bench/docs/methodology.md`.",
    ]
    return rows


def _render_md(doc: dict) -> str:
    lines = []
    env = doc.get("environment", {})
    profile = doc.get("profile", {})

    lines += ["# Benchmark Results", ""]
    if doc.get("label"):
        lines += [f"**{doc['label']}**", ""]
    lines += [
        "## Environment",
        "",
        f"| Field | Value |",
        f"|-------|-------|",
        f"| CPU | {env.get('cpu_model', '?')} |",
        f"| Physical cores | {env.get('cpu_physical_cores', '?')} |",
        f"| Threads | {env.get('cpu_threads', '?')} |",
        f"| CPU governor | {env.get('cpu_governor', '?')} |",
        f"| Hyperthreading | {env.get('hyperthreading', '?')} |",
        f"| Kernel | {env.get('kernel', '?')} |",
        f"| Container engine | {env.get('container_engine', '?')} {env.get('container_engine_version', '')} |",
        f"| Load avg (1m) | {env.get('host_load_avg_1m', '?')} |",
        f"| Started | {env.get('bench_started_at', '?')} |",
        "",
    ]

    if doc.get("warnings"):
        lines += ["## Warnings", ""]
        for w in doc["warnings"]:
            lines.append(f"- ⚠ {w}")
        lines.append("")

    lines += [
        "## Profile",
        "",
        f"| Field | Value |",
        f"|-------|-------|",
        f"| Name | {profile.get('name', '?')} |",
        f"| Spans/sample | {profile.get('spans_per_sample', '?')} |",
        f"| Samples | {profile.get('samples', '?')} |",
        f"| Warmup spans | {profile.get('warmup_spans', '?')} |",
        f"| Sink | {profile.get('sink_mode', '?')} |",
        "",
        "## Results",
        "",
    ]

    suts = doc.get("suts", [])
    if suts:
        # Header row
        sut_names = [s["name"] for s in suts]
        lines.append("| Metric | " + " | ".join(sut_names) + " |")
        lines.append("|--------|" + "|".join(["--------"] * len(suts)) + "|")

        def _row(label: str, key: str, fmt=lambda v: str(v)):
            cells = []
            for s in suts:
                summary = s.get("summary", {})
                val = summary.get(key, {})
                if isinstance(val, dict):
                    med = val.get("median", 0)
                    p25 = val.get("p25", 0)
                    p75 = val.get("p75", 0)
                    cells.append(f"{fmt(med)} [{fmt(p25)}–{fmt(p75)}]")
                else:
                    cells.append(fmt(val) if val is not None else "—")
            lines.append(f"| {label} | " + " | ".join(cells) + " |")

        # JSON keys keep their span names; for signal=logs they count log
        # records, and the row labels say so.
        is_logs = profile.get("signal") == "logs"
        op = "Emit" if is_logs else "StartSpan"
        unit = "record" if is_logs else "span"
        _row(f"{op} p50 (ns)",       "latency_p50_ns", lambda v: f"{v:.0f}")
        _row(f"{op} p95 (ns)",       "latency_p95_ns", lambda v: f"{v:.0f}")
        _row(f"{op} p99 (ns)",       "latency_p99_ns", lambda v: f"{v:.0f}")
        _row("Flush latency p50 (ns)", "flush_ns",      lambda v: f"{v:.0f}")
        _row(f"{unit.capitalize()}s/sec", "spans_per_sec", lambda v: f"{v:,.0f}")
        _row("Throughput (Mbps)",     "throughput_mbps", lambda v: f"{v:.1f}")
        # Delivery rate row — computed from sink vs emitted, works even when SUT drop counters are unavailable.
        dr_cells = []
        for s in suts:
            dr = s.get("summary", {}).get("delivery_rate_pct")
            dr_cells.append(f"{dr:.2f}%" if dr is not None else "N/A")
        lines.append(f"| Delivery rate (sink/emitted) | " + " | ".join(dr_cells) + " |")
        _row(f"Wire bytes/{unit}",    "wire_bytes_per_span", lambda v: f"{v:.1f}")

        for s in suts:
            d = s.get("summary", {}).get("spans_dropped", {})
            lines.append(
                f"| {unit.capitalize()}s dropped (total) | " +
                " | ".join(str(ss.get("summary", {}).get("spans_dropped", {}).get("total", 0))
                            for ss in suts) + " |"
            )
            break  # one row covers all

        lines += _cpu_rows(suts)

    # Flamegraph links — only rendered when --flamegraph produced SVGs.
    svg_suts = [s for s in suts if s.get("flamegraph_svg")]
    if svg_suts:
        lines += ["", "## Flamegraphs", ""]
        lines.append(
            "SVGs captured with `perf record -F 99 --call-graph fp` inside "
            "each SUT container (`:perf` image variant, `--cap-add=PERFMON`). "
            "Dedicated profiling sample recorded after all measurement samples."
        )
        lines.append("")
        for s in svg_suts:
            svg_abs = s["flamegraph_svg"]
            # Link relative to results.md (both live in out_dir).
            svg_rel = Path(svg_abs).name
            lines.append(f"- [{s['name']}]({svg_rel})")
        lines.append("")

    lines += [
        "",
        "---",
        "",
        "_Generated by microtel bench driver. "
        "See [`results.json`](results.json) for full data, "
        "[`plots.html`](plots.html) for interactive charts (requires plotly), "
        "and `docs/bench-spec.md` for methodology._",
    ]

    return "\n".join(lines) + "\n"
