# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Interleaved rounds of one profile across several SUTs, for the CPU figures.

    python3 -m driver.rounds --profile fixed-rate-1k \\
        --sut microtel,otelcpp-http,baseline-noop --rounds 5 --out results/X

Each round runs every SUT once, one driver run (``python3 -m driver``) each,
the order rotated by one per round. Before every run it waits until the
1-minute load average is below ``--max-load`` (default 0.5), for at most
``--load-timeout`` seconds (default 300), then records the load and goes on
either way. Each round's baseline (the SUT whose library is "none") is
subtracted from the other SUTs of the same round, and the ratio to microtel is
taken within the round; ``summary.json`` and ``summary.md`` give the median
and range over rounds. See bench/docs/methodology.md.
"""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Optional

from .container import build_image, detect_engine
from .registry import get as get_sut
from .registry import load as load_registry

_BENCH_DIR = Path(__file__).parent.parent
_REGISTRY_PATH = _BENCH_DIR / "sut" / "registry.yaml"
_REFERENCE_SUT = "microtel"
_LOAD_POLL_S = 5.0


def run_order(suts: list[str], rounds: int) -> list[list[str]]:
    """Each round's SUT order: all of @p suts, rotated by one per round."""
    n = len(suts)
    return [[suts[(r + i) % n] for i in range(n)] for r in range(rounds)]


def read_loadavg(path: Path = Path("/proc/loadavg")) -> float:
    """The 1-minute load average."""
    return float(path.read_text().split()[0])


def wait_for_load(max_load: float, timeout_s: float,
                  read_load: Callable[[], float] = read_loadavg,
                  sleep: Callable[[float], None] = time.sleep,
                  clock: Callable[[], float] = time.monotonic) -> dict:
    """Wait until the 1-minute load average is below @p max_load.

    Returns ``{"load", "waited_s", "timed_out"}``: the last reading, how long
    it waited, and whether it gave up after @p timeout_s.
    """
    start = clock()
    load = read_load()
    while load >= max_load:
        if clock() - start >= timeout_s:
            return {"load": load, "waited_s": clock() - start, "timed_out": True}
        sleep(_LOAD_POLL_S)
        load = read_load()
    return {"load": load, "waited_s": clock() - start, "timed_out": False}


def _cpu_of(results_json: Path) -> Optional[dict]:
    """The one SUT's cpu block and span rate from a driver run's results.json."""
    if not results_json.exists():
        return None
    doc = json.loads(results_json.read_text())
    if not doc.get("suts") or not doc["suts"][0].get("cpu"):
        return None
    sut = doc["suts"][0]
    cpu = sut["cpu"]
    paced = [s for s in sut.get("samples", []) if s.get("span_rate_achieved") is not None]
    own = cpu.get("emit_self") or {}
    return {
        "library": sut["library"],
        "ns_per_span": cpu["per_span_ns"]["total"],
        "cores": cpu["cores"],
        "emit_sub_ns_per_span": cpu["per_span_ns"]["emit"],
        "emit_self_ns_per_span": (own.get("per_span_ns") or {}).get("total"),
        "span_rate_target": paced[0]["span_rate_target"] if paced else None,
        "span_rate_achieved": (sum(s["span_rate_achieved"] for s in paced) / len(paced)
                               if paced else None),
    }


_SUBTRACTED = ("ns_per_span", "cores", "emit_self_ns_per_span")


def per_round(runs: dict[str, Optional[dict]]) -> dict[str, dict]:
    """One round's figures per SUT, with that round's baseline subtracted.

    @p runs maps SUT name to _cpu_of()'s result (None for a failed run).
    Adds ``<key>_minus_baseline`` for each of _SUBTRACTED, and
    ``ratio_<key>_minus_baseline`` to microtel's figure in the same round.
    """
    base = next((r for r in runs.values() if r and r["library"] == "none"), None)
    out: dict[str, dict] = {}
    for name, r in runs.items():
        if r is None:
            continue
        row = dict(r)
        if base is not None and r["library"] != "none":
            for k in _SUBTRACTED:
                row[f"{k}_minus_baseline"] = (r[k] - base[k] if r[k] is not None
                                              and base[k] is not None else None)
        out[name] = row
    ref = out.get(_REFERENCE_SUT)
    for row in out.values():
        for k in _SUBTRACTED:
            key = f"{k}_minus_baseline"
            if ref is not None and row.get(key) is not None and ref.get(key):
                row[f"ratio_{key}"] = row[key] / ref[key]
    return out


def median_range(values: list) -> Optional[dict]:
    """``{"median", "min", "max", "n"}`` of the non-None @p values, or None."""
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return {"median": statistics.median(vals), "min": min(vals), "max": max(vals), "n": len(vals)}


_SUMMARY_KEYS = (
    "ns_per_span", "cores", "emit_sub_ns_per_span", "emit_self_ns_per_span",
    "ns_per_span_minus_baseline", "cores_minus_baseline", "emit_self_ns_per_span_minus_baseline",
    "ratio_ns_per_span_minus_baseline", "ratio_cores_minus_baseline",
    "ratio_emit_self_ns_per_span_minus_baseline", "span_rate_target", "span_rate_achieved",
)


def summarize(rounds: list[dict[str, dict]], suts: list[str]) -> dict[str, dict]:
    """Median and range over @p rounds (per_round() outputs) of each SUT's figures."""
    return {name: {k: median_range([r[name].get(k) for r in rounds if name in r])
                   for k in _SUMMARY_KEYS}
            for name in suts}


def _fmt(mr: Optional[dict], fmt: str, suffix: str = "") -> str:
    if mr is None:
        return "—"
    return (f"{format(mr['median'], fmt)}{suffix} "
            f"({format(mr['min'], fmt)}–{format(mr['max'], fmt)})")


_MD_ROWS = (
    ("ns/span, total", "ns_per_span", ",.0f", ""),
    ("ns/span − baseline", "ns_per_span_minus_baseline", ",.0f", ""),
    ("ratio to microtel, ns/span − baseline", "ratio_ns_per_span_minus_baseline", ".2f", "×"),
    ("cores used", "cores", ".4f", ""),
    ("cores − baseline", "cores_minus_baseline", ".4f", ""),
    ("ratio to microtel, cores − baseline", "ratio_cores_minus_baseline", ".2f", "×"),
    ("emitting threads ns/span, self-reported", "emit_self_ns_per_span", ",.0f", ""),
    ("emitting threads ns/span, by subtraction", "emit_sub_ns_per_span", ",.0f", ""),
    ("emitting threads self-reported − baseline", "emit_self_ns_per_span_minus_baseline",
     ",.0f", ""),
    ("ratio to microtel, emitting − baseline", "ratio_emit_self_ns_per_span_minus_baseline",
     ".2f", "×"),
    ("spans/s achieved", "span_rate_achieved", ",.0f", ""),
)


def render_md(doc: dict) -> str:
    suts = doc["suts"]
    lines = [f"# CPU rounds: {doc['profile']}", ""]
    if doc.get("label"):
        lines += [f"**{doc['label']}**", ""]
    target = next((doc["summary"][s]["span_rate_target"] for s in suts
                   if doc["summary"][s]["span_rate_target"]), None)
    lines += [
        f"{doc['rounds']} interleaved rounds; median (min–max) over rounds. Each round's "
        "baseline is subtracted within the round, and ratios are to microtel in the same round."
        + (f" Target {target['median']:,.0f} spans/s." if target else ""),
        "",
        "| | " + " | ".join(suts) + " |",
        "|---|" + "---:|" * len(suts),
    ]
    for label, key, fmt, suffix in _MD_ROWS:
        lines.append(f"| {label} | " + " | ".join(
            _fmt(doc["summary"][s][key], fmt, suffix) for s in suts) + " |")
    lines += ["", "| Run | SUT | load before | waited | timed out |", "|---|---|---:|---:|---|"]
    for i, w in enumerate(doc["runs"], 1):
        lines.append(f"| {i} (round {w['round']}) | {w['sut']} | {w['load']:.2f} | "
                     f"{w['waited_s']:.0f} s | {'yes' if w['timed_out'] else 'no'} |")
    return "\n".join(lines) + "\n"


def _build_all(suts: list, engine: str) -> None:
    """Build each SUT's image, so every run of every round uses the same build."""
    for sut in suts:
        print(f"[rounds] building {sut.name} image ...", flush=True)
        build_image(engine, dockerfile=str(_BENCH_DIR.parent / sut.dockerfile),
                    image_name=sut.image_name, build_context=_BENCH_DIR.parent, verbose=False)


def _parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="python3 -m driver.rounds",
                                description="Interleaved CPU rounds of one profile.")
    p.add_argument("--profile", required=True)
    p.add_argument("--sut", required=True, help="Comma-separated SUT names.")
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--out", required=True)
    p.add_argument("--label", default=None)
    p.add_argument("--max-load", type=float, default=0.5)
    p.add_argument("--load-timeout", type=float, default=300.0)
    p.add_argument("--allow-smt", action="store_true")
    p.add_argument("--build", action="store_true",
                   help="Build every listed SUT's image before the first round "
                        "(the runs themselves never build).")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = _parse_args(argv)
    suts = [s.strip() for s in args.sut.split(",") if s.strip()]
    registry = load_registry(_REGISTRY_PATH)
    libraries = {s: get_sut(registry, s).library for s in suts}
    if args.build:
        _build_all([get_sut(registry, s) for s in suts], detect_engine("auto"))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    runs, rounds = [], []
    for r, order in enumerate(run_order(suts, args.rounds), 1):
        results: dict[str, Optional[dict]] = {}
        for sut in order:
            w = wait_for_load(args.max_load, args.load_timeout)
            print(f"[rounds] round {r} {sut}: load {w['load']:.2f} after {w['waited_s']:.0f} s"
                  + (" (timed out)" if w["timed_out"] else ""), flush=True)
            run_dir = out / f"round{r}" / sut
            cmd = [sys.executable, "-m", "driver", "--profile", args.profile, "--sut", sut,
                   "--out", str(run_dir)]
            cmd += ["--label", args.label] if args.label else []
            cmd += ["--allow-smt"] if args.allow_smt else []
            cmd += ["--no-build"]
            rc = subprocess.run(cmd, cwd=_BENCH_DIR, check=False).returncode
            runs.append({"round": r, "sut": sut, "rc": rc, **w})
            results[sut] = _cpu_of(run_dir / "results.json") if rc == 0 else None
        rounds.append(per_round(results))
    doc = {"profile": args.profile, "label": args.label, "rounds": args.rounds, "suts": suts,
           "libraries": libraries, "max_load": args.max_load,
           "load_timeout_s": args.load_timeout, "runs": runs, "per_round": rounds,
           "summary": summarize(rounds, suts)}
    (out / "summary.json").write_text(json.dumps(doc, indent=2) + "\n")
    (out / "summary.md").write_text(render_md(doc))
    print(f"[rounds] wrote {out / 'summary.md'}", flush=True)
    return 0 if all(w["rc"] == 0 for w in runs) else 1


if __name__ == "__main__":
    sys.exit(main())
