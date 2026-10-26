# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Capture environment fingerprint for embedding in results.json."""

from __future__ import annotations

import dataclasses
import datetime
import os
import platform
import re
import shutil
import subprocess
from pathlib import Path
from typing import Optional


@dataclasses.dataclass
class EnvFingerprint:
    cpu_model: str
    cpu_physical_cores: Optional[int]  # from the CPU topology; None if unreadable
    cpu_threads: int                   # logical CPUs (hardware threads)
    cpu_governor: str           # "performance" | "powersave" | "unknown"
    hyperthreading: Optional[bool]  # None if undeterminable
    kernel: str
    container_engine: str       # "podman" | "docker"
    container_engine_version: str
    host_load_avg_1m: float
    bench_started_at: str       # ISO-8601


def capture(container_engine: str) -> EnvFingerprint:
    return EnvFingerprint(
        cpu_model=_cpu_model(),
        cpu_physical_cores=_physical_cores(),
        cpu_threads=_threads(),
        cpu_governor=_cpu_governor(),
        hyperthreading=_hyperthreading(),
        kernel=platform.release(),
        container_engine=container_engine,
        container_engine_version=_engine_version(container_engine),
        host_load_avg_1m=os.getloadavg()[0],
        bench_started_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    )


def warnings(fp: EnvFingerprint, allow_smt: bool) -> list[str]:
    """Return a list of warning strings for env-guard violations."""
    result = []
    if fp.cpu_governor not in ("performance", "unknown"):
        result.append(
            f"cpu_governor is '{fp.cpu_governor}'; results may be noisy "
            "(set governor to 'performance' for reproducible benchmarks)"
        )
    if fp.host_load_avg_1m > 0.5:
        result.append(
            f"host load average is {fp.host_load_avg_1m:.2f} (> 0.5); "
            "background activity may inflate latency"
        )
    if fp.hyperthreading is True and not allow_smt:
        result.append(
            "hyperthreading (SMT) is enabled; pass --allow-smt to suppress this warning"
        )
    return result


def _cpu_model() -> str:
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.partition(":")[2].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _physical_cores(cpu_root: Path = Path("/sys/devices/system/cpu")) -> Optional[int]:
    """Count distinct (package, core) pairs in the CPU topology, or None."""
    cores = set()
    for cpu in cpu_root.glob("cpu[0-9]*"):
        topo = cpu / "topology"
        try:
            package = (topo / "physical_package_id").read_text(encoding="utf-8").strip()
            core = (topo / "core_id").read_text(encoding="utf-8").strip()
        except OSError:
            continue
        cores.add((package, core))
    return len(cores) or None


def _threads() -> int:
    """Logical CPUs, all of them rather than only those this process may use."""
    try:
        out = subprocess.check_output(
            ["nproc", "--all"], text=True, stderr=subprocess.DEVNULL
        ).strip()
        return int(out)
    except (subprocess.SubprocessError, ValueError, FileNotFoundError):
        pass
    return os.cpu_count() or 1


def _cpu_governor() -> str:
    path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return "unknown"


def _hyperthreading() -> Optional[bool]:
    path = "/sys/devices/system/cpu/smt/active"
    try:
        with open(path, encoding="utf-8") as f:
            val = f.read().strip()
            return val == "1"
    except OSError:
        return None


def _engine_version(engine: str) -> str:
    try:
        out = subprocess.check_output(
            [engine, "--version"], text=True, stderr=subprocess.STDOUT
        ).strip().splitlines()[0]
        return out
    except (subprocess.SubprocessError, FileNotFoundError, IndexError):
        return "unknown"
