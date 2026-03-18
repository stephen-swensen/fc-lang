CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
BIN = fc

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(BIN)

test: $(BIN)
	@bash tests/run_tests.sh

test-parallel: $(BIN)
	@bash tests/run_tests_parallel.sh

.PHONY: all clean test test-parallel
