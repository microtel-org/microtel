"""Group bloaty CSVs and ldd library lists into per-library footprint rows.

usage: python3 -I group.py <bloaty-dir> <libs-dir>
Prints JSON with per-SUT in-binary buckets, section rows and shared-lib rows.
"""
import csv
import json
import os
import sys
from collections import Counter, defaultdict

BLOATY, LIBS = sys.argv[1], sys.argv[2]

# Inline-source path rules, applied to code inside a real compile unit.
INLINE_RULES = [
    ("toml++", lambda p: "tomlplusplus" in p or "/toml++/" in p),
    ("upb", lambda p: "/third_party/upb/" in p or "utf8_range" in p),
    ("header-inlined third-party", lambda p: p.startswith(
        ("/usr/include/google/protobuf", "/usr/include/grpc", "/usr/include/absl"))),
]
# Compile-unit path rules, for everything else in that CU.
CU_RULES = [
    ("microtel", ("/src/src/",)),
    ("upb", ("/src/third_party/upb/",)),
    ("opentelemetry-cpp", ("/src/opentelemetry-cpp/",)),
    ("emit-app", ("/src/bench/emit-app/", "/src/emit-app/")),
]
LIBSTDCXX_HDR = ("include/c++/",)


def cu_bucket(cu):
    for name, prefixes in CU_RULES:
        if cu.startswith(prefixes):
            return name
    return None


def binary(sut):
    buckets = Counter()
    stdcxx = Counter()
    unknown_cu = Counter()
    for cu, inl, _vm, fs in list(csv.reader(open(f"{BLOATY}/{sut}-cu-inlines.csv")))[1:]:
        if cu.startswith("["):
            continue  # section rows handled from the cu-symbols view
        f = int(fs)
        p = inl.rsplit(":", 1)[0]
        b = next((n for n, rule in INLINE_RULES if rule(p)), None) or cu_bucket(cu)
        if b is None:
            unknown_cu[cu] += f
            b = "unattributed"
        buckets[b] += f
        if any(h in p for h in LIBSTDCXX_HDR):
            stdcxx[b] += f
    # Section / unmapped rows: no CU owns them. upb C code (no DWARF) is
    # recovered by symbol name; everything else is unattributed, itemized.
    sections = Counter()
    upb_in_sections = Counter()
    for cu, sym, _vm, fs in list(csv.reader(open(f"{BLOATY}/{sut}-cu-symbols.csv")))[1:]:
        if not cu.startswith("["):
            continue
        f = int(fs)
        if "upb" in sym or "utf8_range" in sym:
            upb_in_sections[cu] += f
            buckets["upb"] += f
        else:
            sections[cu] += f
            buckets["unattributed"] += f
    total = sum(int(r[2]) for r in list(csv.reader(open(f"{BLOATY}/{sut}-compileunits.csv")))[1:])
    return {
        "buckets": dict(buckets), "sections": dict(sections),
        "upb_in_sections": dict(upb_in_sections), "stdcxx_inlined": dict(stdcxx),
        "unknown_cu": dict(unknown_cu), "bloaty_total": total,
    }


LIB_RULES = [
    ("libopentelemetry_proto*.so", lambda n: n.startswith("libopentelemetry_proto")),
    ("gRPC", lambda n: n.startswith(("libgrpc++.so", "libgrpc.so", "libgpr.so", "libupb.so.29"))),
    ("libaddress_sorting.so.29", lambda n: n.startswith("libaddress_sorting.so")),
    ("Abseil", lambda n: n.startswith("libabsl_")),
    ("protobuf", lambda n: n.startswith("libprotobuf.so")),
    ("c-ares", lambda n: n.startswith("libcares.so")),
    ("re2", lambda n: n.startswith("libre2.so")),
    ("nghttp2", lambda n: n.startswith("libnghttp2.so")),
    ("OpenSSL", lambda n: n.startswith(("libssl.so", "libcrypto.so"))),
    ("zlib", lambda n: n.startswith("libz.so")),
    ("C/C++ runtime", lambda n: n.startswith(("libstdc++.so", "libgcc_s.so", "libc.so", "libm.so"))),
]


def shared(sut):
    rows = defaultdict(lambda: {"bytes": 0, "files": []})
    seen = set()
    # Libraries shipped unstripped are counted at their stripped size,
    # measured on a copy (real path -> bytes).
    stripped = {}
    if os.path.exists(f"{LIBS}/{sut}-stripped.txt"):
        stripped = {r: int(b) for r, b in (l.split() for l in open(f"{LIBS}/{sut}-stripped.txt"))}
    for line in open(f"{LIBS}/{sut}-libs.txt"):
        soname, _path, real, size = line.split()[:4]
        if real in seen:
            continue
        seen.add(real)
        size = stripped.get(real, size)
        b = next((n for n, rule in LIB_RULES if rule(soname)), "unattributed (shared)")
        rows[b]["bytes"] += int(size)
        rows[b]["files"].append(f"{real.rsplit('/', 1)[1]} {size}")
    return dict(rows)


out = {}
for sut in ("microtel", "otelcpp-grpc"):
    out[sut] = {"binary": binary(sut), "shared": shared(sut)}
json.dump(out, sys.stdout, indent=1)
