# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Generate interactive Plotly charts from a results.json document.

Plotly is an optional dependency.  If it is not installed, write_plots()
returns None gracefully rather than raising.

Install:  pip install plotly

Output: a single plots.html (Plotly CDN JS) written to out_dir alongside
results.json and results.md.

Charts produced (four separate figures, no subplots):

  1. Latency comparison — grouped bar chart, X = SUT, bars for p50/p95/p99,
     error bars = IQR (p25 → p75) across samples.  At-a-glance comparison
     that maps directly to the summary block.

  2. Latency CDF — X = latency ns (log scale), Y = cumulative %, one line
     per SUT overlaid on shared axes.  Built from the raw 64-bucket histogram
     in each sample (aggregated across all samples).  The detailed view;
     crossings show exactly where one SUT beats another.
     Requires latency_histogram in results.json (present from B3 onward).
     If absent, chart is skipped and a warning is printed.

  3. Run stability — X = sample index, Y = p50 ns, one line per SUT.
     Catches drift, throttling, and warmup bleed.

  4. Wire bytes / span — grouped bar chart, X = SUT, Y = median bytes/span.
     Omitted when byte data is unavailable (collector sink mode).
     Visual correctness gate: microtel SUTs must have identical bytes/span
     for the same payload.  Divergence > 5% is flagged in red with an
     annotation; agreement is confirmed in green.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Optional

_PLOTLY_CDN = "https://cdn.plot.ly/plotly-latest.min.js"

# Colour palette shared across charts.
_COL_P50 = "#1f77b4"
_COL_P95 = "#ff7f0e"
_COL_P99 = "#d62728"
_COL_OK  = "#2ca02c"
_COL_ERR = "#d62728"


def write_plots(doc: dict[str, Any], out_dir: Path) -> Optional[Path]:
    """Build plots.html from a results document; return its path or None.

    Returns None (without raising) when plotly is not installed or the
    document contains no usable SUT data.
    """
    try:
        import plotly.graph_objects as go  # noqa: F401 — presence check only
    except ImportError:
        _warn("plotly not installed — skipping HTML charts "
              "(pip install plotly to enable)")
        return None

    suts = doc.get("suts", [])
    if not suts or not any(s.get("samples") for s in suts):
        return None

    figures: list[tuple[str, Any]] = []  # (title, go.Figure)

    figures.append(("Latency comparison", _latency_bar(suts)))
    cdf = _latency_cdf(suts)
    if cdf is not None:
        figures.append(("Latency CDF", cdf))
    else:
        _warn("CDF chart skipped: latency_histogram not present in results.json. "
              "This field is populated from emit-app B3+ builds. "
              "Re-run the harness with an updated SUT image to enable the CDF.")

    figures.append(("Run stability", _stability(suts)))
    wire = _wire_bytes(suts)
    if wire is not None:
        figures.append(("Wire bytes / span", wire))
    sps = _spans_per_sec(suts)
    if sps is not None:
        figures.append(("Spans/sec", sps))
    tp = _throughput(suts)
    if tp is not None:
        figures.append(("Throughput (Mbps)", tp))
    flush = _flush_latency(suts)
    if flush is not None:
        figures.append(("Flush latency", flush))

    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "plots.html"
    path.write_text(_assemble_html(figures, doc), encoding="utf-8")
    return path


# ---------------------------------------------------------------------------
# Chart 1 — Latency comparison (grouped bar + IQR error bars)
# ---------------------------------------------------------------------------

def _has_byte_data(suts: list[dict]) -> bool:
    """Return True if any SUT has wire_bytes_per_span summary data."""
    return any(
        bool(s.get("summary", {}).get("wire_bytes_per_span"))
        for s in suts
    )


def _latency_bar(suts: list[dict]) -> Any:
    import plotly.graph_objects as go

    fig = go.Figure()
    for key, label, color in [
        ("latency_p50_ns", "p50", _COL_P50),
        ("latency_p95_ns", "p95", _COL_P95),
        ("latency_p99_ns", "p99", _COL_P99),
    ]:
        xs, ys, err_plus, err_minus = [], [], [], []
        for sut in suts:
            stats = sut.get("summary", {}).get(key, {})
            if not stats:
                continue
            xs.append(sut["name"])
            med = stats["median"]
            ys.append(med)
            err_plus.append(stats["p75"] - med)
            err_minus.append(med - stats["p25"])

        if not xs:
            continue
        fig.add_trace(go.Bar(
            name=label,
            x=xs,
            y=ys,
            marker_color=color,
            error_y=dict(
                type="data",
                symmetric=False,
                array=err_plus,
                arrayminus=err_minus,
                thickness=1.5,
                width=4,
            ),
        ))

    fig.update_layout(
        title="Latency comparison — median ± IQR across samples",
        xaxis_title="SUT",
        yaxis_title="nanoseconds",
        barmode="group",
        template="plotly_white",
        legend=dict(orientation="h", y=-0.15),
    )
    return fig


# ---------------------------------------------------------------------------
# Chart 2 — Latency CDF (requires latency_histogram in samples)
# ---------------------------------------------------------------------------

def _latency_cdf(suts: list[dict]) -> Optional[Any]:
    """Return a CDF figure, or None if no sample has latency_histogram data."""
    import plotly.graph_objects as go

    any_histogram = any(
        bool(s.get("latency_histogram"))
        for sut in suts
        for s in sut.get("samples", [])
    )
    if not any_histogram:
        return None

    fig = go.Figure()
    for sut in suts:
        # Aggregate bucket counts across all samples for this SUT.
        agg: list[int] = [0] * 64
        for sample in sut.get("samples", []):
            hist = sample.get("latency_histogram", [])
            for i, count in enumerate(hist[:64]):
                agg[i] += count

        xs, ys = _build_cdf_points(agg)
        if not xs:
            continue

        fig.add_trace(go.Scatter(
            x=xs,
            y=ys,
            mode="lines",
            name=sut["name"],
            line=dict(width=2),
        ))

    fig.update_layout(
        title="Latency CDF — aggregated across all samples",
        xaxis_title="latency (ns, log scale)",
        yaxis_title="cumulative %",
        xaxis_type="log",
        template="plotly_white",
        legend=dict(orientation="h", y=-0.15),
    )
    fig.update_yaxes(range=[0, 100])
    return fig


def _build_cdf_points(buckets: list[int]) -> tuple[list[int], list[float]]:
    """Convert power-of-2 bucket counts to (x_ns, cumulative_pct) series.

    Bucket i covers [2^i, 2^(i+1)) ns (bucket 0 covers [0, 2) ns).
    The representative x value is the midpoint of the bucket's range.
    """
    total = sum(buckets)
    if total == 0:
        return [], []
    xs: list[int] = []
    ys: list[float] = []
    cumulative = 0
    for i, count in enumerate(buckets):
        if count == 0:
            continue
        lo = 0 if i == 0 else (1 << i)
        hi = 1 << (i + 1)
        midpoint = (lo + hi) // 2
        cumulative += count
        xs.append(midpoint)
        ys.append(cumulative / total * 100.0)
    return xs, ys


# ---------------------------------------------------------------------------
# Chart 3 — Run stability (p50 per sample index)
# ---------------------------------------------------------------------------

def _stability(suts: list[dict]) -> Any:
    import plotly.graph_objects as go

    fig = go.Figure()
    for sut in suts:
        samples = sut.get("samples", [])
        if not samples:
            continue
        fig.add_trace(go.Scatter(
            x=list(range(1, len(samples) + 1)),
            y=[float(s["latency_p50_ns"]) for s in samples],
            mode="lines+markers",
            name=sut["name"],
            line=dict(width=2),
            marker=dict(size=6),
        ))

    fig.update_layout(
        title="Run stability — p50 per sample",
        xaxis_title="sample #",
        yaxis_title="p50 (ns)",
        xaxis=dict(dtick=1),
        template="plotly_white",
        legend=dict(orientation="h", y=-0.15),
    )
    return fig


# ---------------------------------------------------------------------------
# Chart 4 — Wire bytes / span with correctness gate
# ---------------------------------------------------------------------------

def _wire_bytes(suts: list[dict]) -> Optional[Any]:
    """Return a wire-bytes bar chart, or None when byte data is unavailable."""
    import plotly.graph_objects as go

    names: list[str] = []
    medians: list[float] = []
    microtel_bytes: dict[str, float] = {}

    for sut in suts:
        wb = sut.get("summary", {}).get("wire_bytes_per_span")
        if not wb or not isinstance(wb, dict) or not wb.get("median"):
            continue
        names.append(sut["name"])
        medians.append(wb["median"])
        if sut.get("library") == "microtel":
            microtel_bytes[sut["name"]] = wb["median"]

    if not names:
        return None

    # Correctness gate: all microtel SUTs must agree on bytes/span (±5%).
    divergence_pct: Optional[float] = None
    if len(microtel_bytes) >= 2:
        vals = list(microtel_bytes.values())
        mean = sum(vals) / len(vals)
        if mean > 0:
            divergence_pct = max(abs(v - mean) / mean for v in vals) * 100.0

    divergence_flagged = divergence_pct is not None and divergence_pct > 5.0

    bar_colors = []
    for name in names:
        if name in microtel_bytes and divergence_flagged:
            bar_colors.append(_COL_ERR)
        else:
            bar_colors.append(_COL_P50)

    fig = go.Figure()
    fig.add_trace(go.Bar(
        x=names,
        y=medians,
        marker_color=bar_colors,
        showlegend=False,
    ))

    # Correctness annotation.
    if divergence_flagged:
        assert divergence_pct is not None
        fig.add_annotation(
            text=(f"⚠ microtel wire-bytes divergence "
                  f"{divergence_pct:.1f}% > 5% — correctness check FAILED"),
            xref="paper", yref="paper",
            x=0.5, y=1.10,
            showarrow=False,
            font=dict(color=_COL_ERR, size=13, family="monospace"),
            bgcolor="rgba(255,200,200,0.8)",
        )
    elif len(microtel_bytes) >= 2:
        fig.add_annotation(
            text="✓ microtel wire-bytes consistent across exporters",
            xref="paper", yref="paper",
            x=0.5, y=1.10,
            showarrow=False,
            font=dict(color=_COL_OK, size=12),
        )

    fig.update_layout(
        title="Wire bytes / span — median",
        xaxis_title="SUT",
        yaxis_title="bytes",
        template="plotly_white",
        margin=dict(t=80),
    )
    return fig


# ---------------------------------------------------------------------------
# Chart 5 — Spans/sec (median ± IQR)
# ---------------------------------------------------------------------------

def _spans_per_sec(suts: list[dict]) -> Optional[Any]:
    """Return a spans/sec bar chart, or None when data is unavailable."""
    import plotly.graph_objects as go

    names: list[str] = []
    medians: list[float] = []
    err_plus: list[float] = []
    err_minus: list[float] = []

    for sut in suts:
        sps = sut.get("summary", {}).get("spans_per_sec")
        if not sps or not isinstance(sps, dict):
            continue
        med = sps.get("median", 0.0)
        if med == 0.0:
            continue
        names.append(sut["name"])
        medians.append(med)
        err_plus.append(sps.get("p75", med) - med)
        err_minus.append(med - sps.get("p25", med))

    if not names:
        return None

    fig = go.Figure(go.Bar(
        x=names,
        y=medians,
        marker_color=_COL_P50,
        showlegend=False,
        error_y=dict(
            type="data",
            symmetric=False,
            array=err_plus,
            arrayminus=err_minus,
            thickness=1.5,
            width=4,
        ),
    ))
    fig.update_layout(
        title="Spans/sec — median ± IQR (wire-delivered spans / (emit+flush) duration)",
        xaxis_title="SUT",
        yaxis_title="spans/sec",
        template="plotly_white",
    )
    return fig


# ---------------------------------------------------------------------------
# Chart 6 — Throughput (Mbps, median ± IQR)
# ---------------------------------------------------------------------------

def _throughput(suts: list[dict]) -> Optional[Any]:
    """Return a throughput bar chart (Mbps), or None when data is unavailable."""
    import plotly.graph_objects as go

    names: list[str] = []
    medians: list[float] = []
    err_plus: list[float] = []
    err_minus: list[float] = []

    for sut in suts:
        tp = sut.get("summary", {}).get("throughput_mbps")
        if not tp or not isinstance(tp, dict):
            continue
        med = tp.get("median", 0.0)
        if med == 0.0:
            continue
        names.append(sut["name"])
        medians.append(med)
        err_plus.append(tp.get("p75", med) - med)
        err_minus.append(med - tp.get("p25", med))

    if not names:
        return None

    fig = go.Figure(go.Bar(
        x=names,
        y=medians,
        marker_color=_COL_OK,
        showlegend=False,
        error_y=dict(
            type="data",
            symmetric=False,
            array=err_plus,
            arrayminus=err_minus,
            thickness=1.5,
            width=4,
        ),
    ))
    fig.update_layout(
        title="Throughput — median Mbps ± IQR (sink bytes_received / sample duration)",
        xaxis_title="SUT",
        yaxis_title="Mbps",
        template="plotly_white",
    )
    return fig


# ---------------------------------------------------------------------------
# Chart 6 — Flush latency (grouped bar: p50/p95/p99 across samples)
# ---------------------------------------------------------------------------

def _flush_latency(suts: list[dict]) -> Optional[Any]:
    """Return a flush-latency bar chart, or None when flush data is unavailable."""
    import plotly.graph_objects as go

    any_flush = any(
        bool(s.get("summary", {}).get("flush_ns"))
        for s in suts
    )
    if not any_flush:
        return None

    fig = go.Figure()
    for pct, label, color in [(0.50, "p50", _COL_P50),
                               (0.95, "p95", _COL_P95),
                               (0.99, "p99", _COL_P99)]:
        xs, ys = [], []
        for sut in suts:
            vals = sorted(
                float(s["flush_ns"])
                for s in sut.get("samples", [])
                if s.get("flush_ns") is not None
            )
            if not vals:
                continue
            idx = (len(vals) - 1) * pct
            lo, hi = int(idx), min(int(idx) + 1, len(vals) - 1)
            val = vals[lo] + (vals[hi] - vals[lo]) * (idx - lo)
            xs.append(sut["name"])
            ys.append(val)
        if xs:
            fig.add_trace(go.Bar(name=label, x=xs, y=ys, marker_color=color))

    fig.update_layout(
        title="Flush latency — batch exporter ForceFlush() duration",
        xaxis_title="SUT",
        yaxis_title="nanoseconds",
        barmode="group",
        template="plotly_white",
        legend=dict(orientation="h", y=-0.15),
    )
    return fig


# ---------------------------------------------------------------------------
# HTML assembly
# ---------------------------------------------------------------------------

def _assemble_html(figures: list[tuple[str, Any]], doc: dict) -> str:
    """Concatenate figures into a single HTML file with one CDN script tag."""
    profile = doc.get("profile", {})
    env = doc.get("environment", {})
    title = (
        f"microtel bench — {profile.get('name', 'results')}  "
        f"({profile.get('spans_per_sample', '?')} spans/sample "
        f"× {profile.get('samples', '?')} samples)"
    )
    cpu = env.get("cpu_model", "")
    started = env.get("bench_started_at", "")

    divs = "\n".join(
        fig.to_html(full_html=False, include_plotlyjs=False)
        for _, fig in figures
    )

    return (
        f'<!DOCTYPE html>\n'
        f'<html lang="en">\n'
        f'<head>\n'
        f'  <meta charset="utf-8">\n'
        f'  <meta name="viewport" content="width=device-width,initial-scale=1">\n'
        f'  <title>{title}</title>\n'
        f'  <script src="{_PLOTLY_CDN}"></script>\n'
        f'</head>\n'
        f'<body style="font-family:sans-serif;max-width:1100px;margin:0 auto;padding:1em">\n'
        f'  <h1 style="font-size:1.3em">{title}</h1>\n'
        f'  <p style="color:#555;font-size:.85em">{cpu}  {started}</p>\n'
        f'  {divs}\n'
        f'</body>\n'
        f'</html>\n'
    )


def _warn(msg: str) -> None:
    print(f"[plots] {msg}", file=sys.stderr, flush=True)
