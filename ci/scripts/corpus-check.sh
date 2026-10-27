#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Crash-corpus regression gate. Replays every committed crashing input under
# tests/fuzz/crashes/<target>/ against its fuzz harness and fails if any of
# them still crashes.
#
# Fuzzing itself is a periodic job, not a PR gate — it is slow and its findings
# are non-deterministic (tests/fuzz/README.md). This script is the part that
# *is* a hard gate: once a crash is found and fixed, its input stays committed
# so the same bug cannot come back silently. Replay is deterministic and takes
# well under a second per input, so it is cheap enough to run on every PR.
#
# libFuzzer runs one input per invocation when given a file rather than a
# directory, and exits non-zero if the harness crashes, asserts, or trips a
# sanitizer. That exit status is the whole test.
#
# Usage:  ci/scripts/corpus-check.sh [build-dir]     (default: build-fuzz)
#
# Exit codes:
#   0  every committed crash input replayed cleanly (or none are committed)
#   1  at least one input still crashes — a regression
#   2  the gate could not run (no build dir, harness binary missing)

set -euo pipefail

BUILD_DIR="${1:-build-fuzz}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CRASH_ROOT="${REPO_ROOT}/tests/fuzz/crashes"

# Per-input wall-clock ceiling. A replay that hangs is a failure too, and
# without this the gate would block a PR queue instead of reporting.
readonly REPLAY_TIMEOUT_SECS=60

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "corpus-check: build directory '$BUILD_DIR' not found" >&2
    echo "corpus-check: fuzz targets are clang-only; build them with" >&2
    echo "    cmake -S . -B $BUILD_DIR \\" >&2
    echo "          -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \\" >&2
    echo "          -DMICROTEL_BUILD_FUZZ=ON -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_BUILD_TESTS=OFF \\" >&2
    echo "          -DMICROTEL_WITH_CONCENTRATOR=ON" >&2
    echo "    cmake --build $BUILD_DIR" >&2
    exit 2
fi

if [[ ! -d "$CRASH_ROOT" ]]; then
    echo "corpus-check: '$CRASH_ROOT' not found — expected one directory per harness" >&2
    exit 2
fi

# The crashes/ tree is the source of truth for which harnesses have a corpus.
# Deriving the target list from it (rather than hardcoding names here) means a
# new harness is covered as soon as someone commits its first finding, with no
# second place to remember to update.
mapfile -t TARGETS < <(find "$CRASH_ROOT" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    echo "corpus-check: no harness directories under tests/fuzz/crashes/" >&2
    echo "corpus-check: refusing to report success for a gate that checked nothing" >&2
    exit 2
fi

echo "corpus-check: ${#TARGETS[@]} harness corpora under tests/fuzz/crashes/"

failures=0
replayed=0
harnesses_with_inputs=0

for target in "${TARGETS[@]}"; do
    binary="${BUILD_DIR}/tests/fuzz/${target}"

    # A missing binary is a setup failure, not a pass. Skipping it would let a
    # renamed or dropped harness silently stop being checked while the gate
    # still reported green — the exact failure mode this script exists to
    # prevent for the code under test.
    if [[ ! -x "$binary" ]]; then
        echo "corpus-check: harness binary '$binary' not found or not executable" >&2
        echo "corpus-check: tests/fuzz/crashes/${target}/ exists, so it must be built" >&2
        exit 2
    fi

    # Exclude .gitkeep and any other dotfile: the directories are committed
    # empty so the layout is self-documenting before the first finding lands.
    mapfile -t inputs < <(
        find "${CRASH_ROOT}/${target}" -mindepth 1 -maxdepth 1 -type f \
            -not -name '.*' -printf '%p\n' | sort
    )

    if [[ ${#inputs[@]} -eq 0 ]]; then
        echo "  ${target}: no committed crashes"
        continue
    fi

    harnesses_with_inputs=$((harnesses_with_inputs + 1))
    echo "  ${target}: replaying ${#inputs[@]} input(s)"

    for input in "${inputs[@]}"; do
        replayed=$((replayed + 1))
        # -runs=1 keeps libFuzzer from mutating; we want this exact input only.
        # Capture status explicitly rather than reading $? after an `if`, where
        # it is easy to pick up the wrong command's status.
        set +e
        output=$(timeout "$REPLAY_TIMEOUT_SECS" "$binary" -runs=1 "$input" 2>&1)
        status=$?
        set -e

        if [[ $status -eq 0 ]]; then
            continue
        fi

        failures=$((failures + 1))
        if [[ $status -eq 124 ]]; then
            echo "    FAIL ${input##*/}: timed out after ${REPLAY_TIMEOUT_SECS}s" >&2
        else
            echo "    FAIL ${input##*/}: exit $status" >&2
        fi
        # The harness output carries the sanitizer report or assertion text,
        # which is the only thing that tells a reviewer what regressed.
        echo "$output" | sed 's/^/      /' >&2
    done
done

if [[ $failures -gt 0 ]]; then
    echo "corpus-check: FAIL — $failures of $replayed committed crash input(s) still crash" >&2
    exit 1
fi

if [[ $replayed -eq 0 ]]; then
    # Legitimate and currently the normal state: every crashes/ directory is
    # committed empty until a finding is confirmed. Passing is correct, but say
    # plainly that nothing was exercised so a green tick is not misread as
    # coverage.
    echo "corpus-check: clean — no crash inputs committed yet (nothing to replay)"
    exit 0
fi

echo "corpus-check: clean — $replayed input(s) across $harnesses_with_inputs harness(es) replayed without crashing"
