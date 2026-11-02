#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Attribute hot-loop cycles to buckets (bench/parity/README.md, part 2).

Usage:  python3 -I bench/parity/perf_buckets.py <perf_profile.py out-dir>

Reads <out-dir>/perf.script and keeps the samples on the emit worker thread
(those whose stack contains bench's WorkerThread). Each is weighted by its
period (cycles) and given one bucket:

  1. libprotobuf: any frame in libprotobuf or in opentelemetry-cpp's OTLP
     recordable / exporter code (so allocation done by the recordable counts
     here, as the plan asks: "incl. under OtlpRecordable");
  2. "harness" if the sample is outside the backend's EmitSpan (the loop, its
     two steady_clock reads, the histogram);
  3. otherwise the innermost frame below EmitSpan that matches one of:
     allocation, atomics and locks, clock reads, sampler, processor enqueue,
     attribute storage; else "other".

Frames perf could not name (internal libc symbols, kernel entry) are skipped
in step 2, so they go to their nearest named caller. Virtual dispatch has no
bucket: an indirect call costs cycles in its caller, not in a frame of its
own, so sampling cannot separate it.

Writes <out-dir>/buckets.json and <out-dir>/worker.folded (for flamegraph.pl)
and prints a Markdown table.
"""

import collections
import json
import pathlib
import re
import sys

PROTOBUF = re.compile(r"OtlpRecordable|OtlpPopulateAttributeUtils|opentelemetry::proto::|"
                      r"google::protobuf|opentelemetry::exporter::otlp::")
RULES = [
    ("allocation", re.compile(
        r"^(malloc|free|cfree|calloc|realloc|aligned_alloc|posix_memalign|memalign|"
        r"__libc_malloc|__libc_free|_int_malloc|_int_free)$|^operator new|^operator delete|tcache")),
    ("atomics and locks", re.compile(
        r"pthread_mutex|pthread_spin|pthread_rwlock|lll_lock|futex|std::mutex|pthread_cond|"
        r"condition_variable|notify_one|notify_all|_Sp_counted_base|std::__atomic|atomic_")),
    ("clock reads", re.compile(
        r"clock_gettime|__vdso|__cvdso|gettimeofday|chrono::_V2::(system|steady)_clock::now|"
        r"SystemTimestamp|SteadyTimestamp")),
    ("sampler", re.compile(r"Sampler")),
    ("processor enqueue", re.compile(
        r"BatchSpanProcessor::(OnEnd|OnStart|Enqueue|MakeRoomFor)|CircularBuffer|AtomicUniquePtr|"
        r"MultiSpanProcessor|SpanProcessor::OnEnd|std::deque<.*QueuedSpan")),
    ("attribute storage", re.compile(
        r"SetAttribute|AttributeMap|KeyValueIterable|AttributeConverter|SeedAttributes|"
        r"TruncateStrings|SpanAttributes")),
]
ORDER = ["libprotobuf"] + [name for name, _ in RULES] + ["other", "harness"]


def frames_of(block):
    """(symbol, dso) per frame, innermost first."""
    out = []
    for line in block[1:]:
        line = line.strip()
        if not line:
            continue
        _, _, rest = line.partition(" ")
        sym, _, dso = rest.rpartition(" (")
        out.append((sym, dso.rstrip(")")))
    return out


def short(sym):
    """Function name without its parameter list, for tables and flame graphs."""
    depth, cut = 0, len(sym)
    for i in range(len(sym) - 1, -1, -1):
        c = sym[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                cut = i
                break
    name = sym[:cut] if sym.endswith(")") and cut < len(sym) else sym
    return name.replace(";", ":")


def classify(frames):
    worker = next((i for i, (s, _) in enumerate(frames) if "WorkerThread" in s), None)
    if worker is None:
        return None, None
    inner = frames[:worker]
    if any(PROTOBUF.search(s) or "libprotobuf" in d for s, d in inner):
        return "libprotobuf", None
    named = [s for s, _ in inner if s and s != "[unknown]"]
    emit = next((i for i, s in enumerate(named) if "EmitSpan" in s), None)
    if emit is None:
        return "harness", None
    for s in named[:emit]:
        for name, rx in RULES:
            if rx.search(s):
                return name, None
    return "other", short(named[0])


def main():
    out = pathlib.Path(sys.argv[1])
    text = out.joinpath("perf.script").read_text(errors="replace")
    totals = collections.Counter()
    other = collections.Counter()
    folded = collections.Counter()
    background = collections.Counter()
    for block in text.split("\n\n"):
        lines = block.strip("\n").split("\n")
        if not lines or not lines[0].strip():
            continue
        head = lines[0].split()
        try:
            period = int(head[-1])
        except (ValueError, IndexError):
            continue
        frames = frames_of(lines)
        bucket, self_sym = classify(frames)
        if bucket is None:
            background[head[0]] += period
            continue
        totals[bucket] += period
        if self_sym:
            other[self_sym] += period
        worker = next(i for i, (s, _) in enumerate(frames) if "WorkerThread" in s)
        stack = [short(s) if s != "[unknown]" else f"[{d.rsplit('/', 1)[-1] or 'kernel'}]"
                 for s, d in reversed(frames[:worker + 1])]
        folded[";".join(stack)] += period
    worker_total = sum(totals.values())
    span_total = worker_total - totals["harness"]
    rows = []
    print("| Bucket | % of span time | % of worker time |")
    print("|---|---:|---:|")
    for b in ORDER:
        pct_span = 100 * totals[b] / span_total if b != "harness" and span_total else None
        pct_worker = 100 * totals[b] / worker_total if worker_total else 0
        rows.append({"bucket": b, "cycles": totals[b], "pct_span": pct_span, "pct_worker": pct_worker})
        span_cell = f"{pct_span:.1f}" if pct_span is not None else "–"
        print(f"| {b} | {span_cell} | {pct_worker:.1f} |")
    print("\nTop of \"other\" (innermost named frame, % of span time):\n")
    for sym, n in other.most_common(12):
        print(f"- {100 * n / span_total:.1f}%  `{sym}`")
    bg = sum(background.values())
    print(f"\nWorker share of all sampled user-space cycles in the process: "
          f"{100 * worker_total / (worker_total + bg):.1f}%")
    out.joinpath("worker.folded").write_text(
        "".join(f"{k} {v}\n" for k, v in folded.items()))
    out.joinpath("buckets.json").write_text(json.dumps(
        {"rows": rows, "other_top": other.most_common(40), "worker_cycles": worker_total,
         "background_cycles": bg}, indent=2))


if __name__ == "__main__":
    main()
