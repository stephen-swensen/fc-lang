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

# Platform-specific binary suffix. On Windows (incl. MSYS2/UCRT64), $(OS) is
# set to "Windows_NT" and we want the .exe extension.
ifeq ($(OS),Windows_NT)
    EXE := .exe
else
    EXE :=
endif

CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g $(OPT)

SRCS = $(wildcard src/*.c)
HDRS = $(wildcard src/*.h)
OBJS = $(SRCS:.c=.o)
BIN  = fcc$(EXE)


# === Build ===

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Dev build: clean rebuild at -O0. Clean is required because Make doesn't track
# CFLAGS changes, so a stale -O2 .o file would otherwise be reused.
dev:
	@$(MAKE) clean
	@$(MAKE) OPT=-O0

clean:
	rm -f src/*.o $(BIN)


# === Install / Uninstall (GNU coding standards) ===

install: $(BIN)
	install -d $(DESTDIR)$(bindir)
	install -m 755 $(BIN) $(DESTDIR)$(bindir)/$(BIN)
	install -d $(DESTDIR)$(datadir)/fcc/stdlib
	install -m 644 stdlib/*.fc $(DESTDIR)$(datadir)/fcc/stdlib/

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(BIN)
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
	@echo "  make clean        Remove build artifacts"
	@echo ""
	@echo "Install (PREFIX=$(PREFIX) by default):"
	@echo "  make install      Install $(BIN) to \$$bindir and stdlib to \$$datadir/fcc/stdlib"
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
        test-all test-all-O2 help
