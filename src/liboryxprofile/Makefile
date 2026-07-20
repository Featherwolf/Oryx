# liboryxprofile — Oryx Part C auto-tuning profile engine. Pure userspace.

CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -fPIC
AR     ?= ar

all: liboryxprofile.a oryxprofile test_profile

profile.o: profile.c profile.h
	$(CC) $(CFLAGS) -c -o $@ profile.c

liboryxprofile.a: profile.o
	$(AR) rcs $@ profile.o

oryxprofile: oryxprofile_cli.c liboryxprofile.a
	$(CC) $(CFLAGS) -o $@ oryxprofile_cli.c -L. -loryxprofile

test_profile: test_profile.c liboryxprofile.a
	$(CC) $(CFLAGS) -o $@ test_profile.c -L. -loryxprofile

check: test_profile
	@./test_profile

clean:
	rm -f profile.o liboryxprofile.a oryxprofile test_profile
	rm -rf _testprofiles

.PHONY: all check clean
