#!/usr/bin/env python3
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Count C++17 and C++20 features in the exporter path (bench/parity/README.md §6).

Usage:  python3 -I bench/parity/cxx_audit.py [repo-root]

A regex scan over comment-stripped source, so it counts spellings, not
semantics: a hit is "this construct appears", and some patterns over- or
under-count (noted per feature). Output is Markdown: per-feature file and
occurrence counts per area, then line counts per area.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()

# The upb + nghttp2 OTLP exporter as it would have to exist inside
# opentelemetry-cpp: transport, wire codecs and encoder, the exporter, and the
# recording path it consumes (span, tracer, batch processor, the record types).
AREAS = {
    "transport": ["src/transport"],
    "wire": ["src/wire"],
    "exporter": ["src/exporter"],
    "recording path": [
        "src/sdk/sdk_span.cpp", "src/sdk/sdk_span.hpp",
        "src/sdk/sdk_tracer.cpp", "src/sdk/sdk_tracer.hpp",
        "src/sdk/batch_span_processor.cpp", "src/sdk/batch_span_processor.hpp",
        "src/sdk/span_limits.cpp", "src/sdk/span_limits.hpp",
        "include/microtel/internal/batch.hpp",
        "include/microtel/span.hpp", "include/microtel/attribute.hpp",
        "include/microtel/context.hpp", "include/microtel/expected.hpp",
    ],
}

# (standard, feature, regex, note)
FEATURES = [
    ("C++20", "concepts / requires", r"\bconcept\b|\brequires\b", ""),
    ("C++20", "std::span", r"\bstd::span\b", ""),
    ("C++20", "ranges / views", r"\bstd::ranges::|\bstd::views::|<ranges>", ""),
    ("C++20", "<bit> (bit_cast, endian, countl_zero, ...)",
     r"\bstd::(bit_cast|endian|countl_zero|countr_zero|popcount|bit_width|bit_ceil|bit_floor|has_single_bit|rotl|rotr)\b|<bit>", ""),
    ("C++20", "designated initializers", r"[{,(]\s*\.[a-z_][a-z0-9_]*\s*=[^=]",
     "aggregate init `{.field = ...}`; C++14 needs positional init or setters"),
    ("C++20", "coroutines", r"\bco_(await|yield|return)\b", ""),
    ("C++20", "three-way comparison / defaulted ==",
     r"<=>|operator==\s*\([^)]*\)\s*const\s*(noexcept\s*)?=\s*default", ""),
    ("C++20", "consteval / constinit", r"\bconsteval\b|\bconstinit\b", ""),
    ("C++20", "std::jthread / stop_token", r"\bstd::(jthread|stop_token|stop_source)\b", ""),
    ("C++20", "<format>", r"\bstd::format\b|<format>", ""),
    ("C++20", "[[likely]] / [[unlikely]]", r"\[\[(un)?likely\]\]", ""),
    ("C++20", "using enum", r"\busing enum\b", ""),
    ("C++20", "template lambdas", r"\[[^\]]*\]\s*<\s*(typename|class|auto)", ""),
    ("C++20", "starts_with / ends_with / contains",
     r"\.(starts_with|ends_with|contains)\(", "string_view/string/map members"),
    ("C++20", "std::erase / erase_if", r"\bstd::erase(_if)?\(", ""),
    ("C++20", "std::remove_cvref / type_identity", r"\bstd::(remove_cvref|type_identity)", ""),
    ("C++20", "std::ssize / to_array / midpoint", r"\bstd::(ssize|to_array|midpoint)\b", ""),
    ("C++20", "atomic wait/notify, latch, barrier, atomic_ref",
     r"\.(wait|notify_one|notify_all)\(\s*[^)]*\)\s*;.*atomic|\bstd::(latch|barrier|atomic_ref)\b",
     "atomic wait under-counted: condition_variable wait/notify is C++11 and excluded"),
    ("C++17", "std::optional", r"\bstd::optional\b|\bstd::nullopt\b", ""),
    ("C++17", "std::string_view", r"\bstd::string_view\b|\bstd::(u8)?string_view_literals\b", ""),
    ("C++17", "std::variant / visit", r"\bstd::(variant|visit|holds_alternative|get_if|monostate)\b", ""),
    ("C++17", "if constexpr", r"\bif\s+constexpr\b", ""),
    ("C++17", "structured bindings", r"\bauto\s*&{0,2}\s*\[[a-z_][^\]]*\]\s*[=:]", ""),
    ("C++17", "if / switch with initializer", r"\b(if|switch)\s*\([^;()]*(\([^()]*\)[^;()]*)*;[^)]", "approximate"),
    ("C++17", "nested namespace a::b", r"\bnamespace\s+\w+::\w+", ""),
    ("C++17", "inline variables", r"\binline\s+(constexpr\s+)?[a-z_:<>]+\s+[a-z_]\w*\s*=", ""),
    ("C++17", "[[nodiscard]] / [[maybe_unused]] / [[fallthrough]]",
     r"\[\[(nodiscard|maybe_unused|fallthrough)\]\]", "attributes: removable, C++14 ignores-or-errors"),
    ("C++17", "std::scoped_lock", r"\bstd::scoped_lock\b", ""),
    ("C++17", "std::byte", r"\bstd::byte\b", ""),
    ("C++17", "fold expressions", r"\(\s*\.\.\.\s*[&|+,]{1,2}|[&|+,]{1,2}\s*\.\.\.\s*\)", ""),
    ("C++17", "CTAD-only spellings (scoped_lock/lock_guard/unique_lock without <>)",
     r"\bstd::(lock_guard|unique_lock|scoped_lock)\s+\w+\s*[{(]", ""),
    ("C++17", "std::invoke / apply / is_invocable", r"\bstd::(invoke|apply|is_invocable\w*|invoke_result\w*)\b", ""),
    ("C++17", "std::filesystem", r"\bstd::filesystem\b|<filesystem>", ""),
]

COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
STRING_RE = re.compile(r'"(?:\\.|[^"\\\n])*"')


def files_for(entries):
    out = []
    for e in entries:
        p = ROOT / e
        if p.is_dir():
            out += sorted(q for q in p.rglob("*") if q.suffix in (".cpp", ".hpp", ".h", ".cc"))
        elif p.exists():
            out.append(p)
    return out


def scrub(text):
    return STRING_RE.sub('""', COMMENT_RE.sub("", text))


def main():
    areas = {a: files_for(e) for a, e in AREAS.items()}
    texts = {f: scrub(f.read_text(errors="replace")) for fs in areas.values() for f in fs}
    print("| Std | Feature | " + " | ".join(f"{a} (files / uses)" for a in areas) + " | Note |")
    print("|---|---|" + "---:|" * len(areas) + "---|")
    for std, name, rx, note in FEATURES:
        r = re.compile(rx, re.M)
        cells = []
        for a, fs in areas.items():
            hits = [len(r.findall(texts[f])) for f in fs]
            nf = sum(1 for h in hits if h)
            cells.append(f"{nf} / {sum(hits)}" if nf else "–")
        print(f"| {std} | {name} | " + " | ".join(cells) + f" | {note} |")
    print()
    print("| Area | Files | Lines (with comments) | Code lines (non-blank, comments stripped) |")
    print("|---|---:|---:|---:|")
    for a, fs in areas.items():
        raw = sum(len(f.read_text(errors="replace").splitlines()) for f in fs)
        code = sum(sum(1 for ln in texts[f].splitlines() if ln.strip()) for f in fs)
        print(f"| {a} | {len(fs)} | {raw:,} | {code:,} |")


if __name__ == "__main__":
    main()
