#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Compares two builds of the same parity image section by section, as a check
# against silent miscompiles on a host that is not memory-tested
# (bench/parity/README.md). Takes /emit_app and the regular files under
# /usr/local/lib from each image and compares every allocated ELF section
# except notes (.note.gnu.build-id and friends may differ legitimately).
#
# Usage:  bench/parity/compare_builds.sh <image-a> <image-b> <work-dir>
# Exit:   0 = every compared section identical, 1 = a section differs.
set -euo pipefail

A=${1:?usage: compare_builds.sh <image-a> <image-b> <work-dir>}
B=${2:?}
WORK=${3:?}
rm -rf "$WORK"; mkdir -p "$WORK"

extract() {
    local img=$1 dest=$2 ctr
    mkdir -p "$dest"
    ctr=$(podman create "$img")
    podman cp "$ctr:/emit_app" "$dest/emit_app"
    podman cp "$ctr:/usr/local/lib/." "$dest/lib" 2>/dev/null || mkdir -p "$dest/lib"
    podman rm "$ctr" > /dev/null
}

# Allocated, file-backed, non-note sections: name per line.
sections() {
    readelf -SW "$1" | sed -n 's/^ *\[ *[0-9]*\] *//p' \
        | awk '$2 != "NULL" && $2 != "NOTE" && $2 != "NOBITS" && $7 ~ /A/ { print $1 }'
}

extract "$A" "$WORK/a"
extract "$B" "$WORK/b"

status=0; compared=0
while IFS= read -r -d '' fa; do
    rel=${fa#"$WORK/a/"}; fb=$WORK/b/$rel
    readelf -h "$fa" > /dev/null 2>&1 || continue
    [[ -f $fb ]] || { echo "MISSING in $B: $rel"; status=1; continue; }
    for s in $(sections "$fa"); do
        objcopy -O binary --only-section="$s" "$fa" "$WORK/sa" 2>/dev/null || true
        objcopy -O binary --only-section="$s" "$fb" "$WORK/sb" 2>/dev/null || true
        compared=$(( compared + 1 ))
        if ! cmp -s "$WORK/sa" "$WORK/sb"; then
            echo "DIFF $rel $s"; status=1
        fi
    done
done < <(find "$WORK/a" -type f -print0)
rm -f "$WORK/sa" "$WORK/sb"
echo "compared $compared sections: $([[ $status == 0 ]] && echo identical || echo DIFFERENT)"
exit $status
