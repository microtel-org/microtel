#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Allocations per span for one SUT image, with malloc_count.so (bench/parity/README.md).

Usage:  python3 -I bench/parity/count_mallocs.py <image> <malloc_count.so> <attrs> <out.json>
        [--spans N] [--warmup N]

Starts the blackhole sink and the SUT on the bench network, with the counter
preloaded into the SUT. Runs a warmup and a flush, takes a per-thread snapshot
(SIGUSR2), runs N spans on one worker thread and a flush, and takes a second
snapshot. The emit-app starts a fresh worker thread for every run command and
joins it before replying, so the measured run's worker is the one thread that
is new in the second snapshot and already gone: its count over N is the
calling thread's allocations per span. Every other thread's growth between the
snapshots, over N, is reported as background allocations per span (batch
processor, exporter, transport).
"""

import argparse
import json
import re
import socket
import subprocess
import sys
import time

NET = "bench-net"
SINK = "bench-sink-malloc"
SUT = "bench-sut-malloc"
CONTROL = 19090
LINE = re.compile(r"malloc_count tid=(\d+) comm=(\S+) allocs=(\d+)")


def podman(*args, check=True):
    return subprocess.run(["podman", *args], capture_output=True, text=True, check=check)


def command(sock_file, sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode())
    return json.loads(sock_file.readline())


def snapshots(logs):
    snaps, cur = [], None
    for line in logs.splitlines():
        if line.startswith("malloc_count begin"):
            cur = {}
        elif line.startswith("malloc_count end"):
            snaps.append(cur)
            cur = None
        elif cur is not None and (m := LINE.match(line)):
            cur[int(m.group(1))] = (m.group(2), int(m.group(3)))
    return snaps


def wait_for(pred, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return
        time.sleep(0.25)
    raise TimeoutError(what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("shim")
    ap.add_argument("attrs", type=int)
    ap.add_argument("out")
    ap.add_argument("--spans", type=int, default=10000)
    ap.add_argument("--warmup", type=int, default=1000)
    a = ap.parse_args()

    podman("network", "create", NET, check=False)
    for name in (SUT, SINK):
        podman("rm", "-f", name, check=False)
    podman("run", "-d", "--name", SINK, "--network", NET, "--network-alias", "sink",
           "bench-sink-blackhole:latest")
    try:
        podman("run", "-d", "--name", SUT, "--network", NET, "-p", f"{CONTROL}:{CONTROL}",
               "-v", f"{a.shim}:/malloc_count.so:ro,Z",
               "-e", "LD_PRELOAD=/malloc_count.so",
               "-e", f"EMIT_ATTRIBUTES_PER_SPAN={a.attrs}",
               "-e", "OTEL_EXPORTER_OTLP_ENDPOINT=http://sink:4318",
               a.image)
        logs = lambda: podman("logs", SUT, check=False).stderr + podman("logs", SUT, check=False).stdout
        wait_for(lambda: "ready on control port" in logs(), 60, "SUT not ready")
        with socket.create_connection(("127.0.0.1", CONTROL), timeout=120) as s:
            f = s.makefile("r")
            command(f, s, {"cmd": "run", "spans": a.warmup, "threads": 1, "rate_hz": 0})
            command(f, s, {"cmd": "flush"})
            podman("kill", "--signal", "USR2", SUT)
            wait_for(lambda: logs().count("malloc_count end") >= 1, 10, "no first snapshot")
            run = command(f, s, {"cmd": "run", "spans": a.spans, "threads": 1, "rate_hz": 0})
            command(f, s, {"cmd": "flush"})
            podman("kill", "--signal", "USR2", SUT)
            wait_for(lambda: logs().count("malloc_count end") >= 2, 10, "no second snapshot")
            command(f, s, {"cmd": "quit"})
        snaps = snapshots(logs())
        before, after = snaps[0], snaps[1]
        new_gone = [t for t in after if t not in before and after[t][0] == "?"]
        if len(new_gone) != 1:
            raise RuntimeError(f"expected one finished worker thread, found {new_gone}")
        worker = new_gone[0]
        background = {t: after[t][1] - before.get(t, ("", 0))[1]
                      for t in after if t != worker}
        result = {
            "image": a.image,
            "image_id": podman("image", "inspect", "--format", "{{.Id}}", a.image).stdout.strip(),
            "attributes_per_span": a.attrs,
            "spans": a.spans,
            "spans_emitted": run.get("spans_emitted"),
            "spans_dropped": run.get("spans_dropped"),
            "calling_thread_allocs": after[worker][1],
            "calling_thread_allocs_per_span": after[worker][1] / a.spans,
            "background_allocs": sum(background.values()),
            "background_allocs_per_span": sum(background.values()) / a.spans,
            "background_by_thread": {f"{t}:{after[t][0]}": n for t, n in background.items() if n},
        }
        with open(a.out, "w") as out:
            json.dump(result, out, indent=2)
        print(json.dumps(result))
    finally:
        for name in (SUT, SINK):
            podman("rm", "-f", name, check=False)


if __name__ == "__main__":
    sys.exit(main())
