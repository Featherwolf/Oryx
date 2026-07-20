# Phase 0 litmus harness — build for an aarch64 Android/Linux device.
#
# Native build (on-device, e.g. under Termux):
#     make
#
# Cross-build with the Android NDK (from a host):
#     make NDK=/path/to/android-ndk API=34
#
# Then push and run on the S26 Ultra:
#     adb push mp_litmus /data/local/tmp/
#     adb shell /data/local/tmp/mp_litmus --mode relaxed --iters 20000000 --cpu-a 0 --cpu-b 6
#     adb shell /data/local/tmp/mp_litmus --mode fenced  --iters 20000000 --cpu-a 0 --cpu-b 6
#
# Core numbering on SM8850 is 2 Prime + 6 Performance. Try several (cpu-a, cpu-b)
# pairs — same cluster, cross cluster — since a hardware TSO restriction (if any)
# may be core-type specific (Apple restricts TSO threads to performance cores).

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pthread
LDFLAGS ?= -pthread

# --- Optional NDK cross-compile ---------------------------------------------
NDK ?=
API ?= 34
ifneq ($(NDK),)
  HOSTTAG := $(shell uname | tr A-Z a-z)-x86_64
  TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/$(HOSTTAG)
  CC := $(TOOLCHAIN)/bin/aarch64-linux-android$(API)-clang
endif
# ---------------------------------------------------------------------------

all: mp_litmus

mp_litmus: mp_litmus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f mp_litmus

.PHONY: all clean
