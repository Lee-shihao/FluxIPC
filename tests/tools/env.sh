#!/usr/bin/env bash
#
# env.sh – source this to put the locally extracted test toolchain on PATH.
#
#   source tests/tools/env.sh
#
# Exports:
#   FLUXIPC_TOOLS      root of the extracted toolchain (.tools)
#   CMOCKA_PREFIX      cmocka install prefix
#   MULL_FRONTEND      clang-14 pass plugin (.so)
#   MULL_RUNNER        mull runner binary
#   LD_LIBRARY_PATH    extended with the extracted libcmocka
#
_FLUXIPC_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export FLUXIPC_TOOLS="$(cd "$_FLUXIPC_ENV_DIR/../.." && pwd)/.tools"
export CMOCKA_PREFIX="$FLUXIPC_TOOLS/usr"
export MULL_FRONTEND="$FLUXIPC_TOOLS/usr/lib/mull-ir-frontend-14"
export MULL_RUNNER="$FLUXIPC_TOOLS/usr/bin/mull-runner-14"
export LD_LIBRARY_PATH="$CMOCKA_PREFIX/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$CMOCKA_PREFIX/bin:$FLUXIPC_TOOLS/usr/bin:${PATH}"
unset _FLUXIPC_ENV_DIR
