.RECIPEPREFIX = >
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -pthread
# make FALLBACK=1 : fuerza la deteccion lectura/escritura por estado de pagina (prueba del respaldo no x86-64)
ifdef FALLBACK
CFLAGS += -DDSM_STATE_FALLBACK
endif
LIB     := src/config.c src/cluster.c src/network.c src/pager.c src/directory.c src/dsm.c src/stats.c src/tasks.c
BINS    := test_basic test_stress test_hb test_leave test_tasks app_matmul app_tasks app_counter dsm_node print_sizes

all: $(addprefix build/,$(BINS))

build:
> mkdir -p build

build/dsm_node: src/main.c $(LIB) src/internal.h | build
> $(CC) $(CFLAGS) -o $@ src/main.c $(LIB)

build/print_sizes: tools/print_sizes.c src/internal.h | build
> $(CC) $(CFLAGS) -o $@ tools/print_sizes.c

build/%: apps/%.c $(LIB) src/internal.h | build
> $(CC) $(CFLAGS) -o $@ $< $(LIB) -lm

test: all
> ./scripts/run_all.sh

clean:
> rm -rf build
.PHONY: all test clean