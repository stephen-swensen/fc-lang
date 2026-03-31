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
	@CC=gcc bash tests/run_tests_parallel.sh

test-clang: $(BIN)
	@echo "=== Testing with clang ==="
	@CC=clang bash tests/run_tests_parallel.sh

test-all: $(BIN)
	@echo "=== Testing with gcc ==="
	@CC=gcc bash tests/run_tests_parallel.sh
	@echo ""
	@echo "=== Testing with clang ==="
	@CC=clang bash tests/run_tests_parallel.sh

.PHONY: all clean test test-parallel test-gcc test-clang test-all
