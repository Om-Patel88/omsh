CC       ?= cc
OPT      ?= -O2
CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror $(OPT) -g
LDFLAGS  :=

# `make asan` leaves build/.san behind so later runs (test, unit) keep using
# the same flags as the objects already built. `make clean` resets it.
SAN ?= $(shell cat build/.san 2>/dev/null)

ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=build/%.o)

UNIT_TESTS := build/test_lexer build/test_parser
UNIT_DEPS  := build/lexer.o build/parser.o

.PHONY: all clean test unit ptytest asan

all: omsh

omsh: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

build/test_%: tests/unit/test_%.c $(UNIT_DEPS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(LDFLAGS) -o $@ $< $(UNIT_DEPS)

build:
	mkdir -p build

test: omsh
	sh tests/run.sh

ptytest: omsh
	python3 tests/pty_test.py

unit: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do ./$$t || exit 1; done

# Rebuild everything (the shell and unit tests) with sanitizers.
asan:
	$(MAKE) clean
	mkdir -p build && echo 1 > build/.san
	$(MAKE) SAN=1 all

clean:
	rm -rf build omsh

-include $(OBJS:.o=.d)
