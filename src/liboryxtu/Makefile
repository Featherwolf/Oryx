# liboryxtu — reference deterministic x86-64 -> AArch64 translator (Part B).
# Depends on liboryxcache (oryx_tu + sha256). Pure userspace; builds on any host.

CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -fPIC -I../liboryxcache
AR     ?= ar

CACHELIB = ../liboryxcache/liboryxcache.a

all: liboryxtu.a test_oryxtu

$(CACHELIB):
	$(MAKE) -C ../liboryxcache liboryxcache.a

oryxtu.o: oryxtu.c oryxtu.h ../liboryxcache/oryxcache.h
	$(CC) $(CFLAGS) -c -o $@ oryxtu.c

liboryxtu.a: oryxtu.o
	$(AR) rcs $@ oryxtu.o

test_oryxtu: test_oryxtu.c liboryxtu.a $(CACHELIB)
	$(CC) $(CFLAGS) -o $@ test_oryxtu.c -L. -loryxtu -L../liboryxcache -loryxcache

check: test_oryxtu
	@./test_oryxtu

clean:
	rm -f oryxtu.o liboryxtu.a test_oryxtu
	rm -rf _tucache

.PHONY: all check clean
