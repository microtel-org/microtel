# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""CPU used by one SUT container over a measurement window (bench/docs/methodology.md).

Two snapshots, one at each end of the window, of:

- the process total: the container's cgroup v2 ``cpu.stat`` (``usage_usec``,
  ``user_usec``, ``system_usec``), or, when that is not readable, the SUT
  process's ``/proc/<pid>/stat`` ``utime``/``stime``. Both include threads that
  exited during the window.
- every live thread: ``/proc/<pid>/task/<tid>/stat`` for its name, and
  ``schedstat`` for its run time in ns (``stat`` ticks when ``schedstat`` is
  missing).

The emit-app names its workload threads ``emit``. They are created per run
command and have exited by the time either snapshot is taken, so the emitting
threads' CPU is the process total minus the CPU of the other threads.
"""

from __future__ import annotations

import dataclasses
import os
import subprocess
import time
from pathlib import Path
from typing import Optional

EMIT_THREAD_NAME = "emit"
_NS_PER_USEC = 1_000
_NS_PER_SEC = 1_000_000_000
# /proc/<pid>/stat fields after the ")" that closes comm: index 0 is field 3
# (state), so utime (field 14) and stime (field 15) are at 11 and 12.
_UTIME_INDEX = 11
_STIME_INDEX = 12


def parse_cpu_stat(text: str) -> dict[str, int]:
    """Key/value pairs of a cgroup v2 ``cpu.stat`` file."""
    out: dict[str, int] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].isdigit():
            out[parts[0]] = int(parts[1])
    return out


def parse_task_stat(text: str) -> tuple[str, int, int]:
    """``(comm, utime, stime)`` from a ``/proc/<pid>[/task/<tid>]/stat`` line.

    utime and stime are in clock ticks. comm may contain spaces and
    parentheses, so it runs from the first "(" to the last ")".
    """
    lpar = text.index("(")
    rpar = text.rindex(")")
    fields = text[rpar + 1:].split()
    return text[lpar + 1:rpar], int(fields[_UTIME_INDEX]), int(fields[_STIME_INDEX])


def parse_schedstat(text: str) -> int:
    """Run time in ns, the first field of ``/proc/<pid>/task/<tid>/schedstat``."""
    return int(text.split()[0])


@dataclasses.dataclass(frozen=True)
class ThreadCpu:
    comm: str
    ns: int


@dataclasses.dataclass(frozen=True)
class CpuSnapshot:
    monotonic_ns: int
    total_ns: int
    user_ns: int
    system_ns: int
    threads: dict[int, ThreadCpu]


class CpuReader:
    """Snapshots of one process's CPU: cgroup totals if readable, else /proc."""

    def __init__(self, pid: int, cgroup_dir: Optional[Path], proc_root: Path = Path("/proc")):
        self._pid = pid
        self._proc = proc_root / str(pid)
        self._tick_ns = _NS_PER_SEC // os.sysconf("SC_CLK_TCK")
        self._cpu_stat = cgroup_dir / "cpu.stat" if cgroup_dir is not None else None
        if self._cpu_stat is not None and "usage_usec" not in _read_cpu_stat(self._cpu_stat):
            self._cpu_stat = None
        self.source = f"cgroup {self._cpu_stat}" if self._cpu_stat else f"proc {self._proc}/stat"
        self.thread_source = "schedstat" if (self._proc / "task" / str(pid) / "schedstat").exists() \
            else "stat"

    def snapshot(self) -> CpuSnapshot:
        now = time.monotonic_ns()
        if self._cpu_stat is not None:
            st = parse_cpu_stat(self._cpu_stat.read_text())
            total, user, system = (st["usage_usec"] * _NS_PER_USEC, st["user_usec"] * _NS_PER_USEC,
                                   st["system_usec"] * _NS_PER_USEC)
        else:
            _, ut, stt = parse_task_stat((self._proc / "stat").read_text())
            user, system = ut * self._tick_ns, stt * self._tick_ns
            total = user + system
        return CpuSnapshot(now, total, user, system, self._threads())

    def _threads(self) -> dict[int, ThreadCpu]:
        threads: dict[int, ThreadCpu] = {}
        for task in (self._proc / "task").iterdir():
            try:
                comm, ut, stt = parse_task_stat((task / "stat").read_text())
                ns = (parse_schedstat((task / "schedstat").read_text())
                      if self.thread_source == "schedstat" else (ut + stt) * self._tick_ns)
            except FileNotFoundError:
                continue  # exited while listing
            threads[int(task.name)] = ThreadCpu(comm, ns)
        return threads


def _read_cpu_stat(path: Path) -> dict[str, int]:
    try:
        return parse_cpu_stat(path.read_text())
    except OSError:
        return {}


def container_reader(engine: str, name: str) -> CpuReader:
    """A CpuReader for a running container's main process (the emit-app).

    The cgroup is podman's ``State.CgroupPath``; for docker, or when podman
    gives none, the process's own entry in ``/proc/<pid>/cgroup``.
    """
    pid = int(_inspect(engine, name, "{{.State.Pid}}"))
    cgroup = _inspect(engine, name, "{{.State.CgroupPath}}") if engine == "podman" else ""
    if not cgroup:
        cgroup = cgroup_from_proc(Path(f"/proc/{pid}/cgroup").read_text())
    return CpuReader(pid, Path("/sys/fs/cgroup") / cgroup.lstrip("/") if cgroup else None)


def cgroup_from_proc(text: str) -> str:
    """The cgroup v2 path from ``/proc/<pid>/cgroup`` (the ``0::`` line), or ""."""
    for line in text.splitlines():
        if line.startswith("0::"):
            return line[3:]
    return ""


def _inspect(engine: str, name: str, fmt: str) -> str:
    return subprocess.run([engine, "inspect", "--format", fmt, name], capture_output=True,
                          text=True, check=True).stdout.strip()


def window(s0: CpuSnapshot, s1: CpuSnapshot, spans: int, basis: str) -> dict:
    """CPU between two snapshots, in total and per span.

    @p spans is the per-span denominator and @p basis says what it counts
    ("delivered" at the sink, or "emitted"). Threads named ``emit`` at either
    snapshot, and every thread that ran and exited in between, count as
    emitting; ``threads_exited`` lists the non-emit threads alive at the first
    snapshot and gone at the second, whose CPU after the first snapshot is
    counted as emitting too.
    """
    other_ns = 0
    others = []
    for tid, t in s1.threads.items():
        if t.comm == EMIT_THREAD_NAME:
            continue
        before = s0.threads.get(tid)
        ns = t.ns - (before.ns if before is not None else 0)
        other_ns += ns
        others.append({"tid": tid, "comm": t.comm, "ns": ns})
    others.sort(key=lambda o: -o["ns"])
    total = s1.total_ns - s0.total_ns
    out = {
        "window_ns": s1.monotonic_ns - s0.monotonic_ns,
        "total_ns": total,
        "user_ns": s1.user_ns - s0.user_ns,
        "system_ns": s1.system_ns - s0.system_ns,
        "emit_ns": total - other_ns,
        "other_ns": other_ns,
        "spans": spans,
        "per_span_basis": basis,
        "other_threads": others,
        "threads_exited": sorted(t.comm for tid, t in s0.threads.items()
                                 if tid not in s1.threads and t.comm != EMIT_THREAD_NAME),
    }
    out["cores"] = total / out["window_ns"] if out["window_ns"] > 0 else None
    out["per_span_ns"] = {k: (out[f"{k}_ns"] / spans if spans > 0 else None)
                          for k in ("total", "user", "system", "emit", "other")}
    return out
