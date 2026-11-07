# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Interleaved CPU rounds: run order, the load gate, per-round baseline
subtraction and ratios, and the median / range summary."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.rounds import (
    _cpu_of,
    median_range,
    per_round,
    read_loadavg,
    render_md,
    run_order,
    summarize,
    wait_for_load,
)


def test_run_order_rotates_each_round():
    assert run_order(["a", "b", "c"], 4) == [
        ["a", "b", "c"], ["b", "c", "a"], ["c", "a", "b"], ["a", "b", "c"]]


def test_read_loadavg(tmp_path):
    p = tmp_path / "loadavg"
    p.write_text("0.42 0.80 1.10 1/900 12345\n")
    assert read_loadavg(p) == pytest.approx(0.42)


class _Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


def test_wait_for_load_returns_once_below():
    loads = iter([0.9, 0.7, 0.4])
    clk = _Clock()
    w = wait_for_load(0.5, 300, read_load=lambda: next(loads), sleep=clk.sleep, clock=clk)
    assert w == {"load": 0.4, "waited_s": 10.0, "timed_out": False}


def test_wait_for_load_does_not_wait_when_already_below():
    clk = _Clock()
    w = wait_for_load(0.5, 300, read_load=lambda: 0.1, sleep=clk.sleep, clock=clk)
    assert w == {"load": 0.1, "waited_s": 0.0, "timed_out": False}


def test_wait_for_load_times_out_and_records_the_load():
    clk = _Clock()
    w = wait_for_load(0.5, 30, read_load=lambda: 2.0, sleep=clk.sleep, clock=clk)
    assert w["timed_out"] and w["load"] == 2.0 and w["waited_s"] == pytest.approx(30.0)


def _run(lib, ns, cores, own, rate=990.0):
    return {"library": lib, "ns_per_span": ns, "cores": cores, "emit_sub_ns_per_span": own,
            "emit_self_ns_per_span": own, "span_rate_target": 996, "span_rate_achieved": rate}


def _round(m_ns=9_000, o_ns=11_000, b_ns=4_000):
    return per_round({
        "microtel": _run("microtel", m_ns, m_ns / 1e6, m_ns - 1_000),
        "otelcpp-http": _run("otelcpp", o_ns, o_ns / 1e6, o_ns - 1_000),
        "baseline-noop": _run("none", b_ns, b_ns / 1e6, b_ns),
    })


def test_per_round_subtracts_the_same_rounds_baseline():
    r = _round()
    assert r["microtel"]["ns_per_span_minus_baseline"] == 5_000
    assert r["otelcpp-http"]["ns_per_span_minus_baseline"] == 7_000
    assert r["otelcpp-http"]["cores_minus_baseline"] == pytest.approx(0.007)
    assert r["otelcpp-http"]["emit_self_ns_per_span_minus_baseline"] == 6_000
    assert "ns_per_span_minus_baseline" not in r["baseline-noop"]


def test_per_round_ratios_are_to_microtel_in_the_same_round():
    r = _round()
    assert r["otelcpp-http"]["ratio_ns_per_span_minus_baseline"] == pytest.approx(1.4)
    assert r["otelcpp-http"]["ratio_cores_minus_baseline"] == pytest.approx(1.4)
    assert r["otelcpp-http"]["ratio_emit_self_ns_per_span_minus_baseline"] == pytest.approx(1.5)
    assert r["microtel"]["ratio_ns_per_span_minus_baseline"] == pytest.approx(1.0)


def test_per_round_skips_failed_runs_and_missing_baseline():
    r = per_round({"microtel": _run("microtel", 9_000, 0.009, 8_000), "otelcpp-http": None})
    assert list(r) == ["microtel"]
    assert "ns_per_span_minus_baseline" not in r["microtel"]


def test_median_range():
    assert median_range([3, None, 1, 2]) == {"median": 2, "min": 1, "max": 3, "n": 3}
    assert median_range([None]) is None


def test_summarize_over_rounds():
    rounds = [_round(m_ns=9_000), _round(m_ns=10_000), _round(m_ns=8_000)]
    s = summarize(rounds, ["microtel", "otelcpp-http", "baseline-noop"])
    assert s["microtel"]["ns_per_span_minus_baseline"] == {
        "median": 5_000, "min": 4_000, "max": 6_000, "n": 3}
    assert s["otelcpp-http"]["ratio_ns_per_span_minus_baseline"]["median"] == pytest.approx(1.4)
    assert s["baseline-noop"]["ns_per_span_minus_baseline"] is None


def test_cpu_of_reads_a_driver_results_json(tmp_path):
    doc = {"suts": [{"library": "microtel", "samples": [
        {"span_rate_target": 996, "span_rate_achieved": 994.0}],
        "cpu": {"per_span_ns": {"total": 9_000, "emit": 8_000}, "cores": 0.009,
                "emit_self": {"per_span_ns": {"total": 7_900}}}}]}
    p = tmp_path / "results.json"
    p.write_text(json.dumps(doc))
    assert _cpu_of(p) == {"library": "microtel", "ns_per_span": 9_000, "cores": 0.009,
                          "emit_sub_ns_per_span": 8_000, "emit_self_ns_per_span": 7_900,
                          "span_rate_target": 996, "span_rate_achieved": 994.0}
    assert _cpu_of(tmp_path / "missing.json") is None


def test_render_md_has_medians_ranges_and_load_log():
    rounds = [_round(m_ns=9_000), _round(m_ns=10_000), _round(m_ns=8_000)]
    suts = ["microtel", "otelcpp-http", "baseline-noop"]
    doc = {"profile": "fixed-rate-1k", "label": "UNOFFICIAL", "rounds": 3, "suts": suts,
           "summary": summarize(rounds, suts),
           "runs": [{"round": 1, "sut": "microtel", "load": 0.31, "waited_s": 42.0,
                     "timed_out": False}]}
    md = render_md(doc)
    assert "**UNOFFICIAL**" in md
    assert "| ns/span − baseline | 5,000 (4,000–6,000) | 7,000 (7,000–7,000) | — |" in md
    assert "| 1 (round 1) | microtel | 0.31 | 42 s | no |" in md
    assert "Target 996 spans/s." in md
