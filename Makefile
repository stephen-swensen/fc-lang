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

# === Version components ===
# Hand-maintained SemVer prefix. Bump on intentional releases.
FCC_VERSION_BASE := $(shell cat VERSION 2>/dev/null || echo "0.0.0-unknown")

# Git metadata in UTC. Each falls back independently so a partial git env
# (shallow clone, missing tags) still produces a sensible version. The
# dirty check first verifies we're in a checkout, otherwise tarball builds
# would falsely flag dirty when `git diff` errors out.
FCC_GIT_HASH  := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo nogit)
FCC_GIT_DATE  := $(shell TZ=UTC git log -1 --format=%cd --date=format:%y.%m.%d 2>/dev/null || echo unknown)
FCC_GIT_DIRTY := $(shell (git rev-parse --is-inside-work-tree >/dev/null 2>&1 && ! git diff --quiet HEAD 2>/dev/null) && echo "-dirty")

# Build environment.
FCC_BUILD_DATE := $(shell date -u +%Y-%m-%d)
FCC_BUILD_CC   := $(CC) $(shell $(CC) -dumpfullversion 2>/dev/null || $(CC) -dumpversion 2>/dev/null || echo unknown)

# -I$(BUILD_DIR) so generated fcc_version.h is on the include path.
# -DFCC_DATADIR bakes in the install data dir so `fcc --lsp` can locate the
# installed stdlib (FCC_STDLIB_DIR env overrides; a repo-relative ./stdlib is
# the final fallback).
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g $(OPT) -I$(BUILD_DIR) \
         -DFCC_DATADIR='"$(datadir)"'

SRCS     := $(wildcard src/*.c)
HDRS     := $(wildcard src/*.h src/*.inc)
OBJS     := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
BIN_NAME := fcc$(EXE)
BIN      := $(BUILD_DIR)/$(BIN_NAME)

GEN_VERSION_H := $(BUILD_DIR)/fcc_version.h


# === Build ===

all: $(BIN)

# Echo the binary path, build/<os>/fcc[.exe]. run.sh, demos/*/run.sh and
# tests/run_tests.sh use this instead of repeating the OS detection above.
print-bin:
	@echo $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: src/%.c $(HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Generated version header — depends on FORCE so it re-evaluates every
# `make` invocation, but cmp-and-replace means we only mv it into place
# when contents actually change. version.o is the only object that
# depends on it, so a version-only churn is a one-file rebuild.
.PHONY: FORCE
FORCE:

$(GEN_VERSION_H): FORCE | $(BUILD_DIR)
	@{ \
	    printf '#define FCC_VERSION_BASE "%s"\n' "$(FCC_VERSION_BASE)"; \
	    printf '#define FCC_GIT_HASH "%s"\n'     "$(FCC_GIT_HASH)"; \
	    printf '#define FCC_GIT_DATE "%s"\n'     "$(FCC_GIT_DATE)"; \
	    printf '#define FCC_GIT_DIRTY "%s"\n'    "$(FCC_GIT_DIRTY)"; \
	    printf '#define FCC_BUILD_DATE "%s"\n'   "$(FCC_BUILD_DATE)"; \
	    printf '#define FCC_BUILD_CC "%s"\n'     "$(FCC_BUILD_CC)"; \
	    printf '#define FCC_BUILD_OPT "%s"\n'    "$(OPT)"; \
	} > $@.tmp
	@if cmp -s $@.tmp $@ 2>/dev/null; then rm $@.tmp; else mv $@.tmp $@; fi

$(BUILD_DIR)/version.o: $(GEN_VERSION_H)

$(BUILD_DIR):
	@mkdir -p $@

# Dev build: clean rebuild at -O0. Clean is required because Make doesn't track
# CFLAGS changes, so a stale -O2 .o file would otherwise be reused.
dev:
	@$(MAKE) clean
	@$(MAKE) OPT=-O0

# Removes every OS subdirectory under build/.
clean:
	rm -rf build


# === Install / Uninstall (GNU coding standards) ===

install: $(BIN)
	install -d $(DESTDIR)$(bindir)
	install -m 755 $(BIN) $(DESTDIR)$(bindir)/$(BIN_NAME)
	install -d $(DESTDIR)$(datadir)/fcc/stdlib
	install -m 644 stdlib/*.fc $(DESTDIR)$(datadir)/fcc/stdlib/

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(BIN_NAME)
	rm -rf $(DESTDIR)$(datadir)/fcc


# === VSCode extension (Linux) ===
# The extension is dependency-free (it drives `fcc --lsp` with VSCode's built-in
# Node child_process — no npm, no vscode-languageclient). install.sh packages a
# .vsix by hand (no vsce) and installs it via the editor CLI (`code
# --install-extension`), since modern VSCode ignores folders merely copied into
# the extensions dir. Run WITHOUT sudo — it installs into your own $HOME.
# Orthogonal to `make install` (which puts fcc on PATH). Override the editor with
# FC_CODE_CLI=codium (etc.). Reload the VSCode window afterwards.
VSCODE_SRC := editors/vscode

install-vscode: $(BIN)
	@bash $(VSCODE_SRC)/install.sh install

uninstall-vscode:
	@bash $(VSCODE_SRC)/install.sh uninstall


# === Tests ===
# Every suite runs tests/run_tests.sh. CC picks the compiler for the generated
# C, CC_OPT its optimization flags, FCC_EXTRA_ARGS adds fcc options, and FILTER
# is a grep pattern over category/test_name.
RUN_TESTS = FILTER=$(FILTER) bash tests/run_tests.sh

# `check` is the GNU canonical test target.
check: check-ascii test-all

# Compiler sources are plain ASCII. The one exception is src/builtin_docs.inc,
# user-facing hover markdown that keeps its typography.
check-ascii:
	@if LC_ALL=C grep -nH "$$(printf '[\200-\377]')" src/*.c src/*.h; then \
	  echo "error: non-ASCII bytes in src/ (see lines above)"; exit 1; fi

test-gcc: $(BIN)
	@echo "=== Testing with gcc ==="
	@CC=gcc $(RUN_TESTS)

test-clang: $(BIN)
	@echo "=== Testing with clang ==="
	@CC=clang $(RUN_TESTS)

test-gcc-O2: $(BIN)
	@echo "=== Testing with gcc (-O2) ==="
	@CC=gcc CC_OPT=-O2 $(RUN_TESTS)

test-clang-O2: $(BIN)
	@echo "=== Testing with clang (-O2) ==="
	@CC=clang CC_OPT=-O2 $(RUN_TESTS)

# The whole suite with 16-bit stored slice lengths. --len-repr changes only
# the representation, so the retro configuration is testable on the host.
test-gcc-len16: $(BIN)
	@echo "=== Testing with gcc (--len-repr 16) ==="
	@CC=gcc FCC_EXTRA_ARGS="--len-repr 16" $(RUN_TESTS)

test-clang-len16: $(BIN)
	@echo "=== Testing with clang (--len-repr 16) ==="
	@CC=clang FCC_EXTRA_ARGS="--len-repr 16" $(RUN_TESTS)

# Run the suite under gcc and clang at the same time and print each log whole.
# $(1) is CC_OPT for the generated C (empty for the default, unoptimized).
define test_both
	@bash -c '\
	  start=$$(date +%s%N); \
	  tmpdir=$$(mktemp -d); \
	  trap "rm -rf $$tmpdir" EXIT; \
	  for cc in gcc clang; do \
	    (CC=$$cc CC_OPT=$(1) $(RUN_TESTS) > "$$tmpdir/$$cc.out" 2>&1; \
	      echo $$? > "$$tmpdir/$$cc.rc") & \
	  done; \
	  wait; \
	  rc=0; \
	  for cc in gcc clang; do \
	    echo "=== Testing with $$cc$(if $(1), ($(1))) ==="; \
	    cat "$$tmpdir/$$cc.out"; echo ""; \
	    [ "$$(cat "$$tmpdir/$$cc.rc")" = 0 ] || rc=1; \
	  done; \
	  elapsed_ms=$$(( ($$(date +%s%N) - start) / 1000000 )); \
	  printf "Total time: %d.%03ds\n" $$((elapsed_ms / 1000)) $$((elapsed_ms % 1000)); \
	  exit $$rc'
endef

test-all: $(BIN)
	$(call test_both,)

test-all-O2: $(BIN)
	$(call test_both,-O2)

# Language-server wire tests. Kept out of `check` because they need python3.
test-lsp: $(BIN)
	@echo "=== Testing LSP server ==="
	@bash tests/lsp/run_lsp_tests.sh


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
	@echo "  make install         Install $(BIN_NAME) to \$$bindir and stdlib to \$$datadir/fcc/stdlib"
	@echo "  make uninstall       Remove installed binary and stdlib"
	@echo "  make install-vscode  Install the VSCode extension (Linux, no deps; run without sudo)"
	@echo "  make uninstall-vscode Remove the installed VSCode extension"
	@echo "  Override PREFIX, DESTDIR, bindir, or datadir to customize install paths."
	@echo ""
	@echo "Test:"
	@echo "  make check        check-ascii, then the full test suite (test-all)"
	@echo "  make check-ascii  Fail on non-ASCII bytes in src/*.c and src/*.h"
	@echo "  make test-all     Run tests with both gcc and clang"
	@echo "  make test-gcc     Run tests with gcc only"
	@echo "  make test-clang   Run tests with clang only"
	@echo "  make test-{gcc,clang,all}-O2   Same, but compile generated C at -O2"
	@echo "  make test-{gcc,clang}-len16    Same, but with --len-repr 16 (16-bit slice lens)"
	@echo "  make test-lsp     Run the LSP server wire tests (needs python3)"
	@echo "  ... FILTER=pattern             Run only tests matching pattern"


.PHONY: all dev clean install uninstall install-vscode uninstall-vscode \
        check check-ascii test-lsp \
        test-gcc test-clang test-gcc-O2 test-clang-O2 \
        test-gcc-len16 test-clang-len16 \
        test-all test-all-O2 help print-bin
