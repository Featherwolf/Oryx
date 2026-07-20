# Top-level build for the Oryx userspace components (host-buildable).
#
#   make        build liboryxmm, liboryxcache, liboryxprofile + tools
#   make check  build + run all unit tests
#   make clean
#
# The kernel modules (kmod/) build separately against a target-device kernel.

# liboryxtu depends on liboryxcache; liboryxmap and liboryxrun depend on both. Keep order.
USERSPACE = liboryxmm liboryxcache liboryxtu liboryxmap liboryxprofile liboryxrun

all:
	@for d in $(USERSPACE); do echo "== build $$d =="; $(MAKE) -C $$d all || exit 1; done

check:
	@for d in $(USERSPACE); do echo "== check $$d =="; $(MAKE) -C $$d check || exit 1; done
	@echo ""
	@echo "All Oryx userspace test suites passed."

# Execute the translated blocks for real: static AArch64 build under qemu.
# Skips cleanly if the cross toolchain / qemu are absent.
check-qemu:
	$(MAKE) -C liboryxrun check-qemu

clean:
	@for d in $(USERSPACE); do $(MAKE) -C $$d clean; done

.PHONY: all check check-qemu clean
