.RECIPEPREFIX = >
CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -pthread
# make FALLBACK=1 : fuerza la deteccion lectura/escritura por estado de pagina (prueba del respaldo no x86-64)
ifdef FALLBACK
CFLAGS += -DDSM_STATE_FALLBACK
endif
LIB     := src/config.c src/cluster.c src/network.c src/pager.c src/directory.c src/dsm.c src/stats.c src/tasks.c
MATHLIB := build/libdsm_math.a
BINS    := test_basic test_task app_tasks app_pagerank app_heat3d dsm_node print_sizes

all: $(addprefix build/,$(BINS))

build:
> mkdir -p build

$(MATHLIB): lib/libdsm_math.c lib/libdsm_math.h | build
> $(CC) $(CFLAGS) -c -o build/libdsm_math.o lib/libdsm_math.c
> $(AR) rcs $@ build/libdsm_math.o

build/dsm_node: src/main.c $(LIB) $(MATHLIB) src/internal.h | build
> $(CC) $(CFLAGS) -o $@ src/main.c $(LIB) $(MATHLIB)

build/print_sizes: tools/print_sizes.c $(MATHLIB) src/internal.h | build
> $(CC) $(CFLAGS) -o $@ tools/print_sizes.c $(MATHLIB)

build/%: apps/%.c $(LIB) $(MATHLIB) src/internal.h | build
> $(CC) $(CFLAGS) -o $@ $< $(LIB) $(MATHLIB)

test: all
> ./scripts/run_local.sh dsm.conf

clean:
> rm -rf build
.PHONY: all test clean
