#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Allocation call sites for one SUT image, with malloc_stacks.so (bench/parity/README.md).

Usage:  python3 -I bench/parity/malloc_stacks.py <image> <malloc_stacks.so> <attrs> <out-dir>
        [--spans N] [--warmup N] [--env KEY=VALUE ...]

Same sequence as count_mallocs.py: warmup, flush, snapshot, N spans on one
worker thread, flush, snapshot. The difference between the snapshots is
symbolized against the image's own files (`podman export`), and every stack is
assigned to the emit worker (a frame matching "WorkerThread") or to the
background. Writes to <out-dir>:

    summary.md         allocations per span by thread and by call site
    worker.folded      folded stacks (flamegraph.pl input), worker thread
    background.folded  folded stacks, every other thread
    summary.json
"""

import argparse
import bisect
import collections
import json
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import time

NET = "bench-net"
SINK = "bench-sink-stacks"
SUT = "bench-sut-stacks"
CONTROL = 19090
# Frames that only say "this allocates"; the call site is the first frame
# outside them.
ALLOC_FRAME = re.compile(
    r"^(operator new|operator delete|malloc|calloc|realloc|__libc_|_int_malloc|"
    r"std::|__gnu_cxx::|__cxx|void std::|google::protobuf::internal::|"
    r"google::protobuf::Arena|google::protobuf::RepeatedPtrField|"
    r"google::protobuf::RepeatedField|absl::)")


def podman(*args, check=True):
    return subprocess.run(["podman", *args], capture_output=True, text=True, check=check)


def command(f, s, obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    return json.loads(f.readline())


def wait_for(pred, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return
        time.sleep(0.25)
    raise TimeoutError(what)


def parse(path):
    maps, tasks, stacks, overflow = [], {}, {}, 0
    for line in pathlib.Path(path).read_text().splitlines():
        kind, _, rest = line.partition(" ")
        if kind == "map":
            maps.append(rest)
        elif kind == "task":
            tid, _, comm = rest.partition(" ")
            tasks[int(tid)] = comm
        elif kind == "stack":
            parts = rest.split()
            stacks[(int(parts[0]), tuple(int(p, 16) for p in parts[2:]))] = int(parts[1])
        elif kind == "overflow":
            overflow = int(rest)
    return maps, tasks, stacks, overflow


class Symbolizer:
    def __init__(self, maps, root, shim_name, shim_host):
        self.root, self.regions, self.files = root, [], {}
        self.shim_name, self.shim_host = shim_name, shim_host
        for m in maps:
            f = m.split(None, 5)
            if len(f) < 6 or not f[5].startswith("/"):
                continue
            lo, hi = (int(x, 16) for x in f[0].split("-"))
            self.regions.append((lo, hi, int(f[2], 16), f[5].strip()))
        self.regions.sort()
        self.starts = [r[0] for r in self.regions]

    def _file(self, path):
        if path in self.files:
            return self.files[path]
        host = self.shim_host if path == self.shim_name else self.root / path.lstrip("/")
        loads, syms = [], []
        if host.exists():
            ph = subprocess.run(["readelf", "-lW", str(host)], capture_output=True, text=True).stdout
            for line in ph.splitlines():
                p = line.split()
                if p and p[0] == "LOAD":
                    loads.append((int(p[1], 16), int(p[2], 16), int(p[4], 16)))
            for flag in ([], ["-D"]):
                out = subprocess.run(["nm", "-C", "-n", "-S", "--defined-only", *flag, str(host)],
                                     capture_output=True, text=True).stdout
                for line in out.splitlines():
                    p = line.split(None, 3)
                    if len(p) == 4 and p[2] in "tTwWiu":
                        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
                if syms:
                    break
        syms.sort()
        self.files[path] = (loads, syms, [s[0] for s in syms])
        return self.files[path]

    def name(self, pc):
        i = bisect.bisect_right(self.starts, pc) - 1
        if i < 0 or pc >= self.regions[i][1]:
            return "??"
        lo, _, off, path = self.regions[i]
        loads, syms, addrs = self._file(path)
        file_off = pc - lo + off
        vaddr = file_off
        for p_off, p_vaddr, p_filesz in loads:
            if p_off <= file_off < p_off + p_filesz:
                vaddr = file_off - p_off + p_vaddr
                break
        j = bisect.bisect_right(addrs, vaddr) - 1
        base = pathlib.PurePosixPath(path).name
        if j < 0 or vaddr >= syms[j][0] + max(syms[j][1], 1):
            return f"?? [{base}]"
        return syms[j][2]


def short(sym):
    """Drops template arguments and parameter lists, for readable call sites."""
    out, depth = [], 0
    for ch in sym:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out).replace(" const", "").strip() or sym


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("shim")
    ap.add_argument("attrs", type=int)
    ap.add_argument("out")
    ap.add_argument("--spans", type=int, default=10000)
    ap.add_argument("--warmup", type=int, default=1000)
    ap.add_argument("--env", action="append", default=[])
    a = ap.parse_args()
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    shim = pathlib.Path(a.shim).resolve()

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        dumps = tmp / "dumps"
        dumps.mkdir()
        dumps.chmod(0o777)
        podman("network", "create", NET, check=False)
        for name in (SUT, SINK):
            podman("rm", "-f", name, check=False)
        podman("run", "-d", "--name", SINK, "--network", NET, "--network-alias", "sink",
               "bench-sink-blackhole:latest")
        try:
            podman("run", "-d", "--name", SUT, "--network", NET, "-p", f"{CONTROL}:{CONTROL}",
                   "-v", f"{shim}:/malloc_stacks.so:ro,Z", "-v", f"{dumps}:/out:Z",
                   "-e", "LD_PRELOAD=/malloc_stacks.so", "-e", "MALLOC_STACKS_OUT=/out/stacks",
                   "-e", f"EMIT_ATTRIBUTES_PER_SPAN={a.attrs}",
                   "-e", "OTEL_EXPORTER_OTLP_ENDPOINT=http://sink:4318",
                   *[x for kv in a.env for x in ("-e", kv)], a.image)
            logs = lambda: podman("logs", SUT, check=False).stderr + podman("logs", SUT, check=False).stdout
            wait_for(lambda: "ready on control port" in logs(), 120, "SUT not ready")
            with socket.create_connection(("127.0.0.1", CONTROL), timeout=600) as s:
                f = s.makefile("r")
                command(f, s, {"cmd": "run", "spans": a.warmup, "threads": 1, "rate_hz": 0})
                command(f, s, {"cmd": "flush"})
                podman("kill", "--signal", "USR2", SUT)
                wait_for(lambda: (dumps / "stacks.0.done").exists(), 60, "no first snapshot")
                run = command(f, s, {"cmd": "run", "spans": a.spans, "threads": 1, "rate_hz": 0})
                flush = command(f, s, {"cmd": "flush"})
                podman("kill", "--signal", "USR2", SUT)
                wait_for(lambda: (dumps / "stacks.1.done").exists(), 60, "no second snapshot")
                command(f, s, {"cmd": "quit"})
            root = tmp / "root"
            root.mkdir()
            exp = subprocess.Popen(["podman", "export", SUT], stdout=subprocess.PIPE)
            subprocess.run(["tar", "-x", "-C", str(root), "--exclude=dev/*"], stdin=exp.stdout,
                           check=False, capture_output=True)
            exp.wait()
            maps0, _, before, _ = parse(dumps / "stacks.0")
            maps1, tasks, after, overflow = parse(dumps / "stacks.1")
        finally:
            for name in (SUT, SINK):
                podman("rm", "-f", name, check=False)

        sym = Symbolizer(maps1, root, "/malloc_stacks.so", shim)
        cache = {}

        def names(pcs):
            res = []
            for pc in pcs:
                if pc not in cache:
                    cache[pc] = sym.name(pc - 1)
                res.append(cache[pc])
            return res

        n = a.spans
        by_thread = collections.Counter()
        sites = {"worker": collections.Counter(), "background": collections.Counter()}
        folded = {"worker": collections.Counter(), "background": collections.Counter()}
        worker_tids = set()
        for key, count in after.items():
            delta = count - before.get(key, 0)
            if delta <= 0:
                continue
            tid, pcs = key
            frames = names(pcs)
            role = "worker" if any("WorkerThread" in fr for fr in frames) else "background"
            if role == "worker":
                worker_tids.add(tid)
            by_thread[(tid, role)] += delta
            site = next((short(fr) for fr in frames if not ALLOC_FRAME.match(short(fr))),
                        short(frames[0]) if frames else "??")
            sites[role][site] += delta
            folded[role][";".join(short(fr) for fr in reversed(frames))] += delta

    total = {r: sum(v for (t, rr), v in by_thread.items() if rr == r) for r in ("worker", "background")}
    lines = [f"# Allocation call sites: `{a.image}`, {a.attrs} attributes, {n} spans", "",
             f"Worker thread: {total['worker'] / n:.4f} allocations per span. "
             f"Background threads: {total['background'] / n:.4f}. Table overflow: {overflow}.", "",
             "| Thread | Role | Allocations | Per span |", "|---|---|---:|---:|"]
    for (tid, role), v in sorted(by_thread.items(), key=lambda kv: -kv[1]):
        lines.append(f"| {tid} ({tasks.get(tid, 'exited')}) | {role} | {v} | {v / n:.4f} |")
    for role in ("background", "worker"):
        lines += ["", f"## {role.capitalize()}: first frame outside allocator and container code", "",
                  "| Call site | Allocations | Per span |", "|---|---:|---:|"]
        for site, v in sites[role].most_common(25):
            lines.append(f"| `{site}` | {v} | {v / n:.4f} |")
        (out / f"{role}.folded").write_text(
            "".join(f"{k} {v}\n" for k, v in folded[role].most_common()))
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    (out / "summary.json").write_text(json.dumps({
        "image": a.image,
        "image_id": podman("image", "inspect", "--format", "{{.Id}}", a.image).stdout.strip(),
        "attributes_per_span": a.attrs, "spans": n, "env": a.env,
        "spans_emitted": run.get("spans_emitted"), "flush": flush,
        "worker_allocs_per_span": total["worker"] / n,
        "background_allocs_per_span": total["background"] / n,
        "table_overflow": overflow,
        "sites": {r: dict(sites[r].most_common(50)) for r in sites},
    }, indent=2))
    print("\n".join(lines))


if __name__ == "__main__":
    sys.exit(main())
