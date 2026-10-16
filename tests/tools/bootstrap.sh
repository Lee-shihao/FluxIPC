#!/usr/bin/env bash
#
# bootstrap.sh – install the FluxIPC test toolchain into .tools/ WITHOUT root.
#
# Pulls the following packages and extracts them into <repo>/.tools:
#   - mull-14                mutation testing (clang-14 plugin + runner)
#   - libcmocka-dev          C unit-test framework (headers + dev symlink)
#   - libcmocka0             cmocka runtime
#   - lcov                   coverage HTML/report tooling (genhtml)
#
# Requires: apt with the standard Ubuntu repos enabled, dpkg-deb, curl-less.
# Network access is used only here; runtime needs nothing but the extracted dir.
#
# Usage:  tests/tools/bootstrap.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/.tools"
STAGE="$(mktemp -d /tmp/fluxipc-tools.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

PACKAGES=(mull-14 libcmocka-dev libcmocka0 lcov)

echo "== FluxIPC test toolchain bootstrap =="
echo "   repo: $ROOT"
echo "   dest: $DEST"

mkdir -p "$DEST"

for pkg in "${PACKAGES[@]}"; do
    echo "-- downloading $pkg"
    ( cd "$STAGE" && apt-get download "$pkg" >/dev/null 2>&1 ) \
        || { echo "!! failed to download $pkg"; exit 1; }
done

for deb in "$STAGE"/*.deb; do
    echo "-- extracting $(basename "$deb")"
    dpkg-deb -x "$deb" "$DEST"
done

echo
echo "Done. Toolchain installed:"
echo "  cmocka : $DEST/usr/include/cmocka.h"
echo "  mull   : $DEST/usr/bin/mull-runner-14"
echo "           $DEST/usr/lib/mull-ir-frontend-14"
echo "  lcov   : $DEST/usr/bin/genhtml"
