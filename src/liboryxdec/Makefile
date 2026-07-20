# liboryxdec — x86-64 decoder. Decode correctness runs on any host; `make
# check-qemu` decodes real compiled functions and EXECUTES them under qemu.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -fPIC -I../liboryxcache -I../liboryxtu -I../liboryxrun
AR      ?= ar

CACHEDIR = ../liboryxcache
TUDIR    = ../liboryxtu
RUNDIR   = ../liboryxrun
CACHELIB = $(CACHEDIR)/liboryxcache.a
TULIB    = $(TUDIR)/liboryxtu.a
RUNLIB   = $(RUNDIR)/liboryxrun.a

DEP_SRCS = $(RUNDIR)/oryxrun.c $(TUDIR)/oryxtu.c $(CACHEDIR)/oryxcache.c $(CACHEDIR)/sha256.c

XCC  ?= aarch64-linux-gnu-gcc
QEMU ?= qemu-aarch64-static

all: liboryxdec.a test_oryxdec

$(CACHELIB):
	$(MAKE) -C $(CACHEDIR) liboryxcache.a
$(TULIB):
	$(MAKE) -C $(TUDIR) liboryxtu.a
$(RUNLIB):
	$(MAKE) -C $(RUNDIR) liboryxrun.a

oryxdec.o: oryxdec.c oryxdec.h $(CACHEDIR)/oryxcache.h $(TUDIR)/oryxtu.h
	$(CC) $(CFLAGS) -c -o $@ oryxdec.c

liboryxdec.a: oryxdec.o
	$(AR) rcs $@ oryxdec.o

test_oryxdec: test_oryxdec.c liboryxdec.a $(RUNLIB) $(TULIB) $(CACHELIB)
	$(CC) $(CFLAGS) -o $@ test_oryxdec.c -L. -loryxdec -L$(RUNDIR) -loryxrun -L$(TUDIR) -loryxtu -L$(CACHEDIR) -loryxcache

# Host check: decode-correctness only (execution self-skips off AArch64).
check: test_oryxdec
	@./test_oryxdec

# Real check: decode real compiled bytes and EXECUTE them under qemu.
check-qemu: test_oryxdec.c oryxdec.c $(DEP_SRCS)
	@command -v $(XCC)  >/dev/null 2>&1 || { echo "SKIP check-qemu: $(XCC) not found"; exit 0; }
	@command -v $(QEMU) >/dev/null 2>&1 || { echo "SKIP check-qemu: $(QEMU) not found"; exit 0; }
	$(XCC) -O2 -std=c11 -Wall -Wextra -static -I$(CACHEDIR) -I$(TUDIR) -I$(RUNDIR) \
		-o test_oryxdec_a64 test_oryxdec.c oryxdec.c $(DEP_SRCS)
	$(QEMU) ./test_oryxdec_a64

clean:
	rm -f oryxdec.o liboryxdec.a test_oryxdec test_oryxdec_a64

.PHONY: all check check-qemu clean
