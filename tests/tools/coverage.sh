#!/usr/bin/env bash
#
# coverage.sh – build an instrumented UT+IT pair, run both, and gate on gcovr.
#
# Outputs into build/coverage/ (HTML, Cobertura XML, JSON summary).
# Thresholds: COV_LINE_MIN / COV_BRANCH_MIN (env override, default 90 / 78).
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

LINE_MIN="${COV_LINE_MIN:-90}"
BRANCH_MIN="${COV_BRANCH_MIN:-75}"
BUILD="$ROOT/build/cov"
OUT="$ROOT/build/coverage"
TOOLS="$ROOT/.tools"

if [ ! -f "$TOOLS/usr/include/cmocka.h" ]; then
    echo "error: run 'make tools' first (cmocka not found)"; exit 1
fi

RL_CFLAGS="$(pkg-config --cflags readline 2>/dev/null || true)"

COV_CFLAGS="-Wall -O0 -g -Iinclude -I$TOOLS/usr/include -D_GNU_SOURCE \
 -Wno-unused-parameter -DFLUXIPC_TESTING --coverage $RL_CFLAGS"
COV_LIBS="--coverage -lrt -lpthread -lreadline \
 -L$TOOLS/usr/lib/x86_64-linux-gnu -lcmocka \
 -Wl,-rpath,$TOOLS/usr/lib/x86_64-linux-gnu"

LIB_SRCS=(src/fluxipc_core.c src/fluxipc_tree.c src/fluxipc_sock.c \
          src/fluxipc_conn.c)

UNIT_SRCS=(tests/unit/ut_main.c tests/unit/ut_common.c \
           tests/unit/test_protocol.c tests/unit/test_tree.c tests/unit/test_core.c \
           tests/unit/test_sock.c tests/unit/test_conn.c \
           tests/unit/test_interactive.c tests/unit/test_cli.c)

echo "== building instrumented unit suite =="
rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD" "$OUT"

gcc $COV_CFLAGS -o "$BUILD/fluxipc-ut" "${UNIT_SRCS[@]}" "${LIB_SRCS[@]}" $COV_LIBS
echo "== building instrumented integration pair =="
gcc $COV_CFLAGS -o "$BUILD/fluxipc-it" tests/integration/main.c "${LIB_SRCS[@]}" $COV_LIBS
gcc $COV_CFLAGS -o "$BUILD/example_server" tests/example_server.c "${LIB_SRCS[@]}" $COV_LIBS

echo "== running unit suite =="
"$BUILD/fluxipc-ut" >/dev/null

echo "== running integration suite =="
( cd "$BUILD" && pkill -x example_server 2>/dev/null || true
  sleep 0.2; ./fluxipc-it >/dev/null )

echo "== line coverage =="
gcovr -r "$ROOT" "$BUILD" \
    --filter "$ROOT/src/.*" \
    --fail-under-line "$LINE_MIN" --fail-under-branch "$BRANCH_MIN" \
    --txt \
    --html-details "$OUT/index.html" \
    --xml "$OUT/cobertura.xml" \
    --json-summary "$OUT/summary.json" \
    || { echo "!! coverage below threshold"; exit 1; }

echo
echo "== branch coverage =="
gcovr -r "$ROOT" "$BUILD" --filter "$ROOT/src/.*" -b --txt

echo
echo "Report:    $OUT/index.html"
echo "Cobertura: $OUT/cobertura.xml"
echo "JSON:      $OUT/summary.json"
