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

## Toward a shippable interface

If the probe succeeds, the production form is **not** this scanning module but the
`PR_SET_MEM_MODEL` handler described in [Part A](../../docs/partA-hardware-tso.md): a signed
GKI/Magisk module that sets exactly the discovered bit, only for opted-in threads, restoring
weak ordering on context-switch away, honoring any core restriction found here.
