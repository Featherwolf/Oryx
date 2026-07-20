# liboryxmm — userspace client for the Oryx memory-model driver.
# Builds on any host (pure open/ioctl); no arch dependency.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -fPIC
AR      ?= ar

all: liboryxmm.a oryxmm_demo

oryxmm.o: oryxmm.c oryxmm.h
	$(CC) $(CFLAGS) -c -o $@ oryxmm.c

liboryxmm.a: oryxmm.o
	$(AR) rcs $@ $<

oryxmm_demo: oryxmm_demo.c liboryxmm.a
	$(CC) $(CFLAGS) -o $@ oryxmm_demo.c -L. -loryxmm

check: oryxmm_demo
	@echo "-- fail-closed self-test (no driver present on this host) --"
	@./oryxmm_demo

clean:
	rm -f oryxmm.o liboryxmm.a oryxmm_demo

.PHONY: all check clean
