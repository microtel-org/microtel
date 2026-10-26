"""Render markdown tables from grouped.json.  usage: python3 -I render.py grouped.json"""
import json
import sys

d = json.load(open(sys.argv[1]))
STATIC_LABEL = {
    "microtel": "microtel (static)", "opentelemetry-cpp": "opentelemetry-cpp (static)",
    "upb": "upb (vendored, incl. utf8_range)", "toml++": "toml++ (header-only)",
    "header-inlined third-party": "header-inlined third-party (protobuf/gRPC headers)",
    "emit-app": "emit-app",
}
RUNTIME = "C/C++ runtime (libstdc++, libgcc_s, libc, libm)"


SHARED_BASE = ("OpenSSL", "zlib", "C/C++ runtime")
SUBTOTALS = {}


def mb(b):
    return f"{b / 1e6:.1f}"


def footprint(sut):
    v = d[sut]
    rows = []
    unattributed = 0
    for k, b in v["binary"]["buckets"].items():
        if k == "unattributed":
            unattributed += b
        else:
            rows.append((STATIC_LABEL[k], "in binary", b))
    for k, x in v["shared"].items():
        if k.startswith("unattributed"):
            unattributed += x["bytes"]
            continue
        label = {"C/C++ runtime": RUNTIME}.get(k, k)
        n = len(x["files"])
        if k == "Abseil":
            label = f"Abseil ({n} libabsl_*.so files)"
        elif k == "gRPC":
            label = "gRPC (libgrpc++, libgrpc, libgpr, libupb.so.29)"
        elif k == "OpenSSL":
            label = "OpenSSL (libssl + libcrypto)"
        elif k == "libaddress_sorting.so.29":
            label = "libaddress_sorting.so.29 (gRPC package, not in the listed gRPC set)"
        elif k == "libopentelemetry_proto*.so":
            label = "libopentelemetry_proto*.so (2 files, stripped copy; 9,165,400 B as shipped)"
        rows.append((label, "shared library", x["bytes"]))
    rows.sort(key=lambda r: -r[2])
    rows.append(("unattributed (bloaty [section]/[Unmapped]/ELF rows, see below)", "in binary", unattributed))
    total = sum(r[2] for r in rows)
    rt = next(r[2] for r in rows if r[0] == RUNTIME)
    out = ["| Library | Where | Bytes | MB | % of total |", "|---|---|---:|---:|---:|"]
    for label, where, b in rows:
        out.append(f"| {label} | {where} | {b:,} | {mb(b)} | {100 * b / total:.1f}% |")
    out.append(f"| **Total, incl. C/C++ runtime** | | **{total:,}** | **{mb(total)}** | 100.0% |")
    out.append(f"| **Total, excl. C/C++ runtime** | | **{total - rt:,}** | **{mb(total - rt)}** | |")
    base = sum(x["bytes"] for k, x in v["shared"].items() if k in SHARED_BASE)
    SUBTOTALS[sut] = (base, total - base, total)
    return "\n".join(out)


def inbinary(sut):
    b = d[sut]["binary"]
    total = b["bloaty_total"]
    out = ["| Bucket | Bytes | % of stripped binary | of which inlined libstdc++ template code |",
           "|---|---:|---:|---:|"]
    for k, x in sorted(b["buckets"].items(), key=lambda kv: (kv[0] == "unattributed", -kv[1])):
        out.append(f"| {STATIC_LABEL.get(k, k)} | {x:,} | {100 * x / total:.1f}% | {b['stdcxx_inlined'].get(k, 0):,} |")
    out.append(f"| **Total = stripped /emit_app** | **{sum(b['buckets'].values()):,}** | 100.0% | |")
    sec = ["| bloaty row | Bytes |", "|---|---:|"]
    for k, x in sorted(b["sections"].items(), key=lambda kv: -kv[1]):
        if x:
            sec.append(f"| `{k}` | {x:,} |")
    sec.append(f"| **Sum (= unattributed)** | **{sum(b['sections'].values()):,}** |")
    return "\n".join(out), "\n".join(sec)


def libfiles(sut):
    out = ["| Group | File (real path basename) | Bytes |", "|---|---|---:|"]
    for k, x in d[sut]["shared"].items():
        for f in x["files"]:
            name, size = f.rsplit(" ", 1)
            out.append(f"| {k} | `{name}` | {int(size):,} |")
    return "\n".join(out)


for sut in ("otelcpp-grpc", "microtel"):
    ib, sec = inbinary(sut)
    print(f"@@FOOTPRINT {sut}\n{footprint(sut)}\n@@INBINARY {sut}\n{ib}\n@@SECTIONS {sut}\n{sec}\n@@LIBS {sut}\n{libfiles(sut)}")

sub = ["| Subtotal | otelcpp-grpc bytes | MB | microtel bytes | MB |", "|---|---:|---:|---:|---:|"]
names = ("Shared base (OpenSSL + zlib + C/C++ runtime)", "Telemetry stack (everything else)", "**Total**")
o, m = SUBTOTALS["otelcpp-grpc"], SUBTOTALS["microtel"]
for i, n in enumerate(names):
    sub.append(f"| {n} | {o[i]:,} | {mb(o[i])} | {m[i]:,} | {mb(m[i])} |")
sub.append("")
sub.append(f"Telemetry-stack ratio, otelcpp-grpc / microtel: {o[1]:,} / {m[1]:,} = **{o[1] / m[1]:.1f}×** (unrounded {o[1] / m[1]:.4f}).")
print("@@SUBTOTALS all\n" + "\n".join(sub))
