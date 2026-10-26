# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from driver.env_fingerprint import capture, warnings, EnvFingerprint, _physical_cores


def test_capture_returns_fingerprint():
    fp = capture("podman")
    assert fp.container_engine == "podman"
    assert isinstance(fp.cpu_threads, int)
    assert fp.cpu_threads >= 1
    # Physical cores come from the CPU topology, never the thread count.
    assert fp.cpu_physical_cores is None or 1 <= fp.cpu_physical_cores <= fp.cpu_threads
    assert isinstance(fp.cpu_model, str)
    assert len(fp.cpu_model) > 0
    assert isinstance(fp.host_load_avg_1m, float)
    assert fp.bench_started_at.endswith("+00:00")


def test_cpu_governor_is_string():
    fp = capture("podman")
    assert isinstance(fp.cpu_governor, str)
    assert len(fp.cpu_governor) > 0


def test_hyperthreading_is_bool_or_none():
    fp = capture("podman")
    assert fp.hyperthreading is None or isinstance(fp.hyperthreading, bool)


def test_warnings_high_load():
    fp = EnvFingerprint(
        cpu_model="test",
        cpu_physical_cores=4,
        cpu_threads=8,
        cpu_governor="performance",
        hyperthreading=False,
        kernel="6.0",
        container_engine="podman",
        container_engine_version="5.0",
        host_load_avg_1m=1.5,
        bench_started_at="2026-01-01T00:00:00+00:00",
    )
    w = warnings(fp, allow_smt=False)
    assert any("load average" in msg for msg in w)


def test_warnings_bad_governor():
    fp = EnvFingerprint(
        cpu_model="test",
        cpu_physical_cores=4,
        cpu_threads=8,
        cpu_governor="powersave",
        hyperthreading=False,
        kernel="6.0",
        container_engine="podman",
        container_engine_version="5.0",
        host_load_avg_1m=0.1,
        bench_started_at="2026-01-01T00:00:00+00:00",
    )
    w = warnings(fp, allow_smt=False)
    assert any("governor" in msg for msg in w)


def test_warnings_smt_suppressed_with_flag():
    fp = EnvFingerprint(
        cpu_model="test",
        cpu_physical_cores=4,
        cpu_threads=8,
        cpu_governor="performance",
        hyperthreading=True,
        kernel="6.0",
        container_engine="podman",
        container_engine_version="5.0",
        host_load_avg_1m=0.1,
        bench_started_at="2026-01-01T00:00:00+00:00",
    )
    assert warnings(fp, allow_smt=True) == []
    assert any("hyperthreading" in msg for msg in warnings(fp, allow_smt=False))


def test_warnings_clean_env():
    fp = EnvFingerprint(
        cpu_model="test",
        cpu_physical_cores=4,
        cpu_threads=8,
        cpu_governor="performance",
        hyperthreading=False,
        kernel="6.0",
        container_engine="podman",
        container_engine_version="5.0",
        host_load_avg_1m=0.1,
        bench_started_at="2026-01-01T00:00:00+00:00",
    )
    assert warnings(fp, allow_smt=False) == []


def _fake_cpu(root, index, package, core):
    topo = root / f"cpu{index}" / "topology"
    topo.mkdir(parents=True)
    (topo / "physical_package_id").write_text(f"{package}\n")
    (topo / "core_id").write_text(f"{core}\n")


def test_physical_cores_counts_cores_not_threads(tmp_path):
    # Two cores with two SMT threads each: cpu0/cpu2 share core 0.
    for index, core in enumerate([0, 1, 0, 1]):
        _fake_cpu(tmp_path, index, package=0, core=core)
    (tmp_path / "cpufreq").mkdir()  # a non-cpuN entry must be ignored
    assert _physical_cores(tmp_path) == 2


def test_physical_cores_distinguishes_packages(tmp_path):
    _fake_cpu(tmp_path, 0, package=0, core=0)
    _fake_cpu(tmp_path, 1, package=1, core=0)
    assert _physical_cores(tmp_path) == 2


def test_physical_cores_unknown_without_topology(tmp_path):
    assert _physical_cores(tmp_path) is None
