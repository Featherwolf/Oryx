# liboryxrun — execution runtime for translated blocks.
# Host build proves it compiles/links and exercises arch-independent guards.
# `make check-qemu` cross-builds static AArch64 and EXECUTES the blocks under
# qemu-aarch64-static — the real behavioural test.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -fPIC -I../liboryxcache -I../liboryxtu
AR      ?= ar

CACHEDIR = ../liboryxcache
TUDIR    = ../liboryxtu
CACHELIB = $(CACHEDIR)/liboryxcache.a
TULIB    = $(TUDIR)/liboryxtu.a

DEP_SRCS = $(TUDIR)/oryxtu.c $(CACHEDIR)/oryxcache.c $(CACHEDIR)/sha256.c

# cross toolchain for the real execution test
XCC   ?= aarch64-linux-gnu-gcc
QEMU  ?= qemu-aarch64-static

all: liboryxrun.a test_oryxrun

$(CACHELIB):
	$(MAKE) -C $(CACHEDIR) liboryxcache.a
$(TULIB):
	$(MAKE) -C $(TUDIR) liboryxtu.a

oryxrun.o: oryxrun.c oryxrun.h $(CACHEDIR)/oryxcache.h $(TUDIR)/oryxtu.h
	$(CC) $(CFLAGS) -c -o $@ oryxrun.c

liboryxrun.a: oryxrun.o
	$(AR) rcs $@ oryxrun.o

test_oryxrun: test_oryxrun.c liboryxrun.a $(TULIB) $(CACHELIB)
	$(CC) $(CFLAGS) -o $@ test_oryxrun.c -L. -loryxrun -L$(TUDIR) -loryxtu -L$(CACHEDIR) -loryxcache

# Host check: builds + runs; on x86 the execution assertions self-skip.
check: test_oryxrun
	@./test_oryxrun

# Real check: static AArch64 build, executed under qemu. This is where the
# translated STLR/LDAPR/LDR/atomics actually run.
check-qemu: test_oryxrun.c oryxrun.c $(DEP_SRCS)
	@command -v $(XCC)  >/dev/null 2>&1 || { echo "SKIP check-qemu: $(XCC) not found"; exit 0; }
	@command -v $(QEMU) >/dev/null 2>&1 || { echo "SKIP check-qemu: $(QEMU) not found"; exit 0; }
	$(XCC) -O2 -std=c11 -Wall -Wextra -static -I$(CACHEDIR) -I$(TUDIR) \
		-o test_oryxrun_a64 test_oryxrun.c oryxrun.c $(DEP_SRCS)
	$(QEMU) ./test_oryxrun_a64

clean:
	rm -f oryxrun.o liboryxrun.a test_oryxrun test_oryxrun_a64

.PHONY: all check check-qemu clean
