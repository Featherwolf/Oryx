# Build the oryx_memmodel driver against the TARGET DEVICE's kernel tree.
# Requires arm64 + CONFIG_PREEMPT_NOTIFIERS=y.
#
#   make KDIR=/path/to/s26-kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
#
# Load (rooted device), pinning the Phase-0-confirmed control bit:
#   insmod oryx_memmodel.ko oryx_reg_idx=0 oryx_bit=<N> oryx_confirmed=1 \
#          oryx_eligible=0xFF
#
# Until oryx_confirmed=1, ENABLE_TSO fails closed with -ENODEV by design.

KDIR          ?= /lib/modules/$(shell uname -r)/build
ARCH          ?= arm64
CROSS_COMPILE ?=

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: default clean
