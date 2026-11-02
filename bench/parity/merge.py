#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Merge bench/parity/run_rounds.sh output into one table (bench/parity/README.md).

Usage:  python3 -I bench/parity/merge.py <out-dir> [reference-sut] > table.md

Pools every sample of a SUT across all rounds, as the published snapshot does
(docs/bench-results/README.md: 50 samples per SUT from five runs), and reports
the median of the per-sample p50 and p95 span-emit latencies with their
interquartile range, and the median spans/s. Also writes <out-dir>/merged.json.
"""

import json
import pathlib
import statistics
import sys


def quartiles(xs):
    xs = sorted(xs)
    if len(xs) < 2:
        return xs[0], xs[0], xs[0]
    q = statistics.quantiles(xs, n=4, method="inclusive")
    return q[0], statistics.median(xs), q[2]


def main():
    out = pathlib.Path(sys.argv[1])
    ref = sys.argv[2] if len(sys.argv) > 2 else None
    pooled, meta, rounds = {}, {}, {}
    # A round in which any SUT failed (run_rounds.sh marks it) is dropped for
    # every SUT, so each SUT's pool comes from the same rounds.
    dropped = sorted(p.parent.name for p in out.glob("round*/FAILED"))
    if dropped:
        print(f"Dropped rounds (a SUT failed): {', '.join(dropped)}\n")
    for res in sorted(out.glob("round*/*/results.json")):
        if res.parent.parent.name in dropped:
            continue
        data = json.loads(res.read_text())
        rnd = res.parent.parent.name
        for sut in data.get("suts", []):
            name = sut["name"]
            pooled.setdefault(name, []).extend(sut["samples"])
            rounds.setdefault(name, []).append(
                {"round": rnd, "samples": len(sut["samples"]),
                 "bench_started_at": data.get("environment", {}).get("bench_started_at")
                 or data.get("generated_at"),
                 "load_avg_1m": data.get("environment", {}).get("load_avg_1m")})
            meta[name] = {"image_id": sut.get("image_id"), "image_tag": sut.get("image_tag")}
    rows = {}
    for name, samples in pooled.items():
        p50 = quartiles([s["latency_p50_ns"] for s in samples])
        p95 = quartiles([s["latency_p95_ns"] for s in samples])
        # None when nothing reaches the sink (the in-memory exporter variant).
        rates = [s["spans_per_sec"] for s in samples if s["spans_per_sec"] is not None]
        sps = statistics.median(rates) if rates else None
        drops = sum(s["spans_dropped"]["total"] for s in samples)
        rows[name] = {"n": len(samples), "rounds": len(rounds[name]), "p50": p50, "p95": p95,
                      "spans_per_sec": sps, "dropped": drops, **meta[name]}
    refp50 = rows[ref]["p50"][1] if ref in rows else None
    print("| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | Spans/s | "
          + (f"p50 ÷ {ref} |" if refp50 else "|"))
    print("|---|---:|---:|---:|---:|" + ("---:|" if refp50 else ""))
    for name in sorted(rows, key=lambda k: rows[k]["p50"][1]):
        r = rows[name]
        lo, med, hi = r["p50"]
        cell = f"| {name} | {r['n']} ({r['rounds']} rounds) | **{med:.0f}** ({lo:.0f}–{hi:.0f}) | " \
               f"{r['p95'][1]:.0f} | " \
               + (f"{r['spans_per_sec']:,.0f}" if r["spans_per_sec"] is not None else "–") + " |"
        if refp50:
            cell += f" {med / refp50:.2f}× |"
        print(cell)
    (out / "merged.json").write_text(
        json.dumps({"rows": rows, "rounds": rounds, "dropped_rounds": dropped}, indent=2))


if __name__ == "__main__":
    main()
