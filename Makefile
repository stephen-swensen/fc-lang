# === Configuration (overridable) ===

CC      ?= cc
PREFIX  ?= /usr/local
DESTDIR ?=
exec_prefix = $(PREFIX)
bindir      = $(exec_prefix)/bin
datadir     = $(PREFIX)/share

# Default to release optimization. For dev iteration with clearer diagnostics,
# use `make dev` (-O0 -g) or override OPT (e.g. `make OPT=-O0`). `make clean`
# is required when switching OPT values since Make doesn't track CFLAGS
# changes.
OPT ?= -O2

# Platform detection. Detect Windows two ways: $(OS) is "Windows_NT" in most
# shells, but some MSYS2/MINGW/UCRT setups don't propagate it to make — fall
# back to looking for "_NT" anywhere in `uname -s` (which on every MSYS2-
# flavour shell returns something like MSYS_NT / MINGW64_NT / UCRT64_NT and
# never matches Linux/Darwin/*BSD). Either match flips on the .exe suffix.
UNAME_S    := $(shell uname -s 2>/dev/null)
IS_WINDOWS := $(if $(filter Windows_NT,$(OS)),1,)$(if $(findstring _NT,$(UNAME_S)),1,)

ifneq ($(IS_WINDOWS),)
    EXE := .exe
else
    EXE :=
endif

# Per-OS build subdirectory. Lets a single source tree shared across two
# operating systems (e.g. WSL Linux + MSYS2 on the same Windows box,
# accessing the WSL filesystem via //wsl.localhost/...) hold both binaries
# without one stomping the other's mtimes — without this, `make` on the
# second OS sees an "up-to-date" binary built for the first OS and refuses
# to rebuild, then the wrong-arch ./fcc dies with "Exec format error".
ifneq ($(IS_WINDOWS),)
    BUILD_OS := windows
else ifeq ($(UNAME_S),Darwin)
    BUILD_OS := macos
else ifeq ($(UNAME_S),Linux)
    BUILD_OS := linux
else
    BUILD_OS := $(UNAME_S)
endif
BUILD_DIR := build/$(BUILD_OS)

CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g $(OPT)

SRCS     := $(wildcard src/*.c)
HDRS     := $(wildcard src/*.h)
OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
BIN_NAME := fcc$(EXE)
BIN      := $(BUILD_DIR)/$(BIN_NAME)


# === Build ===

all: $(BIN)

# Echo the binary path. run.sh, demos/*/run.sh, and tests/run_tests*.sh use
# this so they don't have to replicate the OS-detection logic — `make
# print-bin` returns build/<os>/fcc[.exe] for whichever OS Make ran on.
print-bin:
	@echo $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: src/%.c $(HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $@

# Dev build: clean rebuild at -O0. Clean is required because Make doesn't track
# CFLAGS changes, so a stale -O2 .o file would otherwise be reused.
dev:
	@$(MAKE) clean
	@$(MAKE) OPT=-O0

# Removes every OS subdirectory under build/. Also sweeps any stale src/*.o
# from older in-tree builds (pre per-OS subdirectory layout).
clean:
	rm -rf build
	rm -f src/*.o


# === Install / Uninstall (GNU coding standards) ===

install: $(BIN)
	install -d $(DESTDIR)$(bindir)
	install -m 755 $(BIN) $(DESTDIR)$(bindir)/$(BIN_NAME)
	install -d $(DESTDIR)$(datadir)/fcc/stdlib
	install -m 644 stdlib/*.fc $(DESTDIR)$(datadir)/fcc/stdlib/

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(BIN_NAME)
	rm -rf $(DESTDIR)$(datadir)/fcc


# === Tests ===

# `check` is the GNU canonical test target.
check: test-all

test: $(BIN)
	@bash tests/run_tests.sh

test-parallel: $(BIN)
	@bash tests/run_tests_parallel.sh

test-gcc: $(BIN)
	@echo "=== Testing with gcc ==="
	@CC=gcc FILTER=$(FILTER) bash tests/run_tests_parallel.sh

test-clang: $(BIN)
	@echo "=== Testing with clang ==="
	@CC=clang FILTER=$(FILTER) bash tests/run_tests_parallel.sh

test-gcc-O2: $(BIN)
	@echo "=== Testing with gcc (-O2) ==="
	@CC=gcc CC_OPT=-O2 FILTER=$(FILTER) bash tests/run_tests_parallel.sh

test-clang-O2: $(BIN)
	@echo "=== Testing with clang (-O2) ==="
	@CC=clang CC_OPT=-O2 FILTER=$(FILTER) bash tests/run_tests_parallel.sh

test-all-O2: $(BIN)
	@bash -c '\
	  start=$$(date +%s%N); \
	  tmpdir=$$(mktemp -d); \
	  trap "rm -rf $$tmpdir" EXIT; \
	  (CC=gcc CC_OPT=-O2 FILTER=$(FILTER) bash tests/run_tests_parallel.sh > "$$tmpdir/gcc.out" 2>&1; \
	    echo $$? > "$$tmpdir/gcc.rc"; \
	    echo "=== Testing with gcc (-O2) ==="; cat "$$tmpdir/gcc.out"; echo "") & \
	  (CC=clang CC_OPT=-O2 FILTER=$(FILTER) bash tests/run_tests_parallel.sh > "$$tmpdir/clang.out" 2>&1; \
	    echo $$? > "$$tmpdir/clang.rc"; \
	    echo "=== Testing with clang (-O2) ==="; cat "$$tmpdir/clang.out"; echo "") & \
	  wait; \
	  gcc_rc=$$(cat "$$tmpdir/gcc.rc"); clang_rc=$$(cat "$$tmpdir/clang.rc"); \
	  end=$$(date +%s%N); \
	  elapsed_ms=$$(( (end - start) / 1000000 )); \
	  printf "Total time: %d.%03ds\n" $$((elapsed_ms / 1000)) $$((elapsed_ms % 1000)); \
	  if [ $$gcc_rc -ne 0 ] || [ $$clang_rc -ne 0 ]; then exit 1; fi'

test-all: $(BIN)
	@bash -c '\
	  start=$$(date +%s%N); \
	  tmpdir=$$(mktemp -d); \
	  trap "rm -rf $$tmpdir" EXIT; \
	  (CC=gcc FILTER=$(FILTER) bash tests/run_tests_parallel.sh > "$$tmpdir/gcc.out" 2>&1; \
	    echo $$? > "$$tmpdir/gcc.rc"; \
	    echo "=== Testing with gcc ==="; cat "$$tmpdir/gcc.out"; echo "") & \
	  (CC=clang FILTER=$(FILTER) bash tests/run_tests_parallel.sh > "$$tmpdir/clang.out" 2>&1; \
	    echo $$? > "$$tmpdir/clang.rc"; \
	    echo "=== Testing with clang ==="; cat "$$tmpdir/clang.out"; echo "") & \
	  wait; \
	  gcc_rc=$$(cat "$$tmpdir/gcc.rc"); clang_rc=$$(cat "$$tmpdir/clang.rc"); \
	  end=$$(date +%s%N); \
	  elapsed_ms=$$(( (end - start) / 1000000 )); \
	  printf "Total time: %d.%03ds\n" $$((elapsed_ms / 1000)) $$((elapsed_ms % 1000)); \
	  if [ $$gcc_rc -ne 0 ] || [ $$clang_rc -ne 0 ]; then exit 1; fi'


# === Help ===

help:
	@echo "Build:"
	@echo "  make              Build $(BIN) (release, OPT=-O2)"
	@echo "  make dev          Clean rebuild at -O0 for clearer diagnostics"
	@echo "  make OPT=-O3      Build at a specific opt level (also: -O0, -O1, etc.)"
	@echo "  make print-bin    Echo the per-OS binary path (for run.sh / scripts)"
	@echo "  make clean        Remove build/ (every OS subdirectory)"
	@echo ""
	@echo "Install (PREFIX=$(PREFIX) by default):"
	@echo "  make install      Install $(BIN_NAME) to \$$bindir and stdlib to \$$datadir/fcc/stdlib"
	@echo "  make uninstall    Remove installed binary and stdlib"
	@echo "  Override PREFIX, DESTDIR, bindir, or datadir to customize install paths."
	@echo ""
	@echo "Test:"
	@echo "  make check        Run full test suite (GNU alias of test-all)"
	@echo "  make test-all     Run tests with both gcc and clang"
	@echo "  make test-gcc     Run tests with gcc only"
	@echo "  make test-clang   Run tests with clang only"
	@echo "  make test-{gcc,clang,all}-O2   Same, but compile generated C at -O2"
	@echo "  ... FILTER=pattern             Run only tests matching pattern"


.PHONY: all dev clean install uninstall check test test-parallel \
        test-gcc test-clang test-gcc-O2 test-clang-O2 \
        test-all test-all-O2 help print-bin
