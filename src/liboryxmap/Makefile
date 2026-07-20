# liboryxmap — offline memory-class map (Door 3 no-root lever).
# Depends on liboryxcache (store + sha256) and liboryxtu (guest IR). Host-buildable.

CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -fPIC -I../liboryxcache -I../liboryxtu
AR     ?= ar

CACHELIB = ../liboryxcache/liboryxcache.a
TULIB    = ../liboryxtu/liboryxtu.a

all: liboryxmap.a test_oryxmap

$(CACHELIB):
	$(MAKE) -C ../liboryxcache liboryxcache.a
$(TULIB):
	$(MAKE) -C ../liboryxtu liboryxtu.a

oryxmap.o: oryxmap.c oryxmap.h ../liboryxcache/oryxcache.h ../liboryxtu/oryxtu.h
	$(CC) $(CFLAGS) -c -o $@ oryxmap.c

liboryxmap.a: oryxmap.o
	$(AR) rcs $@ oryxmap.o

test_oryxmap: test_oryxmap.c liboryxmap.a $(TULIB) $(CACHELIB)
	$(CC) $(CFLAGS) -o $@ test_oryxmap.c -L. -loryxmap -L../liboryxtu -loryxtu -L../liboryxcache -loryxcache

check: test_oryxmap
	@./test_oryxmap

clean:
	rm -f oryxmap.o liboryxmap.a test_oryxmap
	rm -rf _mapcache

.PHONY: all check clean
