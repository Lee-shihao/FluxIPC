SUMMARY = "FluxIPC shared memory IPC library"
DESCRIPTION = "High-performance shared memory IPC framework"
HOMEPAGE = "https://example.com/fluxipc"
SECTION = "libs"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=29556d878be283d63551e090338bf46a"

SRC_URI = "file://FluxIPC-main.tar.gz"
# SRC_URI[sha256sum] = "f5fa0c0853647721e097d5c3096026bac25055e098c0a395d22d4cffb463b184"
S = "${WORKDIR}/FluxIPC-main"

inherit pkgconfig

# readline 依赖
DEPENDS += "readline"

# ─────────────────────────────────────────────
# Build
# ─────────────────────────────────────────────

do_compile() {
    oe_runmake all
}

# ─────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────

do_install() {
    oe_runmake install \
        DESTDIR=${D} \
        PREFIX=${prefix} \
        LIBDIR=${libdir} \
        INCLUDEDIR=${includedir}/fluxipc
}

# ─────────────────────────────────────────────
# Package split
# ─────────────────────────────────────────────

# runtime package
FILES:${PN} += "\
    ${libdir}/libfluxipc.so.* \
"

# development package
FILES:${PN}-dev += "\
    ${libdir}/libfluxipc.so \
    ${includedir}/fluxipc \
"

# debug symbols
FILES:${PN}-dbg += "\
    ${libdir}/.debug \
"

# ─────────────────────────────────────────────
# QA
# ─────────────────────────────────────────────

# 避免开发链接检查误报
INSANE_SKIP:${PN} += "dev-so"

# readline runtime dependency
RDEPENDS:${PN} += "readline"
