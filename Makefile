.RECIPEPREFIX = >
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -pthread
SRC     := src/config.c src/cluster.c src/network.c src/pager.c src/directory.c src/dsm.c

all: build/test_basic build/print_sizes

build:
> mkdir -p build

build/test_basic: $(SRC) apps/test_basic.c src/internal.h | build
> $(CC) $(CFLAGS) -o $@ $(SRC) apps/test_basic.c

build/print_sizes: tools/print_sizes.c src/internal.h | build
> $(CC) $(CFLAGS) -o $@ tools/print_sizes.c

test: all
> ./scripts/run_local.sh

clean:
> rm -rf build
.PHONY: all test clean
