CC       ?= gcc
CFLAGS   += -Wall -Wextra -O2 -g -std=gnu11 \
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
            src/fluxipc_sock.c \
            src/fluxipc_conn.c \
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

.PHONY: all clean install uninstall distclean lib static cli examples debug compdb ut it test cov mutate tools check-tools ut-asan it-asan bench test-mem

all: lib static cli examples

lib: $(TARGET_LIB)

static: $(TARGET_STATIC)

cli: $(TARGET_CLI)

examples: example_server

# ── Tests ────────────────────────────────────────────
#
# The unit suite uses cmocka; the mutation build uses Mull (clang-14 plugin).
# Both live in .tools/ and are installed by tests/tools/bootstrap.sh (no root).
#
#   make tools     – download + extract the toolchain into .tools/
#   make ut        – run the cmocka unit suite
#   make it        – run the IPC integration suite
#   make cov       – gcovr coverage report + threshold gate
#   make mutate    – Mull mutation testing + score gate

TOOLS_DIR      ?= .tools
CMOCKA_PREFIX  ?= $(TOOLS_DIR)/usr
CMOCKA_CFLAGS  := -I$(abspath $(CMOCKA_PREFIX))/include
CMOCKA_LIBDIR  := $(abspath $(CMOCKA_PREFIX))/lib/x86_64-linux-gnu
CMOCKA_LDFLAGS := -L$(CMOCKA_LIBDIR) -lcmocka -Wl,-rpath,$(CMOCKA_LIBDIR)

MULL_RUNNER    := $(abspath $(TOOLS_DIR))/usr/bin/mull-runner-14
MULL_FRONTEND  := $(abspath $(TOOLS_DIR))/usr/lib/mull-ir-frontend-14
CLANG          ?= clang-14

# Coverage / mutation thresholds (see tests/UNIT_TEST_PLAN.md).
COV_LINE_MIN   ?= 90
COV_BRANCH_MIN ?= 75
MUT_SCORE_MIN  ?= 70

TEST_CFLAGS := $(CFLAGS) $(CMOCKA_CFLAGS) -DFLUXIPC_TESTING

# Unit tests are built straight from sources (never via the static archive) so
# that coverage/mutation instrumentation sees every translation unit.  The
# interactive module is pulled in by test_interactive.c (white-box include),
# so it is excluded from the link list to avoid duplicate symbols.
UT_BIN       := fluxipc-ut
IT_BIN       := fluxipc-it
BENCH_BIN    := fluxipc-bench
UT_SRCS      := tests/unit/ut_main.c tests/unit/ut_common.c \
                tests/unit/test_protocol.c \
                tests/unit/test_tree.c tests/unit/test_core.c \
                tests/unit/test_sock.c tests/unit/test_conn.c \
                tests/unit/test_interactive.c tests/unit/test_cli.c
UT_LIB_SRCS  := $(filter-out src/fluxipc_interactive.c,$(LIB_SRCS))
IT_SRCS      := tests/integration/main.c
BENCH_SRCS   := tests/bench/bench.c

# AddressSanitizer + UBSan integration pair (memory errors / leaks).
ASAN_DIR     := build/asan
IT_ASAN_BIN  := $(ASAN_DIR)/fluxipc-it
IT_ASAN_SRV  := $(ASAN_DIR)/example_server
ASAN_FLAGS   := -fsanitize=address,undefined -fno-omit-frame-pointer

check-tools:
	@test -f "$(CMOCKA_LIBDIR)/libcmocka.so" || { \
		echo "error: test toolchain missing; run 'make tools' first"; exit 1; }

$(UT_BIN): include/fluxipc.h include/fluxipc_internal.h
$(UT_BIN): check-tools $(UT_SRCS) $(UT_LIB_SRCS)
	$(CC) $(TEST_CFLAGS) -o $@ $(UT_SRCS) $(UT_LIB_SRCS) $(TEST_LDFLAGS) \
	    $(LDLIBS) $(CMOCKA_LDFLAGS)

$(IT_BIN): include/fluxipc.h include/fluxipc_internal.h
$(IT_BIN): $(IT_SRCS) $(TARGET_STATIC)
	$(CC) $(CFLAGS) -o $@ $(IT_SRCS) $(TARGET_STATIC) $(LDFLAGS) $(LDLIBS)

# Unit tests: cmocka suites, fork-free, run in-process.
ut: $(UT_BIN)
	./$(UT_BIN)

# Integration tests: forks example_server, exercises IPC protocol
# robustness, concurrency, large payloads and RSS growth.
it: $(IT_BIN) example_server
	-@pkill -x example_server 2>/dev/null; sleep 0.2
	./$(IT_BIN)

# ASan/UBSan integration pair: both client and server are instrumented so
# LeakSanitizer catches leaks in the real server lifecycle.
$(IT_ASAN_BIN): include/fluxipc.h include/fluxipc_internal.h
$(IT_ASAN_BIN): $(IT_SRCS) $(LIB_SRCS)
	@mkdir -p $(ASAN_DIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ $(IT_SRCS) $(LIB_SRCS) \
	    $(LDFLAGS) $(LDLIBS) $(ASAN_FLAGS)

$(IT_ASAN_SRV): include/fluxipc.h
$(IT_ASAN_SRV): tests/example_server.c $(LIB_SRCS)
	@mkdir -p $(ASAN_DIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o $@ tests/example_server.c $(LIB_SRCS) \
	    $(LDFLAGS) $(LDLIBS) $(ASAN_FLAGS)

it-asan: $(IT_ASAN_BIN) $(IT_ASAN_SRV)
	-@pkill -x example_server 2>/dev/null; sleep 0.2
	FLUXIPC_EXAMPLE_SERVER=$(abspath $(IT_ASAN_SRV)) \
	FLUXIPC_TEST_VERBOSE=1 \
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(IT_ASAN_BIN)

# Performance baseline: p50/p95/p99, throughput, server RSS.
$(BENCH_BIN): include/fluxipc.h
$(BENCH_BIN): $(BENCH_SRCS) $(TARGET_STATIC)
	$(CC) $(CFLAGS) -o $@ $(BENCH_SRCS) $(TARGET_STATIC) $(LDFLAGS) $(LDLIBS)

bench: $(BENCH_BIN) example_server
	-@pkill -x example_server 2>/dev/null; sleep 0.2
	./$(BENCH_BIN)

# Memory-safety gate: unit ASan + full integration ASan/leak check.
test-mem: ut-asan it-asan

# ── Toolchain bootstrap ──────────────────────────────

tools:
	tests/tools/bootstrap.sh

test: ut it

# Coverage: instrumented UT + IT, gcovr report + threshold gate.
cov:
	tests/tools/coverage.sh

# Mutation testing: Mull instrumented UT + score gate.
mutate:
	tests/tools/mutate.sh

# AddressSanitizer + UBSan build of the unit suite (leak-checked).
ASAN_BIN := $(UT_BIN)-asan
ut-asan: check-tools $(UT_SRCS) $(UT_LIB_SRCS)
	$(CC) $(TEST_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(ASAN_BIN) $(UT_SRCS) $(UT_LIB_SRCS) $(TEST_LDFLAGS) \
	    $(LDLIBS) $(CMOCKA_LDFLAGS) -fsanitize=address,undefined
	ASAN_OPTIONS=detect_leaks=1 ./$(ASAN_BIN)

debug:
	$(MAKE) all

# Regenerate compile_commands.json for clangd / VS Code IntelliSense.
# Needs `bear` (apt install bear).  Clean build so every file is captured.
compdb:
	bear -- $(MAKE) clean all
	@echo "compile_commands.json regenerated"

# ── Shared library ───────────────────────────────────

$(TARGET_LIB): $(LIB_PIC_OBJS)
	$(CC) $(LDFLAGS) -shared \
	    -Wl,-soname,$(SONAME) \
	    -o $@ \
	    $^ $(LDLIBS)
	ln -sf $(TARGET_LIB) $(SONAME)
	ln -sf $(TARGET_LIB) libfluxipc.so

# Header changes must rebuild every dependent object; otherwise a struct
# layout change (e.g. fluxipc_conn_t) can leave stale objects with mismatched
# ABIs and cause crashes.
$(LIB_OBJS) $(LIB_PIC_OBJS): include/fluxipc.h include/fluxipc_internal.h
cli/fluxipc_cli.o: include/fluxipc.h

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

example_server: include/fluxipc.h
example_server: tests/example_server.c $(TARGET_STATIC)
	$(CC) $(CFLAGS) -o $@ $< $(TARGET_STATIC) $(LDLIBS)

# ── pkg-config ───────────────────────────────────────

PKG_CONFIG := fluxipc.pc

$(PKG_CONFIG):
	printf '%s\n' \
	    "prefix=$(PREFIX)" \
	    "exec_prefix=\$${prefix}" \
	    "libdir=\$${prefix}/lib" \
	    "includedir=\$${prefix}/include/fluxipc" \
	    '' \
	    "Name: FluxIPC" \
	    "Description: Namespace-tree IPC library for Linux" \
	    "Version: $(VERSION)" \
	    "Libs: -L\$${libdir} -lfluxipc -lpthread -lrt -lreadline" \
	    "Cflags: -I\$${includedir}" \
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
	rm -f $(UT_BIN) $(ASAN_BIN) $(IT_BIN) $(BENCH_BIN)
	rm -rf $(ASAN_DIR)
	rm -f $(PKG_CONFIG)
	rm -f /tmp/fluxipc_*.sock

distclean: clean
