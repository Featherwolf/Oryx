# liboryxcache — content-addressed translation/shader cache (Part B).
# Pure userspace, no external deps; builds and tests on any host.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -fPIC
AR      ?= ar

OBJS = sha256.o oryxcache.o

all: liboryxcache.a oryxcache test_oryxcache

sha256.o: sha256.c sha256.h
	$(CC) $(CFLAGS) -c -o $@ sha256.c
oryxcache.o: oryxcache.c oryxcache.h sha256.h
	$(CC) $(CFLAGS) -c -o $@ oryxcache.c

liboryxcache.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

oryxcache: oryxcache_cli.c liboryxcache.a
	$(CC) $(CFLAGS) -o $@ oryxcache_cli.c -L. -loryxcache

test_oryxcache: test_oryxcache.c liboryxcache.a
	$(CC) $(CFLAGS) -o $@ test_oryxcache.c -L. -loryxcache

check: test_oryxcache
	@./test_oryxcache

clean:
	rm -f $(OBJS) liboryxcache.a oryxcache test_oryxcache
	rm -rf _testcache

.PHONY: all check clean
