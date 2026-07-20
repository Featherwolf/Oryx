# Phase 0 litmus harness — build for an aarch64 Android/Linux device (or self-test on any host).
#
# Native build (on-device, e.g. under Termux):        make
# Cross-build with the Android NDK (from a host):      make NDK=/path/to/android-ndk API=34
#
# Deploy + run on the S26 Ultra:
#     adb push oryx_litmus /data/local/tmp/
#     adb push run_phase0.sh /data/local/tmp/
#     adb shell "cd /data/local/tmp && sh run_phase0.sh"
#
# See run_phase0.sh for the full experimental protocol and the decision rule.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pthread
LDFLAGS ?= -pthread

NDK ?=
API ?= 34
ifneq ($(NDK),)
  HOSTTAG   := $(shell uname | tr A-Z a-z)-x86_64
  TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/$(HOSTTAG)
  CC        := $(TOOLCHAIN)/bin/aarch64-linux-android$(API)-clang
endif

all: oryx_litmus

oryx_litmus: oryx_litmus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Layer A — validate the x86-TSO -> AArch64 *mapping* on the device (no root).
# Runs plain/rcpc/sc/dmb x MP/SB and prints the exact-TSO verdict.
layerA: oryx_litmus
	@sh run_layerA.sh

# Quick self-test: proves the binary builds and the SB control fires on the build host.
# (On an x86 host, which is TSO, MP must be 0 and SB should be > 0.)
selftest: oryx_litmus
	@echo "-- SB (expect witnesses > 0 on any OoO core, incl. this x86 host) --"
	@./oryx_litmus --test sb --mode relaxed --iters 2000000 --cpu-a 0 --cpu-b 1 || true
	@echo "-- MP relaxed (x86 host is TSO -> expect 0) --"
	@./oryx_litmus --test mp --mode relaxed --iters 2000000 --cpu-a 0 --cpu-b 1 || true
	@echo "-- MP fenced (must be 0 everywhere) --"
	@./oryx_litmus --test mp --mode fenced --iters 2000000 --cpu-a 0 --cpu-b 1 || true
	@echo "-- mapping variants build+run (rcpc/sc/dmb; sc-vs-rcpc SB contrast is ARM-only) --"
	@for m in rcpc sc dmb; do ./oryx_litmus --test mp --map $$m --iters 1000000 --cpu-a 0 --cpu-b 1 --json || true; done

clean:
	rm -f oryx_litmus

.PHONY: all selftest layerA clean
