# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""CPU accounting: cpu.stat and /proc stat parsing, the window arithmetic, and
the report's ratios and baseline subtraction."""

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.__main__ import _cpu_end, _reply_timeout_s
from driver.cpu import (
    CpuReader,
    CpuSnapshot,
    ThreadCpu,
    cgroup_from_proc,
    parse_cpu_stat,
    parse_schedstat,
    parse_task_stat,
    window,
)
from driver.report import add_cpu_comparisons, _render_md

TICK_NS = 1_000_000_000 // os.sysconf("SC_CLK_TCK")

CPU_STAT = """usage_usec 1106493
user_usec 1098615
system_usec 7878
nr_periods 0
nr_throttled 0
throttled_usec 0
nr_bursts 0
burst_usec 0
"""


def _stat_line(pid, comm, utime, stime):
    # Fields 3..15 of /proc/<pid>/stat: state, ppid, pgrp, session, tty_nr,
    # tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime; then more.
    return (f"{pid} ({comm}) S 0 1 1 0 -1 4194560 120 0 0 0 {utime} {stime} "
            f"0 0 20 0 3 0 1234 0 0\n")


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def test_parse_cpu_stat_reads_every_counter():
    st = parse_cpu_stat(CPU_STAT)
    assert st["usage_usec"] == 1106493
    assert st["user_usec"] == 1098615
    assert st["system_usec"] == 7878
    assert st["nr_periods"] == 0


def test_parse_cpu_stat_ignores_malformed_lines():
    assert parse_cpu_stat("usage_usec 5\ngarbage\nsome_key x\n\n") == {"usage_usec": 5}


def test_parse_task_stat_utime_stime():
    assert parse_task_stat(_stat_line(42, "emit_app", 311, 27)) == ("emit_app", 311, 27)


def test_parse_task_stat_comm_with_spaces_and_parens():
    # comm is free text: split on the last ")", not on whitespace.
    assert parse_task_stat(_stat_line(7, "a (b) c", 5, 6)) == ("a (b) c", 5, 6)


def test_parse_schedstat_first_field_is_run_ns():
    assert parse_schedstat("1108759326 290653 44\n") == 1108759326


def test_cgroup_from_proc_takes_the_v2_line():
    text = "12:pids:/legacy\n0::/user.slice/libpod-abc.scope/container\n"
    assert cgroup_from_proc(text) == "/user.slice/libpod-abc.scope/container"
    assert cgroup_from_proc("3:cpu:/x\n") == ""


# ---------------------------------------------------------------------------
# CpuReader against a fake /proc and cgroup tree
# ---------------------------------------------------------------------------

def _fake_proc(root: Path, pid: int, proc_ticks, threads, schedstat=True):
    """threads: {tid: (comm, utime, stime, run_ns)}."""
    d = root / str(pid)
    (d / "task").mkdir(parents=True)
    (d / "stat").write_text(_stat_line(pid, "emit_app", *proc_ticks))
    for tid, (comm, ut, st, ns) in threads.items():
        t = d / "task" / str(tid)
        t.mkdir()
        (t / "stat").write_text(_stat_line(tid, comm, ut, st))
        if schedstat:
            (t / "schedstat").write_text(f"{ns} 0 0\n")


def test_reader_uses_cgroup_when_readable(tmp_path):
    _fake_proc(tmp_path / "proc", 10, (1, 1), {10: ("emit_app", 1, 1, 5_000)})
    cg = tmp_path / "cg"
    cg.mkdir()
    (cg / "cpu.stat").write_text(CPU_STAT)
    r = CpuReader(10, cg, proc_root=tmp_path / "proc")
    assert r.source == f"cgroup {cg / 'cpu.stat'}"
    assert r.thread_source == "schedstat"
    s = r.snapshot()
    assert (s.total_ns, s.user_ns, s.system_ns) == (1106493000, 1098615000, 7878000)
    assert s.threads == {10: ThreadCpu("emit_app", 5_000)}


def test_reader_falls_back_to_proc_stat_without_cgroup(tmp_path):
    # /proc/<pid>/stat counts exited threads too, which is why the fallback is
    # the process line and not the sum of task/*/stat.
    _fake_proc(tmp_path / "proc", 10, (30, 7), {10: ("emit_app", 1, 1, 5_000)})
    r = CpuReader(10, tmp_path / "missing", proc_root=tmp_path / "proc")
    assert r.source.startswith("proc ")
    s = r.snapshot()
    assert (s.user_ns, s.system_ns, s.total_ns) == (30 * TICK_NS, 7 * TICK_NS, 37 * TICK_NS)


def test_reader_falls_back_when_cpu_stat_lacks_usage(tmp_path):
    _fake_proc(tmp_path / "proc", 10, (1, 0), {10: ("emit_app", 1, 0, 1)})
    cg = tmp_path / "cg"
    cg.mkdir()
    (cg / "cpu.stat").write_text("nr_periods 0\n")
    assert CpuReader(10, cg, proc_root=tmp_path / "proc").source.startswith("proc ")


def test_reader_thread_ticks_without_schedstat(tmp_path):
    _fake_proc(tmp_path / "proc", 10, (1, 1),
               {10: ("emit_app", 1, 1, 0), 11: ("worker", 3, 2, 0)}, schedstat=False)
    r = CpuReader(10, None, proc_root=tmp_path / "proc")
    assert r.thread_source == "stat"
    assert r.snapshot().threads[11] == ThreadCpu("worker", 5 * TICK_NS)


# ---------------------------------------------------------------------------
# Window arithmetic
# ---------------------------------------------------------------------------

def _snap(t_ns, total, user, system, threads):
    return CpuSnapshot(t_ns, total, user, system,
                       {tid: ThreadCpu(c, ns) for tid, (c, ns) in threads.items()})


S0 = _snap(0, 1_000, 800, 200, {1: ("emit_app", 100), 2: ("exporter", 300), 3: ("io", 50)})
# Between the snapshots the emit threads ran and exited; the exporter used
# 2,000 ns, the I/O thread 1,000, a new thread 500 and the main thread 0.
S1 = _snap(2_000_000, 11_000, 9_000, 2_000,
           {1: ("emit_app", 100), 2: ("exporter", 2_300), 3: ("io", 1_050), 4: ("curl", 500)})


def test_window_totals_and_split():
    w = window(S0, S1, spans=10, basis="delivered")
    assert (w["total_ns"], w["user_ns"], w["system_ns"]) == (10_000, 8_200, 1_800)
    assert w["other_ns"] == 3_500
    assert w["emit_ns"] == 6_500
    assert w["window_ns"] == 2_000_000
    assert w["cores"] == pytest.approx(0.005)


def test_window_per_span():
    w = window(S0, S1, spans=10, basis="delivered")
    assert w["per_span_ns"] == {"total": 1_000, "user": 820, "system": 180,
                                "emit": 650, "other": 350}
    assert (w["spans"], w["per_span_basis"]) == (10, "delivered")


def test_window_lists_other_threads_largest_first():
    w = window(S0, S1, spans=10, basis="delivered")
    assert [(o["comm"], o["ns"]) for o in w["other_threads"]] == [
        ("exporter", 2_000), ("io", 1_000), ("curl", 500), ("emit_app", 0)]


def test_window_counts_a_live_emit_thread_as_emitting():
    s1 = _snap(1, 11_000, 9_000, 2_000, {2: ("exporter", 2_300), 9: ("emit", 4_000)})
    w = window(S0, s1, spans=1, basis="delivered")
    assert w["other_ns"] == 2_000
    assert w["emit_ns"] == 8_000


def test_window_reports_exited_non_emit_threads():
    s1 = _snap(1, 2_000, 1_000, 1_000, {1: ("emit_app", 100)})
    s0 = _snap(0, 1_000, 500, 500, {1: ("emit_app", 100), 2: ("exporter", 1), 5: ("emit", 1)})
    assert window(s0, s1, spans=1, basis="delivered")["threads_exited"] == ["exporter"]


def test_window_without_spans_has_no_per_span_figures():
    w = window(S0, S1, spans=0, basis="delivered")
    assert all(v is None for v in w["per_span_ns"].values())


# ---------------------------------------------------------------------------
# Driver glue
# ---------------------------------------------------------------------------

class _FakeReader:
    source = "cgroup /sys/fs/cgroup/x/cpu.stat"
    thread_source = "schedstat"

    def snapshot(self):
        return S1


def test_cpu_end_divides_by_delivered_spans():
    samples = [{"items_received": 4, "spans_expected": 6}, {"items_received": 6, "spans_expected": 6}]
    c = _cpu_end(_FakeReader(), S0, samples, "microtel")
    assert (c["spans"], c["per_span_basis"]) == (10, "delivered")
    assert c["source"] == _FakeReader.source
    assert c["thread_source"] == "schedstat"


def test_cpu_end_divides_by_emitted_spans_for_the_noop_baseline():
    samples = [{"items_received": 0, "spans_expected": 6}, {"items_received": 0, "spans_expected": 4}]
    c = _cpu_end(_FakeReader(), S0, samples, "none")
    assert (c["spans"], c["per_span_basis"]) == (10, "emitted")
    assert c["per_span_ns"]["total"] == 1_000


def test_cpu_end_without_reader_is_none():
    assert _cpu_end(None, None, [], "microtel") is None


def test_reply_timeout_covers_a_paced_run():
    assert _reply_timeout_s(10_000, 0) == 60.0
    assert _reply_timeout_s(1_000, 1_000) == 60.0
    assert _reply_timeout_s(19_920, 332) == pytest.approx(120.0)


# ---------------------------------------------------------------------------
# Report: ratios to microtel and baseline subtraction
# ---------------------------------------------------------------------------

def _cpu(total, emit, other, cores, user=None, system=None):
    return {"per_span_ns": {"total": total, "user": user if user is not None else total,
                            "system": system if system is not None else 0,
                            "emit": emit, "other": other},
            "cores": cores, "source": "cgroup /x/cpu.stat", "per_span_basis": "delivered",
            "spans": 100, "window_ns": 60_000_000_000}


def _suts():
    return [
        {"name": "microtel", "library": "microtel", "cpu": _cpu(1_000, 600, 400, 0.010)},
        {"name": "otelcpp-http", "library": "otelcpp", "cpu": _cpu(1_800, 1_200, 600, 0.018)},
        {"name": "baseline-noop", "library": "none", "cpu": _cpu(200, 200, 0, 0.002)},
    ]


def test_ratios_to_microtel():
    suts = _suts()
    add_cpu_comparisons(suts)
    r = suts[1]["cpu"]["ratio_to_microtel"]
    assert r["total"] == pytest.approx(1.8)
    assert r["emit"] == pytest.approx(2.0)
    assert r["other"] == pytest.approx(1.5)
    assert r["cores"] == pytest.approx(1.8)
    assert suts[0]["cpu"]["ratio_to_microtel"]["total"] == pytest.approx(1.0)


def test_baseline_subtraction_and_its_ratio():
    suts = _suts()
    add_cpu_comparisons(suts)
    b = suts[1]["cpu"]["baseline_subtracted"]
    assert b["baseline_sut"] == "baseline-noop"
    assert b["total_ns_per_span"] == pytest.approx(1_600)
    assert b["cores"] == pytest.approx(0.016)
    assert suts[0]["cpu"]["baseline_subtracted"]["total_ns_per_span"] == pytest.approx(800)
    # (1,800 − 200) / (1,000 − 200)
    assert suts[1]["cpu"]["ratio_to_microtel"]["baseline_subtracted_total"] == pytest.approx(2.0)
    assert "baseline_subtracted" not in suts[2]["cpu"]


def test_no_reference_or_baseline_adds_nothing():
    suts = [{"name": "otelcpp-http", "library": "otelcpp", "cpu": _cpu(1_800, 1_200, 600, 0.018)}]
    add_cpu_comparisons(suts)
    assert "ratio_to_microtel" not in suts[0]["cpu"]
    assert "baseline_subtracted" not in suts[0]["cpu"]


def test_suts_without_cpu_are_skipped():
    suts = _suts() + [{"name": "go-otel", "library": "go-otel", "cpu": None}]
    add_cpu_comparisons(suts)
    assert suts[3]["cpu"] is None


def test_markdown_has_cpu_rows_source_and_label():
    suts = _suts()
    add_cpu_comparisons(suts)
    for s in suts:
        s["summary"] = {}
    md = _render_md({"label": "UNOFFICIAL: test", "suts": suts,
                     "environment": {}, "profile": {}})
    assert "**UNOFFICIAL: test**" in md
    assert "| CPU ns/span, total (÷ microtel) | 1,000 (1.00×) | 1,800 (1.80×) | 200 (0.20×) |" in md
    assert "| CPU ns/span, total − baseline | 800 (1.00×) | 1,600 (2.00×) | — |" in md
    assert "| CPU cores used | 0.010 (1.00×) | 0.018 (1.80×) | 0.002 (0.20×) |" in md
    assert "| CPU source | cgroup | cgroup | cgroup |" in md
    assert "from the end of warm-up to after the last sample's flush" in md
