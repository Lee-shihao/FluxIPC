CC       ?= gcc
CFLAGS   += -Wall -Wextra -O2 -g \
            -Iinclude \
            -D_GNU_SOURCE \
            -Wno-unused-parameter \
            $(shell pkg-config --cflags readline 2>/dev/null)
LDFLAGS  +=
LDLIBS   += -lrt -lpthread -lreadline

VERSION      ?= 1.0.0
PREFIX       ?= /usr
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include/fluxipc
BINDIR       ?= $(PREFIX)/bin
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

# ── Sources ──────────────────────────────────────────

LIB_SRCS := src/fluxipc_core.c \
            src/fluxipc_tree.c \
            src/fluxipc_shm.c  \
            src/fluxipc_sock.c \
            src/fluxipc_symlink.c \
            src/fluxipc_interactive.c

CLI_SRCS := cli/fluxipc_cli.c $(LIB_SRCS)

LIB_PIC_OBJS := $(LIB_SRCS:.c=.pic.o)
LIB_OBJS     := $(LIB_SRCS:.c=.o)
CLI_OBJS     := $(CLI_SRCS:.c=.o)

TARGET_LIB  := libfluxipc.so.1.0.0
SONAME      := libfluxipc.so.1
TARGET_STATIC := libfluxipc.a
TARGET_CLI  := fluxipc-cli

# ── Top-level targets ────────────────────────────────

.PHONY: all clean install uninstall distclean lib static cli examples

all: lib static cli examples

lib: $(TARGET_LIB)

static: $(TARGET_STATIC)

cli: $(TARGET_CLI)

examples: example_server

# ── Shared library ───────────────────────────────────

$(TARGET_LIB): $(LIB_PIC_OBJS)
	$(CC) $(LDFLAGS) -shared \
	    -Wl,-soname,$(SONAME) \
	    -o $@ \
	    $^ $(LDLIBS)
	ln -sf $(TARGET_LIB) $(SONAME)
	ln -sf $(TARGET_LIB) libfluxipc.so

src/%.pic.o: src/%.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# ── Static library ───────────────────────────────────

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET_STATIC): $(LIB_OBJS)
	ar rcs $@ $^

# ── CLI ──────────────────────────────────────────────

cli/%.o: cli/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET_CLI): $(CLI_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# ── Examples ─────────────────────────────────────────

example_server: tests/example_server.c $(TARGET_STATIC)
	$(CC) $(CFLAGS) -rdynamic -o $@ $< $(TARGET_STATIC) $(LDLIBS)

# ── pkg-config ───────────────────────────────────────

PKG_CONFIG := fluxipc.pc

$(PKG_CONFIG):
	printf '%s\n' \
	    "prefix=$(PREFIX)" \
	    "exec_prefix=$${prefix}" \
	    "libdir=$${prefix}/lib" \
	    "includedir=$${prefix}/include" \
	    '' \
	    "Name: FluxIPC" \
	    "Description: Lightweight namespace-tree IPC library with shared-memory registry" \
	    "Version: $(VERSION)" \
	    "Libs: -L$${libdir} -lfluxipc -lpthread -lrt -lreadline" \
	    "Cflags: -I$${includedir}" \
	    > $@

# ── Install ──────────────────────────────────────────

install: all $(PKG_CONFIG)
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -m 755 $(TARGET_LIB) $(DESTDIR)$(LIBDIR)/
	ln -sf $(TARGET_LIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME)       $(DESTDIR)$(LIBDIR)/libfluxipc.so
	install -m 644 $(TARGET_STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 644 include/fluxipc.h $(DESTDIR)$(INCLUDEDIR)/
	install -m 755 $(TARGET_CLI) $(DESTDIR)$(BINDIR)/
	install -m 644 $(PKG_CONFIG) $(DESTDIR)$(PKGCONFIGDIR)/
	ldconfig $(DESTDIR)$(LIBDIR) 2>/dev/null || true
	@echo "FluxIPC installed to $(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET_LIB)
	rm -f $(DESTDIR)$(LIBDIR)/$(SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libfluxipc.so
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET_STATIC)
	rm -f $(DESTDIR)$(INCLUDEDIR)/fluxipc.h
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET_CLI)
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/$(PKG_CONFIG)
	-rmdir --ignore-fail-on-non-empty $(DESTDIR)$(PKGCONFIGDIR)
	-rmdir --ignore-fail-on-non-empty $(DESTDIR)$(BINDIR)
	-rmdir --ignore-fail-on-non-empty $(DESTDIR)$(INCLUDEDIR)
	-rmdir --ignore-fail-on-non-empty $(DESTDIR)$(LIBDIR)
	ldconfig $(DESTDIR)$(LIBDIR) 2>/dev/null || true
	@echo "FluxIPC uninstalled"

# ── Clean ────────────────────────────────────────────

clean:
	rm -f src/*.o src/*.pic.o cli/*.o
	rm -f $(TARGET_LIB) $(SONAME) libfluxipc.so
	rm -f $(TARGET_STATIC)
	rm -f $(TARGET_CLI)
	rm -f example_server
	rm -f $(PKG_CONFIG)
	rm -f /dev/shm/fluxipc_*
	rm -f /tmp/fluxipc_*.sock

distclean: clean
