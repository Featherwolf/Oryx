# Oryon TSO probe — method (Step 2)

> ⚠️ **Danger.** This step programs IMPLEMENTATION DEFINED EL1 system registers on a live
> SoC. It can hang, panic, or brick the device. Run **only** on a dedicated,
> bootloader-unlocked S26 Ultra test unit with a known recovery path (EDL / Odin). Never on
> a device you care about. The module must fail closed and restore all registers on unload.

This directory holds the *method* for a kernel module that finds and toggles Oryon's x86
memory-ordering control, validated against the [litmus harness](../litmus/). Source is
intentionally not included yet — it is written against a specific test kernel and reverse-
engineering notes gathered on-device.

## Background: how the mechanism looks on Apple

- Apple exposes hardware TSO via a bit in `ACTLR_EL1` (an ARM-architected IMPDEF auxiliary
  control register). A support bit lives in `AIDR_EL1`. The kernel sets the `ACTLR_EL1` bit
  on kernel-exit for threads that requested TSO; it is per-thread and runtime-toggleable.
- Asahi Linux wired this to `prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO)`.

Oryon is a different microarchitecture, so the exact register/bit will differ — but the
*shape* (an EL1-writable IMPDEF control that changes observable memory ordering) is the
hypothesis.

## Search procedure

1. **Enumerate candidates.** From an EL1 kernel module, read Oryon's IMPDEF system-register
   space — the `ACTLR_EL1` / `ACTLR_EL2` / `AIDR_EL1` architected registers plus the
   `S3_<op1>_C15_Cm_op2` IMPDEF block (where vendor controls typically live). Dump and log
   their reset values.
2. **Cross-reference.** Compare against (a) Apple's known `ACTLR_EL1` TSO bit position, and
   (b) any observable difference in these registers between a Prism-emulating Windows context
   and a native one on the same Oryon silicon, if obtainable. This narrows the candidate set.
3. **Bisect with the litmus test.** For each candidate bit:
   - Set the bit for the current thread (via the module).
   - Run the `mp_litmus` kernel in `relaxed` mode (no barriers) pinned to the thread's core.
   - If the `reordered` count drops from nonzero to **zero** with the bit set and returns to
     nonzero when cleared — that bit is the memory-ordering control.
4. **Characterize.** Once found, determine:
   - **Scope:** per-thread vs per-core vs global. (Set on one thread; does another thread on
     the same/other core still reorder?)
   - **Core restriction:** does it work on Prime cores, Performance cores, or both?
   - **Cost:** re-run a throughput microbenchmark with the bit on vs off to measure the
     hardware-TSO penalty (Apple's is ~8.94%).
   - **Stability:** soak test under load; confirm no corruption, no thermal/clock anomalies.

## Output → decision

Feed the result into the [Phase 0 decision table](../README.md#step-3--the-decision):
per-thread + toggleable + stable ⇒ **PASS**, build Part A. Anything else routes to a
fallback or a stop.

## Files

| File | What |
|------|------|
| `oryx_probe.c` | The finder module: debugfs `control`/`log` files to `dump` and `poke` candidate IMPDEF EL1 registers on a chosen CPU (via `smp_call_function_single`). |
| `Kbuild`, `Makefile` | Build against the **target device** kernel (`make KDIR=... ARCH=arm64 CROSS_COMPILE=...`). Cannot build on an x86 host kernel. |
| `probe_scan.sh` | Orchestrator: sweeps (register, bit), pokes each on a core, runs the litmus MP+SB tests, and flags any bit that flips WEAK→TSO. |

## Concrete usage (rooted test device)

```sh
# build against the device kernel, push, and load
make KDIR=/path/to/s26-kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
adb push oryx_probe.ko probe_scan.sh /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/oryx_probe.ko'

# scan for the control bit on a chosen core pair (target=6, peer=7)
adb shell su -c 'cd /data/local/tmp && sh probe_scan.sh 6 7'
adb shell su -c 'cat /sys/kernel/debug/oryx_probe/log'

adb shell su -c 'rmmod oryx_probe'
```

debugfs command reference (`echo '<cmd>' > /sys/kernel/debug/oryx_probe/control`):

| Command | Effect |
|---------|--------|
| `list` | list candidate registers + indices |
| `dump <cpu>` | read every candidate register on `<cpu>` |
| `poke <idx> <bit> <cpu> <0\|1>` | clear/set one bit of candidate `<idx>` on `<cpu>` (logs before→after; flags RAZ/ignored bits) |
| `clearlog` | reset the log buffer |

The candidate register table in `oryx_probe.c` starts with `ACTLR_EL1`/`AIDR_EL1` and a few
`S3_0_C15_*` IMPDEF guesses; refine those encodings as on-device RE narrows them.

## Toward a shippable interface

If the probe succeeds, the production form is **not** this scanning module but the
per-thread memory-model driver in
[`src/kmod/oryx_memmodel`](../../src/kmod/oryx_memmodel/): it sets exactly the discovered
`(register, bit)` for opted-in threads only, reapplies it on the correct core via a preempt
notifier, restores weak ordering on context-switch away, and honors any core restriction
found here. See [Part A design](../../docs/partA-hardware-tso.md).
