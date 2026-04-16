CC = cc
OPT ?= -O0
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g $(OPT)
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
BIN = fc

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

HDRS = $(wildcard src/*.h)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(BIN)

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

.PHONY: all clean test test-parallel test-gcc test-clang test-all test-gcc-O2 test-clang-O2 test-all-O2
