#!/usr/bin/env bash
# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
#
# Clean-host package build: runs the Conan recipe in
# packaging/conan/recipes/microtel against this checkout, on a host with no
# system nghttp2, OpenSSL or zlib headers.
#
# Every other build job installs libnghttp2-dev, libssl-dev and zlib1g-dev, so
# their headers sit in /usr/include and every target finds them whether or not
# it links the dependency. A target that includes a dependency's header without
# linking it builds there and fails anywhere the dependency lives elsewhere: a
# Conan or vcpkg package, a Yocto sysroot. microtel_sdk did exactly that from
# v1.2.0 to v1.2.4. Here Conan supplies every dependency from its own prefix, so
# a missing link is a missing include path and the build fails.
#
# The recipe is used as published, except that conandata.yml is replaced by a
# single source entry for a `git archive` of HEAD, so the checkout is built
# rather than a release tarball, and no release patches are applied.
#
# Usage:  ci/scripts/conan-clean.sh
#
# Needs `conan` (2.x) on PATH and a default profile. Builds the recipe twice:
# with its default options, and with leaf and concentrator on.
#
# Exit codes:
#   0  both builds and their test_package runs passed
#   1  a build failed
#   2  the host is not clean (a system nghttp2/OpenSSL/zlib header exists)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

for header in /usr/include/nghttp2/nghttp2.h /usr/include/openssl/ssl.h /usr/include/zlib.h; do
    if [[ -e "$header" ]]; then
        echo "conan-clean: $header exists; this check needs a host without it" >&2
        exit 2
    fi
done

version="$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n 1)"
if [[ -z "$version" ]]; then
    echo "conan-clean: could not read project(microtel VERSION …) from CMakeLists.txt" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

archive="$work/microtel-$version.tar.gz"
git -C "$REPO_ROOT" archive --format=tar.gz --prefix="microtel-$version/" HEAD > "$archive"
sha256="$(sha256sum "$archive" | cut -d' ' -f1)"

cp -r "$REPO_ROOT/packaging/conan/recipes/microtel" "$work/recipe"
cat > "$work/recipe/all/conandata.yml" <<EOF
sources:
  "$version":
    url: "file://$archive"
    sha256: "$sha256"
EOF

echo "conan-clean: microtel $version from HEAD ($(git -C "$REPO_ROOT" rev-parse --short HEAD))"
conan create "$work/recipe/all" --version="$version" \
    -s compiler.cppstd=20 --build=missing
conan create "$work/recipe/all" --version="$version" \
    -s compiler.cppstd=20 --build=missing \
    -o "microtel/*:leaf=True" -o "microtel/*:concentrator=True"
echo "conan-clean: PASS"
