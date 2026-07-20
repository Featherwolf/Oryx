# Top-level build for the Oryx userspace components (host-buildable).
#
#   make        build liboryxmm, liboryxcache, liboryxprofile + tools
#   make check  build + run all unit tests
#   make clean
#
# The kernel modules (kmod/) build separately against a target-device kernel.

USERSPACE = liboryxmm liboryxcache liboryxprofile

all:
	@for d in $(USERSPACE); do echo "== build $$d =="; $(MAKE) -C $$d all || exit 1; done

check:
	@for d in $(USERSPACE); do echo "== check $$d =="; $(MAKE) -C $$d check || exit 1; done
	@echo ""
	@echo "All Oryx userspace test suites passed."

clean:
	@for d in $(USERSPACE); do $(MAKE) -C $$d clean; done

.PHONY: all check clean
