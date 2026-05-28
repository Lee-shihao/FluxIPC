#!/usr/bin/env bash
# fluxipc install/uninstall helper
# Usage: ./packaging/install.sh [install|uninstall] [prefix]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
ACTION="${1:-install}"
PREFIX="${2:-/usr/local}"
BUILD_DIR="$ROOT_DIR/build"

install_fluxipc() {
    echo "[fluxipc] Building with prefix=$PREFIX ..."
    mkdir -p "$BUILD_DIR"
    make -C "$ROOT_DIR" all PREFIX="$PREFIX"

    echo "[fluxipc] Installing to $PREFIX ..."
    make -C "$ROOT_DIR" install PREFIX="$PREFIX"

    echo "[fluxipc] Creating runtime directories ..."
    install -d -m 0755 /run/fluxipc/bin 2>/dev/null || true
    if [ -d /etc/tmpfiles.d ]; then
        cat > /etc/tmpfiles.d/fluxipc.conf <<'EOF'
d /run/fluxipc     0755 root root -
d /run/fluxipc/bin 0755 root root -
EOF
        echo "[fluxipc] Written /etc/tmpfiles.d/fluxipc.conf"
    fi

    if command -v ldconfig &>/dev/null; then
        ldconfig
        echo "[fluxipc] ldconfig updated"
    fi

    echo "[fluxipc] Installation complete."
    echo "  Library : $PREFIX/lib/libfluxipc.so.1.0.0"
    echo "  Header  : $PREFIX/include/fluxipc.h"
    echo "  CLI     : $PREFIX/bin/fluxipc-cli"
    echo "  pkg-cfg : $PREFIX/lib/pkgconfig/fluxipc.pc"
}

uninstall_fluxipc() {
    echo "[fluxipc] Uninstalling from prefix=$PREFIX ..."
    make -C "$ROOT_DIR" uninstall PREFIX="$PREFIX"

    rmdir /run/fluxipc/bin /run/fluxipc 2>/dev/null || true
    rm -f /etc/tmpfiles.d/fluxipc.conf

    if command -v ldconfig &>/dev/null; then ldconfig; fi
    echo "[fluxipc] Uninstall complete."
}

case "$ACTION" in
    install)   install_fluxipc ;;
    uninstall) uninstall_fluxipc ;;
    *)
        echo "Usage: $0 [install|uninstall] [prefix]"
        exit 1
        ;;
esac
