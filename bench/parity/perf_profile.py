#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""perf profile of the span hot loop for one SUT (bench/parity/README.md, part 2).

Usage:  python3 -I bench/parity/perf_profile.py <perf-image> <cycles> <out-dir> [--env KEY=VALUE ...]

Starts the blackhole sink and the SUT image, runs a 10,000-span warmup, then
starts the host's `perf record -e cycles:u -F 4999 --call-graph fp` on the
container's process and repeats <cycles> times what one benchmark sample does: a run of
10,000 spans on one worker thread, then a flush. Writes perf.data, the
`perf script` text (symbols from the exported container filesystem) and
the run results to <out-dir>; classify with
perf_buckets.py.
"""

import argparse
import json
import pathlib
import signal
import socket
import subprocess
import time

NET = "bench-net"
SINK = "bench-sink-perf"
SUT = "bench-sut-perf"
CONTROL = 19090
SPANS_PER_RUN = 10000


def podman(*args, check=True, **kw):
    return subprocess.run(["podman", *args], capture_output=True, text=True, check=check, **kw)


def command(f, s, obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    return json.loads(f.readline())


CLK_TCK = 100


def proc_times():
    """utime and stime of the SUT (PID 1 in its container), in clock ticks."""
    stat = podman("exec", SUT, "cat", "/proc/1/stat").stdout
    fields = stat.rsplit(")", 1)[1].split()
    return int(fields[11]), int(fields[12])


def logs():
    r = podman("logs", SUT, check=False)
    return r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("cycles", type=int)
    ap.add_argument("out")
    ap.add_argument("--env", action="append", default=[],
                    help="extra SUT environment, e.g. EMIT_OTELCPP_BSP_MAX_QUEUE_SIZE=16384")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    podman("network", "create", NET, check=False)
    for name in (SUT, SINK):
        podman("rm", "-f", name, check=False)
    podman("run", "-d", "--name", SINK, "--network", NET, "--network-alias", "sink",
           "bench-sink-blackhole:latest")
    try:
        podman("run", "-d", "--name", SUT, "--network", NET, "-p", f"{CONTROL}:{CONTROL}",
               "--cap-add", "PERFMON", "--cap-add", "SYS_PTRACE",
               "-e", "OTEL_EXPORTER_OTLP_ENDPOINT=http://sink:4318",
               *[x for kv in a.env for x in ("-e", kv)], a.image)
        deadline = time.monotonic() + 60
        while "ready on control port" not in logs():
            if time.monotonic() > deadline:
                raise TimeoutError("SUT not ready")
            time.sleep(0.25)
        perf_version = subprocess.run(["perf", "--version"], capture_output=True,
                                      text=True, check=False).stdout.strip()
        with socket.create_connection(("127.0.0.1", CONTROL), timeout=300) as s:
            f = s.makefile("r")
            command(f, s, {"cmd": "run", "spans": SPANS_PER_RUN, "threads": 1, "rate_hz": 0})
            command(f, s, {"cmd": "flush"})
            # perf runs on the host against the container's PID: rootless
            # podman's seccomp profile refuses perf_event_open inside the
            # container, and perf_event_paranoid=2 lets the host user sample
            # user space of its own processes (cycles:u).
            host_pid = podman("inspect", "--format", "{{.State.Pid}}", SUT).stdout.strip()
            perf = subprocess.Popen(
                ["perf", "record", "-e", "cycles:u", "-F", "4999", "--call-graph", "fp",
                 "-p", host_pid, "-o", str(out / "perf.data")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            time.sleep(2)
            cpu0 = proc_times()
            emitted = dropped = 0
            p50s = []
            t0 = time.monotonic()
            for _ in range(a.cycles):
                r = command(f, s, {"cmd": "run", "spans": SPANS_PER_RUN, "threads": 1, "rate_hz": 0})
                command(f, s, {"cmd": "flush"})
                emitted += r["spans_emitted"]
                dropped += r["spans_dropped"]["total"] if isinstance(r["spans_dropped"], dict) \
                    else r["spans_dropped"]
                p50s.append(r["latency_p50_ns"])
            wall = time.monotonic() - t0
            cpu1 = proc_times()
            perf.send_signal(signal.SIGINT)
            perf_log = perf.communicate(timeout=120)[0]
            # The container's filesystem, for perf script --symfs.
            symfs = out / "symfs"
            symfs.mkdir(exist_ok=True)
            exp = subprocess.Popen(["podman", "export", SUT], stdout=subprocess.PIPE)
            subprocess.run(["tar", "-x", "-C", str(symfs), "--exclude=dev/*"], stdin=exp.stdout,
                           check=False, capture_output=True)
            exp.wait()
            command(f, s, {"cmd": "quit"})
        script = subprocess.run(
            ["perf", "script", "-i", str(out / "perf.data"), "--symfs", str(out / "symfs"),
             "--no-inline", "-F", "comm,tid,period,ip,sym,dso"],
            capture_output=True, text=True, check=False)
        (out / "perf.script").write_text(script.stdout)
        p50s.sort()
        meta = {
            "image": a.image,
            "image_id": podman("image", "inspect", "--format", "{{.Id}}", a.image).stdout.strip(),
            "perf_version": perf_version,
            "cycles": a.cycles, "spans_per_run": SPANS_PER_RUN,
            "spans_emitted": emitted, "spans_dropped": dropped,
            "run_p50_median_ns": p50s[len(p50s) // 2],
            "wall_s": wall,
            # Whole process (all threads, including exited ones), during the
            # profiled cycles. perf samples user space only (cycles:u: the
            # host has perf_event_paranoid=2), so this is the kernel share
            # the profile cannot see, as an upper bound for the worker.
            "process_utime_s": (cpu1[0] - cpu0[0]) / CLK_TCK,
            "process_stime_s": (cpu1[1] - cpu0[1]) / CLK_TCK,
            # The output directory is replaced so no local path is recorded.
            "perf_record_log": perf_log[-2000:].replace(str(out), "<out>"),
            "perf_script_stderr": script.stderr[-2000:].replace(str(out), "<out>"),
        }
        (out / "meta.json").write_text(json.dumps(meta, indent=2))
        print(json.dumps({k: v for k, v in meta.items() if "log" not in k and "stderr" not in k}))
    finally:
        for name in (SUT, SINK):
            podman("rm", "-f", name, check=False)


if __name__ == "__main__":
    main()
