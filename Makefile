CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
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

test-all: $(BIN)
	@bash -c '\
	  tmpdir=$$(mktemp -d); \
	  trap "rm -rf $$tmpdir" EXIT; \
	  CC=gcc FILTER=$(FILTER) bash tests/run_tests_parallel.sh > "$$tmpdir/gcc.out" 2>&1 & gcc_pid=$$!; \
	  CC=clang FILTER=$(FILTER) bash tests/run_tests_parallel.sh > "$$tmpdir/clang.out" 2>&1 & clang_pid=$$!; \
	  gcc_rc=0; wait $$gcc_pid || gcc_rc=$$?; \
	  clang_rc=0; wait $$clang_pid || clang_rc=$$?; \
	  echo "=== Testing with gcc ==="; cat "$$tmpdir/gcc.out"; \
	  echo ""; \
	  echo "=== Testing with clang ==="; cat "$$tmpdir/clang.out"; \
	  if [ $$gcc_rc -ne 0 ] || [ $$clang_rc -ne 0 ]; then exit 1; fi'

.PHONY: all clean test test-parallel test-gcc test-clang test-all
