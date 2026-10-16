#!/usr/bin/env bash
#
# mutate.sh – build the unit suite with Mull instrumentation and run Mull.
#
# Requires:
#   - make tools  (mull-14 + cmocka in .tools/)
#   - clang-14 on PATH
#
# The mutation score gate is MUT_SCORE_MIN (default 75).
#
# NOTE: every translation unit must be compiled in its own clang invocation.
# Mull (0.34.x) re-parses each source using the command line recorded via
# -grecord-command-line; a single multi-file invocation produces N compiler
# jobs and Mull refuses to parse the file.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SCORE_MIN="${MUT_SCORE_MIN:-75}"
WORKERS="${MUT_WORKERS:-0}"
BUILD="$ROOT/build/mull"
OUT="$ROOT/build/mutation"
TOOLS="$ROOT/.tools"
CLANG="${CLANG:-clang-14}"

FRONTEND="$TOOLS/usr/lib/mull-ir-frontend-14"
RUNNER="$TOOLS/usr/bin/mull-runner-14"

if [ ! -x "$RUNNER" ] || [ ! -f "$FRONTEND" ]; then
    echo "error: Mull not found; run 'make tools'"; exit 1
fi

CJSON_CFLAGS="$(pkg-config --cflags libcjson 2>/dev/null || true)"
RL_CFLAGS="$(pkg-config --cflags readline 2>/dev/null || true)"
CJSON_LIBS="$(pkg-config --libs libcjson 2>/dev/null || echo -lcjson)"

# -grecord-command-line lets Mull recover per-TU compiler flags; -fno-inline
# keeps mutants visible at -O0.
#
# The oracle is strengthened with AddressSanitizer + UBSan: mutants that
# introduce out-of-bounds access, use-after-free or UB abort the test program
# and are counted as killed (this is what makes memory-safety bugs observable).
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
MCFLAGS="-Wall -O0 -g -fno-inline -grecord-command-line -Iinclude \
 -I$TOOLS/usr/include -D_GNU_SOURCE -Wno-unused-parameter -DFLUXIPC_TESTING \
 $SAN -fpass-plugin=$FRONTEND $CJSON_CFLAGS $RL_CFLAGS"
MLIBS="-lrt -lpthread -lreadline $CJSON_LIBS \
 -L$TOOLS/usr/lib/x86_64-linux-gnu -lcmocka $SAN \
 -Wl,-rpath,$TOOLS/usr/lib/x86_64-linux-gnu"
# Leak detection is disabled: the Mull runner's own allocations are reported
# as leaks and make the baseline run fail.  Memory errors still abort.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:exitcode=99"

LIB_SRCS=(src/fluxipc_core.c src/fluxipc_tree.c src/fluxipc_sock.c \
          src/fluxipc_conn.c src/fluxipc_util.c src/fluxipc_http.c \
          src/fluxipc_interactive.c)
UNIT_SRCS=(tests/unit/ut_main.c tests/unit/ut_common.c \
           tests/unit/test_protocol.c tests/unit/test_util.c \
           tests/unit/test_tree.c tests/unit/test_core.c \
           tests/unit/test_sock.c tests/unit/test_conn.c \
           tests/unit/test_http.c tests/unit/test_interactive.c \
           tests/unit/test_cli.c)

# The interactive module is pulled in by test_interactive.c.
ULIB=()
for s in "${LIB_SRCS[@]}"; do
    [ "$s" = "src/fluxipc_interactive.c" ] && continue
    ULIB+=("$s")
done

echo "== building Mull-instrumented unit suite (per-TU) =="
rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD/obj" "$OUT"

OBJS=()
for src in "${UNIT_SRCS[@]}" "${ULIB[@]}"; do
    obj="$BUILD/obj/$(basename "$src" .c).o"
    echo "   $src"
    "$CLANG" $MCFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done

"$CLANG" -o "$BUILD/fluxipc-ut-mull" "${OBJS[@]}" $MLIBS

echo "== mull-runner (threshold ${SCORE_MIN}%) =="
RUNNER_ARGS=(
    --mutation-score-threshold "$SCORE_MIN"
    --reporters IDE
    --report-dir "$OUT"
    --timeout 3000
    --ld-search-path "$TOOLS/usr/lib/x86_64-linux-gnu"
    --ld-search-path /lib/x86_64-linux-gnu
    --ld-search-path /usr/lib/x86_64-linux-gnu
)
[ "$WORKERS" != "0" ] && RUNNER_ARGS+=(--workers "$WORKERS")
"$RUNNER" "${RUNNER_ARGS[@]}" "$BUILD/fluxipc-ut-mull"
