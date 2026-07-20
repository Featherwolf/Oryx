# Build the oryx_probe kernel module against the TARGET DEVICE's kernel tree.
#
# You need the S26 Ultra kernel source (or GKI headers) matching the running
# kernel, and an aarch64 cross-compiler. This module cannot be built against a
# host x86 kernel.
#
#   make KDIR=/path/to/s26-kernel ARCH=arm64 \
#        CROSS_COMPILE=aarch64-linux-gnu-
#
# Then, on the (rooted, test-only) device:
#   insmod oryx_probe.ko
#   sh probe_scan.sh          # drives the debugfs interface + litmus
#   rmmod oryx_probe
#
# ⚠  This module writes IMPDEF EL1 registers. Test devices only. See README.md.

KDIR          ?= /lib/modules/$(shell uname -r)/build
ARCH          ?= arm64
CROSS_COMPILE ?=

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: default clean
