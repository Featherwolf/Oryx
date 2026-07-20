# Project Oryx — top-level convenience targets.
#
#   make check     build + run every host test suite (the quick one to run)
#   make all       build the userspace libraries and tools
#   make aarch64   cross-compile everything for the S26 Ultra's architecture
#   make clean
#
# See TESTING.md for the full guide (host tests, CI, and on-device Phase 0).

.PHONY: all check aarch64 clean

all:
	$(MAKE) -C src all

check:
	$(MAKE) -C src check
	@echo ""
	@echo "== Phase 0 litmus self-test (host) =="
	$(MAKE) -C experiments/phase0-tso-probe/litmus selftest
	@echo ""
	@echo "All Oryx host checks complete."

# Prove the whole tree builds for aarch64 (the Snapdragon 8 Elite Gen 5 target).
aarch64:
	$(MAKE) -C experiments/phase0-tso-probe/litmus clean
	$(MAKE) -C experiments/phase0-tso-probe/litmus CC=aarch64-linux-gnu-gcc
	$(MAKE) -C src clean
	$(MAKE) -C src CC=aarch64-linux-gnu-gcc
	@echo "aarch64 cross-build OK"

clean:
	$(MAKE) -C src clean
	$(MAKE) -C experiments/phase0-tso-probe/litmus clean
