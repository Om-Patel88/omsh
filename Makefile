CC       ?= cc
CPPFLAGS := -D_POSIX_C_SOURCE=200809L
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g
LDFLAGS  :=

ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=build/%.o)

UNIT_TESTS := build/test_lexer build/test_parser
UNIT_DEPS  := build/lexer.o build/parser.o

.PHONY: all clean test unit asan

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

unit: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do ./$$t || exit 1; done

# Rebuild everything (the shell and unit tests) with sanitizers.
asan:
	$(MAKE) clean
	$(MAKE) SAN=1 all

clean:
	rm -rf build omsh

-include $(OBJS:.o=.d)
